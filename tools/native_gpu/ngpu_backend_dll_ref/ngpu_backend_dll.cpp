// ngpu_backend.dll - see ngpu_backend_api.h. The same lockstep glue as Fable II's in-app integration
// (native_gpu_present.cpp: BackendLockstepSync / BackendLockstepDraw / OnBridgeSwap / RevealHold), with nothing
// Fable-specific left in it: the host tells it when a load finished, everything else comes from the plugin.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <emmintrin.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#define NGPU_BACKEND_BUILD 1
#include "ngpu_backend_dll/ngpu_backend_api.h"
#include "native_gpu_backend.h"
#include "rtc_d3d12/facade.h"

namespace backend = fable2::ngpu::backend;

// Every accessor-only setting of the vendored backend (generated - tools/native_gpu/gen_setting_table.py).
enum class SettingKind { kBool, kInt, kDouble, kString };
#include "ngpu_backend_dll/ngpu_setting_table.inc"
namespace {
struct SettingEntry { const char* name; SettingKind kind; void* (*get)(); };
const SettingEntry kSettings[] = {NGPU_SETTING_TABLE};
const SettingEntry* FindSetting(const char* name) {
  for (const auto& e : kSettings)
    if (name && std::strcmp(e.name, name) == 0) return &e;
  return nullptr;
}
}  // namespace

namespace {
NgpuBackendOptions g_opt{};
bool g_started = false;

// ---- the plugin ABI (rexgpu-xenos.dll, rexglue-src command_processor.cpp) ---------------------------------------
struct RexNgpuDraw {
  uint32_t size;
  const uint32_t* regs;
  uint32_t reg_count;
  uint32_t draw_initiator;
  uint32_t index_addr;
  uint32_t index_size;
  uint32_t inline_addr;
  uint32_t vs_addr, vs_dwords, ps_addr, ps_dwords;
  uint32_t vs_inline, ps_inline;
  uint32_t predicated;
  uint64_t bin_mask, bin_select;
  const uint8_t* vs_code;
  const uint8_t* ps_code;
  uint32_t vs_code_dwords, ps_code_dwords;
};
using RexNgpuDrawFn = void (*)(const RexNgpuDraw*);
using RexNgpuSwapFn = void (*)(uint32_t, uint32_t, uint32_t);
HMODULE Plugin() { static HMODULE m = GetModuleHandleA("rexgpu-xenos.dll"); return m; }
template <typename T> T PluginProc(const char* name) { return Plugin() ? reinterpret_cast<T>(GetProcAddress(Plugin(), name)) : nullptr; }

// ---- guest memory: readable-page check (VirtualQuery results cached, positive for good, negative for 100 ms) -------
const uint8_t* PBase() {
  static const uint8_t* b = rex::system::kernel_state()->memory()->physical_membase();
  return b;
}
bool PageReadable(const void* host) {
  static std::vector<uint64_t> seen((512u * 256) / 64, 0);
  static std::unique_ptr<std::atomic<uint32_t>[]> neg(new std::atomic<uint32_t>[512u * 256]());
  const uint8_t* pb = PBase();
  const uint8_t* h = static_cast<const uint8_t*>(host);
  int64_t page = (h >= pb && h < pb + (uint64_t(512) << 20)) ? int64_t((h - pb) >> 12) : -1;
  if (page >= 0 && ((seen[size_t(page) >> 6] >> (page & 63)) & 1)) return true;
  const uint32_t now_ms = uint32_t(GetTickCount64());
  if (page >= 0) { const uint32_t t = neg[size_t(page)].load(std::memory_order_relaxed); if (t && now_ms - t < 100u) return false; }
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(host, &mbi, sizeof(mbi))) return false;
  const bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD);
  if (page >= 0) {
    const uint8_t* rend = std::min(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize, pb + (uint64_t(512) << 20));
    const int64_t pages = std::min<int64_t>(int64_t((rend - h + 0xFFF) >> 12), 65536);
    for (int64_t q = page; q < page + std::max<int64_t>(pages, 1); ++q) {
      if (ok) seen[size_t(q) >> 6] |= uint64_t(1) << (q & 63);
      else neg[size_t(q)].store(now_ms ? now_ms : 1u, std::memory_order_relaxed);
    }
  }
  return ok;
}

// ---- register sync: the plugin's dirty bitmap, self-checked against a full diff ---------------------------------
uint32_t g_prev[0x5003];
bool g_prev_valid = false;
const uint32_t* g_live_regs = nullptr;
std::atomic<uint64_t> g_selfcheck_runs{0}, g_selfcheck_bad{0};

