// BACKEND TRANSPLANT - the replay driver. See native_gpu_backend.h.
#include "ng2_native_backend.h"

#include <d3d12.h>
#include <windows.h>

extern uint64_t g_ng2_ph[12];   // NG2 PATCH: [draw phases] (vendored command_processor.cpp)

#include <cstring>
#include <memory>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <string>

#include "rtc_d3d12/command_processor.h"
#include "rtc_d3d12/facade.h"
#include "rtc_d3d12/graphics_system_standin.h"

// Vendored accessor storage defined in rtc_d3d12/command_processor.cpp, texture_cache.cpp and flags.cpp (global
// scope). Read by PublishStatCvars - the kit's native_gpu_backend.cpp does the same (Fable 012fcf3).
int32_t& FLAGS_guest_fps_x10_storage_();
int32_t& FLAGS_gpu_frame_draws_storage_();
int32_t& FLAGS_gpu_frame_depth_draws_storage_();
int32_t& FLAGS_ng2_uw_mode_storage_();
double& FLAGS_ng2_fov_k_storage_();
int32_t& FLAGS_texture_pack_original_storage_();
int32_t& FLAGS_texture_pack_replaced_storage_();
int32_t& FLAGS_texture_warm_total_storage_();
int32_t& FLAGS_texture_warm_done_storage_();

