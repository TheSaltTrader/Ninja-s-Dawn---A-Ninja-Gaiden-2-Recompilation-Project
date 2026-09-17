#include "ng2_native_gpu.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>

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

GpuSetDrawCallbackFn g_setter = nullptr;

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
#endif
}

void Stop() {
#if defined(_WIN32)
  if (!g_setter) return;
  // The plugin holds a pointer into this module; clear it before anything here
  // goes away.
  g_setter(nullptr);
  g_setter = nullptr;
  REXLOG_INFO("[ng2-ngpu] totals: {} draws ({} indexed, {} auto), {} indices{}{}{}",
              g_draws.load(), g_indexed.load(), g_auto.load(), g_indices.load(),
              g_bad_size.load() ? fmt::format(" | {} ABI MISMATCH", g_bad_size.load()) : "",
              g_no_regs.load() ? fmt::format(" | {} without registers", g_no_regs.load()) : "",
              g_no_shader.load() ? fmt::format(" | {} without a shader", g_no_shader.load()) : "");
#endif
}

}  // namespace ng2::ngpu
