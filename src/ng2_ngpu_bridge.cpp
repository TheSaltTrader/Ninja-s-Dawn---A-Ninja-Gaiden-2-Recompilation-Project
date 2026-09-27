// NG2 native-GPU bridge: the plugin's callbacks -> the transplanted backend, in lockstep. See ng2_ngpu_bridge.h.
//
// Two backends, one glue. The DLL route (ngpu_backend.dll, Fable II's game-agnostic build of the same backend plus
// its lockstep glue; NATIVE_GPU_MIGRATION_KIT/bin/ngpu_backend_dll) is the delivery the user asked for and is the
// default when the DLL sits beside the exe. The in-exe route (src/native_gpu_xlat + ng2_native_backend.cpp) is the
// same code compiled into ng2.exe, kept as the fallback and for diffing. Both are fed from the same plugin callbacks
// and present through the same native window.
//
// Adapted from Fable II's app glue (NATIVE_GPU_MIGRATION_KIT/app_integration/lockstep_bridge_reference.cpp, the
// 2026-09-26 state that measured 0.0027-0.0030 against the plugin and 59.6-60.0 fps in Bowerstone under offload).
// What is NG2's own here: the loading -> world signal for the reveal hold (the game's mode word), the present
// request into NG2's raw-D3D12 window, the ultrawide two-homes handling, and the counters.
#include "ng2_ngpu_bridge.h"

#include <windows.h>

#include <emmintrin.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>

#include "ng2_native_backend.h"
#include "ngpu_backend_api.h"
#include "rtc_d3d12/facade.h"

// The in-exe backend's own copies of the pack counters (accessor-only statics
// at global scope in src/native_gpu_xlat/rtc_d3d12/texture_cache.cpp).
int32_t& FLAGS_texture_pack_replaced_storage_();
int32_t& FLAGS_texture_pack_original_storage_();

// The vendored backend's own copies (src/native_gpu_xlat/rtc_d3d12/flags.cpp; declared in the vendored
// include/rex/graphics/flags.h). Declared here by their storage names so this file does not depend on which
// flags.h the include order picks.
double& FLAGS_ng2_fov_k_storage_();
int32_t& FLAGS_ng2_uw_mode_storage_();

// ---------------------------------------------------------------------------------------------------------------------
// cvars. The exe owns these names; the vendored backend reads the ones it needs through the accessors at the bottom,
// the DLL receives them as NgpuBackendOptions.
// ---------------------------------------------------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(ngpu_backend, false, "GPU",
                    "Native-GPU BACKEND TRANSPLANT (read at startup): the plugin's own D3D12 backend renders every "
                    "draw on the native window's device; test lever NG2_NATIVE_GPU=1");
REXCVAR_DEFINE_BOOL(ngpu_backend_dll, true, "GPU",
                    "Use ngpu_backend.dll beside the exe (the game-agnostic build) when it loads and answers ABI 1; "
                    "otherwise the copy compiled into ng2.exe. NG2_TUNE=ngpu_backend_dll=false forces the in-exe copy");
REXCVAR_DEFINE_BOOL(ngpu_backend_lockstep, true, "GPU",
                    "Feed the transplanted backend INSIDE the plugin's draw / swap callbacks (its GPU thread, the "
                    "same instant) - guest memory is exactly what the plugin reads");
REXCVAR_DEFINE_BOOL(ngpu_backend_async_submit, true, "GPU",
                    "The backend's submissions (deferred command list replay + ExecuteCommandLists) run on their "
                    "own thread instead of the plugin's GPU thread (Fable II: 48.5 -> 54.4 fps)");
REXCVAR_DEFINE_BOOL(ngpu_gpu_prof, false, "GPU",
                    "Time the backend's GPU work by category with timestamp queries, reported every 5 s (inflates "
                    "GPU time ~50%: shares only)");
REXCVAR_DEFINE_BOOL(ngpu_backend_hoist_uploads, true, "GPU",
                    "Uploads into guest pages no GPU work of the open submission has touched go in a prologue "
                    "under one transition (Fable II: GPU time 16.4 -> 11.7 ms)");
REXCVAR_DEFINE_BOOL(ngpu_backend_fast_valid, true, "GPU",
                    "Shared-memory 'all pages valid' and texture 'no scaled resolve' answered from bitmaps without "
                    "the global critical region");
REXCVAR_DEFINE_BOOL(ngpu_backend_upload_skip, true, "GPU",
                    "Skip re-uploading 4 KB guest pages whose XXH3 equals the last upload's (Fable II: 86% of the "
                    "town's upload volume; NG2 under clear_memory_page_state is expected to gain more)");
REXCVAR_DEFINE_BOOL(ngpu_reveal_hold, true, "GPU",
                    "After a load (chapter card -> gameplay) keep showing the last frame until the stage is "
                    "complete: no draw skipped for a compiling pipeline, none compiling, a steady draw count");
REXCVAR_DEFINE_BOOL(ngpu_one_window, true, "GPU",
                    "Present the native backend's frame in the game's own window through the runtime presenter (needs "
                    "the plugin's RexNgpuSetOutputProvider and gpu_offload_to_native); off = the separate native window");
REXCVAR_DEFINE_INT32(ngpu_present_wait_ms, 4, "GPU",
                     "One window: how long a swap waits for the backend's submit thread before it presents whatever "
                     "frame is on the queue (ms)");
REXCVAR_DEFINE_INT32(ngpu_reveal_hold_frames, 6, "GPU", "Consecutive complete swaps that end the reveal hold");
REXCVAR_DEFINE_INT32(ngpu_reveal_hold_max_ms, 2500, "GPU", "The reveal hold never lasts longer than this (ms)");
REXCVAR_DEFINE_INT32(ngpu_backend_selfcheck_every, 1024, "GPU",
                     "Every Nth draw the dirty-bitmap register sync is checked against a full scalar diff "
                     "(1 = every draw: a correctness leg, timing-invalid; 0 = off)");

