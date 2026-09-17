#include "ng2_plume_renderer.h"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <mutex>
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

// THE SHADER REGISTRY, keyed by guest address BY VALUE.
//
// Constraint 1 of this file's header: no raw pointers into an evictable cache.
// The key is the address the IM_LOAD named, and nothing here holds a pointer
// into the plugin's shader cache - which can evict, and whose erase sites
// destroy the owner. A stale entry here is a wrong ANSWER, recoverable; a stale
// pointer would be a use-after-free far from its cause.
struct ShaderSeen {
  uint32_t dwords = 0;
  uint64_t draws = 0;
  bool immediate = false;
  bool available = false;  // true once a translated program exists for it
};
std::mutex g_shader_mutex;
std::map<uint32_t, ShaderSeen> g_vertex_shaders;
std::map<uint32_t, ShaderSeen> g_pixel_shaders;

// THE MANIFEST: full-program microcode hash -> translated artefact.
//
// Keyed by CONTENT, not by address, and that is forced rather than preferred.
// The containers were extracted from a guest memory dump and are named for
// their offset in it; the runtime IM_LOAD addresses are physical. There is no
// reliable mapping between those two address spaces from out here - but the
// microcode is the same bytes in both, so content is the key that survives.
//
// The hash covers the WHOLE program. The first code dword of the first shader
// this game loads, 0x30052003, is shared by fifteen of the 625 containers -
// Xenos prologues are highly repetitive, and a short needle has already
// produced one false finding on this project.
struct ManifestEntry {
  uint32_t dwords = 0;
  bool is_pixel = false;
  std::string artefact;
};
std::map<uint64_t, ManifestEntry> g_manifest;
std::atomic<uint64_t> g_manifest_hits{0};
std::atomic<uint64_t> g_manifest_unknown{0};
std::atomic<uint64_t> g_manifest_stage_mismatch{0};
// PER STAGE. "unknown" without its stage cannot answer the question the
// symmetric lookup exists to ask - which is precisely which stage is failing.
std::atomic<uint64_t> g_hit_vs{0}, g_hit_ps{0};
std::atomic<uint64_t> g_unknown_vs{0}, g_unknown_ps{0};
// The first few unmatched programs, named, so the failure can be chased
// offline against the manifest instead of by re-running.
std::mutex g_unknown_log_mutex;
int g_unknown_logged = 0;

uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

void LoadManifest(const char* path) {
  std::ifstream f(path);
  if (!f) {
    REXLOG_INFO("[ng2-plume] no shader manifest at {} - every lookup will miss", path);
    return;
  }
  std::string line;
  size_t n = 0;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream is(line);
    std::string hash_s, stage_s, name;
    uint32_t dwords = 0;
    if (!(is >> hash_s >> dwords >> stage_s >> name)) continue;
    ManifestEntry e;
    e.dwords = dwords;
    e.is_pixel = (stage_s == "p");
    e.artefact = name;
    g_manifest[std::strtoull(hash_s.c_str(), nullptr, 16)] = e;
    ++n;
  }
  REXLOG_INFO("[ng2-plume] shader manifest: {} programs ({} distinct hashes)", n, g_manifest.size());
}
std::atomic<uint64_t> g_vs_miss{0}, g_ps_miss{0};
std::atomic<uint64_t> g_vs_immediate{0}, g_ps_immediate{0};

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
  // Forward slashes deliberately: written with backslashes, "D:\ng2_..." makes
  // \n a NEWLINE and the path silently became "D:". The loader then reported
  // "no shader manifest at D:" and every lookup missed, which looks exactly
  // like an empty registry. Windows accepts forward slashes everywhere.
  const char* manifest = std::getenv("NG2_SHADER_MANIFEST");
  LoadManifest(manifest ? manifest : "D:/ng2_frameinterp/shaders/ng2_shaders.manifest");
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
    size_t vs_n = 0, ps_n = 0;
    {
      std::lock_guard<std::mutex> lock(g_shader_mutex);
      vs_n = g_vertex_shaders.size();
      ps_n = g_pixel_shaders.size();
    }
    // The shader side is reported with the SAME shape as the draw side: what is
    // missing, named per stage, so an asymmetry between vertex and pixel is
    // visible as a number rather than as a uniformly unlit picture.
    REXLOG_INFO("[ng2-plume] frame {}: rendered {} of {} draws ({:.1f}%) - {} MISSED"
                " | shaders seen VS {} PS {} | misses VS {} PS {} | immediate VS {} PS {}",
                frame, drawn, handed, 100.0 * double(drawn) / double(handed), handed - drawn,
                vs_n, ps_n, g_vs_miss.load(), g_ps_miss.load(),
                g_vs_immediate.load(), g_ps_immediate.load());
    REXLOG_INFO("[ng2-plume] manifest: {} programs | matched VS {} PS {} | unknown VS {} PS {}"
                " | stage-mismatch {}",
                g_manifest.size(), g_hit_vs.load(), g_hit_ps.load(), g_unknown_vs.load(),
                g_unknown_ps.load(), g_manifest_stage_mismatch.load());
  }
#endif
}

