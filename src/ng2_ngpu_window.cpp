// The native window. See ng2_ngpu_window.h.
#include "ng2_ngpu_window.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ng2_ngpu_bridge.h"
#include "ngpu_shaders/ng2_ngpu_blit_ps.h"
#include "ngpu_shaders/ng2_ngpu_blit_vs.h"

namespace ng2::ngpu::render {
namespace {

template <typename T>
struct Com {
  T* p = nullptr;
  ~Com() { reset(); }
  void reset() { if (p) { p->Release(); p = nullptr; } }
  T** put() { reset(); return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

constexpr uint32_t kBuffers = 3;
constexpr DXGI_FORMAT kSwapFormat = DXGI_FORMAT_R10G10B10A2_UNORM;   // the backend's guest-output format: no conversion

WindowSpec g_spec;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_ready{false};
HANDLE g_started = nullptr;     // signalled once device creation succeeded or failed
HANDLE g_present_request = nullptr;
HWND g_hwnd = nullptr;

Com<ID3D12Device> g_device;
Com<ID3D12CommandQueue> g_queue;
Com<IDXGISwapChain3> g_swap;
Com<ID3D12DescriptorHeap> g_rtv_heap;
Com<ID3D12DescriptorHeap> g_srv_heap;
Com<ID3D12Resource> g_backbuffers[kBuffers];
Com<ID3D12CommandAllocator> g_allocators[kBuffers];
Com<ID3D12GraphicsCommandList> g_list;
Com<ID3D12RootSignature> g_root;
Com<ID3D12PipelineState> g_pso;
Com<ID3D12Fence> g_fence;
HANDLE g_fence_event = nullptr;
uint64_t g_fence_values[kBuffers] = {};
uint64_t g_fence_next = 1;
uint32_t g_rtv_size = 0;
bool g_tearing = false;
uint32_t g_client_w = 0, g_client_h = 0;
std::atomic<bool> g_resize_pending{false};

Stats g_stats;
std::atomic<uint64_t> g_requests{0};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
      // Closing the native window must never take the game with it.
      ShowWindow(hwnd, SW_HIDE);
      return 0;
    case WM_SIZE:
      g_resize_pending.store(true, std::memory_order_relaxed);
      return 0;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

struct MonitorPick { int index = 0, found = 0; RECT rect = {}; };

// The game's own top-level window (this process, title "Ninja Gaiden II  -  v..."): the native window takes its
// client size and its monitor, so a picture comparison resamples both from identical pixels (Fable II measured a
// client-size mismatch alone at ~0.002 of difference) and both windows live under one DPI scale.
struct GameWindowPick { HWND hwnd = nullptr; };
BOOL CALLBACK PickGameWindow(HWND h, LPARAM lp) {
  auto* pick = reinterpret_cast<GameWindowPick*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
  char title[128] = {};
  GetWindowTextA(h, title, sizeof(title));
  if (std::strstr(title, "Ninja Gaiden II") && !std::strstr(title, "native")) {
    pick->hwnd = h;
    return FALSE;
  }
  return TRUE;
}
BOOL CALLBACK PickMonitor(HMONITOR mon, HDC, LPRECT rect, LPARAM lp) {
  auto* pick = reinterpret_cast<MonitorPick*>(lp);
  if (pick->found == pick->index) { pick->rect = *rect; }
  ++pick->found;
  return TRUE;
}

bool CreateNativeWindow() {
  HINSTANCE inst = GetModuleHandle(nullptr);
  WNDCLASSA wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = "NG2NativeGpu";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  RegisterClassA(&wc);
  // The monitor the settings name, in EnumDisplayMonitors order (the same order ng2_platform.cpp enumerates).
  MonitorPick pick;
  pick.index = g_spec.monitor;
  EnumDisplayMonitors(nullptr, nullptr, PickMonitor, reinterpret_cast<LPARAM>(&pick));
  if (pick.found <= pick.index) {
    pick.rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  }
  int x, y, w, h;
  DWORD style;
  GameWindowPick game;
  EnumWindows(PickGameWindow, reinterpret_cast<LPARAM>(&game));
  RECT game_client = {};
  POINT game_origin = {};
  bool mirror = false;
  if (game.hwnd && GetClientRect(game.hwnd, &game_client) && ClientToScreen(game.hwnd, &game_origin) &&
      game_client.right > 0 && game_client.bottom > 0) {
    mirror = true;
  }
  if (mirror && !g_spec.fullscreen) {
    // The same client size, beside the game's window on its monitor (offset so both stay visible to a person;
    // captures do not need visibility).
    style = WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, game_client.right, game_client.bottom};
    AdjustWindowRect(&r, style, FALSE);
    w = r.right - r.left; h = r.bottom - r.top;
    x = game_origin.x + 40; y = game_origin.y + 40;
    REXLOG_INFO("[ngpu-window] mirroring the game window's client {}x{} at {},{}", game_client.right,
                game_client.bottom, game_origin.x, game_origin.y);
  } else if (g_spec.fullscreen) {
    // Borderless on the whole monitor - the same shape the game's own fullscreen takes.
    style = WS_POPUP;
    x = pick.rect.left; y = pick.rect.top;
    w = pick.rect.right - pick.rect.left; h = pick.rect.bottom - pick.rect.top;
  } else {
    style = WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, g_spec.width, g_spec.height};
    AdjustWindowRect(&r, style, FALSE);
    w = r.right - r.left; h = r.bottom - r.top;
    x = pick.rect.left + 40; y = pick.rect.top + 40;
  }
  g_hwnd = CreateWindowExA(0, wc.lpszClassName, "Ninja Gaiden II - native", style, x, y, w, h, nullptr, nullptr,
                           inst, nullptr);
  if (!g_hwnd) {
    REXLOG_INFO("[ngpu-window] CreateWindowEx failed (error {})", GetLastError());
    return false;
  }
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  return true;
}

bool MakeBackbuffers() {
  const auto start = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  for (uint32_t i = 0; i < kBuffers; ++i) {
    if (FAILED(g_swap->GetBuffer(i, IID_PPV_ARGS(g_backbuffers[i].put())))) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE h = start;
    h.ptr += size_t(i) * g_rtv_size;
    g_device->CreateRenderTargetView(g_backbuffers[i].p, nullptr, h);
  }
  return true;
}

void WaitForGpuIdle() {
  if (!g_queue || !g_fence) return;
  const uint64_t v = g_fence_next++;
  g_queue->Signal(g_fence.p, v);
  if (g_fence->GetCompletedValue() < v) {
    g_fence->SetEventOnCompletion(v, g_fence_event);
    WaitForSingleObject(g_fence_event, 2000);
  }
}

bool CreateDevice() {
  Com<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())))) {
    REXLOG_INFO("[ngpu-window] CreateDXGIFactory2 failed");
    return false;
  }
  BOOL tearing = FALSE;
  if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
    g_tearing = tearing != FALSE;
  // The default adapter: the plugin's own device is on it too (one GPU on this machine).
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(g_device.put())))) {
    REXLOG_INFO("[ngpu-window] D3D12CreateDevice (feature level 12_0) failed");
    return false;
  }
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  // High priority, as the plugin's own queue (d3d12_queue_priority = 1 in ng2_tuning.h).
  qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(g_queue.put())))) {
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(g_queue.put())))) {
      REXLOG_INFO("[ngpu-window] CreateCommandQueue failed");
      return false;
    }
  }
  RECT cr;
  GetClientRect(g_hwnd, &cr);
  g_client_w = uint32_t(cr.right - cr.left);
  g_client_h = uint32_t(cr.bottom - cr.top);
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = g_client_w;
  sd.Height = g_client_h;
  sd.Format = kSwapFormat;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kBuffers;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  Com<IDXGISwapChain1> swap1;
  if (FAILED(factory->CreateSwapChainForHwnd(g_queue.p, g_hwnd, &sd, nullptr, nullptr, swap1.put()))) {
    REXLOG_INFO("[ngpu-window] CreateSwapChainForHwnd failed");
    return false;
  }
  factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
  if (FAILED(swap1->QueryInterface(IID_PPV_ARGS(g_swap.put())))) return false;

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = kBuffers;
  if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(g_rtv_heap.put())))) return false;
  g_rtv_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 1;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(g_srv_heap.put())))) return false;
  if (!MakeBackbuffers()) return false;
  for (uint32_t i = 0; i < kBuffers; ++i)
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(g_allocators[i].put()))))
      return false;
  if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].p, nullptr,
                                         IID_PPV_ARGS(g_list.put()))))
    return false;
  g_list->Close();
  if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(g_fence.put())))) return false;
  g_fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

  // The blit: one SRV table, one static linear-clamp sampler, a full-screen triangle (ngpu_shaders/ng2_ngpu_blit.hlsl).
  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER param = {};
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  param.DescriptorTable.NumDescriptorRanges = 1;
  param.DescriptorTable.pDescriptorRanges = &range;
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rs = {};
  rs.NumParameters = 1;
  rs.pParameters = &param;
  rs.NumStaticSamplers = 1;
  rs.pStaticSamplers = &sampler;
  rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  Com<ID3DBlob> blob, err;
  if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), err.put()))) {
    REXLOG_INFO("[ngpu-window] root signature: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  if (FAILED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(g_root.put()))))
    return false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = g_root.p;
  pd.VS = {kNg2NgpuBlitVs, sizeof(kNg2NgpuBlitVs)};
  pd.PS = {kNg2NgpuBlitPs, sizeof(kNg2NgpuBlitPs)};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = kSwapFormat;
  pd.SampleDesc.Count = 1;
  if (FAILED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(g_pso.put())))) {
    REXLOG_INFO("[ngpu-window] CreateGraphicsPipelineState (blit) failed");
    return false;
  }
  REXLOG_INFO("[ngpu-window] device up: {}x{} client, {} buffers, R10G10B10A2, tearing {}", g_client_w, g_client_h,
              kBuffers, g_tearing ? "allowed" : "not supported");
  return true;
}