namespace ng2::ngpu::backend {

using rex::graphics::RegisterFile;
using rex::graphics::ngpu_d3d12::D3D12CommandProcessor;
using rex::graphics::ngpu_d3d12::D3D12GraphicsSystem;
namespace xenos = rex::graphics::xenos;

// The friend the vendored D3D12CommandProcessor names (vendor_rtc_d3d12.py SOURCE_PATCHES): it performs what the
// PM4 parser (CommandProcessor::ExecutePacket*) would, from the bridge's records instead of the ring.
class Driver {
 public:
  bool Init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    auto& n = ::ng2::ngpu::rtc::Native();
    n.device = device;
    n.queue = queue;
    auto* ks = rex::system::kernel_state();
    regs_ = std::make_unique<RegisterFile>();
    gs_ = std::make_unique<D3D12GraphicsSystem>(ks->memory(), ks, regs_.get());
    cp_ = std::make_unique<D3D12CommandProcessor>(gs_.get(), ks);
    // CommandProcessor::Initialize would start the ring worker thread; the replay needs only its gamma defaults
    // (command_processor.cpp Initialize, verbatim) and then the D3D12 context.
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t value = i * 0x3FF / 0xFF;
      auto& e = cp_->gamma_ramp_256_entry_table_[i];
      e.color_10_blue = value; e.color_10_green = value; e.color_10_red = value;
    }
    for (uint32_t i = 0; i < 128; ++i) {
      rex::graphics::reg::DC_LUT_PWL_DATA e = {};
      e.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
      e.delta = i < 0x7F ? 0x200 : 0;
      for (uint32_t c = 0; c < 3; ++c) cp_->gamma_ramp_pwl_rgb_[i][c] = e;
    }
    if (!cp_->SetupContext()) {
      REXLOG_ERROR("[ngpu] BACKEND: the transplanted command processor's SetupContext FAILED");
      cp_.reset();
      return false;
    }
    REXLOG_INFO("[ngpu] BACKEND: transplanted D3D12 command processor ready (plume device, plume direct queue)");
    return true;
  }
  bool Ready() const { return cp_ != nullptr; }

  void WriteRegister(uint32_t index, uint32_t value) { cp_->WriteRegister(index, value); }

  rex::graphics::Shader* Load(xenos::ShaderType type, uint32_t addr, const uint32_t* code, uint32_t dwords,
                              const uint32_t*& last_code, uint32_t& last_dwords, rex::graphics::Shader*& last) {
    if (!code || !dwords) return nullptr;
    // No pointer cache: the pipeline cache hashes the microcode itself (as on every shader packet in the plugin); a
    // pointer into guest memory can hold different code later.
    ++stats_.shader_loads;
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    rex::graphics::Shader* s = cp_->LoadShader(type, addr, code, dwords);
    QueryPerformanceCounter(&q1);
    g_ng2_ph[9] += uint64_t(q1.QuadPart - q0.QuadPart);   // NG2 PATCH: [draw phases]
    if (!s) ++stats_.shader_load_failed;
    last_code = code; last_dwords = dwords; last = s;
    return s;
  }

  bool Draw(const DrawRecord& d) {
    // command_processor.cpp: the shader-setting packets set the active shaders; the draw packet writes
    // VGT_DRAW_INITIATOR (and VGT_DMA_BASE / VGT_DMA_SIZE for kDMA) and calls IssueDraw.
    cp_->active_vertex_shader_ = Load(xenos::ShaderType::kVertex, d.vs_addr, d.vs_code, d.vs_dwords, last_vs_code_, last_vs_dwords_, last_vs_);
    cp_->active_pixel_shader_ = Load(xenos::ShaderType::kPixel, d.ps_addr, d.ps_code, d.ps_dwords, last_ps_code_, last_ps_dwords_, last_ps_);
    rex::graphics::reg::VGT_DRAW_INITIATOR vdi;
    vdi.value = d.draw_initiator;
    cp_->WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, vdi.value);
    bool is_indexed = false;
    D3D12CommandProcessor::IndexBufferInfo ibi;
    if (vdi.source_select == xenos::SourceSelect::kDMA) {
      is_indexed = true;
      cp_->WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, d.index_addr);
      cp_->WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, d.index_size);
      rex::graphics::reg::VGT_DMA_SIZE ds;
      ds.value = d.index_size;
      const uint32_t isz = vdi.index_size == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
      ibi.guest_base = d.index_addr & ~(isz - 1);
      ibi.endianness = ds.swap_mode;
      ibi.format = vdi.index_size;
      ibi.length = ds.num_words * isz;
      ibi.count = vdi.num_indices;
    } else if (vdi.source_select == xenos::SourceSelect::kAutoIndex) {
      ibi.guest_base = 0;
      ibi.length = 0;
    } else {
      ++stats_.draw_failed;   // kImmediate is not supported by the plugin either
      return false;
    }
    auto viz = regs_->Get<rex::graphics::reg::PA_SC_VIZ_QUERY>();
    if (viz.viz_query_ena && viz.kill_pix_post_hi_z) return true;   // the plugin drops these too
    const bool explicit_major = xenos::IsMajorModeExplicit(vdi.major_mode, vdi.prim_type);
    ++stats_.draws;
    const bool ok = cp_->IssueDraw(vdi.prim_type, vdi.num_indices, is_indexed ? &ibi : nullptr, explicit_major);
    if (!ok) ++stats_.draw_failed;
    return ok;
  }

  void Swap(uint32_t fb, uint32_t w, uint32_t h, const uint32_t* fetch0, const uint32_t* table, const uint32_t* pwl) {
    if (fetch0)
      for (uint32_t i = 0; i < 6; ++i) cp_->WriteRegister(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i, fetch0[i]);
    if (table && std::memcmp(cp_->gamma_ramp_256_entry_table_, table, 256 * 4)) {
      std::memcpy(cp_->gamma_ramp_256_entry_table_, table, 256 * 4);
      cp_->gamma_ramp_256_entry_table_up_to_date_ = false;
    }
    if (pwl && std::memcmp(cp_->gamma_ramp_pwl_rgb_, pwl, 128 * 3 * 4)) {
      std::memcpy(cp_->gamma_ramp_pwl_rgb_, pwl, 128 * 3 * 4);
      cp_->gamma_ramp_pwl_up_to_date_ = false;
    }
    ++stats_.swaps;
    {
      LARGE_INTEGER q0, q1;
      QueryPerformanceCounter(&q0);
      cp_->IssueSwap(fb, w, h);
      QueryPerformanceCounter(&q1);
      g_ng2_ph[10] += uint64_t(q1.QuadPart - q0.QuadPart);   // NG2 PATCH: [draw phases]
    }
    PublishStatCvars();
    ::ng2::ngpu::rtc::NoteSwapSubmission(cp_->LastQueuedSubmission());   // [async submit] the frame waits for it
  }

  void EndFrameNoSwap() { cp_->EndSubmission(false); }
  void InitShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id) {
    if (cp_) cp_->InitializeShaderStorage(cache_root, title_id, true);   // blocking: pipelines exist before the draws
  }
  void ShutdownShaderStorage() {
    if (cp_ && cp_->pipeline_cache_) cp_->pipeline_cache_->ShutdownShaderStorage();
  }
  // PUBLISHED VALUES (ported from the kit's native_gpu_backend.cpp, Fable 012fcf3): settings the backend WRITES
  // (REXCVAR_SET in the vendored code) land in this module's accessor storage, not in the runtime registry other
  // modules read by name. Under offload the plugin no longer computes them, so at each swap the changed ones are
  // copied into the registry: the app's HUD (guest_fps_x10, gpu_frame_draws), texture warming / pack counters
  // (the F10 menu's "ON and in use" and the notify overlay's "N enhanced loaded" read texture_pack_replaced), and
  // the ultrawide presenter half (ng2_uw_mode / ng2_fov_k). In lockstep the plugin publishes its own. The DLL
  // route has the same code inside the DLL; this is the in-exe fallback's copy (2026-09-26 inexe1: registry 0
  // beside a backend count without it).
  void PublishStatCvars() {
    if (!::ng2::ngpu::rtc::NativeOwnsGuestMemory()) return;
    struct IntPub { const char* name; int32_t& (*get)(); int32_t last; };
    static IntPub ints[] = {
        {"guest_fps_x10", &FLAGS_guest_fps_x10_storage_, INT32_MIN},
        {"gpu_frame_draws", &FLAGS_gpu_frame_draws_storage_, INT32_MIN},
        {"gpu_frame_depth_draws", &FLAGS_gpu_frame_depth_draws_storage_, INT32_MIN},
        {"ng2_uw_mode", &FLAGS_ng2_uw_mode_storage_, INT32_MIN},
        {"texture_pack_original", &FLAGS_texture_pack_original_storage_, INT32_MIN},
        {"texture_pack_replaced", &FLAGS_texture_pack_replaced_storage_, INT32_MIN},
        {"texture_warm_total", &FLAGS_texture_warm_total_storage_, INT32_MIN},
        {"texture_warm_done", &FLAGS_texture_warm_done_storage_, INT32_MIN},
    };
    for (auto& p : ints) {
      const int32_t v = p.get();
      if (v == p.last) continue;
      p.last = v;
      rex::cvar::SetFlagByName(p.name, std::to_string(v));
    }
    static double last_k = -1.0;
    const double k = FLAGS_ng2_fov_k_storage_();
    if (k != last_k) {
      last_k = k;
      rex::cvar::SetFlagByName("ng2_fov_k", std::to_string(k));
    }
  }
  Readiness GetReadiness() {
    Readiness r;
    if (!cp_) return r;
    r.pipeline_not_ready_draws = cp_->draw_census_.pipeline_not_ready;
    r.creating_pipelines = cp_->pipeline_cache_ && cp_->pipeline_cache_->IsCreatingPipelines();
    return r;
  }

  Stats stats_;

 private:
  std::unique_ptr<RegisterFile> regs_;
  std::unique_ptr<D3D12GraphicsSystem> gs_;
  std::unique_ptr<D3D12CommandProcessor> cp_;
  const uint32_t* last_vs_code_ = nullptr; uint32_t last_vs_dwords_ = 0; rex::graphics::Shader* last_vs_ = nullptr;
  const uint32_t* last_ps_code_ = nullptr; uint32_t last_ps_dwords_ = 0; rex::graphics::Shader* last_ps_ = nullptr;
};

