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
#include <set>
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
  // THE CONTENT HASH, BY VALUE, not a pointer to the bytes. The draw path
  // looks the DXIL up through g_dxil on every use, per constraint 1 - a cache
  // that can grow under a held pointer is how the sibling renderer got a
  // use-after-free reachable through five early returns.
  uint64_t hash = 0;
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

// THE TRANSLATED BYTES, keyed by the SAME content hash as the manifest.
//
// One key from microcode to DXIL, deliberately. The moment this is keyed by
// anything else - address, artefact name, stage - there are two lookups that
// can disagree, which is the failure this file's constraint 2 is about.
//
// Loaded lazily on first match and never evicted, so the map only grows; that
// is what lets the draw path hold a HASH rather than a pointer.
std::map<uint64_t, std::vector<uint8_t>> g_dxil;
// A MATCH WHOSE ARTEFACT WILL NOT LOAD IS NOT A MATCH. It is a third outcome,
// distinct from "unmatched", and it must never read as success: the manifest
// said a translation exists and the disk disagreed. Counted separately and
// named, because silently treating it as a miss would hide a broken build.
std::atomic<uint64_t> g_dxil_loaded{0};
std::atomic<uint64_t> g_dxil_failed{0};
std::atomic<uint64_t> g_dxil_bytes{0};
int g_dxil_fail_logged = 0;
std::atomic<uint64_t> g_manifest_hits{0};
// Immediate programs matched by content. Separate from the address-keyed hits
// because they are served by a different mechanism and their coverage can move
// independently - folding them together would hide one path failing while the
// other carried the total.
std::atomic<uint64_t> g_immediate_matched{0};
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
// UNKNOWN EVENTS AND UNKNOWN PROGRAMS ARE DIFFERENT QUESTIONS, and this project
// has already read one as the other once: 54,018 per-draw misses looked like a
// broken lookup when the truth was ten heavily-used programs. Immediate shaders
// make the gap far wider - the game reloads the same program thousands of times,
// so 13,611 unknown vertex EVENTS came from just 4 distinct programs. A count
// that cannot say which it is cannot locate the gap.
std::set<uint64_t> g_unknown_hashes;

// THE PAIR THE NEXT DRAW WILL USE. Xenos state is a current vertex shader and a
// current pixel shader, so the pipeline key is whatever was most recently made
// available for each stage. Touched only from the GPU worker thread - the
// callback's own thread - so no atomics, the same reasoning as g_last_serial.
uint64_t g_cur_vs_hash = 0;
uint64_t g_cur_ps_hash = 0;

uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

// THE RENDER-TARGET CACHE.
//
// Sixty-one distinct surfaces were measured in Chapter 1 gameplay, so this is a
// cache from the first line rather than a back buffer that grows one later.
//
// Created on the RENDER THREAD, requested from the GPU worker thread. The draw
// path only records what it wants; nothing here calls the device off its own
// thread, which is the kind of cross-thread resource creation that fails rarely
// and far from its cause.
struct RtKey {
  uint32_t pitch, msaa, color_base, color_format, depth_base, edram_mode;
  bool operator<(const RtKey& o) const {
    if (pitch != o.pitch) return pitch < o.pitch;
    if (msaa != o.msaa) return msaa < o.msaa;
    if (color_base != o.color_base) return color_base < o.color_base;
    if (color_format != o.color_format) return color_format < o.color_format;
    if (depth_base != o.depth_base) return depth_base < o.depth_base;
    return edram_mode < o.edram_mode;
  }
};

struct RtEntry {
  uint32_t width = 0, height = 0;
  std::unique_ptr<RenderTexture> texture;
  bool created = false;
  bool failed = false;
  bool unmapped_format = false;
  bool approximated = false;
};

std::mutex g_rt_mutex;
std::map<RtKey, RtEntry> g_rt;
std::atomic<uint64_t> g_rt_created{0};
std::atomic<uint64_t> g_rt_failed{0};
std::atomic<uint64_t> g_rt_unmapped{0};
std::atomic<uint64_t> g_rt_approx{0};
int g_rt_unmapped_logged = 0;