void SyncRegisters(const uint32_t* regs) {
  static const uint32_t kRanges[][2] = {{0x2000, 0x2400}, {0x4000, 0x4928}};
  using DirtyFn = uint64_t* (*)(uint32_t*);
  static uint32_t dirty_words = 0;
  static uint64_t* dirty = [] {
    auto f = PluginProc<DirtyFn>("RexNgpuDirtyRegs");
    uint64_t* d = f ? f(&dirty_words) : nullptr;
    if (d && dirty_words * 64 < 0x4928) d = nullptr;
    REXLOG_INFO("[ngpu_backend.dll] plugin dirty-register bitmap {}", d ? "found" : "MISSING - full register scan per draw");
    return d;
  }();
  static bool dirty_ok = true;
  static uint64_t n = 0;
  const uint32_t every = uint32_t(std::max(0, g_opt.selfcheck_every));
  const bool check = g_prev_valid && every && ((++n % every) == 0);
  uint64_t expect = 0, written = 0;
  if (check)
    for (const auto& r : kRanges)
      for (uint32_t k = r[0]; k < r[1]; ++k) expect += g_prev[k] != regs[k];
  auto forward = [&](uint32_t k) {
    g_prev[k] = regs[k];
    backend::WriteRegister(k, regs[k]);
    ++written;
  };
  if (dirty && dirty_ok && g_prev_valid) {
    for (const auto& r : kRanges)
      for (uint32_t w = r[0] >> 6; w <= (r[1] - 1) >> 6; ++w) {
        uint64_t bits = dirty[w];
        if (!bits) continue;
        dirty[w] = 0;
        while (bits) {
          unsigned long b;
          _BitScanForward64(&b, bits);
          bits &= bits - 1;
          const uint32_t k = (w << 6) + uint32_t(b);
          if (k < r[0] || k >= r[1] || g_prev[k] == regs[k]) continue;
          forward(k);
        }
      }
  } else {
    if (dirty) std::memset(dirty, 0, dirty_words * sizeof(uint64_t));
    for (const auto& r : kRanges)
      for (uint32_t i = r[0]; i < r[1]; i += 16) {
        if (g_prev_valid && i + 16 <= r[1]) {
          const __m128i* a = reinterpret_cast<const __m128i*>(&regs[i]);
          const __m128i* p = reinterpret_cast<const __m128i*>(&g_prev[i]);
          const __m128i eq = _mm_and_si128(
              _mm_and_si128(_mm_cmpeq_epi32(_mm_loadu_si128(a), _mm_loadu_si128(p)),
                            _mm_cmpeq_epi32(_mm_loadu_si128(a + 1), _mm_loadu_si128(p + 1))),
              _mm_and_si128(_mm_cmpeq_epi32(_mm_loadu_si128(a + 2), _mm_loadu_si128(p + 2)),
                            _mm_cmpeq_epi32(_mm_loadu_si128(a + 3), _mm_loadu_si128(p + 3))));
          if (_mm_movemask_epi8(eq) == 0xFFFF) continue;
        }
        for (uint32_t k = i; k < i + 16 && k < r[1]; ++k)
          if (!g_prev_valid || g_prev[k] != regs[k]) forward(k);
      }
  }
  g_prev_valid = true;
  if (check) {
    ++g_selfcheck_runs;
    if (written != expect) {
      ++g_selfcheck_bad;
      if (dirty_ok && dirty) {
        dirty_ok = false;
        REXLOG_WARN("[ngpu_backend.dll] the dirty bitmap MISSED a register write ({} written, {} differ) - full scan from now on", written, expect);
      }
    }
  }
}