void HandleResize() {
  if (!g_resize_pending.exchange(false, std::memory_order_relaxed) || !g_swap) return;
  RECT cr;
  GetClientRect(g_hwnd, &cr);
  const uint32_t w = uint32_t(cr.right - cr.left), h = uint32_t(cr.bottom - cr.top);
  if (!w || !h || (w == g_client_w && h == g_client_h)) return;
  WaitForGpuIdle();
  for (auto& b : g_backbuffers) b.reset();
  if (FAILED(g_swap->ResizeBuffers(kBuffers, w, h, kSwapFormat, g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0))) {
    REXLOG_INFO("[ngpu-window] ResizeBuffers {}x{} failed", w, h);
    return;
  }
  g_client_w = w;
  g_client_h = h;
  MakeBackbuffers();
}

// THE PRESENTER HALF: where the guest output lands in the window.
//   ng2_uw_mode 1 (gameplay, ultrawide on): FILL the client - the plugin widened the field of view for exactly this.
//   ng2_uw_mode 2 (menu / video):            the largest box of the OUTPUT's aspect, centred (pillarbox 16:9).
//   ng2_uw_mode 0 (feature off):             the player's Keep-aspect setting: on = fit, off = stretch.
D3D12_VIEWPORT ViewportFor(uint32_t out_w, uint32_t out_h, uint32_t& mode_out) {
  // The BACKEND's ng2_uw_mode (its vendored IssueSwap writes it every swap), not the plugin registry's - see
  // ng2_ngpu_bridge.h: under offload the plugin's own swap path may not run its detection at all.
  const int m = ng2::ngpu::UltrawideMode();
  const uint32_t mode = (m >= 0 && m <= 2) ? uint32_t(m) : 0u;
  bool fit;
  if (mode == 1) fit = false;
  else if (mode == 2) fit = true;
  else {
    const std::string lb = rex::cvar::GetFlagByName("present_letterbox");
    fit = lb.empty() ? g_spec.letterbox : (lb == "true" || lb == "1");
  }
  mode_out = mode;
  D3D12_VIEWPORT vp = {0.0f, 0.0f, float(g_client_w), float(g_client_h), 0.0f, 1.0f};
  if (fit && out_w && out_h) {
    const double sx = double(g_client_w) / double(out_w), sy = double(g_client_h) / double(out_h);
    const double s = sx < sy ? sx : sy;
    vp.Width = float(out_w * s);
    vp.Height = float(out_h * s);
    vp.TopLeftX = float((g_client_w - vp.Width) * 0.5);
    vp.TopLeftY = float((g_client_h - vp.Height) * 0.5);
  }
  return vp;
}