// XENOS COLOUR FORMAT -> HOST FORMAT, with the approximations NAMED.
//
// Three outcomes, not two. An EXACT mapping is a mapping; an APPROXIMATION is a
// different format that happens to hold the values, and a renderer that
// silently approximates produces a plausible picture built from the wrong
// precision - the same shape as the silent shader substitution constraint 5 is
// about. k_2_10_10_10_FLOAT is Xenos 7e3, which has no host equivalent at all;
// k_16_16 and k_16_16_16_16 are FIXED point -32..32 stored in float targets
// here. Those are counted separately so the number is never mistaken for
// correctness.
RenderFormat MapColorFormat(uint32_t xenos_format, bool* approximated, bool* unmapped) {
  *approximated = false;
  *unmapped = false;
  switch (xenos_format) {
    case 0:   // k_8_8_8_8
      return RenderFormat::R8G8B8A8_UNORM;
    case 1:   // k_8_8_8_8_GAMMA - the curve belongs in the shader, not the format
      *approximated = true;
      return RenderFormat::R8G8B8A8_UNORM;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return RenderFormat::R10G10B10A2_UNORM;
    case 3:   // k_2_10_10_10_FLOAT - Xenos 7e3, no host equivalent
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
      *approximated = true;
      return RenderFormat::R16G16B16A16_FLOAT;
    case 4:   // k_16_16 - fixed point -32..32
      *approximated = true;
      return RenderFormat::R16G16_FLOAT;
    case 5:   // k_16_16_16_16 - fixed point -32..32
      *approximated = true;
      return RenderFormat::R16G16B16A16_FLOAT;
    case 6:   // k_16_16_FLOAT
      return RenderFormat::R16G16_FLOAT;
    case 7:   // k_16_16_16_16_FLOAT
      return RenderFormat::R16G16B16A16_FLOAT;
    case 14:  // k_32_FLOAT
      return RenderFormat::R32_FLOAT;
    case 15:  // k_32_32_FLOAT
      return RenderFormat::R32G32_FLOAT;
    default:
      // NEVER DEFAULTED TO RGBA8. An unknown format that quietly becomes the
      // commonest one renders something, which is worse than rendering nothing.
      *unmapped = true;
      return RenderFormat::UNKNOWN;
  }
}

// THE PIPELINE LAYOUT, matching the bindless model XenosRecomp emits EXACTLY.
//
// Measured from the translated HLSL rather than guessed (constraint 9):
//     Texture2D    g_Texture2DDescriptorHeap[]   t0, space0
//     Texture3D    g_Texture3DDescriptorHeap[]   t0, space1
//     TextureCube  g_TextureCubeDescriptorHeap[] t0, space2
//     SamplerState g_SamplerDescriptorHeap[]     s0, space3
//     StructuredBuffer<uint> g_VertexStreamHeap[] t0, space4
//     cbuffer VertexShaderConstants              b0, space4
//     cbuffer SharedConstants                    b2, space4
//
// Plume maps descriptor SET INDEX to D3D12 REGISTER SPACE one to one
// (plume_d3d12.cpp: descriptorRange.RegisterSpace = i), so the sets are built
// in space order and the order is load-bearing.
//
// NO PUSH CONSTANTS. The push-constant path in the generated HLSL is inside
// `#ifdef __spirv__`; these artefacts are DXIL, so the `#else` branch is live
// and every constant arrives through the cbuffers above. Declaring a push
// constant range anyway would not fail loudly - it would simply describe a
// pipeline the shader never asked for.
std::unique_ptr<RenderPipelineLayout> g_pipeline_layout;
bool g_layout_failed = false;

constexpr uint32_t kBoundless = 1024;  // upper bound for the variable-sized arrays