// ---- reveal hold --------------------------------------------------------------------------------------------------
std::atomic<uint32_t> g_reveal_requests{0};
std::atomic<bool> g_should_present{true};
std::atomic<uint64_t> g_presented{0}, g_held{0};
bool RevealHold() {
  static uint32_t seen = 0;
  static bool holding = false;
  static int64_t start_ms = 0;
  static uint64_t last_not_ready = 0, last_draws = 0, prev_frame_draws = 0;
  static int complete_run = 0, held = 0;
  const auto rd = backend::GetReadiness();
  const uint64_t draws = backend::GetStats().draws;
  const uint64_t frame_draws = draws - last_draws, skipped = rd.pipeline_not_ready_draws - last_not_ready;
  last_draws = draws;
  last_not_ready = rd.pipeline_not_ready_draws;
  const uint64_t prev = prev_frame_draws;
  prev_frame_draws = frame_draws;
  const int64_t now_ms = int64_t(GetTickCount64());
  const uint32_t req = g_reveal_requests.load();
  if (req != seen) {
    seen = req;
    if (g_opt.reveal_hold) {
      holding = true; start_ms = now_ms; complete_run = 0; held = 0;
      REXLOG_INFO("[ngpu_backend.dll] reveal: load finished - holding the last frame until the stage is complete");
    }
  }
  if (!holding) return false;
  const bool steady = prev && frame_draws && (frame_draws > prev ? frame_draws - prev : prev - frame_draws) * 100 <= prev * 3;
  complete_run = (skipped == 0 && !rd.creating_pipelines && steady) ? complete_run + 1 : 0;
  ++held;
  const bool done = complete_run >= std::max(1, g_opt.reveal_hold_frames);
  const bool cap = now_ms - start_ms >= g_opt.reveal_hold_max_ms;
  if (done || cap) {
    holding = false;
    REXLOG_INFO("[ngpu_backend.dll] reveal: showing the stage after {} held frames / {} ms ({})", held, now_ms - start_ms,
                done ? "complete" : "the cap");
    return false;
  }
  return true;
}

// ---- own window + presenter (own_window=1) --------------------------------------------------------------------------
struct Presenter {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  HWND hwnd = nullptr;
  IDXGISwapChain3* swap = nullptr;
  ID3D12CommandAllocator* alloc = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE fence_event = nullptr;
  uint64_t fence_value = 0;
  uint32_t sw = 0, sh = 0;
  std::mutex mu;
  std::condition_variable cv;
  uint64_t requested = 0, done = 0;
  std::thread thread;

  static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { ShowWindow(h, SW_MINIMIZE); return 0; }   // the game owns the lifetime
    return DefWindowProcW(h, m, w, l);
  }
  bool CreateWindowAndChain() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NgpuBackendWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExW(0, wc.lpszClassName, g_opt.window_title ? g_opt.window_title : L"Native renderer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = sw = 1280; d.Height = sh = 720;
    d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    const bool ok = SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &d, nullptr, nullptr, &sc1)) &&
                    SUCCEEDED(sc1->QueryInterface(IID_PPV_ARGS(&swap)));
    if (sc1) sc1->Release();
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    if (!ok) return false;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return alloc && list && fence && fence_event;
  }
  void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    list->ResourceBarrier(1, &br);
  }
  void PresentOne() {
    uint32_t ow = 0, oh = 0;
    ID3D12Resource* out = backend::GuestOutput(ow, oh);
    if (!out || !ow || !oh) return;
    if (ow != sw || oh != sh) {   // the swap chain follows the guest output size; DXGI stretches to the window
      if (FAILED(swap->ResizeBuffers(2, ow, oh, DXGI_FORMAT_R10G10B10A2_UNORM, 0))) return;
      sw = ow; sh = oh;
    }
    ID3D12Resource* back = nullptr;
    if (FAILED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)))) return;
    alloc->Reset();
    list->Reset(alloc, nullptr);
    Barrier(out, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(back, out);
    Barrier(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->Close();
    ::fable2::ngpu::rtc::WaitSwapSubmitted(100);   // the backend's swap submission (the output we copy) goes first
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    swap->Present(1, 0);
    queue->Signal(fence, ++fence_value);
    if (fence->GetCompletedValue() < fence_value) {
      fence->SetEventOnCompletion(fence_value, fence_event);
      WaitForSingleObject(fence_event, 1000);
    }
    back->Release();
    ++g_presented;
  }
  void Run() {
    if (!CreateWindowAndChain()) { REXLOG_ERROR("[ngpu_backend.dll] could not create the presenter window / swap chain"); return; }
    REXLOG_INFO("[ngpu_backend.dll] presenter window up (own thread)");
    for (;;) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
      uint64_t want;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::milliseconds(16), [&] { return requested != done; });
        want = requested;
      }
      if (want != done) { PresentOne(); done = want; }
    }
  }
  void Request() {
    { std::lock_guard<std::mutex> lk(mu); ++requested; }
    cv.notify_one();
  }
};
Presenter* g_presenter = nullptr;