void PresentOnce() {
  // [async submit] the backend's swap submission (the guest output this frame samples) must be on the queue first.
  if (!ng2::ngpu::BackendWaitSwapSubmitted(100)) ++g_stats.waits_timed_out;
  uint32_t ow = 0, oh = 0;
  ID3D12Resource* out = ng2::ngpu::BackendGuestOutput(ow, oh);
  if (!out) { ++g_stats.skipped_no_output; return; }
  HandleResize();
  const uint32_t index = g_swap->GetCurrentBackBufferIndex();
  // The allocator of this slot: wait for the frame that used it last.
  if (g_fence->GetCompletedValue() < g_fence_values[index]) {
    g_fence->SetEventOnCompletion(g_fence_values[index], g_fence_event);
    WaitForSingleObject(g_fence_event, 2000);
  }
  g_allocators[index]->Reset();
  g_list->Reset(g_allocators[index].p, g_pso.p);
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  g_device->CreateShaderResourceView(out, &sd, g_srv_heap->GetCPUDescriptorHandleForHeapStart());
  D3D12_RESOURCE_BARRIER to_rt = {};
  to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_rt.Transition.pResource = g_backbuffers[index].p;
  to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  g_list->ResourceBarrier(1, &to_rt);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += size_t(index) * g_rtv_size;
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  g_list->ClearRenderTargetView(rtv, black, 0, nullptr);
  uint32_t mode = 0;
  const D3D12_VIEWPORT vp = ViewportFor(ow, oh, mode);
  const D3D12_RECT scissor = {0, 0, LONG(g_client_w), LONG(g_client_h)};
  g_list->RSSetViewports(1, &vp);
  g_list->RSSetScissorRects(1, &scissor);
  g_list->SetGraphicsRootSignature(g_root.p);
  ID3D12DescriptorHeap* heaps[] = {g_srv_heap.p};
  g_list->SetDescriptorHeaps(1, heaps);
  g_list->SetGraphicsRootDescriptorTable(0, g_srv_heap->GetGPUDescriptorHandleForHeapStart());
  g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_list->DrawInstanced(3, 1, 0, 0);
  D3D12_RESOURCE_BARRIER to_present = to_rt;
  to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  g_list->ResourceBarrier(1, &to_present);
  g_list->Close();
  ID3D12CommandList* lists[] = {g_list.p};
  g_queue->ExecuteCommandLists(1, lists);   // after the backend's submissions, in queue order
  g_swap->Present(1, 0);
  g_fence_values[index] = g_fence_next++;
  g_queue->Signal(g_fence.p, g_fence_values[index]);
  ++g_stats.presented;
  g_stats.last_mode = mode;
  static uint32_t logs = 0;
  if (logs < 3) {
    ++logs;
    REXLOG_INFO("[ngpu-window] presenting the backend's {}x{} guest output (uw mode {}, viewport {}x{} at {},{})", ow,
                oh, mode, uint32_t(vp.Width), uint32_t(vp.Height), int(vp.TopLeftX), int(vp.TopLeftY));
  }
}