bool EnsurePipelineLayout() {
  if (g_pipeline_layout) return true;
  if (g_layout_failed) return false;

  const RenderDescriptorRange tex2d(RenderDescriptorRangeType::TEXTURE, 0, kBoundless);
  const RenderDescriptorRange tex3d(RenderDescriptorRangeType::TEXTURE, 0, kBoundless);
  const RenderDescriptorRange texcube(RenderDescriptorRangeType::TEXTURE, 0, kBoundless);
  const RenderDescriptorRange samplers(RenderDescriptorRangeType::SAMPLER, 0, kBoundless);
  // space4 holds three things, and the BOUNDLESS one must be last: Plume marks
  // only the final range of a set as variable-sized.
  // b0 VertexShaderConstants, b1 PixelShaderConstants, b2 SharedConstants.
  //
  // b1 WAS MISSING and nothing at runtime noticed. I derived this layout by
  // reading one VERTEX shader's HLSL, which declares only b0 and b2 - pixel
  // shaders declare b1 instead of b0, and 169 of the 634 artefacts bind it.
  // Generalising a layout from one population again, which is the third time
  // on this project. Found by check_root_signature.py against what the DXIL
  // actually declares, never by a run: the debug layer is off in Release, so
  // 121 pipelines were built against a root signature that could not satisfy
  // 169 of their shaders.
  const RenderDescriptorRange cb0(RenderDescriptorRangeType::CONSTANT_BUFFER, 0, 1);
  const RenderDescriptorRange cb1(RenderDescriptorRangeType::CONSTANT_BUFFER, 1, 1);
  const RenderDescriptorRange cb2(RenderDescriptorRangeType::CONSTANT_BUFFER, 2, 1);
  const RenderDescriptorRange streams(RenderDescriptorRangeType::STRUCTURED_BUFFER, 0, kBoundless);
  const RenderDescriptorRange space4[] = {cb0, cb1, cb2, streams};

  const RenderDescriptorSetDesc sets[] = {
      RenderDescriptorSetDesc(&tex2d, 1, true, kBoundless),
      RenderDescriptorSetDesc(&tex3d, 1, true, kBoundless),
      RenderDescriptorSetDesc(&texcube, 1, true, kBoundless),
      RenderDescriptorSetDesc(&samplers, 1, true, kBoundless),
      RenderDescriptorSetDesc(space4, 4, true, kBoundless),
  };

  RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = sets;
  desc.descriptorSetDescsCount = 5;
  // allowInputLayout: 320 of the 406 vertex shaders still DECLARE
  // input-assembler inputs even though all of them read through the stream
  // heap, so the layout has to permit an input layout to exist.
  desc.allowInputLayout = true;
  g_pipeline_layout = g_gpu.device->createPipelineLayout(desc);
  if (!g_pipeline_layout) {
    g_layout_failed = true;
    REXLOG_INFO("[ng2-plume] pipeline layout creation FAILED - no pipeline can be built");
    return false;
  }
  REXLOG_INFO("[ng2-plume] pipeline layout created: 5 bindless sets, spaces 0..4, no push constants");
  return true;
}

// THE PIPELINE CACHE, keyed by the two shader hashes AND the target format.
//
// The format is part of the key because a pipeline is compiled against its
// render target's format; the same shader pair drawn into an RGBA8 target and a
// float target are two different pipelines, and keying on the pair alone would
// silently return one for the other.
struct PsoKey {
  uint64_t vs, ps;
  uint32_t format;
  bool operator<(const PsoKey& o) const {
    if (vs != o.vs) return vs < o.vs;
    if (ps != o.ps) return ps < o.ps;
    return format < o.format;
  }
};

struct PsoEntry {
  std::unique_ptr<RenderShader> vs_module, ps_module;
  std::unique_ptr<RenderPipeline> pipeline;
  bool created = false;
  bool failed = false;
};

std::mutex g_pso_mutex;
std::map<PsoKey, PsoEntry> g_pso;
std::atomic<uint64_t> g_pso_created{0};
std::atomic<uint64_t> g_pso_failed{0};
std::atomic<uint64_t> g_pso_no_dxil{0};
int g_pso_fail_logged = 0;