// The accessors the vendored facade / command processor declare (rtc_d3d12/facade.cpp, command_processor.cpp).
namespace ng2::ngpu::rtc {
bool NgpuBackendAsyncSubmitCvar() { return REXCVAR_GET(ngpu_backend_async_submit); }
bool NgpuBackendFastValidCvar() { return REXCVAR_GET(ngpu_backend_fast_valid); }
bool NgpuHoistUploadsCvar() { return REXCVAR_GET(ngpu_backend_hoist_uploads); }
bool NgpuGpuProfCvar() { return REXCVAR_GET(ngpu_gpu_prof); }
bool NgpuBackendUploadSkipCvar() { return REXCVAR_GET(ngpu_backend_upload_skip); }
}  // namespace ng2::ngpu::rtc

namespace ng2::ngpu {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// The plugin's hand-off ABI (rexglue-src src/graphics/command_processor.cpp, `extern "C"` block), declared HERE as a
// consumer in another module would: the `size` field is the version check, and it is only a check if the two sides
// do not share a header. Must match the plugin's definition field for field.
// ---------------------------------------------------------------------------------------------------------------------
extern "C" {
struct RexNgpuDraw {
  uint32_t size;               // sizeof on the plugin side, so the two sides can differ in version safely
  const uint32_t* regs;        // the register file, indexed by register number
  uint32_t reg_count;
  uint32_t draw_initiator;     // VGT_DRAW_INITIATOR
  uint32_t index_addr;         // kDMA: the index buffer, GPU physical
  uint32_t index_size;         // as the packet gave it
  uint32_t inline_addr;        // kImmediate: guest address of the inline index data
  uint32_t vs_addr, vs_dwords, ps_addr, ps_dwords;
  uint32_t vs_inline, ps_inline;    // microcode came in the packet, not at an address
  uint32_t predicated;
  uint64_t bin_mask, bin_select;    // the tiling predicate in force at this draw
  // (size >= offsetof end) the microcode of an INLINE shader, big-endian dwords as the guest wrote them, valid for
  // the duration of the callback; null when the shader was loaded from an address.
  const uint8_t* vs_code;
  const uint8_t* ps_code;
  uint32_t vs_code_dwords, ps_code_dwords;
};
}
using DrawFn = void (*)(const RexNgpuDraw*);
using SetDrawFn = void (*)(DrawFn);
using SwapFn = void (*)(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height);
using SetSwapFn = void (*)(SwapFn);
using DirtyFn = uint64_t* (*)(uint32_t* word_count);
using GammaFn = bool (*)(uint32_t* table_256, uint32_t* pwl_rgb);
using RevealFn = void (*)();
using OutputFn = int (*)(ID3D12Resource**, uint32_t*, uint32_t*, int*);   // ONE WINDOW (fork f6fc6d4c)
using SetOutputFn = void (*)(OutputFn);
using GetDeviceFn = int (*)(ID3D12Device**, ID3D12CommandQueue**);
using PresentStatsFn = void (*)(uint64_t*);

constexpr uint32_t kRegisterFileCount = 0x5000;   // >= the plugin's RegisterFile::kRegisterCount (0x4928 forwarded)
constexpr uint32_t kForwardedEnd = 0x4928;
// The registers forwarded to the backend (Fable II's town changed only CP/sync/display registers outside these; the
// coverage report below says whether NG2 differs - CHECK IT on this title).
constexpr uint32_t kRanges[][2] = {{0x2000, 0x2400}, {0x4000, 0x4928}};

SetDrawFn g_set_draw = nullptr;
SetSwapFn g_set_swap = nullptr;
GammaFn g_get_gamma = nullptr;
RevealFn g_reveal_plugin = nullptr;
SetOutputFn g_set_output = nullptr;
GetDeviceFn g_get_device = nullptr;
PresentStatsFn g_present_stats = nullptr;
bool g_one_window = false;       // ONE WINDOW: the backend on the plugin's device, its frames through the presenter
bool g_hold_this_swap = false;   // the reveal hold's verdict at the last swap; the provider reads it
uint64_t g_provider_calls = 0, g_provider_held = 0, g_provider_waits_timed_out = 0, g_provider_no_output = 0;
uint64_t* g_dirty = nullptr;
uint32_t g_dirty_words = 0;

// ---------------------------------------------------------------------------------------------------------------------
// ngpu_backend.dll, bound by name so a missing or older DLL falls back to the in-exe copy instead of failing to load
// the exe (the .lib is deliberately not linked).
// ---------------------------------------------------------------------------------------------------------------------
struct BackendDll {
  HMODULE module = nullptr;
  uint32_t (*AbiVersion)() = nullptr;
  void (*DefaultOptions)(NgpuBackendOptions*) = nullptr;
  int (*Start)(const NgpuBackendOptions*, ID3D12Device*, ID3D12CommandQueue*) = nullptr;
  void (*OnDraw)(const void*) = nullptr;
  void (*OnSwap)(uint32_t, uint32_t, uint32_t) = nullptr;
  void (*RevealAfterLoad)() = nullptr;
  int (*ShouldPresent)() = nullptr;
  int (*WaitSwapSubmitted)(uint32_t) = nullptr;
  ID3D12Resource* (*GuestOutput)(uint32_t*, uint32_t*) = nullptr;
  void (*Stats)(NgpuBackendStatsT*) = nullptr;
  void (*PresentMode)(int32_t*, double*) = nullptr;
  int (*GetSetting)(const char*, char*, uint32_t) = nullptr;
  int (*SetSetting)(const char*, const char*) = nullptr;
};
BackendDll g_dll;
bool g_use_dll = false;          // the DLL route is live
bool g_backend_on = false;       // some backend initialised on the native window's device
bool g_lockstep = false;
const uint32_t* g_live_regs = nullptr;
uint32_t g_live_reg_count = 0;
double g_last_fov_k = 0.0;       // pushed into the DLL at Start if ApplyFov ran first

// Register sync state (GPU thread only) - the in-exe route; the DLL does its own.
uint32_t g_prev[kRegisterFileCount];
bool g_prev_valid = false;
bool g_dirty_ok = true;
uint64_t g_regs_written = 0;
uint64_t g_dirty_outside[kRegisterFileCount / 64 + 1];

// Counters.
std::atomic<uint64_t> g_draws_seen{0}, g_draws_bad_size{0}, g_draws_lockstep{0};
uint64_t g_fail_nocode = 0, g_fail_withcode = 0;
uint64_t g_nocode[2][4] = {};   // [stage][inline-len-mismatch, inline-null, unreadable, no-addr]
uint64_t g_check_n = 0, g_check_bad = 0, g_check_runs = 0;

template <typename T>
T Bind(HMODULE m, const char* name) {
  return reinterpret_cast<T>(GetProcAddress(m, name));
}

std::string ExeFolder() {
  char path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string s(path, n);
  const size_t slash = s.find_last_of("\\/");
  return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

bool LoadBackendDll() {
  const std::string path = ExeFolder() + "\\ngpu_backend.dll";
  HMODULE m = LoadLibraryA(path.c_str());
  if (!m) {
    REXLOG_INFO("[ngpu] no ngpu_backend.dll beside the exe ({}; error {}) - the in-exe copy of the backend runs", path,
                GetLastError());
    return false;
  }
  g_dll.module = m;
  g_dll.AbiVersion = Bind<decltype(g_dll.AbiVersion)>(m, "NgpuBackendAbiVersion");
  g_dll.DefaultOptions = Bind<decltype(g_dll.DefaultOptions)>(m, "NgpuBackendDefaultOptions");
  g_dll.Start = Bind<decltype(g_dll.Start)>(m, "NgpuBackendStart");
  g_dll.OnDraw = Bind<decltype(g_dll.OnDraw)>(m, "NgpuBackendOnDraw");
  g_dll.OnSwap = Bind<decltype(g_dll.OnSwap)>(m, "NgpuBackendOnSwap");
  g_dll.RevealAfterLoad = Bind<decltype(g_dll.RevealAfterLoad)>(m, "NgpuBackendRevealAfterLoad");
  g_dll.ShouldPresent = Bind<decltype(g_dll.ShouldPresent)>(m, "NgpuBackendShouldPresent");
  g_dll.WaitSwapSubmitted = Bind<decltype(g_dll.WaitSwapSubmitted)>(m, "NgpuBackendWaitSwapSubmitted");
  g_dll.GuestOutput = Bind<decltype(g_dll.GuestOutput)>(m, "NgpuBackendGuestOutput");
  g_dll.Stats = Bind<decltype(g_dll.Stats)>(m, "NgpuBackendStats");
  g_dll.PresentMode = Bind<decltype(g_dll.PresentMode)>(m, "NgpuBackendPresentMode");
  g_dll.GetSetting = Bind<decltype(g_dll.GetSetting)>(m, "NgpuBackendGetSetting");
  g_dll.SetSetting = Bind<decltype(g_dll.SetSetting)>(m, "NgpuBackendSetSetting");
  const bool complete = g_dll.AbiVersion && g_dll.DefaultOptions && g_dll.Start && g_dll.OnDraw && g_dll.OnSwap &&
                        g_dll.RevealAfterLoad && g_dll.ShouldPresent && g_dll.WaitSwapSubmitted &&
                        g_dll.GuestOutput && g_dll.Stats && g_dll.PresentMode && g_dll.GetSetting &&
                        g_dll.SetSetting;
  const uint32_t abi = g_dll.AbiVersion ? g_dll.AbiVersion() : 0;
  if (!complete || abi != NGPU_BACKEND_ABI) {
    REXLOG_INFO("[ngpu] ngpu_backend.dll at {} is {} (ABI {} vs {} expected) - the in-exe copy of the backend runs",
                path, complete ? "complete" : "MISSING exports", abi, NGPU_BACKEND_ABI);
    FreeLibrary(m);
    g_dll = BackendDll();
    return false;
  }
  REXLOG_INFO("[ngpu] ngpu_backend.dll loaded from {} (ABI {})", path, abi);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Guest memory helpers.
// ---------------------------------------------------------------------------------------------------------------------
bool PageReadable(const void* p) {
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  return (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

const uint8_t* Phys(uint32_t physical_address) {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) return nullptr;
  return reinterpret_cast<const uint8_t*>(ks->memory()->TranslatePhysical(physical_address));
}

bool ReadGuestU32(uint32_t va, uint32_t& out) {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) return false;
  const uint8_t* p = ks->memory()->TranslateVirtual<const uint8_t*>(va);
  if (!PageReadable(p)) return false;
  out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// IN-EXE ROUTE: the register sync - the plugin's dirty bitmap first, the SSE2 blocked full scan as the fallback, and
// the SELF-CHECK that keeps the bitmap honest (Fable II: 30.9 M + 21.9 M + 23.1 M draws, 0 mismatches).
// ---------------------------------------------------------------------------------------------------------------------
void LockstepSync(const uint32_t* regs) {
  static const uint32_t check_every = uint32_t(std::max(0, REXCVAR_GET(ngpu_backend_selfcheck_every)));
  const bool check = g_prev_valid && check_every && ((++g_check_n % check_every) == 0);
  static const DWORD sync_thread = GetCurrentThreadId();
  static uint64_t other_thread_syncs = 0;
  if (GetCurrentThreadId() != sync_thread && (++other_thread_syncs <= 3))
    REXLOG_INFO("[ngpu] LOCKSTEP: register sync on thread {} (first was {}) - the dirty bitmap assumes one thread",
                GetCurrentThreadId(), sync_thread);
  uint64_t expect = 0;
  const uint64_t written_before = g_regs_written;
  if (check)
    for (const auto& r : kRanges)
      for (uint32_t k = r[0]; k < r[1]; ++k) expect += g_prev[k] != regs[k];
  if (g_dirty && g_dirty_ok && g_prev_valid) {
    for (const auto& r : kRanges) {
      for (uint32_t w = r[0] >> 6; w <= (r[1] - 1) >> 6; ++w) {
        uint64_t bits = g_dirty[w];
        if (!bits) continue;
        g_dirty[w] = 0;
        if ((w << 6) + 64 > r[1]) g_dirty_outside[w] |= bits & ~((uint64_t(1) << (r[1] & 63)) - 1);
        while (bits) {
          unsigned long b;
          _BitScanForward64(&b, bits);
          const uint32_t k = (w << 6) + uint32_t(b);
          bits &= bits - 1;
          if (k < r[0] || k >= r[1] || g_prev[k] == regs[k]) continue;
          g_prev[k] = regs[k];
          backend::WriteRegister(k, regs[k]);
          ++g_regs_written;
        }
      }
    }
  } else {
    if (g_dirty) std::memset(g_dirty, 0, g_dirty_words * sizeof(uint64_t));
    for (const auto& r : kRanges) {
      for (uint32_t i = r[0]; i < r[1]; i += 16) {
        if (g_prev_valid && i + 16 <= r[1]) {
          const __m128i* a = reinterpret_cast<const __m128i*>(&regs[i]);
          const __m128i* b = reinterpret_cast<const __m128i*>(&g_prev[i]);
          const __m128i eq = _mm_and_si128(
              _mm_and_si128(_mm_cmpeq_epi32(_mm_loadu_si128(a), _mm_loadu_si128(b)),
                            _mm_cmpeq_epi32(_mm_loadu_si128(a + 1), _mm_loadu_si128(b + 1))),
              _mm_and_si128(_mm_cmpeq_epi32(_mm_loadu_si128(a + 2), _mm_loadu_si128(b + 2)),
                            _mm_cmpeq_epi32(_mm_loadu_si128(a + 3), _mm_loadu_si128(b + 3))));
          if (_mm_movemask_epi8(eq) == 0xFFFF) continue;
        }
        for (uint32_t k = i; k < i + 16 && k < r[1]; ++k) {
          if (g_prev_valid && g_prev[k] == regs[k]) continue;
          g_prev[k] = regs[k];
          backend::WriteRegister(k, regs[k]);
          ++g_regs_written;
        }
      }
    }
  }
  g_prev_valid = true;
  if (check) {
    ++g_check_runs;
    const uint64_t written = g_regs_written - written_before;
    if (written != expect) {
      ++g_check_bad;
      if (g_dirty_ok && g_dirty) {
        g_dirty_ok = false;
        REXLOG_INFO("[ngpu] LOCKSTEP: the dirty bitmap MISSED a register write ({} written, {} differ) - back to the "
                    "full scan for this run", written, expect);
      }
    }
    if ((g_check_runs % 4096) == 1 || (written != expect && g_check_bad <= 5))
      REXLOG_INFO("[ngpu] LOCKSTEP diff self-check: {} checks, {} mismatches (last: {} written, {} expected)",
                  g_check_runs, g_check_bad, written, expect);
  }
}

// Registers whose VALUE CHANGED outside the forwarded ranges, accumulated over the run and printed every 600 swaps.
// A title may need a range Fable II did not; this is how NG2 finds out (in-exe route; the DLL reports its own).
void DirtyCoverageAtSwap() {
  if (!g_dirty) return;
  const uint32_t n = std::min<uint32_t>(g_dirty_words, uint32_t(std::size(g_dirty_outside)));
  for (uint32_t w = 0; w < n; ++w) {
    const bool inside = (w >= (0x2000 >> 6) && w < (0x2400 >> 6)) || (w >= (0x4000 >> 6) && w < (kForwardedEnd >> 6));
    if (inside || !g_dirty[w]) continue;
    const uint64_t outside_mask =
        (w == (kForwardedEnd >> 6)) ? ~((uint64_t(1) << (kForwardedEnd & 63)) - 1) : ~uint64_t(0);
    g_dirty_outside[w] |= g_dirty[w] & outside_mask;
    g_dirty[w] &= ~outside_mask;   // the in-range bits stay for the next draw's sync
  }
  static uint32_t swaps = 0;
  if ((++swaps % 600) != 1) return;
  uint32_t total = 0;
  std::string list;
  for (uint32_t w = 0; w < n; ++w)
    for (uint64_t b = g_dirty_outside[w]; b; b &= b - 1) {
      unsigned long i;
      _BitScanForward64(&i, b);
      if (++total <= 80) list += fmt::format(" {:04X}", (w << 6) + uint32_t(i));
    }
  REXLOG_INFO("[ngpu] LOCKSTEP REGISTER COVERAGE: {} distinct registers whose value changed outside the forwarded "
              "ranges so far this run:{}{}", total, list, total > 80 ? " ..." : "");
}

// The in-exe backend, brought up on the native window's device the first time a draw arrives.
bool LockstepReady() {
  if (g_backend_on) return true;
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  if (g_one_window) {
    // The plugin's device and direct queue, valid once its SetupContext has run (before its first draw callback).
    if (!g_get_device || !g_get_device(&device, &queue) || !device || !queue) return false;
  } else {
    if (!render::Ready()) return false;
    device = render::Device();
    queue = render::Queue();
  }
  static bool tried = false;
  if (tried) return false;
  tried = true;
  g_backend_on = backend::Init(device, queue);
  g_lockstep = g_backend_on;
  REXLOG_INFO("[ngpu] LOCKSTEP (in-exe): {}", g_backend_on
                                                   ? "the plugin's draw callback feeds the transplanted backend directly"
                                                   : "backend initialisation FAILED - the native path stays off");
  return g_backend_on;
}

void LockstepDraw(const RexNgpuDraw* d) {
  if (!LockstepReady()) return;
  LockstepSync(d->regs);
  auto code = [](int stage, uint32_t addr, uint32_t& dwords, bool inl, const uint8_t* inline_code,
                 uint32_t inline_dwords) -> const uint32_t* {
    if (inl) {
      if (!inline_code) { ++g_nocode[stage][1]; return nullptr; }
      // The packet's own copy is authoritative; its length is the one to hand the pipeline cache (Fable II trap:
      // the register's count instead failed 11% of draws).
      if (inline_dwords != dwords) ++g_nocode[stage][0];
      dwords = inline_dwords;
      return reinterpret_cast<const uint32_t*>(inline_code);
    }
    if (!addr || !dwords) { ++g_nocode[stage][3]; return nullptr; }
    const uint8_t* p = Phys(addr);
    if (!(p && PageReadable(p) && PageReadable(p + dwords * 4 - 1))) { ++g_nocode[stage][2]; return nullptr; }
    return reinterpret_cast<const uint32_t*>(p);
  };
  const bool has_inline = d->size >= sizeof(RexNgpuDraw);
  backend::DrawRecord br;
  br.draw_initiator = d->draw_initiator;
  br.index_addr = d->index_addr;
  br.index_size = d->index_size;
  br.vs_addr = d->vs_addr; br.vs_dwords = d->vs_dwords;
  br.ps_addr = d->ps_addr; br.ps_dwords = d->ps_dwords;
  br.vs_code = code(0, d->vs_addr, br.vs_dwords, d->vs_inline != 0, has_inline ? d->vs_code : nullptr,
                    has_inline ? d->vs_code_dwords : 0);
  br.ps_code = code(1, d->ps_addr, br.ps_dwords, d->ps_inline != 0, has_inline ? d->ps_code : nullptr,
                    has_inline ? d->ps_code_dwords : 0);
  if (!backend::Draw(br)) ++(br.vs_code ? g_fail_withcode : g_fail_nocode);
  g_draws_lockstep.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------------
// REVEAL HOLD, NG2's trigger. The game's mode word at guest 0x84C25070 (mapped for the ultrawide work, v1.0.19):
// 0 = front-end (title / difficulty / load), 2 = the chapter card, 3 = gameplay, 128+ = in-engine cinematic.
// A transition INTO gameplay from the front-end or the chapter card is a load finishing - the moment the stage's
// pipelines are still compiling and the picture is partial. The DLL holds on its own once told; the in-exe route
// counts held frames on the backend's readiness below.
// ---------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kModeWordAddress = 0x84C25070;
constexpr uint32_t kModeGameplay = 3;

uint32_t g_world_entries = 0;
uint32_t g_last_mode = 0xFFFFFFFF;

void NoteModeWord() {
  uint32_t mode = 0;
  if (!ReadGuestU32(kModeWordAddress, mode)) return;
  if (g_last_mode != 0xFFFFFFFF && mode == kModeGameplay && g_last_mode != kModeGameplay && g_last_mode < 128) {
    ++g_world_entries;
    if (g_reveal_plugin) g_reveal_plugin();   // the plugin path's own hold (its presenter), when it presents
    if (g_use_dll && g_dll.RevealAfterLoad) g_dll.RevealAfterLoad();
  }
  g_last_mode = mode;
}

bool RevealHold() {
  static uint32_t seen_entries = 0;
  static bool holding = false;
  static int64_t hold_start_ms = 0;
  static uint64_t last_not_ready = 0, last_draws = 0, prev_frame_draws = 0;
  static int complete_run = 0, held_frames = 0;
  const auto rd = backend::GetReadiness();
  const uint64_t draws = backend::GetStats().draws;
  const uint64_t frame_draws = draws - last_draws;
  const uint64_t skipped = rd.pipeline_not_ready_draws - last_not_ready;
  last_draws = draws;
  last_not_ready = rd.pipeline_not_ready_draws;
  const int64_t now_ms = int64_t(GetTickCount64());
  if (g_world_entries != seen_entries) {
    seen_entries = g_world_entries;
    if (REXCVAR_GET(ngpu_reveal_hold)) {
      holding = true;
      hold_start_ms = now_ms;
      complete_run = 0;
      held_frames = 0;
      REXLOG_INFO("[ngpu] REVEAL: load finished (mode word -> gameplay) - holding the last frame until the stage is complete");
    }
  }
  const uint64_t prev = prev_frame_draws;
  prev_frame_draws = frame_draws;
  if (!holding) return false;
  const bool steady = prev && frame_draws &&
                      (frame_draws > prev ? frame_draws - prev : prev - frame_draws) * 100 <= prev * 3;
  const bool complete = skipped == 0 && !rd.creating_pipelines && steady;
  complete_run = complete ? complete_run + 1 : 0;
  ++held_frames;
  const int64_t held_ms = now_ms - hold_start_ms;
  const bool done = complete_run >= std::max(1, REXCVAR_GET(ngpu_reveal_hold_frames));
  const bool cap = held_ms >= REXCVAR_GET(ngpu_reveal_hold_max_ms);
  if (done || cap) {
    holding = false;
    REXLOG_INFO("[ngpu] REVEAL: showing the stage after {} held swaps / {} ms ({})", held_frames, held_ms,
                done ? "complete" : "the cap - the stage was still changing");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The callbacks, on the plugin's GPU thread.
// ---------------------------------------------------------------------------------------------------------------------
void OnDraw(const RexNgpuDraw* d) {
  // Older plugins end at bin_select (no inline-microcode fields).
  if (!d || (d->size != sizeof(RexNgpuDraw) && d->size != offsetof(RexNgpuDraw, vs_code))) {
    g_draws_bad_size.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_draws_seen.fetch_add(1, std::memory_order_relaxed);
  if (g_use_dll) {
    g_dll.OnDraw(d);   // the DLL does its own register sync, self-check and coverage
    return;
  }
  if (REXCVAR_GET(ngpu_backend_lockstep) && d->regs && d->reg_count >= kForwardedEnd) {
    // The live-regs pointer FIRST: the swap callback reads fetch constant 0 through it. Forgetting it in Fable II
    // meant no swaps, and the bindless descriptors were exhausted within seconds.
    g_live_regs = d->regs;
    g_live_reg_count = d->reg_count;
    LockstepDraw(d);
  }
}

// The texture-pack counters have TWO HOMES too (see the header on ng2_uw_mode):
// the backend's accessor copy, which its texture cache writes, and the runtime
// registry, which the F10 menu and the notify overlay read (REXCVAR_QUERY -
// the plugin owns those cvars, and under offload the plugin's own texture cache
// loads nothing, so the registry copy stays 0 unless the backend publishes).
// The DLL publishes its copy into the registry at every swap (Fable 012fcf3).
// Printing both side by side is the check that the publish lands; a registry
// 0 beside a backend N is the overlay lying, not the pack failing (2026-09-26
// evening: an evening was spent bisecting "0 enhanced" against the backend's
// optimisations before the instrument was checked).
int32_t BackendPackCounter(const char* name) {
  if (g_use_dll) {
    char buf[32] = {};
    if (g_dll.GetSetting && g_dll.GetSetting(name, buf, sizeof(buf))) return std::atoi(buf);
    return -1;
  }
  if (std::strcmp(name, "texture_pack_replaced") == 0) return FLAGS_texture_pack_replaced_storage_();
  if (std::strcmp(name, "texture_pack_original") == 0) return FLAGS_texture_pack_original_storage_();
  return -1;
}

void LogPackHomes() {
  const int32_t reg_r = rex::cvar::Query<int32_t>("texture_pack_replaced");
  const int32_t reg_o = rex::cvar::Query<int32_t>("texture_pack_original");
  const int32_t be_r = BackendPackCounter("texture_pack_replaced");
  const int32_t be_o = BackendPackCounter("texture_pack_original");
  REXLOG_INFO("[ngpu] PACK COUNTERS: registry (what the overlay reads) replaced {} original {} | backend copy "
              "replaced {} original {}{}",
              reg_r, reg_o, be_r, be_o,
              (be_r > 0 && reg_r == 0) ? " - THE REGISTRY IS NOT PUBLISHED: the overlay would read 0 enhanced"
                                       : "");
}

void LogPeriodic() {
  static uint32_t n = 0;
  if ((++n % 300) != 1) return;
  const auto ws = render::GetStats();
  if (g_use_dll) {
    NgpuBackendStatsT st = {};
    g_dll.Stats(&st);
    int32_t mode = 0;
    double k = 0.0;
    g_dll.PresentMode(&mode, &k);
    REXLOG_INFO("[ngpu] LOCKSTEP (dll): {} draws seen, {} fed ({} failed), {} swaps, self-check {} runs / {} "
                "mismatches, {} presented / {} held by the DLL; uw mode {} fov_k {:.4f} | window presented {} of {} "
                "requests ({} without output, {} waits timed out)",
                g_draws_seen.load(), st.draws, st.draw_failed, st.swaps, st.selfcheck_runs, st.selfcheck_mismatches,
                st.frames_presented, st.frames_held, mode, k, ws.presented, ws.requests, ws.skipped_no_output,
                ws.waits_timed_out);
    LogPackHomes();
    return;
  }
  const auto st = backend::GetStats();
  REXLOG_INFO("[ngpu] LOCKSTEP (in-exe): {} draws seen, {} fed ({} failed: {} with no VS microcode, {} with it), {} "
              "swaps, {} shader loads ({} failed), {} register writes, dirty bitmap {}; no microcode VS inline-len {} "
              "inline-null {} unreadable {} no-addr {} | PS {} {} {} {} | window presented {} of {} requests "
              "({} without output, {} waits timed out), uw mode {}",
              g_draws_seen.load(), st.draws, st.draw_failed, g_fail_nocode, g_fail_withcode, st.swaps,
              st.shader_loads, st.shader_load_failed, g_regs_written,
              g_dirty ? (g_dirty_ok ? "in use" : "abandoned after a miss") : "absent",
              g_nocode[0][0], g_nocode[0][1], g_nocode[0][2], g_nocode[0][3],
              g_nocode[1][0], g_nocode[1][1], g_nocode[1][2], g_nocode[1][3],
              ws.presented, ws.requests, ws.skipped_no_output, ws.waits_timed_out, ws.last_mode);
  LogPackHomes();
  if (g_one_window) {
    uint64_t ps[4] = {};
    if (g_present_stats) g_present_stats(ps);
    REXLOG_INFO("[ngpu] ONE WINDOW: provider calls {} (held {}, no output {}, waits past {} ms {}); the plugin "
                "presented {} through the runtime presenter (no output {}, size/format mismatch {}, refresh failed {})",
                g_provider_calls, g_provider_held, g_provider_no_output, REXCVAR_GET(ngpu_present_wait_ms),
                g_provider_waits_timed_out, ps[0], ps[1], ps[2], ps[3]);
  }
}

// ONE WINDOW: the plugin's IssueSwap calls this on the GPU thread right after OnSwap, for the frame to present.
// The backend's swap submission must be on the direct queue before the plugin enqueues its copy: wait for the
// submit thread (bounded); past the bound the copy still lands between two whole submissions, so it shows either
// this frame or the previous one, never a partial frame. 0 = leave the previous image on screen (reveal hold).
int ProvideOutput(ID3D12Resource** resource, uint32_t* width, uint32_t* height, int* is_8bpc) {
  ++g_provider_calls;
  if (!g_backend_on || g_use_dll) return 0;
  if (g_hold_this_swap) { ++g_provider_held; return 0; }
  if (!rtc::WaitSwapSubmitted(uint32_t(std::max(0, REXCVAR_GET(ngpu_present_wait_ms))))) ++g_provider_waits_timed_out;
  uint32_t w = 0, h = 0;
  ID3D12Resource* out = backend::GuestOutput(w, h);
  if (!out || !w || !h) { ++g_provider_no_output; return 0; }
  *resource = out;
  *width = w;
  *height = h;
  *is_8bpc = backend::GuestOutputIs8bpc() ? 1 : 0;
  return 1;
}

void OnSwap(uint32_t fb, uint32_t fb_w, uint32_t fb_h) {
  NoteModeWord();
  if (g_use_dll) {
    g_dll.OnSwap(fb, fb_w, fb_h);
    if (g_dll.ShouldPresent()) render::RequestPresent();
    LogPeriodic();
    return;
  }
  if (!g_live_regs || !g_backend_on || !g_lockstep) return;
  uint32_t fetch0[6];
  for (uint32_t i = 0; i < 6; ++i) fetch0[i] = g_live_regs[0x4800 + i];
  static uint32_t table[256];
  static uint32_t pwl[128 * 3];
  const bool gamma = g_get_gamma && g_get_gamma(table, pwl);   // valid only during this callback
  LockstepSync(g_live_regs);
  backend::Swap(fb, fb_w, fb_h, fetch0, gamma ? table : nullptr, gamma ? pwl : nullptr);
  // PRESENT AT THE SWAP. The guest's present hook fires when the CPU submits the frame, ahead of the GPU thread
  // reaching this swap (Fable II: frames behind while walking, a whole menu behind in pause).
  g_hold_this_swap = RevealHold();
  if (!g_one_window && !g_hold_this_swap) render::RequestPresent();
  DirtyCoverageAtSwap();
  LogPeriodic();
}

}  // namespace

void Start(const render::WindowSpec& window) {
  if (!REXCVAR_GET(ngpu_backend)) {
    REXLOG_INFO("[ngpu] native backend off (ngpu_backend=false; NG2_NATIVE_GPU=1 turns it on)");
    return;
  }
  HMODULE m = GetModuleHandleA("rexgpu-xenos.dll");
  if (!m) {
    REXLOG_INFO("[ngpu] rexgpu-xenos.dll is not loaded - native path stays off");
    return;
  }
  g_set_draw = Bind<SetDrawFn>(m, "RexNgpuSetDrawCallback");
  g_set_swap = Bind<SetSwapFn>(m, "RexNgpuSetSwapCallback");
  g_get_gamma = Bind<GammaFn>(m, "RexNgpuGetGammaRamp");
  g_reveal_plugin = Bind<RevealFn>(m, "RexNgpuRevealAfterLoad");
  g_set_output = Bind<SetOutputFn>(m, "RexNgpuSetOutputProvider");   // ONE WINDOW (fork f6fc6d4c)
  g_get_device = Bind<GetDeviceFn>(m, "RexNgpuGetDevice");
  g_present_stats = Bind<PresentStatsFn>(m, "RexNgpuPresentStats");
  if (auto dirty_fn = Bind<DirtyFn>(m, "RexNgpuDirtyRegs")) {
    g_dirty = dirty_fn(&g_dirty_words);
    if (g_dirty && g_dirty_words * 64 < kForwardedEnd) g_dirty = nullptr;
  }
  if (!g_set_draw || !g_set_swap) {
    REXLOG_INFO("[ngpu] this plugin exports {}{} - native path stays off (a plugin built from the fork's "
                "native-integration branch has both)",
                g_set_draw ? "" : "no RexNgpuSetDrawCallback ", g_set_swap ? "" : "no RexNgpuSetSwapCallback");
    g_set_draw = nullptr;
    g_set_swap = nullptr;
    return;
  }
  REXLOG_INFO("[ngpu] plugin exports: draw callback, swap callback, gamma ramp {}, dirty bitmap {} ({} words), "
              "reveal-after-load {}",
              g_get_gamma ? "found" : "MISSING - the linear default ramp", g_dirty ? "found" : "MISSING - full scan",
              g_dirty_words, g_reveal_plugin ? "found" : "missing");
  const std::string offload = rex::cvar::GetFlagByName("gpu_offload_to_native");
  REXLOG_INFO("[ngpu] plugin gpu_offload_to_native = '{}' ({})", offload,
              (offload == "true" || offload == "1") ? "the native backend is the ONLY GPU; the plugin's window stays black"
                                                    : "lockstep beside the plugin: both render, compare the windows");
  const bool offload_on = offload == "true" || offload == "1";
  g_one_window = REXCVAR_GET(ngpu_one_window) && offload_on && g_set_output && g_get_device;
  if (g_one_window) {
    // No native window: the in-exe backend comes up on the plugin's device at its first draw (LockstepReady) and
    // the plugin's IssueSwap copies its frames into the runtime presenter (ultrawide fill / pillarbox, letterbox,
    // the F10 and HUD overlays and the input all stay the game window's).
    REXLOG_INFO("[ngpu] ONE WINDOW: the backend runs on the plugin's device; its frames go to the game's own window "
                "through the runtime presenter (present wait {} ms)", REXCVAR_GET(ngpu_present_wait_ms));
    g_set_output(&ProvideOutput);
  } else {
    if (REXCVAR_GET(ngpu_one_window) && offload_on)
      REXLOG_INFO("[ngpu] one-window mode unavailable: this plugin lacks RexNgpuSetOutputProvider / RexNgpuGetDevice "
                  "- the native window presents");
    if (!render::Start(window)) {
      REXLOG_INFO("[ngpu] the native window / device did not come up - native path stays off");
      return;
    }
  }
  // The DLL first, when asked for and present; the in-exe copy otherwise. Not in one-window mode: the DLL is
  // started at Start, before the plugin's device exists, and it has no is_8bpc / provider ABI.
  if (g_one_window && REXCVAR_GET(ngpu_backend_dll))
    REXLOG_INFO("[ngpu] ngpu_backend_dll ignored in one-window mode - the in-exe copy of the backend runs");
  if (!g_one_window && REXCVAR_GET(ngpu_backend_dll) && LoadBackendDll()) {
    NgpuBackendOptions o = {};
    g_dll.DefaultOptions(&o);
    o.size = sizeof(o);
    o.own_window = 0;          // NG2's window and presenter half (ng2_ngpu_window.cpp)
    o.register_callbacks = 0;  // this module forwards the plugin's callbacks
    o.async_submit = REXCVAR_GET(ngpu_backend_async_submit) ? 1 : 0;
    o.upload_skip = REXCVAR_GET(ngpu_backend_upload_skip) ? 1 : 0;
    o.hoist_uploads = REXCVAR_GET(ngpu_backend_hoist_uploads) ? 1 : 0;
    o.fast_valid = REXCVAR_GET(ngpu_backend_fast_valid) ? 1 : 0;
    o.gpu_prof = REXCVAR_GET(ngpu_gpu_prof) ? 1 : 0;
    o.reveal_hold = REXCVAR_GET(ngpu_reveal_hold) ? 1 : 0;
    o.reveal_hold_frames = REXCVAR_GET(ngpu_reveal_hold_frames);
    o.reveal_hold_max_ms = REXCVAR_GET(ngpu_reveal_hold_max_ms);
    o.selfcheck_every = REXCVAR_GET(ngpu_backend_selfcheck_every);
    if (g_dll.Start(&o, render::Device(), render::Queue())) {
      g_use_dll = true;
      g_backend_on = true;
      g_lockstep = true;
      if (g_last_fov_k > 0.0) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.6f", g_last_fov_k);
        g_dll.SetSetting("ng2_fov_k", text);
      }
      REXLOG_INFO("[ngpu] ngpu_backend.dll started in manual mode on the native window's device (async_submit {} "
                  "upload_skip {} hoist {} fast_valid {} reveal_hold {}/{} frames/{} ms selfcheck every {})",
                  o.async_submit, o.upload_skip, o.hoist_uploads, o.fast_valid, o.reveal_hold, o.reveal_hold_frames,
                  o.reveal_hold_max_ms, o.selfcheck_every);
    } else {
      REXLOG_INFO("[ngpu] NgpuBackendStart FAILED - the in-exe copy of the backend runs");
      FreeLibrary(g_dll.module);
      g_dll = BackendDll();
    }
  }
  std::memset(g_dirty_outside, 0, sizeof(g_dirty_outside));
  g_set_swap(&OnSwap);
  g_set_draw(&OnDraw);
  REXLOG_INFO("[ngpu] lockstep consumer installed (record {} bytes; backend: {})", sizeof(RexNgpuDraw),
              g_use_dll ? "ngpu_backend.dll" : "in-exe copy, initialised at the first draw");
}

void Stop() {
  // The plugin holds raw pointers into this module: clear them before anything here goes away.
  if (g_set_draw) g_set_draw(nullptr);
  if (g_set_swap) g_set_swap(nullptr);
  if (g_set_output) g_set_output(nullptr);
  g_set_output = nullptr;
  g_set_draw = nullptr;
  g_set_swap = nullptr;
  render::Stop();
  if (g_use_dll) {
    NgpuBackendStatsT st = {};
    g_dll.Stats(&st);
    REXLOG_INFO("[ngpu] totals (dll): {} draws seen ({} bad size), {} fed, {} failed, {} swaps, self-check {} runs / "
                "{} mismatches, {} presented / {} held, {} loads into gameplay",
                g_draws_seen.load(), g_draws_bad_size.load(), st.draws, st.draw_failed, st.swaps, st.selfcheck_runs,
                st.selfcheck_mismatches, st.frames_presented, st.frames_held, g_world_entries);
  } else if (g_backend_on) {
    const auto st = backend::GetStats();
    REXLOG_INFO("[ngpu] totals (in-exe): {} draws seen ({} bad size), {} fed, {} failed, {} swaps, {} shader loads "
                "({} failed), {} loads into gameplay, self-check {} runs / {} mismatches",
                g_draws_seen.load(), g_draws_bad_size.load(), st.draws, st.draw_failed, st.swaps, st.shader_loads,
                st.shader_load_failed, g_world_entries, g_check_runs, g_check_bad);
  }
}

void SetFovK(double k) {
  g_last_fov_k = k;
  FLAGS_ng2_fov_k_storage_() = k;
  if (g_use_dll && g_dll.SetSetting) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6f", k);
    g_dll.SetSetting("ng2_fov_k", text);
  }
}

int UltrawideMode() {
  if (g_use_dll && g_dll.PresentMode) {
    int32_t mode = 0;
    double k = 0.0;
    g_dll.PresentMode(&mode, &k);
    return int(mode);
  }
  return int(FLAGS_ng2_uw_mode_storage_());
}

bool BackendWaitSwapSubmitted(uint32_t timeout_ms) {
  if (g_use_dll) return g_dll.WaitSwapSubmitted(timeout_ms) != 0;
  return rtc::WaitSwapSubmitted(timeout_ms);
}

ID3D12Resource* BackendGuestOutput(uint32_t& width, uint32_t& height) {
  if (g_use_dll) return g_dll.GuestOutput(&width, &height);
  return backend::GuestOutput(width, height);
}

}  // namespace ng2::ngpu