void Thread() {
  const bool ok = CreateNativeWindow() && CreateDevice();
  g_ready.store(ok);
  SetEvent(g_started);
  if (!ok) {
    g_running.store(false);
    return;
  }
  while (g_running.load(std::memory_order_relaxed)) {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    // One present per request; the swap callback requests at most one per guest frame. The timeout keeps the
    // message pump alive when the game is idle or the backend is not fed.
    if (WaitForSingleObject(g_present_request, 50) == WAIT_OBJECT_0) {
      ++g_stats.requests;
      PresentOnce();
    }
  }
  WaitForGpuIdle();
  g_ready.store(false);
  for (auto& b : g_backbuffers) b.reset();
  for (auto& a : g_allocators) a.reset();
  g_list.reset();
  g_pso.reset();
  g_root.reset();
  g_srv_heap.reset();
  g_rtv_heap.reset();
  g_swap.reset();
  g_fence.reset();
  // The queue and device outlive the backend's use of them only until Stop; the backend is not torn down separately
  // (the plugin's callbacks were removed first), so releasing here is the end of the line.
  g_queue.reset();
  g_device.reset();
  if (g_fence_event) { CloseHandle(g_fence_event); g_fence_event = nullptr; }
  if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}

}  // namespace

bool Start(const WindowSpec& spec) {
  if (g_running.exchange(true)) return g_ready.load();
  g_spec = spec;
  g_started = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  g_present_request = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  g_thread = std::thread(Thread);
  WaitForSingleObject(g_started, 10000);
  return g_ready.load();
}

void Stop() {
  // Join on the thread being joinable, not on the running flag: the thread clears the flag itself when device
  // creation fails, and a joinable std::thread destroyed unjoined is std::terminate.
  g_running.store(false);
  if (g_thread.joinable()) g_thread.join();
  if (g_started) { CloseHandle(g_started); g_started = nullptr; }
  if (g_present_request) { CloseHandle(g_present_request); g_present_request = nullptr; }
  REXLOG_INFO("[ngpu-window] presented {} frames of {} requests ({} without output, {} waits timed out)",
              g_stats.presented, g_stats.requests, g_stats.skipped_no_output, g_stats.waits_timed_out);
}

bool Ready() { return g_ready.load(std::memory_order_acquire); }
ID3D12Device* Device() { return g_device.p; }
ID3D12CommandQueue* Queue() { return g_queue.p; }
void RequestPresent() { if (g_present_request) SetEvent(g_present_request); }
Stats GetStats() { return g_stats; }

}  // namespace ng2::ngpu::render