// ---- the plugin callbacks (plugin GPU thread) ---------------------------------------------------------------------
void OnDraw(const RexNgpuDraw* d) {
  if (!g_started || !d || !d->regs || d->reg_count < 0x4928) return;
  g_live_regs = d->regs;
  SyncRegisters(d->regs);
  auto code = [](uint32_t addr, uint32_t& dwords, bool inl, const uint8_t* inline_code, uint32_t inline_dwords) -> const uint32_t* {
    if (inl) {
      if (!inline_code) return nullptr;
      dwords = inline_dwords;   // the PACKET's own length is the one the pipeline cache needs
      return reinterpret_cast<const uint32_t*>(inline_code);
    }
    if (!addr || !dwords) return nullptr;
    const uint8_t* p = PBase() + (addr & 0x1FFFFFFF);
    if (!(PageReadable(p) && PageReadable(p + dwords * 4 - 1))) return nullptr;
    return reinterpret_cast<const uint32_t*>(p);
  };
  const bool has_inline = d->size >= sizeof(RexNgpuDraw);
  backend::DrawRecord br{};
  br.draw_initiator = d->draw_initiator;
  br.index_addr = d->index_addr;
  br.index_size = d->index_size;
  br.vs_addr = d->vs_addr; br.vs_dwords = d->vs_dwords;
  br.ps_addr = d->ps_addr; br.ps_dwords = d->ps_dwords;
  br.vs_code = code(d->vs_addr, br.vs_dwords, d->vs_inline != 0, has_inline ? d->vs_code : nullptr, has_inline ? d->vs_code_dwords : 0);
  br.ps_code = code(d->ps_addr, br.ps_dwords, d->ps_inline != 0, has_inline ? d->ps_code : nullptr, has_inline ? d->ps_code_dwords : 0);
  backend::Draw(br);
}
void OnSwap(uint32_t fb, uint32_t w, uint32_t h) {
  if (!g_started || !g_live_regs) return;
  uint32_t fetch0[6];
  for (uint32_t i = 0; i < 6; ++i) fetch0[i] = g_live_regs[0x4800 + i];
  using GammaFn = bool (*)(uint32_t*, uint32_t*);
  static GammaFn gamma = PluginProc<GammaFn>("RexNgpuGetGammaRamp");
  static uint32_t table[256], pwl[128 * 3];
  const bool have_gamma = gamma && gamma(table, pwl);
  SyncRegisters(g_live_regs);
  backend::Swap(fb, w, h, fetch0, have_gamma ? table : nullptr, have_gamma ? pwl : nullptr);
  const bool hold = RevealHold();
  g_should_present = !hold;
  if (hold) { ++g_held; return; }
  if (g_presenter) g_presenter->Request();
}
}  // namespace

// ---- the settings the vendored backend asks for (facade.cpp reads these once) ---------------------------------------
namespace fable2::ngpu::rtc {
bool NgpuBackendAsyncSubmitCvar() { return g_opt.async_submit != 0; }
bool NgpuBackendFastValidCvar() { return g_opt.fast_valid != 0; }
bool NgpuBackendUploadSkipCvar() { return g_opt.upload_skip != 0; }
bool NgpuGpuProfCvar() { return g_opt.gpu_prof != 0; }
bool NgpuHoistUploadsCvar() { return g_opt.hoist_uploads != 0; }
}  // namespace fable2::ngpu::rtc

NGPU_API uint32_t NgpuBackendAbiVersion(void) { return NGPU_BACKEND_ABI; }

NGPU_API void NgpuBackendDefaultOptions(NgpuBackendOptions* o) {
  if (!o) return;
  *o = {};
  o->size = sizeof(NgpuBackendOptions);
  o->own_window = 1;
  o->register_callbacks = 1;
  o->async_submit = 1;
  o->upload_skip = 1;
  o->hoist_uploads = 1;
  o->fast_valid = 1;
  o->gpu_prof = 0;
  o->reveal_hold = 1;
  o->reveal_hold_frames = 6;
  o->reveal_hold_max_ms = 2500;
  o->selfcheck_every = 1024;
  o->window_title = L"Native renderer";
}