namespace {
Driver g_driver;
ID3D12Resource* g_output = nullptr;
uint32_t g_output_w = 0, g_output_h = 0;
bool g_output_is_8bpc = false;
}  // namespace

bool Init(ID3D12Device* device, ID3D12CommandQueue* queue) { return g_driver.Ready() || g_driver.Init(device, queue); }
bool Ready() { return g_driver.Ready(); }
void WriteRegister(uint32_t index, uint32_t value) { g_driver.WriteRegister(index, value); }
bool Draw(const DrawRecord& d) { return g_driver.Draw(d); }
void Swap(uint32_t fb, uint32_t w, uint32_t h, const uint32_t* fetch0, const uint32_t* table, const uint32_t* pwl) { g_driver.Swap(fb, w, h, fetch0, table, pwl); }
void EndFrameNoSwap() { g_driver.EndFrameNoSwap(); }
void InitShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id) { g_driver.InitShaderStorage(cache_root, title_id); }
void ShutdownShaderStorage() { g_driver.ShutdownShaderStorage(); }
ID3D12Resource* GuestOutput(uint32_t& w, uint32_t& h) { w = g_output_w; h = g_output_h; return g_output; }
bool GuestOutputIs8bpc() { return g_output_is_8bpc; }
Stats GetStats() { return g_driver.stats_; }
Readiness GetReadiness() { return g_driver.GetReadiness(); }

}  // namespace ng2::ngpu::backend

namespace ng2::ngpu::rtc {
// Stands in for the presenter's RefreshGuestOutput (presenter.cpp): a guest-output texture of the frontbuffer size in
// the presenter's format and state, handed to the plugin's refresher, which applies the gamma ramp / FXAA into it and
// submits. The native frame then samples it (backend::GuestOutput).
bool NativeRefreshGuestOutput(uint32_t frontbuffer_width, uint32_t frontbuffer_height, uint32_t, uint32_t,
                              const std::function<bool(::rex::ui::Presenter::GuestOutputRefreshContext& context)>& refresher) {
  using namespace ::ng2::ngpu::backend;
  if (!frontbuffer_width || !frontbuffer_height) return false;
  if (!g_output || g_output_w != frontbuffer_width || g_output_h != frontbuffer_height) {
    // The previous one may still be in flight in a native frame; the native frame waits for its fence every frame
    // and the command processor's submissions precede it on the same queue, so it is released after that.
    static ID3D12Resource* retired = nullptr;
    if (retired) retired->Release();
    retired = g_output;
    g_output = nullptr;
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = frontbuffer_width;
    rd.Height = frontbuffer_height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = ::rex::ui::ngpu_d3d12::D3D12Presenter::kGuestOutputFormat;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(Native().device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                        ::rex::ui::ngpu_d3d12::D3D12Presenter::kGuestOutputInternalState,
                                                        nullptr, IID_PPV_ARGS(&g_output)))) {
      g_output = nullptr;
      REXLOG_ERROR("[ngpu] BACKEND: could not create the {}x{} guest output", frontbuffer_width, frontbuffer_height);
      return false;
    }
    g_output_w = frontbuffer_width;
    g_output_h = frontbuffer_height;
  }
  bool is_8bpc = false;
  ::rex::ui::ngpu_d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext ctx(is_8bpc, g_output);
  const bool ok = refresher(ctx);
  g_output_is_8bpc = is_8bpc;   // ONE WINDOW: the plugin passes it to the runtime presenter with the copy
  return ok;
}
}  // namespace ng2::ngpu::rtc
