#include "ng2_native_gpu.h"

#include "ng2_plume_renderer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

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

// The frame boundary, on the GPU worker thread - the same thread the draws
// arrive on. Declared locally for the same reason the record is: a consumer in
// another codebase would have to.
using GpuSwapFn = void (*)(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height);
using GpuSetSwapCallbackFn = void (*)(GpuSwapFn);
constexpr const char* kSetSwapCallbackSymbol = "rex_gpu_set_swap_callback";

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

// Stage 2b step 1: index-buffer resolution tallies. Declared here because
// Totals() reports them.
std::atomic<uint64_t> g_idx_virtual_ok{0};
std::atomic<uint64_t> g_idx_physical_ok{0};
std::atomic<uint64_t> g_idx_both{0};
std::atomic<uint64_t> g_idx_neither{0};
std::atomic<uint64_t> g_idx_checked{0};

// Read once at Start(), not per draw: this is on the per-draw path.
bool g_check_inputs = false;

GpuSetDrawCallbackFn g_setter = nullptr;
GpuSetSwapCallbackFn g_swap_setter = nullptr;
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
  std::string idx;
  if (const uint64_t n = g_idx_checked.load()) {
    // Named as a verdict, not as four counters: which address space the index
    // base lives in is the question, so the line answers it.
    idx = fmt::format(" | INDEX BASE of {} draws: {} virtual-only, {} physical-only, {} both, {} neither",
                      n, g_idx_virtual_ok.load(), g_idx_physical_ok.load(),
                      g_idx_both.load(), g_idx_neither.load());
  }
  return fmt::format("{} draws ({} indexed, {} auto), {} indices{}{}{}{}{}",
                     g_draws.load(), g_indexed.load(), g_auto.load(), g_indices.load(), serial, idx,
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
// STAGE 2b, step 1: can a draw's index buffer be RESOLVED from guest memory?
//
// Before anything is rendered, the inputs have to be reachable. The record
// carries index_base as the packet named it, and which address space that is
// cannot be assumed: object-held addresses on the Fable side are CPU virtual,
// while fetch constants are GPU physical, and index_base arrives from neither -
// it comes straight off the PM4 stream. So both interpretations are tried and
// the data decides, rather than a rule borrowed from the other title.
//
// Plausibility, deliberately weak: NG2's indexed draws are all 16-bit TRI_STRIP
// (from the census), so a real index buffer is a run of 16-bit values that are
// not all zero, not all 0xFFFF, and whose maximum is within a sane vertex count
// for one draw. A wrong translation lands in unrelated memory and fails at
// least one of those. This cannot prove a translation right - only a rendered
// frame does that - but it can prove one WRONG, cheaply, before any renderer
// depends on it.

bool PlausibleIndexRun(const uint8_t* p, uint32_t count_words, bool big_endian) {
  if (!p) return false;
  const uint32_t n = count_words < 32u ? count_words : 32u;
  if (n < 3) return false;
  uint32_t max_index = 0;
  bool all_zero = true, all_ff = true;
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t v = big_endian ? uint16_t((p[i * 2] << 8) | p[i * 2 + 1])
                                  : uint16_t((p[i * 2 + 1] << 8) | p[i * 2]);
    if (v != 0) all_zero = false;
    if (v != 0xFFFF) all_ff = false;
    if (v > max_index) max_index = v;
  }
  // 65535 is the 16-bit reset index; a buffer that is entirely resets is not
  // geometry. An upper bound of 32k vertices in one draw is generous for NG2,
  // whose largest observed draw is well under that.
  return !all_zero && !all_ff && max_index < 32768u;
}

void CheckIndexBuffer(const GpuDrawRecord* rec) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || !rec->index_base || !rec->index_size_words) return;
  g_idx_checked.fetch_add(1, std::memory_order_relaxed);

  // index_endian 2 is the Xenos 8-in-16 swap, i.e. big-endian halfwords.
  const bool big_endian = rec->index_endian != 0;
  const bool v = PlausibleIndexRun(memory->TranslateVirtual<const uint8_t*>(rec->index_base),
                                   rec->index_size_words, big_endian);
  const bool p = PlausibleIndexRun(memory->TranslatePhysical<const uint8_t*>(rec->index_base),
                                   rec->index_size_words, big_endian);
  if (v && p) g_idx_both.fetch_add(1, std::memory_order_relaxed);
  else if (v) g_idx_virtual_ok.fetch_add(1, std::memory_order_relaxed);
  else if (p) g_idx_physical_ok.fetch_add(1, std::memory_order_relaxed);
  else g_idx_neither.fetch_add(1, std::memory_order_relaxed);
}

// Fired once per guest frame at the swap packet. Closes the coverage oracle's
// frame: draws handed over against draws the renderer actually issued.
void OnSwap(uint32_t, uint32_t, uint32_t) { render::EndFrame(); }

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

  // The coverage oracle's denominator. Counted HERE, where a draw actually
  // arrives, rather than from the renderer's own view - a renderer cannot
  // report what it never saw, which is the whole point of the comparison.
  render::NoteDrawHandedOff();

  const uint32_t di = rec->vgt_draw_initiator;
  g_draws.fetch_add(1, std::memory_order_relaxed);
  g_indices.fetch_add(di >> 16, std::memory_order_relaxed);
  if (((di >> 6) & 0x3) == kSourceDMA) {
    g_indexed.fetch_add(1, std::memory_order_relaxed);
    // Only indexed draws have an index buffer to resolve; the auto-index ones
    // (two thirds of NG2's draws) carry no address at all.
    if (g_check_inputs) CheckIndexBuffer(rec);
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

  // The frame boundary. Optional: an older plugin without it leaves the oracle
  // reporting per-run totals instead of per-frame, which is a worse instrument
  // but not a broken game.
  g_swap_setter = reinterpret_cast<GpuSetSwapCallbackFn>(GetProcAddress(m, kSetSwapCallbackSymbol));
  if (g_swap_setter) {
    g_swap_setter(&OnSwap);
  } else {
    REXLOG_INFO("[ng2-ngpu] this plugin has no {} - no frame boundary, per-run totals only",
                kSetSwapCallbackSymbol);
  }
  REXLOG_INFO("[ng2-ngpu] draw hand-off installed (stage 1: counting only), record {} bytes",
              sizeof(GpuDrawRecord));

  g_check_inputs = std::getenv("NG2_NATIVE_GPU_INPUTS") != nullptr;
  if (g_check_inputs)
    REXLOG_INFO("[ng2-ngpu] resolving index buffers from guest memory (stage 2b step 1)");

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
  if (g_swap_setter) {
    g_swap_setter(nullptr);
    g_swap_setter = nullptr;
  }
  g_setter(nullptr);
  g_setter = nullptr;
  REXLOG_INFO("[ng2-ngpu] totals: {}", Totals());
#endif
}

}  // namespace ng2::ngpu