NGPU_API int NgpuBackendStart(const NgpuBackendOptions* o, ID3D12Device* device, ID3D12CommandQueue* queue) {
  if (g_started) return 1;
  NgpuBackendDefaultOptions(&g_opt);
  if (o) std::memcpy(&g_opt, o, std::min<size_t>(o->size ? o->size : sizeof(g_opt), sizeof(g_opt)));
  if (g_opt.own_window) {
    // Own device and queue: the backend and the presenter share the queue (the copy of the guest output must follow
    // the backend's swap submission on the same queue).
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
      REXLOG_ERROR("[ngpu_backend.dll] D3D12CreateDevice failed");
      return 0;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return 0;
  }
  if (!device || !queue) { REXLOG_ERROR("[ngpu_backend.dll] manual mode needs a device and a queue"); return 0; }
  if (!backend::Init(device, queue)) return 0;
  if (g_opt.own_window) {
    g_presenter = new Presenter();
    g_presenter->device = device;
    g_presenter->queue = queue;
    g_presenter->thread = std::thread([] { g_presenter->Run(); });
    g_presenter->thread.detach();
  }
  g_started = true;
  if (g_opt.register_callbacks) {
    auto set_draw = PluginProc<void (*)(RexNgpuDrawFn)>("RexNgpuSetDrawCallback");
    auto set_swap = PluginProc<void (*)(RexNgpuSwapFn)>("RexNgpuSetSwapCallback");
    if (!set_draw || !set_swap) {
      REXLOG_ERROR("[ngpu_backend.dll] rexgpu-xenos.dll lacks RexNgpuSetDrawCallback / RexNgpuSetSwapCallback");
      return 0;
    }
    set_draw(&OnDraw);
    set_swap(&OnSwap);
  }
  REXLOG_INFO("[ngpu_backend.dll] started (ABI {}, {} window, callbacks {}, async submit {}, upload skip {}, hoist {}, reveal hold {})",
              NGPU_BACKEND_ABI, g_opt.own_window ? "own" : "host", g_opt.register_callbacks ? "registered" : "forwarded by the host",
              g_opt.async_submit, g_opt.upload_skip, g_opt.hoist_uploads, g_opt.reveal_hold);
  return 1;
}

NGPU_API void NgpuBackendOnDraw(const void* d) { OnDraw(static_cast<const RexNgpuDraw*>(d)); }
NGPU_API void NgpuBackendOnSwap(uint32_t fb, uint32_t w, uint32_t h) { OnSwap(fb, w, h); }
NGPU_API void NgpuBackendRevealAfterLoad(void) { g_reveal_requests.fetch_add(1); }
NGPU_API int NgpuBackendShouldPresent(void) { return g_should_present.load() ? 1 : 0; }
NGPU_API int NgpuBackendWaitSwapSubmitted(uint32_t timeout_ms) { return ::fable2::ngpu::rtc::WaitSwapSubmitted(timeout_ms) ? 1 : 0; }
NGPU_API ID3D12Resource* NgpuBackendGuestOutput(uint32_t* w, uint32_t* h) {
  uint32_t ow = 0, oh = 0;
  ID3D12Resource* r = backend::GuestOutput(ow, oh);
  if (w) *w = ow;
  if (h) *h = oh;
  return r;
}
NGPU_API void NgpuBackendPresentMode(int32_t* uw_mode, double* fov_k) {
  int32_t m = 0;
  double k = 1.0;
  backend::PresentMode(m, k);
  if (uw_mode) *uw_mode = m;
  if (fov_k) *fov_k = k;
}
NGPU_API int NgpuBackendGetSetting(const char* name, char* buffer, uint32_t buffer_size) {
  const SettingEntry* e = FindSetting(name);
  if (!e || !buffer || !buffer_size) return 0;
  std::string v;
  void* p = e->get();
  switch (e->kind) {
    case SettingKind::kBool: v = *static_cast<bool*>(p) ? "true" : "false"; break;
    case SettingKind::kInt: v = std::to_string(*static_cast<int32_t*>(p)); break;
    case SettingKind::kDouble: v = std::to_string(*static_cast<double*>(p)); break;
    case SettingKind::kString: v = *static_cast<std::string*>(p); break;
  }
  std::strncpy(buffer, v.c_str(), buffer_size - 1);
  buffer[buffer_size - 1] = 0;
  return 1;
}
NGPU_API int NgpuBackendSetSetting(const char* name, const char* value) {
  const SettingEntry* e = FindSetting(name);
  if (!e || !value) return 0;
  void* p = e->get();
  try {
    switch (e->kind) {
      case SettingKind::kBool: *static_cast<bool*>(p) = std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0; break;
      case SettingKind::kInt: *static_cast<int32_t*>(p) = std::stoi(value); break;
      case SettingKind::kDouble: *static_cast<double*>(p) = std::stod(value); break;
      case SettingKind::kString: *static_cast<std::string*>(p) = value; break;
    }
  } catch (...) {
    return 0;
  }
  return 1;
}
NGPU_API void NgpuBackendStats(NgpuBackendStatsT* out) {
  if (!out) return;
  const auto s = backend::GetStats();
  out->draws = s.draws;
  out->draw_failed = s.draw_failed;
  out->swaps = s.swaps;
  out->selfcheck_runs = g_selfcheck_runs.load();
  out->selfcheck_mismatches = g_selfcheck_bad.load();
  out->frames_presented = g_presented.load();
  out->frames_held = g_held.load();
}
