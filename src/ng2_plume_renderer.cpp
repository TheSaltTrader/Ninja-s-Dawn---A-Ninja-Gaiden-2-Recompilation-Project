#include "ng2_plume_renderer.h"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include <rex/logging.h>

#if defined(_WIN32) && defined(NG2_HAVE_PLUME)
#define NG2_PLUME_ON 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "plume_render_interface.h"
#endif

#if defined(NG2_PLUME_ON)
// Plume exposes its backends as free functions rather than through a factory,
// and declares them nowhere public. This must sit at GLOBAL scope: inside
// ng2::ngpu::render it would declare a different function entirely, and the
// link would fail with a name that looks almost right.
namespace plume {
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}
#endif

namespace ng2::ngpu::render {
namespace {

#if defined(NG2_PLUME_ON)
using namespace plume;

constexpr uint32_t kBufferCount = 2;
constexpr RenderFormat kSwapFormat = RenderFormat::B8G8R8A8_UNORM;

struct Gpu {
  std::unique_ptr<RenderInterface> interface_;
  std::unique_ptr<RenderDevice> device;
  std::unique_ptr<RenderCommandQueue> queue;
  std::unique_ptr<RenderCommandList> cmd;
  std::unique_ptr<RenderCommandFence> fence;
  std::unique_ptr<RenderSwapChain> swap;
  std::unique_ptr<RenderCommandSemaphore> acquire;
  std::vector<std::unique_ptr<RenderCommandSemaphore>> release;
  std::vector<std::unique_ptr<RenderFramebuffer>> framebuffers;
  bool ready = false;
};

Gpu g_gpu;
HWND g_hwnd = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_presented{0};
std::atomic<uint64_t> g_guest_frames{0};

// The coverage oracle. Per-frame pair, reset at EndFrame, plus running totals
// so the final line can state the shortfall over the whole run.
std::atomic<uint64_t> g_draws_handed{0};
std::atomic<uint64_t> g_draws_rendered{0};
std::atomic<uint64_t> g_total_handed{0};
std::atomic<uint64_t> g_total_rendered{0};

void MakeFramebuffers() {
  g_gpu.framebuffers.clear();
  for (uint32_t i = 0; i < g_gpu.swap->getTextureCount(); ++i) {
    const RenderTexture* colour = g_gpu.swap->getTexture(i);
    RenderFramebufferDesc d;
    d.colorAttachments = &colour;
    d.colorAttachmentsCount = 1;
    d.depthAttachment = nullptr;
    g_gpu.framebuffers.push_back(g_gpu.device->createFramebuffer(d));
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
      // The shadow window closing must never take the game with it.
      ShowWindow(hwnd, SW_HIDE);
      return 0;
    case WM_SIZE:
      if (g_gpu.ready && g_gpu.swap) {
        g_gpu.framebuffers.clear();
        g_gpu.swap->resize();
        MakeFramebuffers();
      }
      return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

bool CreateShadowWindow() {
  HINSTANCE inst = GetModuleHandle(nullptr);
  WNDCLASSA wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = "NG2NativeGpuShadow";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassA(&wc);
  g_hwnd = CreateWindowExA(0, wc.lpszClassName, "NG2 native GPU (Plume D3D12) - shadow",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
                           nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) {
    REXLOG_INFO("[ng2-plume] could not create the shadow window (error {})", GetLastError());
    return false;
  }
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  return true;
}

bool CreateDevice() {
  g_gpu.interface_ = plume::CreateD3D12Interface();
  if (!g_gpu.interface_) {
    REXLOG_INFO("[ng2-plume] CreateD3D12Interface returned null - native window stays off");
    return false;
  }
  g_gpu.device = g_gpu.interface_->createDevice();
  if (!g_gpu.device) {
    REXLOG_INFO("[ng2-plume] createDevice failed - native window stays off");
    return false;
  }
  g_gpu.queue = g_gpu.device->createCommandQueue(RenderCommandListType::DIRECT);
  g_gpu.fence = g_gpu.device->createCommandFence();
  g_gpu.swap = g_gpu.queue->createSwapChain(
      RenderSwapChainDesc((RenderWindow)g_hwnd, kSwapFormat, kBufferCount));
  g_gpu.swap->resize();
  g_gpu.cmd = g_gpu.queue->createCommandList();
  g_gpu.acquire = g_gpu.device->createCommandSemaphore();
  MakeFramebuffers();
  g_gpu.ready = true;
  return true;
}

// STAGE 2a: clear and present, nothing else.
//
// The draws are NOT rendered yet, deliberately. What this establishes first is
// that a second D3D12 device can live inside ng2.exe beside the plugin's own,
// hold a swap chain, and present every frame without disturbing the game - and
// that Plume links and runs in this build at all. A renderer built before that
// is established would have two candidate explanations for every failure.
void RenderOneFrame() {
  if (!g_gpu.ready || g_gpu.swap->isEmpty()) return;
  uint32_t index = 0;
  if (!g_gpu.swap->acquireTexture(g_gpu.acquire.get(), &index)) return;

  g_gpu.cmd->begin();
  RenderTexture* tex = g_gpu.swap->getTexture(index);
  g_gpu.cmd->barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(tex, RenderTextureLayout::COLOR_WRITE));
  g_gpu.cmd->setFramebuffer(g_gpu.framebuffers[index].get());
  const uint32_t w = g_gpu.swap->getWidth();
  const uint32_t h = g_gpu.swap->getHeight();
  g_gpu.cmd->setViewports(RenderViewport(0.0f, 0.0f, float(w), float(h)));
  g_gpu.cmd->setScissors(RenderRect(0, 0, w, h));
  g_gpu.cmd->clearColor(0, RenderColor(0.45f, 0.02f, 0.02f, 1.0f));
  g_gpu.cmd->barriers(RenderBarrierStage::NONE,
                      RenderTextureBarrier(tex, RenderTextureLayout::PRESENT));
  g_gpu.cmd->end();

  while (g_gpu.release.size() < g_gpu.swap->getTextureCount())
    g_gpu.release.emplace_back(g_gpu.device->createCommandSemaphore());

  const RenderCommandList* list = g_gpu.cmd.get();
  RenderCommandSemaphore* wait = g_gpu.acquire.get();
  RenderCommandSemaphore* signal = g_gpu.release[index].get();
  g_gpu.queue->executeCommandLists(&list, 1, &wait, 1, &signal, 1, g_gpu.fence.get());
  g_gpu.swap->present(index, &signal, 1);
  g_gpu.queue->waitForCommandFence(g_gpu.fence.get());
  g_presented.fetch_add(1, std::memory_order_relaxed);
}

void Thread() {
  if (!CreateShadowWindow() || !CreateDevice()) {
    g_running.store(false);
    return;
  }
  REXLOG_INFO("[ng2-plume] shadow device up: {}x{}, {} swap textures",
              g_gpu.swap->getWidth(), g_gpu.swap->getHeight(), g_gpu.swap->getTextureCount());

  uint64_t last_report = 0;
  while (g_running.load(std::memory_order_relaxed)) {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    RenderOneFrame();
    const uint64_t n = g_presented.load(std::memory_order_relaxed);
    if (n - last_report >= 600) {
      last_report = n;
      REXLOG_INFO("[ng2-plume] presented {} native frames ({} guest frames seen)", n,
                  g_guest_frames.load(std::memory_order_relaxed));
    }
  }

  // Order matters: the swap chain must go before the queue that made it.
  g_gpu.framebuffers.clear();
  g_gpu.release.clear();
  g_gpu.acquire.reset();
  g_gpu.swap.reset();
  g_gpu.cmd.reset();
  g_gpu.fence.reset();
  g_gpu.queue.reset();
  g_gpu.device.reset();
  g_gpu.interface_.reset();
  g_gpu.ready = false;
  if (g_hwnd) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
}
#endif  // NG2_PLUME_ON

}  // namespace

void Start() {
#if defined(NG2_PLUME_ON)
  if (!std::getenv("NG2_NATIVE_GPU_WINDOW")) return;
  if (g_running.exchange(true)) return;
  g_thread = std::thread(Thread);
  REXLOG_INFO("[ng2-plume] native shadow renderer starting (stage 2a: clear and present)");
#endif
}

void Stop() {
#if defined(NG2_PLUME_ON)
  // Join on the THREAD being joinable, not on the running flag. The thread
  // clears that flag itself when device creation fails, so keying the join off
  // it would leave a joinable std::thread to be destroyed - which is
  // std::terminate, turning a failed renderer into a crash on exit.
  g_running.store(false);
  if (!g_thread.joinable()) return;
  g_thread.join();
  const uint64_t handed = g_total_handed.load();
  const uint64_t drawn = g_total_rendered.load();
  REXLOG_INFO("[ng2-plume] presented {} native frames over {} guest frames | COVERAGE {} of {}"
              " draws ({:.2f}%), {} never rendered",
              g_presented.load(), g_guest_frames.load(), drawn, handed,
              handed ? 100.0 * double(drawn) / double(handed) : 0.0, handed - drawn);
#endif
}

void EndFrame() {
#if defined(NG2_PLUME_ON)
  if (!g_running.load(std::memory_order_relaxed)) return;
  const uint64_t frame = g_guest_frames.fetch_add(1, std::memory_order_relaxed) + 1;

  const uint64_t handed = g_draws_handed.exchange(0, std::memory_order_relaxed);
  const uint64_t drawn = g_draws_rendered.exchange(0, std::memory_order_relaxed);
  g_total_handed.fetch_add(handed, std::memory_order_relaxed);
  g_total_rendered.fetch_add(drawn, std::memory_order_relaxed);

  // Report the SHORTFALL, not the achievement. At stage 2a this reads
  // "0 of N (0%)", which is the honest state of a renderer that draws nothing
  // yet - and it will keep being the number that matters when it draws
  // something, because a native path silently skipping a draw shows up here as
  // a discrepancy rather than as a slightly smaller count nobody queries.
  if (handed && (frame % 300) == 0) {
    REXLOG_INFO("[ng2-plume] frame {}: rendered {} of {} draws ({:.1f}%) - {} MISSED",
                frame, drawn, handed, 100.0 * double(drawn) / double(handed), handed - drawn);
  }
#endif
}

void NoteDrawHandedOff() {
#if defined(NG2_PLUME_ON)
  if (g_running.load(std::memory_order_relaxed))
    g_draws_handed.fetch_add(1, std::memory_order_relaxed);
#endif
}

void NoteDrawRendered() {
#if defined(NG2_PLUME_ON)
  if (g_running.load(std::memory_order_relaxed))
    g_draws_rendered.fetch_add(1, std::memory_order_relaxed);
#endif
}

}  // namespace ng2::ngpu::render
