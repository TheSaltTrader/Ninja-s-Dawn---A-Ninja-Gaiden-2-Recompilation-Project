#include "ng2_native_gpu.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ng2::ngpu {
namespace {

// The hand-off ABI, declared HERE rather than included from the SDK.
//
// Not a workaround for the exe building against the installed headers - it is
// the right shape. The record carries struct_size precisely so a plugin and an
// executable built apart can detect a mismatch, and sharing the header makes
// that field untestable: it would be right by construction, and the first time
// the two really did diverge there would be nothing to catch it. A consumer in
// another codebase would declare it exactly like this, so this one does.
//
// Must match rex/system/gpu_plugin.h. If a field is added there and not here,
// struct_size differs and OnDraw counts a mismatch instead of reading garbage -
// which is the contract working, not failing.
struct GpuDrawRecord {
  uint32_t struct_size;

  uint32_t vgt_draw_initiator;
  uint32_t index_base;
  uint32_t index_size_words;
  uint32_t index_endian;

  uint32_t vs_address, vs_dwords;
  uint32_t ps_address, ps_dwords;
  uint32_t vs_immediate, ps_immediate;

  const uint32_t* registers;
  uint32_t register_count;

  uint64_t bin_mask, bin_select;
  uint32_t predicated;

  // This draw's ordinal in the stream, from 1, counted by the plugin at the
  // hand-off site. See OnDraw: checking it is how this stage proves the
  // boundary without a second instrument to compare against.
  uint64_t draw_serial;
};

using GpuDrawFn = void (*)(const GpuDrawRecord*);
using GpuSetDrawCallbackFn = void (*)(GpuDrawFn);
constexpr const char* kSetDrawCallbackSymbol = "rex_gpu_set_draw_callback";

// VGT_DRAW_INITIATOR, decoded from the bit positions rather than from a struct,
// for the same reason: no SDK header dependency on the consumer side.
//   prim_type 0:5   source_select 6:7   index_size bit 11   num_indices 16:31
// source_select: 0 = kDMA (an index buffer), 1 = kImmediate, 2 = kAutoIndex.
constexpr uint32_t kSourceDMA = 0;

// STAGE 1: count only.
//
// The renderer is deliberately not wired up yet. What has to be established
// first is that the exe and the plugin agree - that the export resolves, the
// record crosses the module boundary intact, and the draws arriving here are
// the same draws the plugin executes. A renderer built on an unverified
// hand-off would attribute its own missing geometry to itself.
//
// The plugin's own NGPU_DRAW_SELFTEST already reconciled an internal consumer
// against its census (2,347 draws / 764,123 indices, exact). This is the same
// reconciliation across the DLL boundary, which an internal consumer cannot
// test.
std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_indices{0};
std::atomic<uint64_t> g_indexed{0};
std::atomic<uint64_t> g_auto{0};
std::atomic<uint64_t> g_bad_size{0};
std::atomic<uint64_t> g_no_regs{0};
std::atomic<uint64_t> g_no_shader{0};

// The serial check. g_last_serial is touched only from the GPU worker thread
// (the callback's thread), so it needs no atomic; the tallies are read by the
// reporter thread and do.
uint64_t g_last_serial = 0;
std::atomic<uint64_t> g_gaps{0};       // records that skipped at least one serial
std::atomic<uint64_t> g_missing{0};    // how many draws those gaps account for
std::atomic<uint64_t> g_repeats{0};    // a serial that did not advance
std::atomic<uint64_t> g_first_gap{0};  // the serial we were at when the first gap appeared

GpuSetDrawCallbackFn g_setter = nullptr;
std::atomic<bool> g_reporting{false};

// Totals as one line. Shared by the periodic reporter and by Stop().
std::string Totals() {
  // The serial verdict is stated in full either way. "0 gaps" out of a named
  // number of draws is the evidence; a bare count of draws would not be.
  const uint64_t gaps = g_gaps.load(), missing = g_missing.load(), repeats = g_repeats.load();
  std::string serial;
  if (!gaps && !repeats) {
    serial = fmt::format(" | SERIAL UNBROKEN 1..{}", g_last_serial);
  } else {
    serial = fmt::format(" | SERIAL BROKEN: {} gap(s) losing {} draw(s){}{}", gaps, missing,
                         repeats ? fmt::format(", {} repeat(s)", repeats) : "",
                         g_first_gap.load() ? fmt::format(", first at {}", g_first_gap.load()) : "");
  }
  return fmt::format("{} draws ({} indexed, {} auto), {} indices{}{}{}{}",
                     g_draws.load(), g_indexed.load(), g_auto.load(), g_indices.load(), serial,
                     g_bad_size.load() ? fmt::format(" | {} ABI MISMATCH", g_bad_size.load()) : "",
                     g_no_regs.load() ? fmt::format(" | {} without registers", g_no_regs.load()) : "",
                     g_no_shader.load() ? fmt::format(" | {} without a shader", g_no_shader.load()) : "");
}

// Report on a timer, not only at shutdown.
//
// Stop() runs on a clean exit and nothing else, and on this branch a run's
// usual ending is a hang in the Chapter 1 intro followed by a kill - so the one
// measurement this stage exists to produce was, in practice, never written. A
// counter you can only read by finishing is not an instrument.
//
// Its own thread rather than a hook in OnDraw: OnDraw is on the GPU worker
// inside packet execution, thousands of times a frame, and this project has
// already spent two builds on the theory that logging from that thread was
// hanging the game. It was not - but the way to keep that answer clean is to
// not log from there at all.
void Reporter(unsigned every_s) {
  while (g_reporting.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    static unsigned t = 0;
    if (++t < every_s) continue;
    t = 0;
    if (g_draws.load()) REXLOG_INFO("[ng2-ngpu] running totals: {}", Totals());
  }
}

// Runs on the GPU worker thread, once per draw, thousands of times a frame.
// Everything here is an atomic increment or a field read; anything heavier
// belongs behind a queue or on another thread.
void OnDraw(const GpuDrawRecord* rec) {
  if (!rec || rec->struct_size != sizeof(GpuDrawRecord)) {
    g_bad_size.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!rec->registers || !rec->register_count) g_no_regs.fetch_add(1, std::memory_order_relaxed);
  if (!rec->vs_address && !rec->vs_immediate) g_no_shader.fetch_add(1, std::memory_order_relaxed);

  // THE MEASUREMENT THIS STAGE EXISTS FOR.
  //
  // Every record should arrive exactly one greater than the last. If it does,
  // no draw was lost between the plugin and here and none was delivered twice -
  // which is the DLL-boundary question, answered continuously and from the
  // first few records, rather than by comparing two cumulative totals that two
  // threads sample at different instants and that only agree at a standstill.
  // (Nor could those totals be had anyway: there is one callback slot, so the
  // plugin's own self-test cannot run while a real consumer is installed.)
  if (rec->draw_serial != g_last_serial + 1) {
    if (rec->draw_serial > g_last_serial) {
      g_gaps.fetch_add(1, std::memory_order_relaxed);
      g_missing.fetch_add(rec->draw_serial - g_last_serial - 1, std::memory_order_relaxed);
      uint64_t expected = 0;
      g_first_gap.compare_exchange_strong(expected, g_last_serial + 1, std::memory_order_relaxed);
    } else {
      g_repeats.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_last_serial = rec->draw_serial;

  const uint32_t di = rec->vgt_draw_initiator;
  g_draws.fetch_add(1, std::memory_order_relaxed);
  g_indices.fetch_add(di >> 16, std::memory_order_relaxed);
  if (((di >> 6) & 0x3) == kSourceDMA) {
    g_indexed.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_auto.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace

void Start() {
#if defined(_WIN32)
  if (!std::getenv("NG2_NATIVE_GPU")) return;

  // The runtime has already loaded the plugin, so this is a handle lookup, not
  // a load.
  HMODULE m = GetModuleHandleA("rexgpu-xenos.dll");
  if (!m) {
    REXLOG_INFO("[ng2-ngpu] rexgpu-xenos.dll is not loaded - native path stays off");
    return;
  }
  g_setter = reinterpret_cast<GpuSetDrawCallbackFn>(GetProcAddress(m, kSetDrawCallbackSymbol));
  if (!g_setter) {
    // An older plugin without the export. Not an error - the game runs exactly
    // as it always has - but said out loud, because a silent no-op here later
    // reads as "the callback fired zero times", which is a different problem.
    REXLOG_INFO("[ng2-ngpu] this plugin has no {} - native path stays off", kSetDrawCallbackSymbol);
    return;
  }
  g_setter(&OnDraw);
  REXLOG_INFO("[ng2-ngpu] draw hand-off installed (stage 1: counting only), record {} bytes",
              sizeof(GpuDrawRecord));

  unsigned every_s = 5;
  if (const char* e = std::getenv("NG2_NATIVE_GPU_EVERY")) {
    const int v = std::atoi(e);
    if (v > 0) every_s = static_cast<unsigned>(v);
  }
  g_reporting.store(true);
  std::thread(Reporter, every_s).detach();
  REXLOG_INFO("[ng2-ngpu] reporting totals every {}s", every_s);
#endif
}

void Stop() {
#if defined(_WIN32)
  if (!g_setter) return;
  g_reporting.store(false);
  // The plugin holds a pointer into this module; clear it before anything here
  // goes away.
  g_setter(nullptr);
  g_setter = nullptr;
  REXLOG_INFO("[ng2-ngpu] totals: {}", Totals());
#endif
}

}  // namespace ng2::ngpu