bool RegisterShaderMicrocode(ShaderStage stage, uint32_t guest_address, const uint8_t* ucode,
                             uint32_t bytes, const uint8_t* preamble128) {
#if defined(NG2_PLUME_ON)
  if (!g_running.load(std::memory_order_relaxed) || !guest_address || !ucode || !bytes) return false;
  const uint64_t h = Fnv1a64(ucode, bytes);

  std::lock_guard<std::mutex> lock(g_shader_mutex);
  auto it = g_manifest.find(h);
  if (it == g_manifest.end()) {
    g_manifest_unknown.fetch_add(1, std::memory_order_relaxed);
    (stage == ShaderStage::kPixel ? g_unknown_ps : g_unknown_vs)
        .fetch_add(1, std::memory_order_relaxed);
    // DUMP IT, so the offline pipeline can translate it and the manifest can
    // grow. The alternative was an address-keyed fallback for the shaders that
    // have no container - but a second lookup path keyed differently from the
    // first is precisely the asymmetry this design exists to prevent, and it
    // would have to be kept in step by hand forever. Dumping keeps ONE key
    // (content) and moves the gap into the pipeline where it belongs.
    if (const char* dir = std::getenv("NG2_SHADER_DUMP")) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s/ucode_%016llX_%s.bin", dir,
                    static_cast<unsigned long long>(h),
                    stage == ShaderStage::kPixel ? "p" : "v");
      std::ofstream out(path, std::ios::binary);
      if (out) out.write(reinterpret_cast<const char*>(ucode), bytes);
      // The preamble beside it, so the offline synthesis has the literals.
      if (preamble128) {
        std::snprintf(path, sizeof(path), "%s/pre_%016llX_%s.bin", dir,
                      static_cast<unsigned long long>(h),
                      stage == ShaderStage::kPixel ? "p" : "v");
        std::ofstream pout(path, std::ios::binary);
        if (pout) pout.write(reinterpret_cast<const char*>(preamble128), 128);
      }
    }
    if (g_unknown_logged < 8) {
      ++g_unknown_logged;
      REXLOG_INFO("[ng2-plume] UNMATCHED {} at {:08X}: {} bytes, hash {:016X}, first dwords"
                  " {:08X} {:08X} {:08X}",
                  stage == ShaderStage::kPixel ? "PS" : "VS", guest_address, bytes, h,
                  (uint32_t(ucode[0]) << 24) | (uint32_t(ucode[1]) << 16) |
                      (uint32_t(ucode[2]) << 8) | ucode[3],
                  bytes > 7 ? (uint32_t(ucode[4]) << 24) | (uint32_t(ucode[5]) << 16) |
                                  (uint32_t(ucode[6]) << 8) | ucode[7] : 0u,
                  bytes > 11 ? (uint32_t(ucode[8]) << 24) | (uint32_t(ucode[9]) << 16) |
                                   (uint32_t(ucode[10]) << 8) | ucode[11] : 0u);
    }
    return false;
  }
  // THE STAGE MUST AGREE. A program that hashes to a pixel artefact while the
  // ring is binding it as a vertex shader means the match is wrong however
  // good the hash is, and serving it would be exactly the silent substitution
  // this design forbids. Counted, named, refused.
  const bool want_pixel = (stage == ShaderStage::kPixel);
  if (it->second.is_pixel != want_pixel) {
    g_manifest_stage_mismatch.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  auto& map = want_pixel ? g_pixel_shaders : g_vertex_shaders;
  auto& seen = map[guest_address];
  if (!seen.available) {
    g_manifest_hits.fetch_add(1, std::memory_order_relaxed);
    (want_pixel ? g_hit_ps : g_hit_vs).fetch_add(1, std::memory_order_relaxed);
  }
  seen.available = true;
  seen.dwords = bytes / 4;
  return true;
#else
  (void)stage; (void)guest_address; (void)ucode; (void)bytes; (void)preamble128;
  return false;
#endif
}

bool WantShader(ShaderStage stage, uint32_t guest_address, uint32_t dword_count,
                bool immediate) {
#if defined(NG2_PLUME_ON)
  if (!g_running.load(std::memory_order_relaxed)) return false;

  // An IMMEDIATE shader has no address to key on - the microcode came inline in
  // the packet. Counted as its own class rather than folded into a miss,
  // because "we have nowhere to cache this" and "we have not translated this
  // yet" are different problems with different fixes.
  if (immediate) {
    (stage == ShaderStage::kVertex ? g_vs_immediate : g_ps_immediate)
        .fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!guest_address) return false;

  // ONE BODY FOR BOTH STAGES. The map is selected here and used here, so a
  // lookup can never be tested against the other map's end() - constraint 2 -
  // and the vertex path cannot acquire a capability the pixel path lacks,
  // which is constraint 5.
  bool available = false;
  {
    std::lock_guard<std::mutex> lock(g_shader_mutex);
    auto& map = (stage == ShaderStage::kVertex) ? g_vertex_shaders : g_pixel_shaders;
    auto& seen = map[guest_address];
    seen.dwords = dword_count;
    seen.immediate = false;
    ++seen.draws;
    available = seen.available;
  }

  // A MISS IS COUNTED, NEVER SUBSTITUTED. Returning false means this draw
  // cannot be rendered natively yet, and the caller must skip it rather than
  // reach for a last-known-good program. Substituting is what turns a missing
  // translation path into a frame of correct geometry that is uniformly wrong.
  if (!available) {
    (stage == ShaderStage::kVertex ? g_vs_miss : g_ps_miss)
        .fetch_add(1, std::memory_order_relaxed);
  }
  return available;
#else
  (void)stage; (void)guest_address; (void)dword_count; (void)immediate;
  return false;
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