// Called on the render thread. Two-phase like the render targets: the device is
// never touched while the cache mutex is held, because the draw path takes that
// same mutex and pipeline compilation is not a fast call.
void CreatePendingPipelines() {
  std::vector<PsoKey> pending;
  {
    std::lock_guard<std::mutex> lock(g_pso_mutex);
    for (auto& kv : g_pso) {
      if (!kv.second.created && !kv.second.failed) pending.push_back(kv.first);
    }
  }
  if (pending.empty()) return;
  if (!EnsurePipelineLayout()) return;

  for (const PsoKey& key : pending) {
    // Copy the DXIL out under the shader lock rather than holding a pointer
    // into g_dxil across a device call - constraint 1, one level down.
    std::vector<uint8_t> vs_bytes, ps_bytes;
    {
      std::lock_guard<std::mutex> lock(g_shader_mutex);
      auto v = g_dxil.find(key.vs);
      auto p = g_dxil.find(key.ps);
      if (v != g_dxil.end()) vs_bytes = v->second;
      if (p != g_dxil.end()) ps_bytes = p->second;
    }
    if (vs_bytes.empty() || ps_bytes.empty()) {
      std::lock_guard<std::mutex> lock(g_pso_mutex);
      auto it = g_pso.find(key);
      if (it != g_pso.end() && !it->second.created) {
        it->second.failed = true;
        g_pso_no_dxil.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }

    auto vs_mod = g_gpu.device->createShader(vs_bytes.data(), vs_bytes.size(), "main",
                                             RenderShaderFormat::DXIL);
    auto ps_mod = g_gpu.device->createShader(ps_bytes.data(), ps_bytes.size(), "main",
                                             RenderShaderFormat::DXIL);
    std::unique_ptr<RenderPipeline> pipe;
    if (vs_mod && ps_mod) {
      RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = g_pipeline_layout.get();
      desc.vertexShader = vs_mod.get();
      desc.pixelShader = ps_mod.get();
      desc.renderTargetCount = 1;
      desc.renderTargetFormat[0] = static_cast<RenderFormat>(key.format);
      desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
      desc.cullMode = RenderCullMode::NONE;
      pipe = g_gpu.device->createGraphicsPipeline(desc);
    }

    std::lock_guard<std::mutex> lock(g_pso_mutex);
    auto it = g_pso.find(key);
    if (it == g_pso.end() || it->second.created || it->second.failed) continue;
    if (!pipe) {
      it->second.failed = true;
      g_pso_failed.fetch_add(1, std::memory_order_relaxed);
      if (g_pso_fail_logged < 8) {
        ++g_pso_fail_logged;
        REXLOG_INFO("[ng2-plume] PIPELINE FAILED vs {:016X} ps {:016X} fmt {} -"
                    " root signature and shader bindings disagree, or the DXIL is bad",
                    key.vs, key.ps, key.format);
      }
      continue;
    }
    it->second.vs_module = std::move(vs_mod);
    it->second.ps_module = std::move(ps_mod);
    it->second.pipeline = std::move(pipe);
    it->second.created = true;
    g_pso_created.fetch_add(1, std::memory_order_relaxed);
  }
}

size_t PsoWanted() {
  std::lock_guard<std::mutex> lock(g_pso_mutex);
  return g_pso.size();
}

// Called on the render thread, once per frame, for whatever the draw path asked
// for since the last one.
void CreatePendingRenderTargets() {
  // TWO PHASE, so the device is never called with the lock held. The draw
  // callback takes this same mutex, and createTexture is not a fast call:
  // holding it across creation would stall the GPU worker thread on the frame
  // sixty-one targets first appear. Collect what needs making, release, make
  // it, then re-acquire to store.
  std::vector<std::pair<RtKey, std::pair<uint32_t, uint32_t>>> pending;
  {
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    for (auto& kv : g_rt) {
      if (kv.second.created || kv.second.failed) continue;
      pending.emplace_back(kv.first, std::make_pair(kv.second.width, kv.second.height));
    }
  }
  if (pending.empty()) return;

  for (auto& p : pending) {
    const RtKey& key = p.first;
    const uint32_t want_w = p.second.first, want_h = p.second.second;
    bool approx = false, unmapped = false;
    const RenderFormat fmt = MapColorFormat(key.color_format, &approx, &unmapped);
    std::unique_ptr<RenderTexture> tex;
    if (!unmapped && want_w && want_h) {
      tex = g_gpu.device->createTexture(RenderTextureDesc::ColorTarget(want_w, want_h, fmt));
    }
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    auto it = g_rt.find(key);
    if (it == g_rt.end()) continue;
    RtEntry& e = it->second;
    if (e.created || e.failed) continue;
    if (unmapped) {
      e.failed = true;
      e.unmapped_format = true;
      g_rt_unmapped.fetch_add(1, std::memory_order_relaxed);
      if (g_rt_unmapped_logged < 8) {
        ++g_rt_unmapped_logged;
        REXLOG_INFO("[ng2-plume] UNMAPPED colour format {} (pitch {} base {}) - no host target,"
                    " and NOT substituted",
                    key.color_format, key.pitch, key.color_base);
      }
      continue;
    }
    if (!tex) {
      e.failed = true;
      g_rt_failed.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    e.texture = std::move(tex);
    e.created = true;
    e.approximated = approx;
    if (approx) g_rt_approx.fetch_add(1, std::memory_order_relaxed);
    g_rt_created.fetch_add(1, std::memory_order_relaxed);
  }
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
  CreatePendingRenderTargets();
  CreatePendingPipelines();
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

size_t UnknownDistinct() {
  std::lock_guard<std::mutex> lock(g_shader_mutex);
  return g_unknown_hashes.size();
}

size_t RtWanted() {
  std::lock_guard<std::mutex> lock(g_rt_mutex);
  return g_rt.size();
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
                " | stage-mismatch {} | DXIL loaded {} ({} KB), UNLOADABLE {}"
                " | RT wanted {} created {} failed {} UNMAPPED-FORMAT {} approximated {}"
                " | immediate matched {} | unknown DISTINCT programs {}"
                " | PSO wanted {} created {} failed {} no-dxil {}",
                g_manifest.size(), g_hit_vs.load(), g_hit_ps.load(), g_unknown_vs.load(),
                g_unknown_ps.load(), g_manifest_stage_mismatch.load(), g_dxil_loaded.load(),
                g_dxil_bytes.load() / 1024, g_dxil_failed.load(),
                RtWanted(), g_rt_created.load(), g_rt_failed.load(),
                g_rt_unmapped.load(), g_rt_approx.load(),
                g_immediate_matched.load(), UnknownDistinct(),
                PsoWanted(), g_pso_created.load(), g_pso_failed.load(),
                g_pso_no_dxil.load());
  }
#endif
}

bool RegisterShaderMicrocode(ShaderStage stage, uint32_t guest_address, const uint8_t* ucode,
                             uint32_t bytes, const uint8_t* preamble128) {
#if defined(NG2_PLUME_ON)
  // GUEST ADDRESS 0 MEANS "IMMEDIATE": the program came inline in the packet
  // and has no address at all. It is registered by CONTENT only - the address
  // map is skipped - because the record's address field is stale rather than
  // empty for those draws (constraint 8), and filing this program under it
  // would give one shader another's identity.
  if (!g_running.load(std::memory_order_relaxed) || !ucode || !bytes) return false;
  const uint64_t h = Fnv1a64(ucode, bytes);

  std::lock_guard<std::mutex> lock(g_shader_mutex);
  auto it = g_manifest.find(h);
  if (it == g_manifest.end()) {
    g_manifest_unknown.fetch_add(1, std::memory_order_relaxed);
    (stage == ShaderStage::kPixel ? g_unknown_ps : g_unknown_vs)
        .fetch_add(1, std::memory_order_relaxed);
    g_unknown_hashes.insert(h);
    // DUMP IT, so the offline pipeline can translate it and the manifest can
    // grow. The alternative was an address-keyed fallback for the shaders that
    // have no container - but a second lookup path keyed differently from the
    // first is precisely the asymmetry this design exists to prevent, and it
    // would have to be kept in step by hand forever. Dumping keeps ONE key
    // (content) and moves the gap into the pipeline where it belongs.
    // ONCE PER PROGRAM, AND THE ENVIRONMENT READ ONCE FOR THE PROCESS.
    //
    // This branch used to call getenv and open a file on EVERY unmatched
    // registration. That was tolerable while unmatched meant ten programs; it
    // stopped being tolerable the moment immediate shaders were registered too,
    // because immediate reloads constantly - this run counted 366,103 unmatched
    // vertex events and 260,078 pixel. That is 626,181 getenv calls and 626,181
    // file opens on the GPU worker thread, rewriting the same handful of files
    // over and over.
    //
    // The sibling project lost three experiments to exactly this shape: a
    // string cvar read per draw, constructing and destroying a std::string
    // every time, which slowed the guest enough that it never reached the
    // world. Nothing in the call's spelling says it is expensive.
    //
    // Both statics are written only under g_shader_mutex, which this function
    // already holds.
    static const char* const dump_dir = std::getenv("NG2_SHADER_DUMP");
    static std::set<uint64_t> dumped;
    if (dump_dir && dumped.insert(h).second) {
      const char* dir = dump_dir;
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
  // LOAD THE TRANSLATED BYTES ONCE, on first match for this program. Until
  // this existed the manifest only ever proved a NAME was known, which is not
  // the same as having something to bind - "matched" was a claim about a text
  // file.
  auto dx = g_dxil.find(h);
  if (dx == g_dxil.end()) {
    std::ifstream df(it->second.artefact, std::ios::binary | std::ios::ate);
    bool ok = false;
    std::vector<uint8_t> bytes_in;
    if (df) {
      const std::streamoff n = df.tellg();
      if (n > 0) {
        bytes_in.resize(static_cast<size_t>(n));
        df.seekg(0);
        ok = static_cast<bool>(df.read(reinterpret_cast<char*>(bytes_in.data()), n));
      }
    }
    if (!ok) {
      g_dxil_failed.fetch_add(1, std::memory_order_relaxed);
      if (g_dxil_fail_logged < 8) {
        ++g_dxil_fail_logged;
        REXLOG_INFO("[ng2-plume] MATCHED BUT UNLOADABLE {} {:016X}: cannot read {}",
                    want_pixel ? "PS" : "VS", h, it->second.artefact);
      }
      return false;
    }
    g_dxil_bytes.fetch_add(bytes_in.size(), std::memory_order_relaxed);
    g_dxil_loaded.fetch_add(1, std::memory_order_relaxed);
    dx = g_dxil.emplace(h, std::move(bytes_in)).first;
  }

  // An immediate program is counted as a hit but NOT filed by address - there
  // is no address it could honestly be filed under.
  if (!guest_address) {
    g_manifest_hits.fetch_add(1, std::memory_order_relaxed);
    (want_pixel ? g_hit_ps : g_hit_vs).fetch_add(1, std::memory_order_relaxed);
    g_immediate_matched.fetch_add(1, std::memory_order_relaxed);
    (want_pixel ? g_cur_ps_hash : g_cur_vs_hash) = h;
    return true;
  }

  auto& map = want_pixel ? g_pixel_shaders : g_vertex_shaders;
  auto& seen = map[guest_address];
  if (!seen.available) {
    g_manifest_hits.fetch_add(1, std::memory_order_relaxed);
    (want_pixel ? g_hit_ps : g_hit_vs).fetch_add(1, std::memory_order_relaxed);
  }
  seen.available = true;
  seen.dwords = bytes / 4;
  seen.hash = h;
  (want_pixel ? g_cur_ps_hash : g_cur_vs_hash) = h;
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
    if (available) {
      (stage == ShaderStage::kVertex ? g_cur_vs_hash : g_cur_ps_hash) = seen.hash;
    }
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

bool WantPipeline(uint32_t xenos_color_format) {
#if defined(NG2_PLUME_ON)
  if (!g_running.load(std::memory_order_relaxed)) return false;
  // Both stages must be available before a pipeline means anything. A draw
  // whose vertex program is known and whose pixel program is not is exactly the
  // half-bound state constraint 5 forbids, and building a pipeline for it would
  // pair a real shader with a stale one.
  if (!g_cur_vs_hash || !g_cur_ps_hash) return false;

  bool approx = false, unmapped = false;
  const RenderFormat fmt = MapColorFormat(xenos_color_format, &approx, &unmapped);
  if (unmapped) return false;

  // ONLY WHEN THE TRIPLE CHANGES. This is called per draw, and taking a mutex
  // and hashing a map key eight million times a run is the per-draw cost that
  // has already bitten this project once. The comparison is three scalars on
  // the callback's own thread.
  static uint64_t last_vs = 0, last_ps = 0;
  static uint32_t last_fmt = 0xFFFFFFFFu;
  const uint32_t fmt_key = static_cast<uint32_t>(fmt);
  if (g_cur_vs_hash == last_vs && g_cur_ps_hash == last_ps && fmt_key == last_fmt) {
    return true;
  }
  last_vs = g_cur_vs_hash;
  last_ps = g_cur_ps_hash;
  last_fmt = fmt_key;

  const PsoKey key{g_cur_vs_hash, g_cur_ps_hash, fmt_key};
  std::lock_guard<std::mutex> lock(g_pso_mutex);
  auto it = g_pso.find(key);
  if (it == g_pso.end()) {
    g_pso[key];  // requested; the render thread builds it
    return false;
  }
  return it->second.created;
#else
  (void)xenos_color_format;
  return false;
#endif
}

bool WantRenderTarget(const SurfaceDesc& desc) {
#if defined(NG2_PLUME_ON)
  if (!g_running.load(std::memory_order_relaxed)) return false;
  const RtKey k{desc.pitch, desc.msaa, desc.color_base,
                desc.color_format, desc.depth_base, desc.edram_mode};
  std::lock_guard<std::mutex> lock(g_rt_mutex);
  RtEntry& e = g_rt[k];
  // The widest scissor seen wins, because a target must be at least as large as
  // anything drawn into it. This is a DERIVATION - a Xenos EDRAM target does not
  // carry its own height - and it is reported as provisional rather than read.
  if (desc.width > e.width) e.width = desc.width;
  if (desc.height > e.height) e.height = desc.height;
  return e.created;
#else
  (void)desc;
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
