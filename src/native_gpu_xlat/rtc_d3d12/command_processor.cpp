// VENDORED from rexglue-src 23ace0b:src/graphics/d3d12/command_processor.cpp - systematic renames only (see vendor_rtc_d3d12.py / ORIGIN.txt):
// namespaces d3d12 -> ngpu_d3d12, plugin headers -> rtc_d3d12/facade.h, cvars -> plugin registry reads (15 bool, 1 string, 15 int).
#include <string>
#include <cstdint>
#include <rex/logging.h>
namespace ng2::ngpu::xlat { bool PluginBool(const char*, bool); std::string PluginString(const char*, const char*); int32_t PluginInt(const char*, int32_t); double PluginDouble(const char*, double); }
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <string>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <chrono>
#include <unordered_set>
#include <algorithm>
#include <map>
#include <cstdarg>
#include <cstdlib>
#include <set>
#include <cstring>
#include <sstream>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>

// Published for the app's on-screen counter: the game's OWN frame rate, from
// its swaps, over the last second, times ten. Read with
// REXCVAR_QUERY(int32_t, guest_fps_x10); 0 until the first second of swaps.
// [scene] Draw counts of the last presented guest frame, for the app's HUD
// overlay: a frame that drew the 3D world has hundreds of depth-tested
// draws, a frame of 2D menus only has none.
int32_t& FLAGS_gpu_frame_draws_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("gpu_frame_draws", 0); return s; }
int32_t& FLAGS_gpu_frame_depth_draws_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("gpu_frame_depth_draws", 0); return s; }
int32_t& FLAGS_guest_fps_x10_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("guest_fps_x10", 0); return s; }
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include "rtc_d3d12/command_processor.h"
#include "rtc_d3d12/graphics_system_standin.h"
#include "rtc_d3d12/shader.h"
#include <rex/graphics/flags.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xthread.h>
#include <condition_variable>
#include <memory>
#include "rtc_d3d12/graphics_system_standin.h"
#include "rtc_d3d12/d3d12_util.h"

bool& FLAGS_d3d12_bindless_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("d3d12_bindless", true); return s; }

bool& FLAGS_d3d12_readback_memexport_storage_() { static bool s = ::ng2::ngpu::rtc::NativeOwnsGuestMemory() ? ::ng2::ngpu::xlat::PluginBool("d3d12_readback_memexport", false) : false; return s; }   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)

bool& FLAGS_d3d12_readback_resolve_storage_() { static bool s = ::ng2::ngpu::rtc::NativeOwnsGuestMemory() ? ::ng2::ngpu::xlat::PluginBool("d3d12_readback_resolve", false) : false; return s; }   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)

// A resolve at an address the readback has not seen waits for the whole GPU
// queue before its copy (readback_resolve fast/some). Fable II's lake and its
// tree impostors resolve to about a thousand new addresses a second, and
// those drains took 2.4 s of every 5 s (33 fps, 2026-09-13). This caps the
// synchronous ones per frame; the rest are copied when their submission
// completes, a frame or two later.
// Off: protecting and releasing three guest views per deferred resolve cost
// two thirds of the frame rate in Bowerstone Market (21 vs 58-60 fps), and
// the game's CPU touched none of those renders in four minutes there.
bool& FLAGS_readback_land_before_texture_upload_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("readback_land_before_texture_upload", true); return s; }
static std::atomic<uint32_t> g_impostor_readbacks_landed{0};
// [readback] Texture uploads that waited for a prior submission's readback, and
// uploads that found a readback still in the OPEN submission (not awaitable here).
static std::atomic<uint32_t> g_readbacks_awaited_before_upload{0};
static std::atomic<uint64_t> g_readbacks_awaited_us{0};
static std::atomic<uint32_t> g_readbacks_open_at_upload{0};
static std::atomic<uint32_t> g_readbacks_valid_no_wait{0};
// [cost] Readback landings (CopyToGuestMemory) and submissions per window.
static std::atomic<uint32_t> g_landing_count{0};
static std::atomic<uint32_t> g_splits_before_load{0};
static std::atomic<uint32_t> g_mirror_copies{0};
static std::atomic<uint32_t> g_superseded_kept{0};
static std::atomic<uint32_t> g_superseded_waited{0};
static std::atomic<uint32_t> g_ring_grown{0};
static std::atomic<uint32_t> g_upload_landed{0};
static std::atomic<uint32_t> g_upload_awaited{0};
static std::atomic<uint32_t> g_upload_open{0};
static std::atomic<uint32_t> g_superseded_dropped{0};
static std::atomic<uint64_t> g_landing_us{0};
static std::atomic<uint64_t> g_landing_lock_us{0};
static std::atomic<uint32_t> g_submission_count{0};
static std::atomic<uint64_t> g_submission_us{0};
bool& FLAGS_readback_resolve_on_demand_storage_() { static bool s = ::ng2::ngpu::rtc::NativeOwnsGuestMemory() ? ::ng2::ngpu::xlat::PluginBool("readback_resolve_on_demand", false) : false; return s; }   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)
// An experiment for the impostor flashes that only a GPU drain seems to cure.
int32_t& FLAGS_readback_resolve_drain_small_kb_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("readback_resolve_drain_small_kb", 0); return s; }
int32_t& FLAGS_readback_resolve_wait_only_kb_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("readback_resolve_wait_only_kb", 0); return s; }
int32_t& FLAGS_readback_resolve_drain_max_kb_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("readback_resolve_drain_max_kb", 0); return s; }
int32_t& FLAGS_readback_resolve_drain_large_kb_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("readback_resolve_drain_large_kb", 0); return s; }
bool& FLAGS_readback_resolve_keep_superseded_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("readback_resolve_keep_superseded", true); return s; }
bool& FLAGS_readback_resolve_mirror_unscaled_storage_() { static bool s = ::ng2::ngpu::rtc::NativeOwnsGuestMemory() ? ::ng2::ngpu::xlat::PluginBool("readback_resolve_mirror_unscaled", false) : false; return s; }   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)
bool& FLAGS_readback_resolve_split_before_load_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("readback_resolve_split_before_load", false); return s; }
int32_t& FLAGS_readback_resolve_submit_small_kb_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("readback_resolve_submit_small_kb", 0); return s; }
int32_t& FLAGS_readback_resolve_rebind_small_kb_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("readback_resolve_rebind_small_kb", 0); return s; }
static std::atomic<uint32_t> g_resolve_splits{0};
// [readback] Size census of the deferred resolves: <=64K, <=256K, <=1M,
// <=4M, >4M guest bytes.
static std::atomic<uint32_t> g_resolve_size_buckets[5];
static std::atomic<uint64_t> g_upload_bytes_window{0};  // [perf] uploads per fence-line window
static std::atomic<uint32_t> g_upload_copies_window{0};  // [perf] upload copies per window
extern std::atomic<uint32_t> g_dcl_command_counts[64];
extern std::atomic<uint32_t> g_dcl_command_total;
bool& FLAGS_readback_resolve_uav_barrier_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("readback_resolve_uav_barrier", false); return s; }
int32_t& FLAGS_fable2_menu_letterbox_gap_ms_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("fable2_menu_letterbox_gap_ms", 150); return s; }
double& FLAGS_fable2_uw_2d_k_storage_() { static double s = ::ng2::ngpu::xlat::PluginDouble("fable2_uw_2d_k", 0.0); return s; }
int32_t& FLAGS_gpu_draw_dump_frames_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("gpu_draw_dump_frames", 0); return s; }
std::string& FLAGS_gpu_draw_dump_file_storage_() { static std::string s = ::ng2::ngpu::xlat::PluginString("gpu_draw_dump_file", ""); return s; }
bool& FLAGS_readback_await_before_texture_upload_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("readback_await_before_texture_upload", true); return s; }
int32_t& FLAGS_shared_memory_upload_spin_ns_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("shared_memory_upload_spin_ns", 0); return s; }
int32_t& FLAGS_shared_memory_upload_touch_bytes_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("shared_memory_upload_touch_bytes", 0); return s; }
bool& FLAGS_readback_fast_path_gpu_written_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("readback_fast_path_gpu_written", false); return s; }
bool& FLAGS_shared_memory_upload_reach_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("shared_memory_upload_reach", false); return s; }
// NATIVE PATCH (2026-09-26): on in the native backend unless ngpu_backend_upload_skip=false (D4: 57.95 -> 59.78 fps,
// spread 4.5 -> 0.6, with the XXH3 page hash); the plugin setting can still force it on.
namespace ng2::ngpu::rtc { bool NgpuBackendUploadSkipCvar(); }
bool& FLAGS_shared_memory_upload_skip_unchanged_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("shared_memory_upload_skip_unchanged", false) || ::ng2::ngpu::rtc::NgpuBackendUploadSkipCvar(); return s; }
bool& FLAGS_shared_memory_upload_churn_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("shared_memory_upload_churn", false); return s; }
int32_t& FLAGS_fable2_2d_census_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("fable2_2d_census", 0); return s; }
int32_t& FLAGS_readback_resolve_sync_budget_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("readback_resolve_sync_budget", 8); return s; }

bool& FLAGS_d3d12_submit_on_primary_buffer_end_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("d3d12_submit_on_primary_buffer_end", true); return s; }

extern "C" void RexNgpuNotifyResolve();

namespace rex::graphics { extern uint64_t g_prof_tex_outdated_by_gpu, g_prof_tex_outdated_by_cpu; }   // texture_cache_base.cpp
namespace rex::graphics::ngpu_d3d12 {

// [ng2-fov] Per-frame counts of 3D world (perspective) draws and 2D screen-space
// UI (c21-c24 ortho) draws, used by IssueSwap's scene detector. Gameplay draws a
// lot of 3D and little 2D (just the HUD); a full-screen menu draws little/no 3D;
// an overlay menu (weapons/pause, which keeps rendering the world behind it)
// draws a lot of BOTH, so a high 2D count is what flags it as a menu. Both the
// draw setup and IssueSwap run on the GPU command-processor thread, so plain
// file statics are safe; the presenter (a different module) learns the result
// through the ng2_uw_mode cvar.
static int g_ng2_persp_this_frame = 0;
static int g_ng2_2d_this_frame = 0;
// [ng2-fade] DIAGNOSTIC: 2D UI draws this frame whose pixel shader samples no
// texture (a solid fill - the shape NG2's full-screen fade-to-black takes).
// Logged to gauge whether the fade is separable from the (textured) HUD.
static int g_ng2_2d_solid_this_frame = 0;

// Generated with `xb buildshaders`.
namespace shaders {
#include "rtc_d3d12/bytecode/apply_gamma_pwl_cs.h"
#include "rtc_d3d12/bytecode/apply_gamma_pwl_fxaa_luma_cs.h"
#include "rtc_d3d12/bytecode/apply_gamma_table_cs.h"
#include "rtc_d3d12/bytecode/apply_gamma_table_fxaa_luma_cs.h"
#include "rtc_d3d12/bytecode/fxaa_cs.h"
#include "rtc_d3d12/bytecode/fxaa_extreme_cs.h"
#include "rtc_d3d12/bytecode/resolve_downscale_cs.h"
}  // namespace shaders

D3D12CommandProcessor::D3D12CommandProcessor(D3D12GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state), deferred_command_list_(*this) {
  legacy_readback_memexport_cvar_name_ = "d3d12_readback_memexport";
}
D3D12CommandProcessor::~D3D12CommandProcessor() = default;

void D3D12CommandProcessor::UpdateDebugMarkersEnabled() {
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void D3D12CommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.BeginDebugMarker(label);
}

void D3D12CommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_list_.EndDebugMarker();
}

void D3D12CommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.InsertDebugMarker(label);
}

void D3D12CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
}

void D3D12CommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void D3D12CommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  for (VertexBufferState& state : vertex_buffer_states_) {
    state.address = UINT32_MAX;
    state.size = UINT32_MAX;
  }
}

void D3D12CommandProcessor::InvalidateVertexBufferResidency(uint32_t vfetch_index) {
  if (vfetch_index >= vertex_buffer_states_.size()) {
    return;
  }
  vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
}

void D3D12CommandProcessor::InvalidateVertexBufferResidencyRange(uint32_t first_vfetch,
                                                                 uint32_t last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= vertex_buffer_states_.size()) {
    return;
  }
  last_vfetch = std::min(last_vfetch, uint32_t(vertex_buffer_states_.size() - 1));
  for (uint32_t vfetch_index = first_vfetch; vfetch_index <= last_vfetch; ++vfetch_index) {
    InvalidateVertexBufferResidency(vfetch_index);
  }
}

void D3D12CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                    uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

bool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);
  if (!sample_counts) {
    DisableHostOcclusionQueries();
    return true;
  }

  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {
    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) {
      return true;
    }
    bool is_end_via_z_pass =
        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
    bool is_end_via_z_fail =
        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      sample_counts->ZPass_A = fake_sample_count;
      sample_counts->Total_A = fake_sample_count;
    }
    return true;
  };

  bool is_end_via_z_pass =
      sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
  bool is_end_via_z_fail =
      sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
  bool is_end = is_end_via_z_pass || is_end_via_z_fail;

  if (!is_end) {
    if (active_occlusion_query_.valid &&
        active_occlusion_query_.sample_count_address != sample_count_addr) {
      DisableHostOcclusionQueries();
      return write_fallback_result();
    }
    if (!BeginGuestOcclusionQuery(sample_count_addr)) {
      return write_fallback_result();
    }
    return true;
  }

  if (!active_occlusion_query_.valid ||
      active_occlusion_query_.sample_count_address != sample_count_addr) {
    DisableHostOcclusionQueries();
    return write_fallback_result();
  }

  if (!EndGuestOcclusionQuery(sample_count_addr, sample_counts)) {
    return write_fallback_result();
  }

  return true;
}

bool D3D12CommandProcessor::PushTransitionBarrier(ID3D12Resource* resource,
                                                  D3D12_RESOURCE_STATES old_state,
                                                  D3D12_RESOURCE_STATES new_state,
                                                  UINT subresource) {
  if (old_state == new_state) {
    return false;
  }
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = old_state;
  barrier.Transition.StateAfter = new_state;
  barriers_.push_back(barrier);
  return true;
}

void D3D12CommandProcessor::PushAliasingBarrier(ID3D12Resource* old_resource,
                                                ID3D12Resource* new_resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Aliasing.pResourceBefore = old_resource;
  barrier.Aliasing.pResourceAfter = new_resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::PushUAVBarrier(ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.UAV.pResource = resource;
  barriers_.push_back(barrier);
}

// [gpu prof] barrier census: ResourceBarrier batches and barriers per frame, and how many are shared-memory transitions
// (the 512 MB buffer changing state stalls every draw still reading it).
uint64_t g_prof_barrier_batches = 0, g_prof_barriers = 0, g_prof_uav_barriers = 0;
uint64_t g_prof_sm_transitions[4] = {};   // to COPY_DEST, to SRV (non-pixel|pixel), to UAV, other
uint64_t g_prof_other_to[6] = {};   // other resources, by state after: RT, depth, SRV, copy, UAV, other
void D3D12CommandProcessor::SubmitBarriers() {
  UINT barrier_count = UINT(barriers_.size());
  if (barrier_count != 0) {
    ++g_prof_barrier_batches;
    g_prof_barriers += barrier_count;
    for (const auto& b : barriers_) {
      if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) ++g_prof_uav_barriers;
      else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && shared_memory_ &&
               b.Transition.pResource == shared_memory_->GetBuffer()) {
        const auto a = b.Transition.StateAfter;
        ++g_prof_sm_transitions[a == D3D12_RESOURCE_STATE_COPY_DEST ? 0
                                : (a & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ? 2
                                : (a & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) ? 1 : 3];
      } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        const auto a = b.Transition.StateAfter;
        ++g_prof_other_to[a == D3D12_RESOURCE_STATE_RENDER_TARGET ? 0
                          : (a & (D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ)) ? 1
                          : (a & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) ? 2
                          : (a & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE)) ? 3
                          : (a & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ? 4 : 5];
      }
    }
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
    barriers_.clear();
  }
}

ID3D12RootSignature* D3D12CommandProcessor::GetRootSignature(const DxbcShader* vertex_shader,
                                                             const DxbcShader* pixel_shader,
                                                             bool tessellated) {
  if (bindless_resources_used_) {
    return tessellated ? root_signature_bindless_ds_ : root_signature_bindless_vs_;
  }

  D3D12_SHADER_VISIBILITY vertex_visibility =
      tessellated ? D3D12_SHADER_VISIBILITY_DOMAIN : D3D12_SHADER_VISIBILITY_VERTEX;

  uint32_t texture_count_vertex =
      uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size());
  uint32_t sampler_count_vertex =
      uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size());
  uint32_t texture_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t sampler_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0;

  // Better put the pixel texture/sampler in the lower bits probably because it
  // changes often.
  uint32_t index = 0;
  uint32_t index_offset = 0;
  index |= texture_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= texture_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= uint32_t(vertex_visibility == D3D12_SHADER_VISIBILITY_DOMAIN) << index_offset;
  ++index_offset;
  assert_true(index_offset <= 32);

  // Try an existing root signature.
  auto it = root_signatures_bindful_.find(index);
  if (it != root_signatures_bindful_.end()) {
    return it->second;
  }

  // Create a new one.
  D3D12_ROOT_SIGNATURE_DESC desc;
  D3D12_ROOT_PARAMETER parameters[kRootParameter_Bindful_Count_Max];
  desc.NumParameters = kRootParameter_Bindful_Count_Base;
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  // Base parameters.

  // Fetch constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FetchConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Vertex float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsVertex];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = vertex_visibility;
  }

  // Pixel float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsPixel];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  // System constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_SystemConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Bool and loop constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_BoolLoopConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Shared memory and, if ROVs are used, EDRAM.
  D3D12_DESCRIPTOR_RANGE shared_memory_and_edram_ranges[3];
  {
    auto& parameter = parameters[kRootParameter_Bindful_SharedMemoryAndEdram];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = shared_memory_and_edram_ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    shared_memory_and_edram_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shared_memory_and_edram_ranges[0].NumDescriptors = 1;
    shared_memory_and_edram_ranges[0].BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
    shared_memory_and_edram_ranges[0].RegisterSpace =
        uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    shared_memory_and_edram_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    shared_memory_and_edram_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    shared_memory_and_edram_ranges[1].NumDescriptors = 1;
    shared_memory_and_edram_ranges[1].BaseShaderRegister =
        UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
    shared_memory_and_edram_ranges[1].RegisterSpace = 0;
    shared_memory_and_edram_ranges[1].OffsetInDescriptorsFromTableStart = 1;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
      ++parameter.DescriptorTable.NumDescriptorRanges;
      shared_memory_and_edram_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      shared_memory_and_edram_ranges[2].NumDescriptors = 1;
      shared_memory_and_edram_ranges[2].BaseShaderRegister =
          UINT(DxbcShaderTranslator::UAVRegister::kEdram);
      shared_memory_and_edram_ranges[2].RegisterSpace = 0;
      shared_memory_and_edram_ranges[2].OffsetInDescriptorsFromTableStart = 2;
    }
  }

  // Extra parameters.

  // Pixel textures.
  D3D12_DESCRIPTOR_RANGE range_textures_pixel;
  if (texture_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_textures_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_pixel.NumDescriptors = texture_count_pixel;
    range_textures_pixel.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_pixel.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Pixel samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_pixel;
  if (sampler_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_samplers_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_pixel.NumDescriptors = sampler_count_pixel;
    range_samplers_pixel.BaseShaderRegister = 0;
    range_samplers_pixel.RegisterSpace = 0;
    range_samplers_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex textures.
  D3D12_DESCRIPTOR_RANGE range_textures_vertex;
  if (texture_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_textures_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_vertex.NumDescriptors = texture_count_vertex;
    range_textures_vertex.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_vertex.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_vertex;
  if (sampler_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_samplers_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_vertex.NumDescriptors = sampler_count_vertex;
    range_samplers_vertex.BaseShaderRegister = 0;
    range_samplers_vertex.RegisterSpace = 0;
    range_samplers_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  ID3D12RootSignature* root_signature =
      ui::ngpu_d3d12::util::CreateRootSignature(GetD3D12Provider(), desc);
  if (root_signature == nullptr) {
    REXGPU_ERROR(
        "Failed to create a root signature with {} pixel textures, {} pixel "
        "samplers, {} vertex textures and {} vertex samplers",
        texture_count_pixel, sampler_count_pixel, texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  root_signatures_bindful_.emplace(index, root_signature);
  return root_signature;
}

uint32_t D3D12CommandProcessor::GetRootBindfulExtraParameterIndices(
    const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
    RootBindfulExtraParameterIndices& indices_out) {
  uint32_t index = kRootParameter_Bindful_Count_Base;
  if (pixel_shader && !pixel_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_pixel = index++;
  } else {
    indices_out.textures_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (pixel_shader && !pixel_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_pixel = index++;
  } else {
    indices_out.samplers_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_vertex = index++;
  } else {
    indices_out.textures_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_vertex = index++;
  } else {
    indices_out.samplers_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  return index;
}

uint64_t D3D12CommandProcessor::RequestViewBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = view_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = view_bindful_heap_pool_->GetLastRequestHeap();
  if (view_bindful_heap_current_ != heap) {
    view_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

uint32_t D3D12CommandProcessor::RequestPersistentViewBindlessDescriptor() {
  assert_true(bindless_resources_used_);
  if (!view_bindless_heap_free_.empty()) {
    uint32_t descriptor_index = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    return descriptor_index;
  }
  if (view_bindless_heap_allocated_ >= kViewBindlessHeapSize) {
    return UINT32_MAX;
  }
  return view_bindless_heap_allocated_++;
}

void D3D12CommandProcessor::ReleaseViewBindlessDescriptorImmediately(uint32_t descriptor_index) {
  assert_true(bindless_resources_used_);
  view_bindless_heap_free_.push_back(descriptor_index);
}

bool D3D12CommandProcessor::RequestOneUseSingleViewDescriptors(
    uint32_t count, ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair* handles_out) {
  assert_true(submission_open_);
  if (!count) {
    return true;
  }
  assert_not_null(handles_out);
  const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
  if (bindless_resources_used_) {
    // Request separate bindless descriptors that will be freed when this
    // submission is completed by the GPU.
    if (count >
        kViewBindlessHeapSize - view_bindless_heap_allocated_ + view_bindless_heap_free_.size()) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t descriptor_index;
      if (!view_bindless_heap_free_.empty()) {
        descriptor_index = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
      } else {
        descriptor_index = view_bindless_heap_allocated_++;
      }
      view_bindless_one_use_descriptors_.push_back(
          std::make_pair(descriptor_index, submission_current_));
      handles_out[i] = std::make_pair(
          provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, descriptor_index),
          provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, descriptor_index));
    }
  } else {
    // Request a range within the current heap for bindful resources path.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_start;
    if (RequestViewBindfulDescriptors(ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid, count,
                                      count, cpu_handle_start, gpu_handle_start) ==
        ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      handles_out[i] = std::make_pair(provider.OffsetViewDescriptor(cpu_handle_start, i),
                                      provider.OffsetViewDescriptor(gpu_handle_start, i));
    }
  }
  return true;
}

ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair D3D12CommandProcessor::GetSystemBindlessViewHandlePair(
    SystemBindlessView view) const {
  assert_true(bindless_resources_used_);
  const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
  return std::make_pair(
      provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, uint32_t(view)),
      provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, uint32_t(view)));
}

ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

uint64_t D3D12CommandProcessor::RequestSamplerBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = sampler_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = sampler_bindful_heap_pool_->GetLastRequestHeap();
  if (sampler_bindful_heap_current_ != heap) {
    sampler_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

ID3D12Resource* D3D12CommandProcessor::RequestScratchGPUBuffer(uint32_t size,
                                                               D3D12_RESOURCE_STATES state) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || size == 0) {
    return nullptr;
  }

  if (size <= scratch_buffer_size_) {
    PushTransitionBarrier(scratch_buffer_, scratch_buffer_state_, state);
    scratch_buffer_state_ = state;
    scratch_buffer_used_ = true;
    return scratch_buffer_;
  }

  size = rex::align(size, kScratchBufferSizeIncrement);

  const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::ngpu_d3d12::util::FillBufferResourceDesc(buffer_desc, size,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12Resource* buffer;
  if (FAILED(device->CreateCommittedResource(&ui::ngpu_d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                             state, nullptr, IID_PPV_ARGS(&buffer)))) {
    REXGPU_ERROR("Failed to create a {} MB scratch GPU buffer", size >> 20);
    return nullptr;
  }
  if (scratch_buffer_ != nullptr) {
    resources_for_deletion_.emplace_back(submission_current_, scratch_buffer_);
  }
  scratch_buffer_ = buffer;
  scratch_buffer_size_ = size;
  scratch_buffer_state_ = state;
  scratch_buffer_used_ = true;
  return scratch_buffer_;
}

void D3D12CommandProcessor::ReleaseScratchGPUBuffer(ID3D12Resource* buffer,
                                                    D3D12_RESOURCE_STATES new_state) {
  assert_true(submission_open_);
  assert_true(scratch_buffer_used_);
  scratch_buffer_used_ = false;
  if (buffer == scratch_buffer_) {
    scratch_buffer_state_ = new_state;
  }
}

void D3D12CommandProcessor::SetExternalPipeline(ID3D12PipelineState* pipeline) {
  if (current_external_pipeline_ != pipeline) {
    current_external_pipeline_ = pipeline;
    current_guest_pipeline_ = nullptr;
    deferred_command_list_.D3DSetPipelineState(pipeline);
  }
}

void D3D12CommandProcessor::SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature) {
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }
  // Force-invalidate because setting a non-guest root signature.
  current_graphics_root_up_to_date_ = 0;
}

void D3D12CommandProcessor::SetViewport(const D3D12_VIEWPORT& viewport) {
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftX != viewport.TopLeftX;
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftY != viewport.TopLeftY;
  ff_viewport_update_needed_ |= ff_viewport_.Width != viewport.Width;
  ff_viewport_update_needed_ |= ff_viewport_.Height != viewport.Height;
  ff_viewport_update_needed_ |= ff_viewport_.MinDepth != viewport.MinDepth;
  ff_viewport_update_needed_ |= ff_viewport_.MaxDepth != viewport.MaxDepth;
  if (ff_viewport_update_needed_) {
    ff_viewport_ = viewport;
    deferred_command_list_.RSSetViewport(ff_viewport_);
    ff_viewport_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetScissorRect(const D3D12_RECT& scissor_rect) {
  ff_scissor_update_needed_ |= ff_scissor_.left != scissor_rect.left;
  ff_scissor_update_needed_ |= ff_scissor_.top != scissor_rect.top;
  ff_scissor_update_needed_ |= ff_scissor_.right != scissor_rect.right;
  ff_scissor_update_needed_ |= ff_scissor_.bottom != scissor_rect.bottom;
  if (ff_scissor_update_needed_) {
    ff_scissor_ = scissor_rect;
    deferred_command_list_.RSSetScissorRect(ff_scissor_);
    ff_scissor_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetStencilReference(uint32_t stencil_ref) {
  ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
  if (ff_stencil_ref_update_needed_) {
    ff_stencil_ref_ = stencil_ref;
    deferred_command_list_.D3DOMSetStencilRef(stencil_ref);
    ff_stencil_ref_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) {
  if (primitive_topology_ != primitive_topology) {
    primitive_topology_ = primitive_topology;
    deferred_command_list_.D3DIASetPrimitiveTopology(primitive_topology);
  }
}

std::string D3D12CommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Direct3D 12";
  if (render_target_cache_) {
    // Rasterizer-ordered views are a feature very rarely used as of 2020 and
    // that faces adoption complications (outside of Direct3D - on Vulkan - at
    // least), but crucial to Xenia - raise awareness of its usage.
    // https://github.com/KhronosGroup/Vulkan-Ecosystem/issues/27#issuecomment-455712319
    // "In Xenia's title bar "D3D12 ROV" can be seen, which was a surprise, as I
    //  wasn't aware that Xenia D3D12 backend was using Raster Order Views
    //  feature" - oscarbg in that issue.
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - RTV/DSV";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - ROV";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  return title.str();
}

bool D3D12CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  InvalidateAllVertexBufferResidency();
  UpdateDebugMarkersEnabled();

  const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  fence_completion_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (fence_completion_event_ == nullptr) {
    REXGPU_ERROR("Failed to create the fence completion event");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission_fence_)))) {
    REXGPU_ERROR("Failed to create the submission fence");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&queue_operations_since_submission_fence_)))) {
    REXGPU_ERROR(
        "Failed to create the fence for awaiting queue operations done since "
        "the latest submission");
    return false;
  }

  // Create the command list and one allocator because it's needed for a command
  // list.
  ID3D12CommandAllocator* command_allocator;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&command_allocator)))) {
    REXGPU_ERROR("Failed to create a command allocator");
    return false;
  }
  command_allocator_writable_first_ = new CommandAllocator;
  command_allocator_writable_first_->command_allocator = command_allocator;
  command_allocator_writable_first_->last_usage_submission = 0;
  command_allocator_writable_first_->next = nullptr;
  command_allocator_writable_last_ = command_allocator_writable_first_;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator,
                                       nullptr, IID_PPV_ARGS(&command_list_)))) {
    REXGPU_ERROR("Failed to create the graphics command list");
    return false;
  }
  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  // Optional - added in Creators Update (SDK 10.0.15063.0).
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));
  async_submit_ = ::ng2::ngpu::rtc::AsyncSubmitEnabled();   // NATIVE PATCH
  gpu_prof_ = async_submit_ && ::ng2::ngpu::rtc::GpuProfEnabled();   // NATIVE PATCH: [gpu prof]
  hoist_uploads_ = async_submit_ && ::ng2::ngpu::rtc::HoistUploadsEnabled();   // NATIVE PATCH: [hoist]
  if (gpu_prof_) {
    D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kGpuProfSlots * kGpuProfPerSlot, 0};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = uint64_t(kGpuProfSlots) * kGpuProfPerSlot * sizeof(uint64_t);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&gpu_prof_heap_))) ||
        FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&gpu_prof_readback_)))) {
      gpu_prof_ = false;
    } else {
      REXGPU_INFO("[ngpu] GPU PROF: timestamping GPU work by category (ngpu_gpu_prof)");
    }
  }
  if (async_submit_) {
    submit_quit_ = false;
    submit_thread_ = std::thread([this] { SubmitThreadMain(); });
    REXGPU_INFO("[ngpu] BACKEND: submissions are executed on their own thread (ngpu_backend_async_submit)");
  }

  bindless_resources_used_ = REXCVAR_GET(d3d12_bindless) &&
                             provider.GetResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_2;

  // Get the draw resolution scale for the render target cache and the texture
  // cache.
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, provider)) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  shared_memory_ = std::make_unique<D3D12SharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }
  // [readback] On-demand copies of resolves the CPU touches early.
  if (!resolve_data_provider_handle_) {
    resolve_data_provider_handle_ =
        ::ng2::ngpu::rtc::GuestDataProviderRegister(memory_, ResolveDataProviderThunk, this) /* NATIVE PATCH */;
  }

  // Initialize the render target cache before configuring binding - need to
  // know if using rasterizer-ordered views for the bindless root signature.
  render_target_cache_ = std::make_unique<D3D12RenderTargetCache>(
      *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this,
      bindless_resources_used_);
  if (!render_target_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the render target cache");
    return false;
  }

  // Initialize resource binding.
  constant_buffer_pool_ = std::make_unique<ui::ngpu_d3d12::D3D12UploadBufferPool>(
      provider, std::max(ui::ngpu_d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                         sizeof(float) * 4 * D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));
  if (bindless_resources_used_) {
    D3D12_DESCRIPTOR_HEAP_DESC view_bindless_heap_desc;
    view_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    view_bindless_heap_desc.NumDescriptors = kViewBindlessHeapSize;
    view_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    view_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&view_bindless_heap_desc,
                                            IID_PPV_ARGS(&view_bindless_heap_)))) {
      REXGPU_ERROR("Failed to create the bindless CBV/SRV/UAV descriptor heap");
      return false;
    }
    view_bindless_heap_cpu_start_ = view_bindless_heap_->GetCPUDescriptorHandleForHeapStart();
    view_bindless_heap_gpu_start_ = view_bindless_heap_->GetGPUDescriptorHandleForHeapStart();
    view_bindless_heap_allocated_ = uint32_t(SystemBindlessView::kCount);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_bindless_heap_desc;
    sampler_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_bindless_heap_desc.NumDescriptors = kSamplerHeapSize;
    sampler_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    sampler_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&sampler_bindless_heap_desc,
                                            IID_PPV_ARGS(&sampler_bindless_heap_current_)))) {
      REXGPU_ERROR("Failed to create the bindless sampler descriptor heap");
      return false;
    }
    sampler_bindless_heap_cpu_start_ =
        sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_gpu_start_ =
        sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_allocated_ = 0;
  } else {
    view_bindful_heap_pool_ = std::make_unique<ui::ngpu_d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewBindfulHeapSize);
    sampler_bindful_heap_pool_ = std::make_unique<ui::ngpu_d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize);
  }

  if (bindless_resources_used_) {
    // Global bindless resource root signatures.
    // No CBV or UAV descriptor ranges with any descriptors to be allocated
    // dynamically (via RequestPersistentViewBindlessDescriptor or
    // RequestOneUseSingleViewDescriptors) should be here, because they would
    // overlap the unbounded SRV range, which is not allowed on Nvidia Fermi!
    D3D12_ROOT_SIGNATURE_DESC root_signature_bindless_desc;
    D3D12_ROOT_PARAMETER
    root_parameters_bindless[kRootParameter_Bindless_Count];
    root_signature_bindless_desc.NumParameters = kRootParameter_Bindless_Count;
    root_signature_bindless_desc.pParameters = root_parameters_bindless;
    root_signature_bindless_desc.NumStaticSamplers = 0;
    root_signature_bindless_desc.pStaticSamplers = nullptr;
    root_signature_bindless_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    // Fetch constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FetchConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Vertex float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // Pixel float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Pixel shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Vertex shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // System constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SystemConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Bool and loop constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_BoolLoopConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Shared memory SRV and UAV.
    D3D12_DESCRIPTOR_RANGE root_shared_memory_view_ranges[2];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SharedMemory];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges =
          uint32_t(rex::countof(root_shared_memory_view_ranges));
      parameter.DescriptorTable.pDescriptorRanges = root_shared_memory_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      {
        auto& range = root_shared_memory_view_ranges[0];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kMain);
        range.OffsetInDescriptorsFromTableStart = 0;
      }
      {
        auto& range = root_shared_memory_view_ranges[1];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 1;
      }
    }
    // Sampler heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_sampler_range;
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SamplerHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &root_bindless_sampler_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      root_bindless_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      root_bindless_sampler_range.NumDescriptors = UINT_MAX;
      root_bindless_sampler_range.BaseShaderRegister = 0;
      root_bindless_sampler_range.RegisterSpace = 0;
      root_bindless_sampler_range.OffsetInDescriptorsFromTableStart = 0;
    }
    // View heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_view_ranges[4];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_ViewHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 0;
      parameter.DescriptorTable.pDescriptorRanges = root_bindless_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // EDRAM.
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kEdram);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kEdramR32UintUAV);
      }
      // Used UAV and SRV ranges must not overlap on Nvidia Fermi, so textures
      // have OffsetInDescriptorsFromTableStart after all static descriptors of
      // other types.
      // 2D array textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures2DArray);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // 3D textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures3D);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // Cube textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTexturesCube);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
    }
    root_signature_bindless_vs_ =
        ui::ngpu_d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_vs_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use without tessellation");
      return false;
    }
    root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_signature_bindless_ds_ =
        ui::ngpu_d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_ds_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use with tessellation");
      return false;
    }
  }

  primitive_processor_ =
      std::make_unique<D3D12PrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  texture_cache_ =
      D3D12TextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                draw_resolution_scale_y, *this, bindless_resources_used_);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }

  pipeline_cache_ = std::make_unique<PipelineCache>(
      *this, *register_file_, *render_target_cache_.get(), bindless_resources_used_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  D3D12_HEAP_FLAGS heap_flag_create_not_zeroed = provider.GetHeapFlagCreateNotZeroed();

  // Create gamma ramp resources.
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  D3D12_RESOURCE_DESC gamma_ramp_buffer_desc;
  ui::ngpu_d3d12::util::FillBufferResourceDesc(gamma_ramp_buffer_desc, (256 + 128 * 3) * 4,
                                          D3D12_RESOURCE_FLAG_NONE);
  // The first action will be uploading.
  gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (FAILED(device->CreateCommittedResource(&ui::ngpu_d3d12::util::kHeapPropertiesDefault,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             gamma_ramp_buffer_state_, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp buffer");
    return false;
  }
  // The upload buffer is frame-buffered.
  gamma_ramp_buffer_desc.Width *= kQueueFrames;
  if (FAILED(device->CreateCommittedResource(&ui::ngpu_d3d12::util::kHeapPropertiesUpload,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_upload_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp upload buffer");
    return false;
  }
  if (FAILED(gamma_ramp_upload_buffer_->Map(
          0, nullptr, reinterpret_cast<void**>(&gamma_ramp_upload_buffer_mapping_)))) {
    REXGPU_ERROR("Failed to map the gamma ramp upload buffer");
    gamma_ramp_upload_buffer_mapping_ = nullptr;
    return false;
  }

  // Initialize compute pipelines for output with gamma ramp.
  D3D12_ROOT_PARAMETER
  apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_constants =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    apply_gamma_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    apply_gamma_root_parameter_constants.Constants.ShaderRegister = 0;
    apply_gamma_root_parameter_constants.Constants.RegisterSpace = 0;
    apply_gamma_root_parameter_constants.Constants.Num32BitValues =
        sizeof(ApplyGammaConstants) / sizeof(uint32_t);
    apply_gamma_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_dest;
  apply_gamma_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  apply_gamma_root_descriptor_range_dest.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_dest.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_dest.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_dest =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kDestination)];
    apply_gamma_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_dest.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_dest;
    apply_gamma_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_source;
  apply_gamma_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_source.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_source.BaseShaderRegister = 1;
  apply_gamma_root_descriptor_range_source.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_source =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kSource)];
    apply_gamma_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_source;
    apply_gamma_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_ramp;
  apply_gamma_root_descriptor_range_ramp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_ramp.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_ramp.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_ramp.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_ramp.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_gamma_ramp =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kRamp)];
    apply_gamma_root_parameter_gamma_ramp.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_ramp;
    apply_gamma_root_parameter_gamma_ramp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC apply_gamma_root_signature_desc;
  apply_gamma_root_signature_desc.NumParameters = UINT(ApplyGammaRootParameter::kCount);
  apply_gamma_root_signature_desc.pParameters = apply_gamma_root_parameters;
  apply_gamma_root_signature_desc.NumStaticSamplers = 0;
  apply_gamma_root_signature_desc.pStaticSamplers = nullptr;
  apply_gamma_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(apply_gamma_root_signature_.ReleaseAndGetAddressOf()) =
      ui::ngpu_d3d12::util::CreateRootSignature(provider, apply_gamma_root_signature_desc);
  if (!apply_gamma_root_signature_) {
    REXGPU_ERROR("Failed to create the gamma ramp application root signature");
    return false;
  }
  *(apply_gamma_table_pipeline_.ReleaseAndGetAddressOf()) = ui::ngpu_d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_table_cs, sizeof(shaders::apply_gamma_table_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline");
    return false;
  }
  *(apply_gamma_table_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::ngpu_d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_table_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_table_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with perceptual luma output");
    return false;
  }
  *(apply_gamma_pwl_pipeline_.ReleaseAndGetAddressOf()) = ui::ngpu_d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_pwl_cs, sizeof(shaders::apply_gamma_pwl_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_pipeline_) {
    REXGPU_ERROR("Failed to create the PWL gamma ramp application compute pipeline");
    return false;
  }
  *(apply_gamma_pwl_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::ngpu_d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_pwl_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "perceptual luma output");
    return false;
  }

  // Initialize compute pipelines for post-processing anti-aliasing.
  D3D12_ROOT_PARAMETER fxaa_root_parameters[UINT(FxaaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_constants =
        fxaa_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    fxaa_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fxaa_root_parameter_constants.Constants.ShaderRegister = 0;
    fxaa_root_parameter_constants.Constants.RegisterSpace = 0;
    fxaa_root_parameter_constants.Constants.Num32BitValues =
        sizeof(FxaaConstants) / sizeof(uint32_t);
    fxaa_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_dest;
  fxaa_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  fxaa_root_descriptor_range_dest.NumDescriptors = 1;
  fxaa_root_descriptor_range_dest.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_dest.RegisterSpace = 0;
  fxaa_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_dest =
        fxaa_root_parameters[UINT(FxaaRootParameter::kDestination)];
    fxaa_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_dest.DescriptorTable.pDescriptorRanges = &fxaa_root_descriptor_range_dest;
    fxaa_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_source;
  fxaa_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  fxaa_root_descriptor_range_source.NumDescriptors = 1;
  fxaa_root_descriptor_range_source.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_source.RegisterSpace = 0;
  fxaa_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_source =
        fxaa_root_parameters[UINT(FxaaRootParameter::kSource)];
    fxaa_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &fxaa_root_descriptor_range_source;
    fxaa_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC fxaa_root_sampler;
  fxaa_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  fxaa_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.MipLODBias = 0.0f;
  fxaa_root_sampler.MaxAnisotropy = 1;
  fxaa_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  fxaa_root_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  fxaa_root_sampler.MinLOD = 0.0f;
  fxaa_root_sampler.MaxLOD = 0.0f;
  fxaa_root_sampler.ShaderRegister = 0;
  fxaa_root_sampler.RegisterSpace = 0;
  fxaa_root_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC fxaa_root_signature_desc;
  fxaa_root_signature_desc.NumParameters = UINT(FxaaRootParameter::kCount);
  fxaa_root_signature_desc.pParameters = fxaa_root_parameters;
  fxaa_root_signature_desc.NumStaticSamplers = 1;
  fxaa_root_signature_desc.pStaticSamplers = &fxaa_root_sampler;
  fxaa_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(fxaa_root_signature_.ReleaseAndGetAddressOf()) =
      ui::ngpu_d3d12::util::CreateRootSignature(provider, fxaa_root_signature_desc);
  if (!fxaa_root_signature_) {
    REXGPU_ERROR("Failed to create the FXAA root signature");
    return false;
  }
  *(fxaa_pipeline_.ReleaseAndGetAddressOf()) = ui::ngpu_d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_cs, sizeof(shaders::fxaa_cs), fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the FXAA compute pipeline");
    return false;
  }
  *(fxaa_extreme_pipeline_.ReleaseAndGetAddressOf()) = ui::ngpu_d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_extreme_cs, sizeof(shaders::fxaa_extreme_cs),
      fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the extreme-quality FXAA compute pipeline");
    return false;
  }

  // Resolve downscale compute pipeline for scaled readback resolve.
  D3D12_ROOT_PARAMETER
  resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& constants_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kConstants)];
    constants_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants_parameter.Constants.ShaderRegister = 0;
    constants_parameter.Constants.RegisterSpace = 0;
    constants_parameter.Constants.Num32BitValues =
        sizeof(ResolveDownscaleConstants) / sizeof(uint32_t);
    constants_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_source_range;
  resolve_downscale_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  resolve_downscale_source_range.NumDescriptors = 1;
  resolve_downscale_source_range.BaseShaderRegister = 0;
  resolve_downscale_source_range.RegisterSpace = 0;
  resolve_downscale_source_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& source_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kSource)];
    source_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    source_parameter.DescriptorTable.NumDescriptorRanges = 1;
    source_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_source_range;
    source_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_destination_range;
  resolve_downscale_destination_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  resolve_downscale_destination_range.NumDescriptors = 1;
  resolve_downscale_destination_range.BaseShaderRegister = 0;
  resolve_downscale_destination_range.RegisterSpace = 0;
  resolve_downscale_destination_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& destination_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kDestination)];
    destination_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    destination_parameter.DescriptorTable.NumDescriptorRanges = 1;
    destination_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_destination_range;
    destination_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC resolve_downscale_root_signature_desc;
  resolve_downscale_root_signature_desc.NumParameters = UINT(ResolveDownscaleRootParameter::kCount);
  resolve_downscale_root_signature_desc.pParameters = resolve_downscale_root_parameters;
  resolve_downscale_root_signature_desc.NumStaticSamplers = 0;
  resolve_downscale_root_signature_desc.pStaticSamplers = nullptr;
  resolve_downscale_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(resolve_downscale_root_signature_.ReleaseAndGetAddressOf()) =
      ui::ngpu_d3d12::util::CreateRootSignature(provider, resolve_downscale_root_signature_desc);
  if (resolve_downscale_root_signature_) {
    *(resolve_downscale_pipeline_.ReleaseAndGetAddressOf()) =
        ui::ngpu_d3d12::util::CreateComputePipeline(device, shaders::resolve_downscale_cs,
                                               sizeof(shaders::resolve_downscale_cs),
                                               resolve_downscale_root_signature_.Get());
  }
  if (!resolve_downscale_root_signature_ || !resolve_downscale_pipeline_) {
    resolve_downscale_pipeline_.Reset();
    resolve_downscale_root_signature_.Reset();
    REXGPU_WARN("Failed to initialize D3D12 resolve-downscale readback pipeline");
  }

  if (bindless_resources_used_) {
    // Create the system bindless descriptors once all resources are
    // initialized.
    // kNullRawSRV.
    ui::ngpu_d3d12::util::CreateBufferRawSRV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawSRV)),
        nullptr, 0);
    // kNullRawUAV.
    ui::ngpu_d3d12::util::CreateBufferRawUAV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawUAV)),
        nullptr, 0);
    // kNullTexture2DArray.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
    null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    null_srv_desc.Texture2DArray.MostDetailedMip = 0;
    null_srv_desc.Texture2DArray.MipLevels = 1;
    null_srv_desc.Texture2DArray.FirstArraySlice = 0;
    null_srv_desc.Texture2DArray.ArraySize = 1;
    null_srv_desc.Texture2DArray.PlaneSlice = 0;
    null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture2DArray)));
    // kNullTexture3D.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    null_srv_desc.Texture3D.MostDetailedMip = 0;
    null_srv_desc.Texture3D.MipLevels = 1;
    null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture3D)));
    // kNullTextureCube.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    null_srv_desc.TextureCube.MostDetailedMip = 0;
    null_srv_desc.TextureCube.MipLevels = 1;
    null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTextureCube)));
    // kSharedMemoryRawSRV.
    shared_memory_->WriteRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawSRV)));
    // kSharedMemoryR32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintSRV)),
        2);
    // kSharedMemoryR32G32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintSRV)),
        3);
    // kSharedMemoryR32G32B32A32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV)),
        4);
    // kSharedMemoryRawUAV.
    shared_memory_->WriteRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawUAV)));
    // kSharedMemoryR32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintUAV)),
        2);
    // kSharedMemoryR32G32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintUAV)),
        3);
    // kSharedMemoryR32G32B32A32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV)),
        4);
    // kEdramRawSRV.
    render_target_cache_->WriteEdramRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawSRV)));
    // kEdramR32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintSRV)),
        2);
    // kEdramR32G32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintSRV)),
        3);
    // kEdramR32G32B32A32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintSRV)),
        4);
    // kEdramRawUAV.
    render_target_cache_->WriteEdramRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawUAV)));
    // kEdramR32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintUAV)),
        2);
    // kEdramR32G32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintUAV)),
        3);
    // kEdramR32G32B32A32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintUAV)),
        4);
    // kGammaRampTableSRV.
    WriteGammaRampSRV(
        false, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                             uint32_t(SystemBindlessView::kGammaRampTableSRV)));
    // kGammaRampPWLSRV.
    WriteGammaRampSRV(
        true, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                            uint32_t(SystemBindlessView::kGammaRampPWLSRV)));
  }

  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}

// [cpu cost] the threads the backend runs on, for CPU ms per frame beside GPU TIME.
static HANDLE g_cost_draw_thread = nullptr, g_cost_submit_thread = nullptr;
static uint64_t ThreadCpu100ns(HANDLE h) {
  FILETIME c, e, k, u;
  if (!h || !GetThreadTimes(h, &c, &e, &k, &u)) return 0;
  return ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
}
static HANDLE DupCurrentThread() {
  HANDLE h = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
  return h;
}

uint8_t D3D12CommandProcessor::GpuCatSet(uint8_t cat) {
  const uint8_t prev = gpu_prof_cat_;
  if (cat == prev) return prev;
  gpu_prof_cat_ = cat;
  if (gpu_prof_ && submission_open_ && gpu_prof_heap_) {
    if (gpu_prof_used_ + 1 < kGpuProfPerSlot) {
      deferred_command_list_.D3DEndQuery(gpu_prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP,
                                         gpu_prof_slot_ * kGpuProfPerSlot + gpu_prof_used_);
      gpu_prof_cats_.push_back(cat);
      ++gpu_prof_used_;
    } else {
      ++gpu_prof_overflow_;
    }
  }
  return prev;
}

void D3D12CommandProcessor::GpuProfBegin() {
  if (!gpu_prof_ || !gpu_prof_heap_) return;
  gpu_prof_slot_ = uint32_t(submission_current_ % kGpuProfSlots);
  gpu_prof_used_ = 0;
  gpu_prof_cats_.clear();
  deferred_command_list_.D3DEndQuery(gpu_prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP, gpu_prof_slot_ * kGpuProfPerSlot);
  gpu_prof_cats_.push_back(gpu_prof_cat_);
  gpu_prof_used_ = 1;
}

void D3D12CommandProcessor::GpuTimeCollect(bool all) {
  if (gpu_prof_mapped_) {
    const uint64_t done = submission_fence_->GetCompletedValue();
    while (!gpu_prof_pending_.empty() && gpu_prof_pending_.front().submission <= done) {
      const GpuProfPending& p = gpu_prof_pending_.front();
      const uint64_t* ts = gpu_prof_mapped_ + size_t(p.slot) * kGpuProfPerSlot;
      for (uint32_t i = 0; i + 1 < p.used && i < p.cats.size(); ++i)
        if (ts[i + 1] > ts[i]) gpu_prof_ticks_[p.cats[i] < kGpuCatCount ? p.cats[i] : 0] += ts[i + 1] - ts[i];
      gpu_prof_pending_.pop_front();
    }
  }
  if (!gpu_time_mapped_) return;
  const uint64_t completed = submission_fence_->GetCompletedValue();
  while (!gpu_time_pending_.empty() && (all || gpu_time_pending_.front() <= completed)) {
    if (gpu_time_pending_.front() > completed) break;
    const uint32_t slot = uint32_t(gpu_time_pending_.front() % kGpuTimeSlots);
    gpu_time_pending_.pop_front();
    const uint64_t b = gpu_time_mapped_[slot * 2], e = gpu_time_mapped_[slot * 2 + 1];
    if (e > b) gpu_time_ticks_ += e - b;
    ++gpu_time_submissions_;
  }
  const uint64_t now_ms = GetTickCount64();
  if (!gpu_time_next_report_ms_) gpu_time_next_report_ms_ = now_ms + 5000;
  if (now_ms >= gpu_time_next_report_ms_ && gpu_time_frequency_) {
    const uint64_t swaps = ::ng2::ngpu::rtc::SwapSubmissionsNoted();
    const uint64_t frames = swaps - gpu_time_swaps_at_report_;
    const double ms = double(gpu_time_ticks_) * 1000.0 / double(gpu_time_frequency_);
    REXGPU_INFO("[ngpu] GPU TIME: {:.2f} ms of backend GPU work per frame ({} frames, {} submissions, {:.1f} ms total in 5 s)",
                frames ? ms / double(frames) : 0.0, frames, gpu_time_submissions_, ms);
    {
      static uint64_t last_draw = 0, last_submit = 0;
      const uint64_t d = ThreadCpu100ns(g_cost_draw_thread), sb = ThreadCpu100ns(g_cost_submit_thread);
      if (frames && last_draw)
        REXGPU_INFO("[ngpu] CPU COST per frame: plugin GPU thread (PM4 parse + backend draws) {:.2f} ms, submit thread {:.2f} ms",
                    double(d - last_draw) / 10000.0 / double(frames), double(sb - last_submit) / 10000.0 / double(frames));
      last_draw = d;
      last_submit = sb;
    }
    if (frames) {
      REXGPU_INFO("[ngpu] BARRIERS per frame: {:.0f} batches, {:.0f} barriers ({:.0f} UAV); shared-memory buffer transitions: "
                  "{:.0f} to copy-dest, {:.0f} to shader-resource, {:.0f} to UAV, {:.0f} other",
                  double(g_prof_barrier_batches) / frames, double(g_prof_barriers) / frames, double(g_prof_uav_barriers) / frames,
                  double(g_prof_sm_transitions[0]) / frames, double(g_prof_sm_transitions[1]) / frames,
                  double(g_prof_sm_transitions[2]) / frames, double(g_prof_sm_transitions[3]) / frames);
      extern uint64_t g_prof_tex_loads, g_prof_tex_load_bytes, g_prof_tex_loads_scaled;
      REXGPU_INFO("[ngpu] TEXTURES per frame: {:.1f} loads ({:.0f} KB, {:.1f} of them scaled-resolve), outdated by GPU writes "
                  "{:.1f}, by CPU writes {:.1f}",
                  double(g_prof_tex_loads) / frames, double(g_prof_tex_load_bytes) / 1024.0 / frames,
                  double(g_prof_tex_loads_scaled) / frames, double(::rex::graphics::g_prof_tex_outdated_by_gpu) / frames,
                  double(::rex::graphics::g_prof_tex_outdated_by_cpu) / frames);
      ::rex::graphics::g_prof_tex_outdated_by_gpu = ::rex::graphics::g_prof_tex_outdated_by_cpu = g_prof_tex_loads = g_prof_tex_load_bytes = g_prof_tex_loads_scaled = 0;
      REXGPU_INFO("[ngpu] OTHER TRANSITIONS per frame: to render-target {:.0f}, depth {:.0f}, shader-resource {:.0f}, copy {:.0f}, UAV {:.0f}, other {:.0f}",
                  double(g_prof_other_to[0]) / frames, double(g_prof_other_to[1]) / frames, double(g_prof_other_to[2]) / frames,
                  double(g_prof_other_to[3]) / frames, double(g_prof_other_to[4]) / frames, double(g_prof_other_to[5]) / frames);
      for (auto& t : g_prof_other_to) t = 0;
      REXGPU_INFO("[ngpu] HOIST: {:.0f} upload batches per frame moved to the prologue", double(hoisted_uploads_) / frames);
      hoisted_uploads_ = 0;
      g_prof_barrier_batches = g_prof_barriers = g_prof_uav_barriers = 0;
      for (auto& t : g_prof_sm_transitions) t = 0;
    }
    if (gpu_prof_mapped_ && frames) {
      auto cat_ms = [&](int c) { return double(gpu_prof_ticks_[c]) * 1000.0 / double(gpu_time_frequency_) / double(frames); };
      REXGPU_INFO("[ngpu] GPU PROF per frame: draws {:.2f} ms, texture loads {:.2f}, uploads {:.2f}, resolves {:.2f}, "
                  "render-target transfers/clears {:.2f}, readback copies {:.2f}, output {:.2f}, other {:.2f} "
                  "({} timestamp overflows)",
                  cat_ms(kGpuCatDraw), cat_ms(kGpuCatTexLoad), cat_ms(kGpuCatUpload), cat_ms(kGpuCatResolve),
                  cat_ms(kGpuCatRtXfer), cat_ms(kGpuCatReadback), cat_ms(kGpuCatOutput), cat_ms(kGpuCatOther),
                  gpu_prof_overflow_);
      for (auto& t : gpu_prof_ticks_) t = 0;
      gpu_prof_overflow_ = 0;
    }
    gpu_time_ticks_ = 0;
    gpu_time_submissions_ = 0;
    gpu_time_swaps_at_report_ = swaps;
    gpu_time_next_report_ms_ = now_ms + 5000;
  }
}

void D3D12CommandProcessor::SubmitThreadMain() {
  ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
  g_cost_submit_thread = DupCurrentThread();   // NATIVE PATCH: [cpu cost]
  {
    ID3D12Device* device = GetD3D12Provider().GetDevice();
    D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kGpuTimeSlots * 2, 0};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kGpuTimeSlots * 2 * sizeof(uint64_t);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr;
    if (SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&gpu_time_heap_))) &&
        SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&gpu_time_readback_))) &&
        SUCCEEDED(gpu_time_readback_->Map(0, nullptr, &mapped)) &&
        SUCCEEDED(direct_queue->GetTimestampFrequency(&gpu_time_frequency_))) {
      gpu_time_mapped_ = static_cast<const uint64_t*>(mapped);
    }
    if (gpu_prof_ && gpu_prof_readback_) {
      void* pm = nullptr;
      if (SUCCEEDED(gpu_prof_readback_->Map(0, nullptr, &pm))) gpu_prof_mapped_ = static_cast<const uint64_t*>(pm);
    }
  }
  for (;;) {
    SubmitJob job;
    {
      std::unique_lock<std::mutex> lk(submit_mutex_);
      submit_cv_.wait(lk, [this] { return submit_quit_ || !submit_jobs_.empty(); });
      if (submit_jobs_.empty()) return;
      job = std::move(submit_jobs_.front());
      submit_jobs_.pop_front();
    }
    // The allocator and command_list_ belong to this thread from EndSubmission until the fence value is signalled;
    // the GPU thread reuses an allocator only once GetCompletedSubmission has passed its submission.
    job.allocator->Reset();
    command_list_->Reset(job.allocator, nullptr);
    const uint32_t ts_slot = uint32_t(job.submission % kGpuTimeSlots);
    if (gpu_time_mapped_) command_list_->EndQuery(gpu_time_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ts_slot * 2);
    if (job.prologue) {
      // [hoist] every hoisted upload under ONE transition pair, before the submission's own commands.
      ID3D12Resource* buffer = shared_memory_->GetBuffer();
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = buffer;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      const bool transition = job.prologue_state != D3D12_RESOURCE_STATE_COPY_DEST;
      if (transition) {
        b.Transition.StateBefore = job.prologue_state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        command_list_->ResourceBarrier(1, &b);
      }
      job.prologue->Execute(command_list_, command_list_1_);
      if (transition) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = job.prologue_state;
        command_list_->ResourceBarrier(1, &b);
      }
      job.prologue->Reset();
      std::lock_guard<std::mutex> lk(submit_mutex_);
      submit_free_lists_.push_back(std::move(job.prologue));
    }
    job.list->Execute(command_list_, command_list_1_);
    if (gpu_time_mapped_) {
      command_list_->EndQuery(gpu_time_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ts_slot * 2 + 1);
      command_list_->ResolveQueryData(gpu_time_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ts_slot * 2, 2, gpu_time_readback_,
                                      uint64_t(ts_slot) * 2 * sizeof(uint64_t));
      gpu_time_pending_.push_back(job.submission);
    }
    if (job.prof_used > 1) gpu_prof_pending_.push_back({job.submission, job.prof_slot, job.prof_used, std::move(job.prof_cats)});
    command_list_->Close();
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
    direct_queue->Signal(submission_fence_, job.submission);
    job.list->Reset();
    {
      std::lock_guard<std::mutex> lk(submit_mutex_);
      submit_free_lists_.push_back(std::move(job.list));
      submit_done_through_ = job.submission;
    }
    ::ng2::ngpu::rtc::NoteSubmissionExecuted(job.submission);
    submit_done_cv_.notify_all();
    GpuTimeCollect(false);
  }
}

void D3D12CommandProcessor::DrainSubmissions() {
  if (!async_submit_) return;
  std::unique_lock<std::mutex> lk(submit_mutex_);
  submit_done_cv_.wait(lk, [this] { return submit_done_through_ >= submit_queued_through_; });
}

void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  if (submit_thread_.joinable()) {
    DrainSubmissions();
    {
      std::lock_guard<std::mutex> lk(submit_mutex_);
      submit_quit_ = true;
    }
    submit_cv_.notify_all();
    submit_thread_.join();
  }
  // [readback] Land what is pending, release every watched page, unhook.
  for (const PendingResolveReadback& p : pending_resolve_readbacks_) {
    ::ng2::ngpu::rtc::GuestDataProvidersDisable(memory_, /* NATIVE PATCH */ p.address, p.length);
  }
  pending_resolve_readbacks_.clear();
  if (resolve_data_provider_handle_) {
    ::ng2::ngpu::rtc::GuestDataProviderUnregister(memory_, resolve_data_provider_handle_);   // NATIVE PATCH
    resolve_data_provider_handle_ = nullptr;
  }
  InvalidateAllVertexBufferResidency();
  ShutdownOcclusionQueryResources();

  ui::ngpu_d3d12::util::ReleaseAndNull(readback_buffer_);
  readback_buffer_size_ = 0;
  for (auto& resolve_readback_pair : readback_buffers_) {
    auto& readback = resolve_readback_pair.second;
    for (uint32_t i = 0; i < kReadbackSlots; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  readback_buffers_.clear();
  for (auto& memexport_readback_pair : memexport_readback_buffers_) {
    auto& readback = memexport_readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  memexport_readback_buffers_.clear();

  ui::ngpu_d3d12::util::ReleaseAndNull(scratch_buffer_);
  scratch_buffer_size_ = 0;
  resolve_downscale_buffer_size_ = 0;
  resolve_downscale_buffer_.Reset();

  for (const std::pair<uint64_t, ID3D12Resource*>& resource_for_deletion :
       resources_for_deletion_) {
    resource_for_deletion.second->Release();
  }
  resources_for_deletion_.clear();

  fxaa_source_texture_submission_ = 0;
  fxaa_source_texture_.Reset();

  fxaa_extreme_pipeline_.Reset();
  fxaa_pipeline_.Reset();
  fxaa_root_signature_.Reset();
  resolve_downscale_pipeline_.Reset();
  resolve_downscale_root_signature_.Reset();

  apply_gamma_pwl_fxaa_luma_pipeline_.Reset();
  apply_gamma_pwl_pipeline_.Reset();
  apply_gamma_table_fxaa_luma_pipeline_.Reset();
  apply_gamma_table_pipeline_.Reset();
  apply_gamma_root_signature_.Reset();

  // Unmapping will be done implicitly by the destruction.
  gamma_ramp_upload_buffer_mapping_ = nullptr;
  gamma_ramp_upload_buffer_.Reset();
  gamma_ramp_buffer_.Reset();

  texture_cache_.reset();

  pipeline_cache_.reset();

  primitive_processor_.reset();

  // Shut down binding - bindless descriptors may be owned by subsystems like
  // the texture cache.

  // Root signatures are used by pipelines, thus freed after the pipelines.
  ui::ngpu_d3d12::util::ReleaseAndNull(root_signature_bindless_ds_);
  ui::ngpu_d3d12::util::ReleaseAndNull(root_signature_bindless_vs_);
  for (auto it : root_signatures_bindful_) {
    it.second->Release();
  }
  root_signatures_bindful_.clear();

  if (bindless_resources_used_) {
    texture_cache_bindless_sampler_map_.clear();
    for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
      sampler_bindless_heap_overflowed.first->Release();
    }
    sampler_bindless_heaps_overflowed_.clear();
    sampler_bindless_heap_allocated_ = 0;
    ui::ngpu_d3d12::util::ReleaseAndNull(sampler_bindless_heap_current_);
    view_bindless_one_use_descriptors_.clear();
    view_bindless_heap_free_.clear();
    ui::ngpu_d3d12::util::ReleaseAndNull(view_bindless_heap_);
  } else {
    sampler_bindful_heap_pool_.reset();
    view_bindful_heap_pool_.reset();
  }
  constant_buffer_pool_.reset();

  render_target_cache_.reset();

  shared_memory_.reset();

  deferred_command_list_.Reset();
  ui::ngpu_d3d12::util::ReleaseAndNull(command_list_1_);
  ui::ngpu_d3d12::util::ReleaseAndNull(command_list_);
  ClearCommandAllocatorCache();

  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));

  // First release the fences since they may reference fence_completion_event_.

  queue_operations_done_since_submission_signal_ = false;
  queue_operations_since_submission_fence_last_ = 0;
  ui::ngpu_d3d12::util::ReleaseAndNull(queue_operations_since_submission_fence_);

  ui::ngpu_d3d12::util::ReleaseAndNull(submission_fence_);
  submission_open_ = false;
  submission_current_ = 1;
  submission_completed_ = 0;

  if (fence_completion_event_) {
    CloseHandle(fence_completion_event_);
    fence_completion_event_ = nullptr;
  }

  device_removed_ = false;

  CommandProcessor::ShutdownContext();
}

void D3D12CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_ != nullptr) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
    InvalidateVertexBufferResidency((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2);
  }
}

void D3D12CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint64_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (frame_open_) {
      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (first_float_constant < 256) {
        uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
      if (last_float_constant >= 256) {
        uint32_t first_pixel_constant =
            first_float_constant >= 256 ? first_float_constant - 256 : 0;
        uint32_t last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_bool_loop_.up_to_date = false;
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_fetch_.up_to_date = false;
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void D3D12CommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void D3D12CommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

// How EVENLY the guest delivers frames, reported every five seconds.
//
// "High fps but laggy" is a pacing complaint, and the host's present rate
// cannot answer it: the UI repaints on its own schedule and re-presents the
// same guest image. This counts guest swaps - each one is a frame the game
// finished - and reports the median interval, the 99th percentile, the worst
// gap and how many gaps were more than twice the median. Steady clock, fixed
// storage, no allocation on the swap path.
namespace {
// [hitch] What the current guest frame did (GPU worker thread only).
uint32_t g_frame_textures_loaded = 0;
uint64_t g_frame_texture_bytes = 0;
uint64_t g_frame_upload_bytes = 0;
uint32_t g_frame_pipeline_waits = 0;
uint64_t g_frame_pipeline_wait_us = 0;
uint32_t g_frame_sync_readbacks = 0;
uint32_t g_frame_draws_snapshot = 0;
float g_recent_p50_ms = 16.7f;  // the last 5 s window's median frame
void ReportGuestSwapRate() {
  using clock = std::chrono::steady_clock;
  static clock::time_point last_swap_at = clock::now();
  static clock::time_point window_start = last_swap_at;
  static float ms[1536] = {};
  static size_t count = 0;

  const auto now = clock::now();
  const float dt = std::chrono::duration<float, std::milli>(now - last_swap_at).count();
  last_swap_at = now;
  if (count < 1536)
    ms[count++] = dt;
  // [hitch] A long frame says what it did. At most ten lines a second.
  {
    static clock::time_point hitch_sec = now;
    static int hitch_lines = 0;
    // Long in absolute terms AND against the recent median: the 30 fps
    // title and loading screens are not hitches.
    if (dt > 25.0f && dt < 2000.0f && dt > 1.8f * g_recent_p50_ms) {
      if (std::chrono::duration<double>(now - hitch_sec).count() > 1.0) {
        hitch_sec = now;
        hitch_lines = 0;
      }
      if (hitch_lines++ < 10) {
        REXLOG_INFO("[hitch] {:.0f} ms frame: {} textures ({} KB), uploads {} KB, pipeline waits {} ({:.1f} ms), sync readbacks {}, draws {}",
                    dt, g_frame_textures_loaded, g_frame_texture_bytes >> 10, g_frame_upload_bytes >> 10,
                    g_frame_pipeline_waits, g_frame_pipeline_wait_us / 1000.0, g_frame_sync_readbacks,
                    g_frame_draws_snapshot);
      }
    }
    g_frame_textures_loaded = 0;
    g_frame_texture_bytes = 0;
    g_frame_upload_bytes = 0;
    g_frame_pipeline_waits = 0;
    g_frame_pipeline_wait_us = 0;
    g_frame_sync_readbacks = 0;
  }
  // The one-second rate, for an on-screen counter. The app's own counter
  // measures host presents, which run at the UI's pace (170-200 a second on
  // a 144 Hz panel) whether the game delivered a new frame or not; a counter
  // that says 190 while the game runs at 30 is worse than no counter.
  {
    static clock::time_point second_start = now;
    static uint32_t swaps = 0;
    ++swaps;
    const double secs = std::chrono::duration<double>(now - second_start).count();
    if (secs >= 1.0) {
      REXCVAR_SET(guest_fps_x10, int32_t(double(swaps) / secs * 10.0 + 0.5));
      second_start = now;
      swaps = 0;
    }
  }
  if (std::chrono::duration<double>(now - window_start).count() < 5.0)
    return;
  const double secs = std::chrono::duration<double>(now - window_start).count();
  window_start = now;
  if (count < 2) {
    count = 0;
    return;
  }
  static float sorted[1536];
  std::memcpy(sorted, ms, count * sizeof(float));
  std::sort(sorted, sorted + count);
  const float p50 = sorted[count / 2];
  const float p99 = sorted[(count * 99) / 100];
  const float worst = sorted[count - 1];
  g_recent_p50_ms = p50;
  int hitches = 0;
  for (size_t i = 0; i < count; ++i)
    if (ms[i] > p50 * 2.0f) ++hitches;
  REXLOG_INFO("[swap] {:.1f} guest fps ({} swaps in {:.1f}s)  interval ms: p50 {:.1f}  "
              "p99 {:.1f}  worst {:.1f}  hitches {}",
              double(count) / secs, count, secs, p50, p99, worst, hitches);
  count = 0;
}
}  // namespace

void D3D12CommandProcessor::NoteTextureLoad(uint64_t guest_bytes) {
  ++g_frame_textures_loaded;
  g_frame_texture_bytes += guest_bytes;
}

uint32_t D3D12CommandProcessor::FrameTextureLoads() const { return g_frame_textures_loaded; }

bool D3D12CommandProcessor::HasPendingResolveReadback(uint32_t address, uint32_t length) const {
  const uint64_t end = uint64_t(address) + length;
  for (const PendingResolveReadback& p : pending_resolve_readbacks_) {
    if (p.address < end && uint64_t(p.address) + p.length > address) return true;
  }
  return false;
}

uint32_t D3D12CommandProcessor::LandCompletedResolveReadback(uint32_t address, uint32_t length) {
  if (pending_resolve_readbacks_.empty()) return 0;
  const uint64_t end = uint64_t(address) + length;
  const uint64_t completed = GetCompletedSubmission();
  uint32_t landed = 0;
  size_t kept = 0;
  for (size_t i = 0; i < pending_resolve_readbacks_.size(); ++i) {
    const PendingResolveReadback p = pending_resolve_readbacks_[i];
    const bool overlaps = !(p.address >= end || uint64_t(p.address) + p.length <= address);
    if (!overlaps || p.submission > completed) {
      pending_resolve_readbacks_[kept++] = p;  // still open, or unrelated
      continue;
    }
    shared_memory_->UnprotectGpuRange(p.address, p.length);
    if (REXCVAR_GET(readback_resolve_on_demand))
      ::ng2::ngpu::rtc::GuestDataProvidersDisable(memory_, /* NATIVE PATCH */ p.address, p.length);
    // [readback] This pending copy leaves the list here either way. If it cannot
    // be landed - the buffer entry is gone, the slot is unmapped, or the copy is
    // longer than the slot - the bytes are DROPPED and guest memory keeps whatever
    // was there before, which is the stale-fill half of the impostor flash. That
    // is counted, because a silent drop here is indistinguishable from a landing.
    bool dropped = true;
    auto it = readback_buffers_.find(p.key);
    if (it != readback_buffers_.end()) {
      ReadbackBuffer& rb = it->second;
      if (rb.buffers[p.index] && rb.mapped_data[p.index] && p.length <= rb.sizes[p.index]) {
        CopyToGuestMemory(p.address, rb.mapped_data[p.index], p.length);
        ++landed;
        dropped = false;
      }
    }
    if (dropped) g_superseded_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  pending_resolve_readbacks_.resize(kept);
  if (landed) g_impostor_readbacks_landed.fetch_add(landed, std::memory_order_relaxed);
  return landed;
}

void D3D12CommandProcessor::LandImpostorReadbackBeforeUpload(uint32_t address, uint32_t length) {
  if (!length || !REXCVAR_GET(readback_land_before_texture_upload)) return;
  if (!HasPendingResolveReadback(address, length)) return;
  LandCompletedResolveReadback(address, length);
  if (!REXCVAR_GET(readback_await_before_texture_upload)) return;
  // [readback] The load reads CPU memory only for pages that are not valid in
  // the GPU buffer; a resolve target's pages are valid (GPU-written) after the
  // resolve, so nothing stale can reach the texture - no wait (s92: the wait
  // for every overlapping load cost 1.3 s per 5 s in the market).
  // This asked AllPagesValid, which was the same question only while
  // clear_memory_page_state was off. With it ON - which Ninja Gaiden II
  // REQUIRES and Fable II turned off - the per-frame refresh drops the valid
  // bit from every page not written by the GPU, so this fast path stopped
  // matching and almost every texture upload fell through to a mid-draw GPU
  // wait instead. GPU-written is the property the argument above needs, and
  // it is the one the refresh preserves.
  const bool fast_path_ok =
      shared_memory_ && (REXCVAR_GET(readback_fast_path_gpu_written)
                             ? shared_memory_->AllPagesGpuWritten(address, length)
                             : shared_memory_->AllPagesValid(address, length));
  if (fast_path_ok) {
    g_readbacks_valid_no_wait.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // [readback] A copy still in flight from an EARLIER submission: wait for that
  // submission only (not the whole queue, as the resolve-time drain did), land
  // it, and the upload below reads fresh bytes. A copy issued in the still-open
  // submission cannot be awaited from inside a draw (its render targets are
  // bound and ending the submission here would drop them); it is counted.
  uint64_t await = 0;
  uint32_t open = 0;
  const uint64_t end = uint64_t(address) + length;
  for (const PendingResolveReadback& p : pending_resolve_readbacks_) {
    if (p.address >= end || uint64_t(p.address) + p.length <= address) continue;
    if (p.submission >= submission_current_) {
      ++open;
      continue;
    }
    await = std::max(await, p.submission);
  }
  if (await) {
    const char* previous_reason = fence_reason_;  // FenceReasonScope is defined below
    fence_reason_ = "readback before upload";
    const auto t0 = std::chrono::steady_clock::now();
    CheckSubmissionFence(await);
    fence_reason_ = previous_reason;
    g_readbacks_awaited_us.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count()),
        std::memory_order_relaxed);
    g_readbacks_awaited_before_upload.fetch_add(1, std::memory_order_relaxed);
    LandCompletedResolveReadback(address, length);
  }
  if (open) g_readbacks_open_at_upload.fetch_add(open, std::memory_order_relaxed);
}

void D3D12CommandProcessor::NoteFreshResolve(uint32_t address, uint32_t length) {
  if (!length || !REXCVAR_GET(readback_resolve_split_before_load)) return;
  if (fresh_resolve_ranges_.size() < 4096) {
    fresh_resolve_ranges_.emplace_back(address, address + length);
  }
}

bool D3D12CommandProcessor::RangeResolvedInOpenSubmission(uint32_t address,
                                                          uint32_t length) const {
  if (!length || !submission_open_ || fresh_resolve_ranges_.empty()) return false;
  const uint64_t end = uint64_t(address) + length;
  for (const auto& r : fresh_resolve_ranges_) {
    if (r.first < end && uint64_t(r.second) > address) return true;
  }
  return false;
}

bool D3D12CommandProcessor::SplitSubmissionForFreshResolve() {
  if (!submission_open_) return false;
  if (!EndSubmission(false)) return false;
  if (!BeginSubmission(true)) return false;
  rt_rebind_after_split_ = true;
  g_splits_before_load.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void D3D12CommandProcessor::LandOrAwaitReadbackForUpload(uint32_t address, uint32_t length) {
  if (!length || pending_resolve_readbacks_.empty()) return;
  if (!HasPendingResolveReadback(address, length)) return;
  const uint32_t landed = LandCompletedResolveReadback(address, length);
  if (landed) g_upload_landed.fetch_add(landed, std::memory_order_relaxed);
  uint64_t await = 0;
  uint32_t open = 0;
  const uint64_t end = uint64_t(address) + length;
  for (const PendingResolveReadback& p : pending_resolve_readbacks_) {
    if (p.address >= end || uint64_t(p.address) + p.length <= address) continue;
    if (p.submission >= submission_current_) {
      ++open;
      continue;
    }
    await = std::max(await, p.submission);
  }
  if (await) {
    const char* previous_reason = fence_reason_;
    fence_reason_ = "readback before upload";
    CheckSubmissionFence(await);
    fence_reason_ = previous_reason;
    g_upload_awaited.fetch_add(1, std::memory_order_relaxed);
    const uint32_t landed_after = LandCompletedResolveReadback(address, length);
    if (landed_after) g_upload_landed.fetch_add(landed_after, std::memory_order_relaxed);
  }
  if (open) g_upload_open.fetch_add(open, std::memory_order_relaxed);
}

bool D3D12CommandProcessor::ShouldDeferTextureUpload(uint32_t address, uint32_t length) {
  if (!length || !REXCVAR_GET(readback_land_before_texture_upload)) return false;
  if (!HasPendingResolveReadback(address, length)) return false;
  LandCompletedResolveReadback(address, length);       // land whatever has finished
  return HasPendingResolveReadback(address, length);   // still pending => defer a frame
}

void D3D12CommandProcessor::NoteSharedMemoryUpload(uint64_t bytes) {
  g_frame_upload_bytes += bytes;
  g_upload_bytes_window.fetch_add(bytes, std::memory_order_relaxed);
  g_upload_copies_window.fetch_add(1, std::memory_order_relaxed);
}

void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  GpuCatScope gpu_cat(*this, kGpuCatOutput);   // NATIVE PATCH: [gpu prof]
  SCOPE_profile_cpu_f("gpu");
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;

  // [ng2-fov] Scene detection, once per guest frame, driving ng2_uw_mode (0 off,
  // 1 ultrawide gameplay, 2 pillarbox 16:9). Two signals: the FOV block counted
  // this frame's 3D world (perspective) draws, and NG2's own pause/menu flags say
  // whether the START/weapons menu is open. Ultrawide when the world is drawn and
  // the game is not paused (gameplay, in-engine cinematics, Ninpo); 16:9 for the
  // pause menu and for the world-less 2D screens (video, main menu, loading).
  // Hysteresis keeps the mode from flickering at the boundary. Inert unless the
  // ng2_fov_k cvar is active, so Fable II and everything else are unaffected.
  {
    static const int s_uw_thr3d = [] {
      const char* e = std::getenv("NG2_UW_MIN3D");
      const int v = e ? std::atoi(e) : 12;
      return v > 0 ? v : 12;
    }();
    // Guest globals NG2 sets to 1 while the in-game pause/weapons (START) menu is
    // open and clears to 0 during play. Found by live memory differencing across
    // pause/play cycles, then confirmed by a 296k-poll watch through combat,
    // jumps and Ninpo casts: these two are 1 ONLY in the pause menu and stayed 0
    // through everything that must remain ultrawide (gameplay, in-engine
    // cinematics, Ninpo). Requiring BOTH keeps a stray bit from ever pillarboxing
    // a cutscene. Addresses are specific to this NG2 build/region. Set
    // NG2_UW_NOPAUSE=1 to fall back to the pure draw-count path below.
    static const uint32_t kNg2PauseFlagA = 0x84C29930u;
    static const uint32_t kNg2PauseFlagB = 0x84C39A4Cu;
    static const bool s_uw_use_pause = std::getenv("NG2_UW_NOPAUSE") == nullptr;
    const int cnt3d = g_ng2_persp_this_frame;
    const int cnt2d = g_ng2_2d_this_frame;
    const int cnt2d_solid = g_ng2_2d_solid_this_frame;  // [ng2-fade] diagnostic
    g_ng2_persp_this_frame = 0;
    g_ng2_2d_this_frame = 0;
    g_ng2_2d_solid_this_frame = 0;
    const double k = REXCVAR_GET(ng2_fov_k);
    const bool feature = k > 0.05 && k < 1.5 && std::fabs(k - 1.0) > 1e-3;
    auto rd_guest_be32 = [&](uint32_t va) -> uint32_t {
      const uint8_t* p =
          memory_ ? memory_->TranslateVirtual<const uint8_t*>(va) : nullptr;
      if (!p) return 0;
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
             (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    const bool pause_menu = s_uw_use_pause &&
                            rd_guest_be32(kNg2PauseFlagA) == 1u &&
                            rd_guest_be32(kNg2PauseFlagB) == 1u;
    // The 3D world is on screen this frame. Gameplay, an in-engine cinematic and
    // a Ninpo cast all render it; a full-screen video, the main menu, a chapter
    // card, a loading screen and a black scene-transition fade do not.
    const bool has_world = cnt3d >= s_uw_thr3d;
    // A frame is ULTRAWIDE gameplay when the world is drawn and the pause menu is
    // not open. That keeps gameplay, in-engine cinematics and Ninpo ultrawide
    // (world present, no pause) and pillarboxes the pause/weapons menu (world
    // present but paused), the chapter card, the front-end menus, full-screen
    // video/loading and the black cross-fades between scenes. The old draw-count
    // RATIO test is gone: it could not tell a paused weapons menu (world still
    // rendered behind it) from a cinematic or a Ninpo 2D-effect spike - which is
    // exactly what used to flip those to 16:9.
    //
    // NOTE on the scene-transition fades: they intentionally stay 16:9
    // (pillarboxed). NG2 draws a fade as a 2D layer at its own 16:9 extent, so
    // forcing the fade to fill an ultrawide screen (mode 1) leaves the FOV-widened
    // 3D scene showing through on the sides around the centered 16:9 black. Making
    // a fade fill the width cleanly would need to widen that 2D fade layer without
    // touching the HUD/menus (which must stay 16:9) - a dedicated change left as
    // future work. See [[ng2-ultrawide-fov]].
    const bool frame_gameplay = has_world && !pause_menu;
    static bool s_uw_gameplay = false;
    static int s_uw_gp_streak = 0;
    static int s_uw_menu_streak = 0;
    if (frame_gameplay) {
      ++s_uw_gp_streak;
      s_uw_menu_streak = 0;
    } else {
      ++s_uw_menu_streak;
      s_uw_gp_streak = 0;
    }
    // Enter gameplay quickly (2 frames); leave it only after a sustained non-
    // gameplay signal (10 frames, ~0.16 s) so a brief 3D-less blip during play - a
    // streaming gap or a one-frame full-screen effect - does not flash 16:9. A
    // real menu, video, card or loading screen lasts far longer than 10 frames.
    if (!s_uw_gameplay && s_uw_gp_streak >= 2) {
      s_uw_gameplay = true;
    } else if (s_uw_gameplay && s_uw_menu_streak >= 10) {
      s_uw_gameplay = false;
    }
    const int mode = !feature ? 0 : (s_uw_gameplay ? 1 : 2);
    REXCVAR_SET(ng2_uw_mode, mode);
    if (std::getenv("NG2_DUMP_VSCONST")) {
      static int s_uw_dbg = 0;
      static int s_uw_last_logged_mode = -1;
      // Log every mode CHANGE the instant it happens (so a brief flip during a
      // Ninpo cast or a cinematic is never missed), plus a periodic heartbeat.
      if (mode != s_uw_last_logged_mode) {
        s_uw_last_logged_mode = mode;
        REXLOG_INFO("[ng2uw] CHANGE -> mode={} (3d={} 2d={} solid2d={} pause={} thr3d={})",
                    mode, cnt3d, cnt2d, cnt2d_solid, pause_menu ? 1 : 0, s_uw_thr3d);
      } else if ((s_uw_dbg++ % 60) == 0) {
        REXLOG_INFO("[ng2uw] 3d={} 2d={} solid2d={} pause={} thr3d={} feature={} mode={}",
                    cnt3d, cnt2d, cnt2d_solid, pause_menu ? 1 : 0, s_uw_thr3d, feature, mode);
      }
    }
  }

  if (!graphics_system_)
    return;
  // NATIVE PATCH: no presenter check - NativeRefreshGuestOutput (below) stands in for the presenter.

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXGPU_ERROR("IssueSwap: BeginSubmission failed");
    return;
  }

  // Obtain the actual swap source texture size (resolution-scaled if it's a
  // resolve destination, or not otherwise).
  D3D12_SHADER_RESOURCE_VIEW_DESC swap_texture_srv_desc;
  xenos::TextureFormat frontbuffer_format;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  ID3D12Resource* swap_texture_resource =
      texture_cache_->RequestSwapTexture(swap_texture_srv_desc, frontbuffer_format,
                                         &frontbuffer_width_unscaled, &frontbuffer_height_unscaled);
  if (!swap_texture_resource) {
    // Dump texture fetch constant 0 for debugging
    const auto& regs = *register_file_;
    auto fetch = regs.GetTextureFetch(0);
    REXGPU_ERROR(
        "IssueSwap: RequestSwapTexture failed - fetch0: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
        fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    return;
  }
  D3D12_RESOURCE_DESC swap_texture_desc = swap_texture_resource->GetDesc();
  // The swap gamma / FXAA pass samples source texels by pixel index, but swap
  // textures may be allocation-padded. Prefer the active frontbuffer region
  // from the swap packet, scaled proportionally to the actual source texture.
  uint32_t source_width_scaled = uint32_t(swap_texture_desc.Width);
  uint32_t source_height_scaled = uint32_t(swap_texture_desc.Height);
  auto get_active_swap_dimension = [](uint32_t packet_unscaled, uint32_t source_unscaled,
                                      uint32_t source_scaled) -> uint32_t {
    if (!source_scaled) {
      return 0;
    }
    uint32_t active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      uint64_t active_scaled =
          (uint64_t(active_unscaled) * source_scaled + (source_unscaled >> 1)) / source_unscaled;
      return uint32_t(std::clamp<uint64_t>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  uint32_t guest_output_width =
      get_active_swap_dimension(frontbuffer_width, frontbuffer_width_unscaled, source_width_scaled);
  uint32_t guest_output_height = get_active_swap_dimension(
      frontbuffer_height, frontbuffer_height_unscaled, source_height_scaled);
  if (!guest_output_width) {
    guest_output_width = source_width_scaled
                             ? source_width_scaled
                             : (frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled);
  }
  if (!guest_output_height) {
    guest_output_height = source_height_scaled ? source_height_scaled
                                               : (frontbuffer_height ? frontbuffer_height
                                                                     : frontbuffer_height_unscaled);
  }
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&
                            (source_width_scaled != frontbuffer_width_unscaled ||
                             source_height_scaled != frontbuffer_height_unscaled);
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXGPU_WARN(
          "D3D12 draw resolution scaling is enabled, but the swap source is "
          "unscaled ({}x{}). This title may be presenting from an unscaled "
          "resolve path.",
          guest_output_width, guest_output_height);
    }
  }

  g_frame_draws_snapshot = frame_draws_;  // [hitch]
  ReportGuestSwapRate();
  // [scene] The frame's draw counts go out before the presenter gets it.
  REXCVAR_SET(gpu_frame_draws, int32_t(frame_draws_));
  REXCVAR_SET(gpu_frame_depth_draws, int32_t(frame_depth_draws_));
  frame_draws_ = 0;
  frame_depth_draws_ = 0;
  // [dd] Per-draw dump: close out a frame, start a dump the app asked for.
  {
    if (dd_file_) {
      std::fprintf(dd_file_, "# swap after frame %u (%u draws)\n", dd_frame_, dd_draw_);
      ++dd_frame_;
      if (dd_frames_left_) --dd_frames_left_;
      if (!dd_frames_left_) {
        std::fclose(dd_file_);
        dd_file_ = nullptr;
        REXLOG_INFO("[dd] draw dump done after {} frames", dd_frame_);
      }
    }
    const int32_t want = REXCVAR_GET(gpu_draw_dump_frames);
    if (want > 0 && !dd_file_) {
      const std::string path = rex::cvar::Query<std::string>("gpu_draw_dump_file");
      dd_file_ = path.empty() ? nullptr : std::fopen(path.c_str(), "w");
      dd_frames_left_ = dd_file_ ? uint32_t(want) : 0u;
      dd_frame_ = 0;
      REXCVAR_SET(gpu_draw_dump_frames, 0);
      REXLOG_INFO("[dd] draw dump: {} frames to '{}'{}", want, path, dd_file_ ? "" : " (open failed)");
    }
    dd_draw_ = 0;
  }

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  ::ng2::ngpu::rtc::NativeRefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, &swap_texture_srv_desc, frontbuffer_format, swap_texture_resource, guest_output_width,
       guest_output_height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
        ID3D12Device* device = provider.GetDevice();

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;
        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc = fxaa_source_texture_->GetDesc();
            if (fxaa_source_texture_desc.Width != guest_output_width ||
                fxaa_source_texture_desc.Height != guest_output_height) {
              if (submission_completed_ < fxaa_source_texture_submission_) {
                fxaa_source_texture_->AddRef();
                resources_for_deletion_.emplace_back(fxaa_source_texture_submission_,
                                                     fxaa_source_texture_.Get());
              }
              fxaa_source_texture_.Reset();
              fxaa_source_texture_submission_ = 0;
            }
          }
          if (!fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc;
            fxaa_source_texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            fxaa_source_texture_desc.Alignment = 0;
            fxaa_source_texture_desc.Width = guest_output_width;
            fxaa_source_texture_desc.Height = guest_output_height;
            fxaa_source_texture_desc.DepthOrArraySize = 1;
            fxaa_source_texture_desc.MipLevels = 1;
            fxaa_source_texture_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_texture_desc.SampleDesc.Count = 1;
            fxaa_source_texture_desc.SampleDesc.Quality = 0;
            fxaa_source_texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            fxaa_source_texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(
                    &ui::ngpu_d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
                    &fxaa_source_texture_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    nullptr, IID_PPV_ARGS(&fxaa_source_texture_)))) {
              REXGPU_ERROR("Failed to create the FXAA input texture");
              swap_post_effect = SwapPostEffect::kNone;
              use_fxaa = false;
            }
          }
        }

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Upload the new gamma ramp, using the upload buffer for the current
        // frame (will close the frame after this anyway, so can't write
        // multiple times per frame).
        if (!(use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                                 : gamma_ramp_256_entry_table_up_to_date_)) {
          uint32_t gamma_ramp_offset_bytes = use_pwl_gamma_ramp ? 256 * 4 : 0;
          uint32_t gamma_ramp_upload_offset_bytes =
              uint32_t(frame_current_ % kQueueFrames) * ((256 + 128 * 3) * 4) +
              gamma_ramp_offset_bytes;
          uint32_t gamma_ramp_size_bytes = (use_pwl_gamma_ramp ? 128 * 3 : 256) * 4;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload_buffer = reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_buffer_entry =
                  gamma_ramp_pwl_upload_buffer[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_buffer_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_buffer_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes,
                        use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                           : static_cast<const void*>(gamma_ramp_256_entry_table()),
                        gamma_ramp_size_bytes);
          }
          PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                                D3D12_RESOURCE_STATE_COPY_DEST);
          gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              gamma_ramp_buffer_.Get(), gamma_ramp_offset_bytes, gamma_ramp_upload_buffer_.Get(),
              gamma_ramp_upload_offset_bytes, gamma_ramp_size_bytes);
          (use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                              : gamma_ramp_256_entry_table_up_to_date_) = true;
        }

        // Destination, source, and if bindful, gamma ramp.
        ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptors[3];
        ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptor_gamma_ramp;
        if (!RequestOneUseSingleViewDescriptors(bindless_resources_used_ ? 2 : 3,
                                                apply_gamma_descriptors)) {
          return false;
        }
        // Must not call anything that can change the descriptor heap from now
        // on!
        if (bindless_resources_used_) {
          apply_gamma_descriptor_gamma_ramp = GetSystemBindlessViewHandlePair(
              use_pwl_gamma_ramp ? SystemBindlessView::kGammaRampPWLSRV
                                 : SystemBindlessView::kGammaRampTableSRV);
        } else {
          apply_gamma_descriptor_gamma_ramp = apply_gamma_descriptors[2];
          WriteGammaRampSRV(use_pwl_gamma_ramp, apply_gamma_descriptor_gamma_ramp.first);
        }

        ID3D12Resource* guest_output_resource =
            static_cast<ui::ngpu_d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
                .resource_uav_capable();

        if (use_fxaa) {
          fxaa_source_texture_submission_ = submission_current_;
        }

        ID3D12Resource* apply_gamma_dest =
            use_fxaa ? fxaa_source_texture_.Get() : guest_output_resource;
        D3D12_RESOURCE_STATES apply_gamma_dest_initial_state =
            use_fxaa ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                     : ui::ngpu_d3d12::D3D12Presenter::kGuestOutputInternalState;
        static_cast<ui::ngpu_d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
            .resource_uav_capable();
        PushTransitionBarrier(apply_gamma_dest, apply_gamma_dest_initial_state,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // From now on, even in case of failure, apply_gamma_dest must be
        // transitioned back to apply_gamma_dest_initial_state!
        D3D12_UNORDERED_ACCESS_VIEW_DESC apply_gamma_dest_uav_desc;
        apply_gamma_dest_uav_desc.Format =
            use_fxaa ? kFxaaSourceTextureFormat : ui::ngpu_d3d12::D3D12Presenter::kGuestOutputFormat;
        apply_gamma_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        apply_gamma_dest_uav_desc.Texture2D.MipSlice = 0;
        apply_gamma_dest_uav_desc.Texture2D.PlaneSlice = 0;
        device->CreateUnorderedAccessView(apply_gamma_dest, nullptr, &apply_gamma_dest_uav_desc,
                                          apply_gamma_descriptors[0].first);

        device->CreateShaderResourceView(swap_texture_resource, &swap_texture_srv_desc,
                                         apply_gamma_descriptors[1].first);

        PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        deferred_command_list_.D3DSetComputeRootSignature(apply_gamma_root_signature_.Get());
        ApplyGammaConstants apply_gamma_constants;
        apply_gamma_constants.size[0] = guest_output_width;
        apply_gamma_constants.size[1] = guest_output_height;
        deferred_command_list_.D3DSetComputeRoot32BitConstants(
            UINT(ApplyGammaRootParameter::kConstants),
            sizeof(apply_gamma_constants) / sizeof(uint32_t), &apply_gamma_constants, 0);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kDestination), apply_gamma_descriptors[0].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kSource), apply_gamma_descriptors[1].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kRamp), apply_gamma_descriptor_gamma_ramp.second);
        ID3D12PipelineState* apply_gamma_pipeline;
        if (use_pwl_gamma_ramp) {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_pwl_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_pwl_pipeline_.Get();
        } else {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_table_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_table_pipeline_.Get();
        }
        SetExternalPipeline(apply_gamma_pipeline);
        SubmitBarriers();
        uint32_t group_count_x = (guest_output_width + 15) / 16;
        uint32_t group_count_y = (guest_output_height + 7) / 8;
        deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);

        // Apply FXAA.
        if (use_fxaa) {
          // Destination and source.
          ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair fxaa_descriptors[2];
          if (!RequestOneUseSingleViewDescriptors(uint32_t(rex::countof(fxaa_descriptors)),
                                                  fxaa_descriptors)) {
            // Failed to obtain descriptors for FXAA - just copy after gamma
            // ramp application without applying FXAA.
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
            PushTransitionBarrier(guest_output_resource,
                                  ui::ngpu_d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
            SubmitBarriers();
            deferred_command_list_.D3DCopyResource(guest_output_resource, apply_gamma_dest);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                  ui::ngpu_d3d12::D3D12Presenter::kGuestOutputInternalState);
            return false;
          } else {
            assert_true(apply_gamma_dest_initial_state ==
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource,
                                  ui::ngpu_d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // From now on, even in case of failure, guest_output_resource must
            // be transitioned back to kGuestOutputInternalState!
            deferred_command_list_.D3DSetComputeRootSignature(fxaa_root_signature_.Get());
            FxaaConstants fxaa_constants;
            fxaa_constants.size[0] = guest_output_width;
            fxaa_constants.size[1] = guest_output_height;
            fxaa_constants.size_inv[0] = 1.0f / float(fxaa_constants.size[0]);
            fxaa_constants.size_inv[1] = 1.0f / float(fxaa_constants.size[1]);
            deferred_command_list_.D3DSetComputeRoot32BitConstants(
                UINT(FxaaRootParameter::kConstants), sizeof(fxaa_constants) / sizeof(uint32_t),
                &fxaa_constants, 0);
            D3D12_UNORDERED_ACCESS_VIEW_DESC fxaa_dest_uav_desc;
            fxaa_dest_uav_desc.Format = ui::ngpu_d3d12::D3D12Presenter::kGuestOutputFormat;
            fxaa_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            fxaa_dest_uav_desc.Texture2D.MipSlice = 0;
            fxaa_dest_uav_desc.Texture2D.PlaneSlice = 0;
            device->CreateUnorderedAccessView(guest_output_resource, nullptr, &fxaa_dest_uav_desc,
                                              fxaa_descriptors[0].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kDestination), fxaa_descriptors[0].second);
            D3D12_SHADER_RESOURCE_VIEW_DESC fxaa_source_srv_desc;
            fxaa_source_srv_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            fxaa_source_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            fxaa_source_srv_desc.Texture2D.MostDetailedMip = 0;
            fxaa_source_srv_desc.Texture2D.MipLevels = 1;
            fxaa_source_srv_desc.Texture2D.PlaneSlice = 0;
            fxaa_source_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(fxaa_source_texture_.Get(), &fxaa_source_srv_desc,
                                             fxaa_descriptors[1].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kSource), fxaa_descriptors[1].second);
            SetExternalPipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                    ? fxaa_extreme_pipeline_.Get()
                                    : fxaa_pipeline_.Get());
            SubmitBarriers();
            deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  ui::ngpu_d3d12::D3D12Presenter::kGuestOutputInternalState);
          }
        } else {
          assert_true(apply_gamma_dest_initial_state ==
                      ui::ngpu_d3d12::D3D12Presenter::kGuestOutputInternalState);
          PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                apply_gamma_dest_initial_state);
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue.
        SubmitBarriers();
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
}

void D3D12CommandProcessor::OnPrimaryBufferEnd() {
  if (REXCVAR_GET(d3d12_submit_on_primary_buffer_end) && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* D3D12CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

// [diag] Which silent failure a draw hit; rate-limited per reason.
static void DrawFailReason(const char* what) {
  static std::atomic<uint32_t> counts[16];
  static const char* names[16] = {};
  uint32_t slot = 0;
  for (; slot < 16; ++slot) {
    if (names[slot] == what) break;
    if (names[slot] == nullptr) { names[slot] = what; break; }
  }
  if (slot >= 16) return;
  const uint32_t n = ++counts[slot];
  if (n == 1 || n % 500 == 0) REXGPU_ERROR("[diag] draw failed: {} ({} so far)", what, n);
}

bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  if (!g_cost_draw_thread) g_cost_draw_thread = DupCurrentThread();   // NATIVE PATCH: [cpu cost]
  GpuCatScope gpu_cat(*this, kGpuCatDraw);   // NATIVE PATCH: [gpu prof]
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const RegisterFile& regs = *register_file_;

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    // Tell a bridge consumer where this resolve sits in the draw stream. Here
    // rather than in the guest's D3D9 hook, because this runs on the ring
    // thread in packet order alongside the draws the consumer records.
    RexNgpuNotifyResolve();
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  // Vertex shader analysis.
  auto vertex_shader = static_cast<D3D12Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    ++draw_census_.no_vertex_shader;
    ReportDrawCensus();
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // [ngpu-pm4] What the ring actually asks for, per frame, decoded from
  // VGT_DRAW_INITIATOR rather than from the guest API calls. This is the
  // independent denominator the native path's coverage metric cannot have:
  // that one counts only the draws passing through the wrappers it hooks.
  {
    static const bool pm4_on = std::getenv("NGPU_PM4") != nullptr;
    if (pm4_on) {
      static uint32_t n_dma = 0, n_imm = 0, n_auto = 0, n_i32 = 0, n_total = 0;
      static std::map<uint32_t, uint32_t> prims;
      static uint64_t last_frame = 0;
      const auto init = regs.Get<reg::VGT_DRAW_INITIATOR>();
      ++n_total;
      switch (uint32_t(init.source_select)) {
        case 0: ++n_dma; break;
        case 1: ++n_imm; break;
        default: ++n_auto; break;
      }
      if (index_buffer_info && index_buffer_info->format == xenos::IndexFormat::kInt32) ++n_i32;
      ++prims[uint32_t(primitive_type)];
      if (frame_current_ != last_frame && (frame_current_ % 300) == 150) {
        last_frame = frame_current_;
        std::string ps;
        for (const auto& kv : prims) { char b[32]; std::snprintf(b, sizeof(b), " prim%u=%u", kv.first, kv.second); ps += b; }
        REXGPU_INFO("[ngpu-pm4] the ring asked for {} draws: {} from an index buffer, {} inline, {} auto-index, {} with 32-bit indices;{}",
                    n_total, n_dma, n_imm, n_auto, n_i32, ps);
        n_dma = n_imm = n_auto = n_i32 = n_total = 0;
        prims.clear();
      }
    }
  }

  // [ngpu-bases] Which EDRAM bases this title actually uses, DECODED rather than
  // read off the raw register. A native path that reports base 0 everywhere
  // while this reports several has a decode bug, not a single-base title.
  {
    static const bool bases_on = std::getenv("NGPU_BASES") != nullptr;
    if (bases_on) {
      static std::set<uint32_t> seen_color, seen_depth;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t b = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_base;
        if (seen_color.insert(b).second)
          REXGPU_INFO("[ngpu-bases] a new COLOUR base: slot {} base {} tiles (bases so far {})", i, b, seen_color.size());
      }
      const uint32_t db = regs.Get<reg::RB_DEPTH_INFO>().depth_base;
      if (seen_depth.insert(db).second)
        REXGPU_INFO("[ngpu-bases] a new DEPTH base: {} tiles (bases so far {})", db, seen_depth.size());
    }
  }

  // [ngpu-census] Every draw this renderer executes, tallied by the vertex
  // shader that runs it, so the native D3D12-HLE path can be diffed against it
  // and told which draws it never sees. Fingerprinted by microcode length and
  // first dwords rather than by a hash, so the two sides need share no hashing
  // convention. Off unless NGPU_DRAW_CENSUS is set in the environment.
  {
    static const bool census_on = std::getenv("NGPU_DRAW_CENSUS") != nullptr;
    if (census_on) {
      struct Row { uint32_t draws, indices, first, dwords, prim, color; uint64_t prefix; };
      static std::map<uint64_t, Row> rows;
      static uint64_t last_report_frame = 0;
      const uint32_t dwords = uint32_t(vertex_shader->ucode_dword_count());
      const uint32_t first = dwords ? vertex_shader->ucode_dwords()[0] : 0u;
      Row& r = rows[vertex_shader->ucode_data_hash()];
      ++r.draws;
      r.indices += index_count;
      r.first = first;
      r.dwords = dwords;
      r.prim = uint32_t(primitive_type);
      // A fixed-length prefix, so the two renderers hash the same bytes
      // whatever each believes the microcode's full length to be.
      r.prefix = XXH3_64bits(vertex_shader->ucode_dwords(), std::min<size_t>(dwords, 64) * sizeof(uint32_t));
      r.color = regs.Get<reg::RB_COLOR_INFO>().value;
      if (frame_current_ != last_report_frame && (frame_current_ % 300) == 150) {
        last_report_frame = frame_current_;
        for (const auto& kv : rows)
          REXGPU_INFO("[ngpu-census] shader {:016X} prefix {:016X} ucode {} dwords first {:08X}: {} draws, {} indices, primitive {}, colour {:08X}",
                      kv.first, kv.second.prefix, kv.second.dwords, kv.second.first, kv.second.draws, kv.second.indices, kv.second.prim, kv.second.color);
        REXGPU_INFO("[ngpu-census] frame {} executed {} distinct vertex shaders", frame_current_, rows.size());
        rows.clear();
      }
    }
  }

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    ++draw_census_.surface_pitch_zero;
    // Doesn't actually draw.
    // Unlikely that zero would even really be legal though.
    return true;
  }
  D3D12Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<D3D12Shader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  if (!BeginSubmission(true)) {
    DrawFailReason("BeginSubmission");
    return false;
  }

  // Process primitives.
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    DrawFailReason("primitive processing");
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    ++draw_census_.no_host_vertices;
    // Nothing to draw.
    return true;
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  // Shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader
          ? pipeline_cache_->GetCurrentPixelShaderModification(
                *pixel_shader, interpolator_mask, ps_param_gen_pos, normalized_depth_control)
          : DxbcShaderTranslator::Modification(0);

  // Set up the render targets - this may perform dispatches and draws.
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    DrawFailReason("render target update");
    return false;
  }

  // Create the pipeline (for this, need the actually used render target formats
  // from the render target cache), translating the shaders - doing this now to
  // obtain the used textures.
  D3D12Shader::D3D12Translation* vertex_shader_translation =
      static_cast<D3D12Shader::D3D12Translation*>(
          vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  D3D12Shader::D3D12Translation* pixel_shader_translation =
      pixel_shader ? static_cast<D3D12Shader::D3D12Translation*>(
                         pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                   : nullptr;
  uint32_t bound_depth_and_color_render_target_bits;
  uint32_t bound_depth_and_color_render_target_formats[1 + xenos::kMaxColorRenderTargets];
  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  if (host_render_targets_used) {
    bound_depth_and_color_render_target_bits =
        render_target_cache_->GetLastUpdateBoundRenderTargets(
            bound_depth_and_color_render_target_formats);
  } else {
    bound_depth_and_color_render_target_bits = 0;
  }
  void* pipeline_handle;
  ID3D12RootSignature* root_signature;
  if (!pipeline_cache_->ConfigurePipeline(
          vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
          normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,
          bound_depth_and_color_render_target_formats, &pipeline_handle, &root_signature)) {
    return false;
  }
  if (REXCVAR_GET(async_shader_compilation) &&
      pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) == nullptr) {
    ++draw_census_.pipeline_not_ready;
    ReportDrawCensus();
    return true;
  }

  // Update the textures - this may bind pipelines.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  rt_rebind_after_split_ = false;
  texture_cache_->RequestTextures(used_texture_mask);
  if (rt_rebind_after_split_) {
    // [split] A texture load ended the submission: the render targets bound
    // above belong to the closed one - bind them again in the new one.
    rt_rebind_after_split_ = false;
    if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                      normalized_color_mask, *vertex_shader)) {
      DrawFailReason("render target re-bind after split");
      return false;
    }
  }

  // Bind the pipeline after configuring it and doing everything that may bind
  // other pipelines.
  if (current_guest_pipeline_ != pipeline_handle) {
    deferred_command_list_.SetPipelineStateHandle(reinterpret_cast<void*>(pipeline_handle));
    current_guest_pipeline_ = pipeline_handle;
    current_external_pipeline_ = nullptr;
  }

  // Get dynamic rasterizer state.
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  bool convert_z_to_float24 =
      host_render_targets_used && render_target_cache_->depth_float24_convert_in_pixel_shader();
  bool ps_writes_depth = pixel_shader && pixel_shader->writes_depth();

  // Build a cache key from all viewport-affecting state to skip redundant
  // recalculation when the viewport registers haven't changed between draws.
  ViewportCacheKey viewport_key;
  viewport_key.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
  viewport_key.pa_cl_vte_cntl = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
  viewport_key.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  viewport_key.pa_su_vtx_cntl = regs[XE_GPU_REG_PA_SU_VTX_CNTL];
  viewport_key.pa_sc_window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  viewport_key.normalized_depth_control = normalized_depth_control.value;
  std::memcpy(viewport_key.vport_regs, &regs[XE_GPU_REG_PA_CL_VPORT_XSCALE],
              sizeof(viewport_key.vport_regs));
  viewport_key.flags = (uint32_t(convert_z_to_float24) << 0) |
                       (uint32_t(host_render_targets_used) << 1) | (uint32_t(ps_writes_depth) << 2);

  draw_util::ViewportInfo viewport_info;
  if (viewport_cache_valid_ && viewport_key == previous_viewport_key_) {
    viewport_info = previous_viewport_info_;
  } else {
    draw_util::GetHostViewportInfo(regs, draw_resolution_scale_x, draw_resolution_scale_y, true,
                                   D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX, false,
                                   normalized_depth_control, convert_z_to_float24,
                                   host_render_targets_used, ps_writes_depth, viewport_info);
    previous_viewport_key_ = viewport_key;
    previous_viewport_info_ = viewport_info;
    viewport_cache_valid_ = true;
  }

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  // Update viewport, scissor, blend factor and stencil reference.
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal, normalized_depth_control);

  // Update system constants before uploading them.
  // TODO(Triang3l): With ROV, pass the disabled render target mask for safety.
  UpdateSystemConstantValues(memexport_used, primitive_polygonal,
                             primitive_processing_result.line_loop_closing_index,
                             primitive_processing_result.host_shader_index_endian, viewport_info,
                             used_texture_mask, normalized_depth_control, normalized_color_mask);

  // Update constant buffers, descriptors and root parameters.
  uw_diag_vertex_count_ = primitive_processing_result.host_draw_vertex_count;
  uw_ppr_ = &primitive_processing_result;
  if (dd_file_) {
    DrawDumpLine(vertex_shader, pixel_shader, primitive_processing_result, viewport_info, scissor,
                 normalized_depth_control);
  }
  if (!UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used)) {
    uw_ppr_ = nullptr;
    DrawFailReason("UpdateBindings");
    return false;
  }
  uw_ppr_ = nullptr;
  // Must not call anything that can change the descriptor heap from now on!

  // Ensure vertex buffers are resident.
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
              "This is incorrect behavior, but you can try bypassing this by "
              "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      VertexBufferState& state = vertex_buffer_states_[vfetch_index];
      if (state.address == vfetch_constant.address && state.size == vfetch_constant.size) {
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
        continue;
      }
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR(
            "Failed to request vertex buffer at 0x{:08X} (size {}) in the "
            "shared memory",
            vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
      state.address = vfetch_constant.address;
      state.size = vfetch_constant.size;
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }

  // Gather memexport ranges and ensure the heaps for them are resident, and
  // also load the data surrounding the export and to fill the regions that
  // won't be modified by the shaders.
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    if (!shared_memory_->RequestRange(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes)) {
      REXGPU_ERROR(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
      return false;
    }
  }
  if (memexport_used && memexport_ranges_.empty()) {
    if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
      REXGPU_ERROR(
          "Failed to request full shared memory residency for unresolved "
          "memexport destinations");
      return false;
    }
  }

  // Primitive topology.
  D3D_PRIMITIVE_TOPOLOGY primitive_topology;
  if (primitive_processing_result.IsTessellated()) {
    switch (primitive_processing_result.host_primitive_type) {
      // TODO(Triang3l): Support all primitive types.
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      default:
        REXGPU_ERROR(
            "Host tessellated primitive type {} returned by the primitive "
            "processor is not supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        break;
      default:
        REXGPU_ERROR(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  }
  SetPrimitiveTopology(primitive_topology);
  // Must not call anything that may change the primitive topology from now on!

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    deferred_command_list_.TagNextDraw(
        {vertex_shader ? vertex_shader->ucode_data_hash() : 0ull,
         pixel_shader ? pixel_shader->ucode_data_hash() : 0ull, index_count,
         uint32_t(primitive_type)});
    ++frame_draws_;  // [scene]
    if (normalized_depth_control.z_enable) ++frame_depth_draws_;
    deferred_command_list_.D3DDrawInstanced(primitive_processing_result.host_draw_vertex_count, 1,
                                            0, 0);
  } else {
    D3D12_INDEX_BUFFER_VIEW index_buffer_view;
    index_buffer_view.SizeInBytes = primitive_processing_result.host_draw_vertex_count;
    if (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16) {
      index_buffer_view.SizeInBytes *= sizeof(uint16_t);
      index_buffer_view.Format = DXGI_FORMAT_R16_UINT;
    } else {
      index_buffer_view.SizeInBytes *= sizeof(uint32_t);
      index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
    }
    ID3D12Resource* scratch_index_buffer = nullptr;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
        if (memexport_used) {
          // If the shared memory is a UAV, it can't be used as an index buffer
          // (UAV is a read/write state, index buffer is a read-only state).
          // Need to copy the indices to a buffer in the index buffer state.
          scratch_index_buffer = RequestScratchGPUBuffer(index_buffer_view.SizeInBytes,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
          if (scratch_index_buffer == nullptr) {
            return false;
          }
          shared_memory_->UseAsCopySource();
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              scratch_index_buffer, 0, shared_memory_->GetBuffer(),
              primitive_processing_result.guest_index_base, index_buffer_view.SizeInBytes);
          PushTransitionBarrier(scratch_index_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_INDEX_BUFFER);
          index_buffer_view.BufferLocation = scratch_index_buffer->GetGPUVirtualAddress();
        } else {
          index_buffer_view.BufferLocation =
              shared_memory_->GetGPUAddress() + primitive_processing_result.guest_index_base;
        }
      } break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer_view.BufferLocation = primitive_processor_->GetConvertedIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer_view.BufferLocation = primitive_processor_->GetBuiltinIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return false;
    }
    deferred_command_list_.D3DIASetIndexBuffer(&index_buffer_view);
    if (memexport_used) {
      shared_memory_->UseForWriting();
    } else {
      shared_memory_->UseForReading();
    }
    SubmitBarriers();
    PROFILE_DRAW_CALL();
    PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
    deferred_command_list_.TagNextDraw(
        {vertex_shader ? vertex_shader->ucode_data_hash() : 0ull,
         pixel_shader ? pixel_shader->ucode_data_hash() : 0ull, index_count,
         uint32_t(primitive_type)});
    ++frame_draws_;  // [scene]
    if (normalized_depth_control.z_enable) ++frame_depth_draws_;
    deferred_command_list_.D3DDrawIndexedInstanced(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
    if (scratch_index_buffer != nullptr) {
      ReleaseScratchGPUBuffer(scratch_index_buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
  }

  if (memexport_used) {
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.
    // TODO(Triang3l): Find some PM4 command that can be used for indication of
    // when memexports should be awaited?
    shared_memory_->MarkUAVWritesCommitNeeded();
    // Invalidate textures in memexported memory and watch for changes.
    if (!memexport_ranges_.empty()) {
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                          memexport_range.size_bytes);
      }
    } else {
      // Stream constants can be invalid or dynamic, so exact destinations may
      // be unknown. Keep invalidation conservative in this case.
      shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
    }
    if (IsReadbackMemexportEnabled(REXCVAR_GET(d3d12_readback_memexport)) &&
        !memexport_ranges_.empty()) {
      uint32_t memexport_total_size = 0;
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        memexport_total_size += memexport_range.size_bytes;
      }
      if (memexport_total_size != 0) {
        if (REXCVAR_GET(readback_memexport_fast)) {
          IssueDraw_MemexportReadbackFastPath(memexport_total_size);
        } else {
          IssueDraw_MemexportReadbackFullPath(memexport_total_size);
        }
      }
    }
  }

  ++draw_census_.issued;
  if (memexport_used) {
    ++draw_census_.memexport;
  }
  ReportDrawCensus();
  return true;
}

void D3D12CommandProcessor::ReportDrawCensus() {
  int32_t interval = REXCVAR_GET(draw_census);
  if (interval <= 0) {
    return;
  }
  uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
  if (!draw_census_.last_report_ms) {
    draw_census_.last_report_ms = now;
    return;
  }
  if (now - draw_census_.last_report_ms < uint64_t(interval) * 1000u) {
    return;
  }
  draw_census_.last_report_ms = now;
  // REXLOG_INFO, not REXGPU_INFO: the [gpu] category is filtered out below
  // debug level, and this project has already lost time to counting [gpu]
  // lines that were never being written.
  REXLOG_INFO(
      "DRAW CENSUS: issued {} (memexport {}) | dropped: no VS {}, surface pitch 0 {}, "
      "not rasterizing + no memexport {}, no host vertices {}, PIPELINE NOT READY {}, "
      "other {}",
      draw_census_.issued, draw_census_.memexport, draw_census_.no_vertex_shader,
      draw_census_.surface_pitch_zero, draw_census_.not_rasterizing_no_memexport,
      draw_census_.no_host_vertices, draw_census_.pipeline_not_ready,
      draw_census_.other_failure);
  if (draw_census_.vs_const_uploads) {
    double values = double(draw_census_.vs_const_values);
    REXLOG_INFO(
        "VS CONSTANTS: {} uploads, {} values (max {} vec4/shader) | NaN {} ({:.2f}%), "
        "Inf {} ({:.2f}%), zero {} ({:.2f}%), |v|>1e9 {} ({:.2f}%)",
        draw_census_.vs_const_uploads, draw_census_.vs_const_values,
        draw_census_.vs_const_max_count, draw_census_.vs_const_nan,
        100.0 * double(draw_census_.vs_const_nan) / values, draw_census_.vs_const_inf,
        100.0 * double(draw_census_.vs_const_inf) / values, draw_census_.vs_const_zero,
        100.0 * double(draw_census_.vs_const_zero) / values, draw_census_.vs_const_huge,
        100.0 * double(draw_census_.vs_const_huge) / values);
    if (draw_census_.vs_const_nan) {
      // Render the affected constants as a range list, because "c48-c95" and
      // "c3, c17, c142" mean very different things.
      std::string ranges;
      int32_t run_start = -1;
      for (int32_t ci = 0; ci <= 256; ++ci) {
        bool set = ci < 256 && (draw_census_.vs_const_nan_bitmap[ci >> 6] >> (ci & 63)) & 1;
        if (set && run_start < 0) {
          run_start = ci;
        } else if (!set && run_start >= 0) {
          if (!ranges.empty()) {
            ranges += ", ";
          }
          ranges += (ci - 1 == run_start) ? fmt::format("c{}", run_start)
                                          : fmt::format("c{}-c{}", run_start, ci - 1);
          run_start = -1;
        }
      }
      uint32_t affected = 0;
      for (int i = 0; i < 4; ++i) {
        affected += uint32_t(rex::bit_count(draw_census_.vs_const_nan_bitmap[i]));
      }
      REXLOG_INFO("VS CONSTANTS NaN: {} of 256 constants affected, first was c{} | {}",
                  affected, draw_census_.vs_const_first_nan_index, ranges);
      // The pattern is the diagnosis: a fill value points at uninitialised
      // memory, a computed QNaN points at guest arithmetic.
      std::vector<std::pair<uint32_t, uint64_t>> pats(
          draw_census_.vs_const_nan_patterns.begin(),
          draw_census_.vs_const_nan_patterns.end());
      std::sort(pats.begin(), pats.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      std::string top;
      for (size_t i = 0; i < pats.size() && i < 4; ++i) {
        if (!top.empty()) {
          top += ", ";
        }
        top += fmt::format("0x{:08X} x{}", pats[i].first, pats[i].second);
      }
      REXLOG_INFO("VS CONSTANTS NaN patterns: {} distinct | {}",
                  draw_census_.vs_const_nan_patterns.size(), top);
    }
    if (draw_census_.draws_classified) {
      const auto pct = [](uint64_t n, uint64_t d) {
        return d ? 100.0 * double(n) / double(d) : 0.0;
      };
      REXLOG_INFO(
          "VS CONSTANTS per draw: {} draws | NaN {} ({:.2f}%), all-zero {} ({:.2f}%) "
          "|| memexport(skinned) {} draws | NaN {} ({:.2f}%), all-zero {} ({:.2f}%)",
          draw_census_.draws_classified, draw_census_.draws_with_nan,
          pct(draw_census_.draws_with_nan, draw_census_.draws_classified),
          draw_census_.draws_all_zero,
          pct(draw_census_.draws_all_zero, draw_census_.draws_classified),
          draw_census_.mx_draws_classified, draw_census_.mx_draws_with_nan,
          pct(draw_census_.mx_draws_with_nan, draw_census_.mx_draws_classified),
          draw_census_.mx_draws_all_zero,
          pct(draw_census_.mx_draws_all_zero, draw_census_.mx_draws_classified));
    }
  }
}

namespace {

// Scoped reason for the fence waits below.
struct FenceReasonScope {
  const char*& slot;
  const char* previous;
  FenceReasonScope(const char*& s, const char* reason) : slot(s), previous(s) { s = reason; }
  ~FenceReasonScope() { slot = previous; }
};

struct FenceWaitStat {
  const char* reason = nullptr;
  uint32_t count = 0;
  uint64_t us = 0;
};
FenceWaitStat g_fence_waits[8];
// [readback] A copy into guest memory that survives the game having freed
// that memory meanwhile (freed pages are no-access; the runtime's vectored
// handler declines faults in the physical view, so the frame handler here
// gets them). Plain C: no objects to unwind past __try.
std::atomic<uint32_t> g_guest_copy_faults{0};
std::atomic<uint32_t> g_guest_copy_quiet{0};
// [readback] On-demand waits by guest threads (any thread: atomics).
std::atomic<uint32_t> g_provider_waits{0};
std::atomic<uint64_t> g_provider_wait_us{0};
std::atomic<uint32_t> g_provider_calls{0};
auto g_fence_report_at = std::chrono::steady_clock::now();

void NoteFenceWait(const char* reason, uint64_t us) {
  for (auto& s : g_fence_waits) {
    if (s.reason == reason || s.reason == nullptr) {
      s.reason = reason;
      ++s.count;
      s.us += us;
      return;
    }
  }
}

// Every five seconds: who waited on the GPU, how often, for how long in
// total. The sampler can say "a fence wait, 37%"; only this says which one.
void MaybeReportFenceWaits() {
  const auto now = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(now - g_fence_report_at).count();
  if (secs < 5.0) return;
  std::string line;
  for (auto& s : g_fence_waits) {
    if (!s.reason) break;
    char b[96];
    std::snprintf(b, sizeof(b), "%s%s %u x %.1f ms", line.empty() ? "" : ", ", s.reason,
                  s.count, s.us / 1000.0);
    line += b;
    s.count = 0;
    s.us = 0;
  }
  {
    const uint32_t faults = g_guest_copy_faults.exchange(0);
    if (faults) {
      char b[96];
      std::snprintf(b, sizeof(b), "%sreadback copies into freed memory skipped %u",
                    line.empty() ? "" : ", ", faults);
      line += b;
    }
    const uint32_t quiet = g_guest_copy_quiet.exchange(0);
    if (quiet) {
      char b[96];
      std::snprintf(b, sizeof(b), "%sreadback copies landed quietly %u",
                    line.empty() ? "" : ", ", quiet);
      line += b;
    }
    const uint32_t splits = g_resolve_splits.exchange(0);
    if (splits) {
      char b[64];
      std::snprintf(b, sizeof(b), "%sresolve splits %u", line.empty() ? "" : ", ", splits);
      line += b;
    }
    const uint32_t landed = g_impostor_readbacks_landed.exchange(0);
    if (landed) {
      char b[80];
      std::snprintf(b, sizeof(b), "%slanded %u impostor readbacks before their texture upload",
                    line.empty() ? "" : ", ", landed);
      line += b;
    }
    {
      const uint32_t awaited = g_readbacks_awaited_before_upload.exchange(0);
      const uint64_t us = g_readbacks_awaited_us.exchange(0);
      const uint32_t open = g_readbacks_open_at_upload.exchange(0);
      const uint32_t valid = g_readbacks_valid_no_wait.exchange(0);
      {
        const uint32_t ln = g_landing_count.exchange(0);
        const uint64_t lus = g_landing_us.exchange(0);
        const uint64_t llk = g_landing_lock_us.exchange(0);
        const uint32_t sn = g_submission_count.exchange(0);
        const uint64_t sus = g_submission_us.exchange(0);
        const uint32_t splits = g_splits_before_load.exchange(0);
        const uint32_t mirrors = g_mirror_copies.exchange(0);
        const uint32_t sk = g_superseded_kept.exchange(0);
        const uint32_t sd = g_superseded_dropped.exchange(0);
        const uint32_t sw = g_superseded_waited.exchange(0);
        const uint32_t rg = g_ring_grown.exchange(0);
        {
          const uint32_t ul = g_upload_landed.exchange(0);
          const uint32_t ua = g_upload_awaited.exchange(0);
          const uint32_t uo = g_upload_open.exchange(0);
          if (ul || ua || uo) {
            char c[120];
            std::snprintf(c, sizeof(c), "%supload landed %u / awaited %u / open %u readbacks",
                          line.empty() ? "" : ", ", ul, ua, uo);
            line += c;
          }
        }
        // `sd` is in the condition deliberately. All four superseded counters are
        // exchanged above whatever happens, so a window with no landings and no
        // submissions used to zero them and print nothing - a dropped readback in
        // a quiet window vanished. A drop is the one event here that must never be
        // silently discarded, so it forces the line out on its own.
        if (ln || sn || sd) {
          char b[300];
          std::snprintf(b, sizeof(b),
                        "%slandings %u x %.1f ms (lock %.1f ms), submissions %u x %.1f ms (%u splits before a load), %u mirror copies, superseded %u landed (%u waited, %u ring grown) %u DROPPED",
                        line.empty() ? "" : ", ", ln, lus / 1000.0, llk / 1000.0, sn, sus / 1000.0, splits, mirrors, sk, sw, rg, sd);
          line += b;
        }
      }
      if (awaited || open || valid) {
        char b[160];
        std::snprintf(b, sizeof(b),
                      "%sawaited %u readbacks before upload (%.1f ms), %u still open, %u pages valid (no wait)",
                      line.empty() ? "" : ", ", awaited, us / 1000.0, open, valid);
        line += b;
      }
    }
    {
      uint32_t sz[5];
      uint32_t total = 0;
      for (size_t i = 0; i < 5; ++i) {
        sz[i] = g_resolve_size_buckets[i].exchange(0);
        total += sz[i];
      }
      if (total) {
        char b[160];
        std::snprintf(b, sizeof(b), "%sdeferred resolves <=64K %u <=256K %u <=1M %u <=4M %u >4M %u",
                      line.empty() ? "" : ", ", sz[0], sz[1], sz[2], sz[3], sz[4]);
        line += b;
      }
    }
    {
      const uint32_t dcl_total = g_dcl_command_total.exchange(0);
      if (dcl_total) {
        std::pair<uint32_t, uint32_t> kinds[64];
        for (uint32_t k = 0; k < 64; ++k) kinds[k] = {g_dcl_command_counts[k].exchange(0), k};
        std::sort(kinds, kinds + 64, [](const auto& a, const auto& b) { return a.first > b.first; });
        char b[200];
        int n = std::snprintf(b, sizeof(b), "%sdcl %u (", line.empty() ? "" : ", ", dcl_total);
        for (int k = 0; k < 6 && kinds[k].first; ++k)
          n += std::snprintf(b + n, sizeof(b) - size_t(n), "%s%u:%u", k ? " " : "", kinds[k].second,
                             kinds[k].first);
        std::snprintf(b + n, sizeof(b) - size_t(n), ")");
        line += b;
      }
    }
    const uint64_t upload_mb = g_upload_bytes_window.exchange(0) >> 20;
    if (upload_mb) {
      char b[64];
      std::snprintf(b, sizeof(b), "%suploads %llu MB in %u copies", line.empty() ? "" : ", ",
                    (unsigned long long)upload_mb, g_upload_copies_window.exchange(0));
      line += b;
    }
    const uint32_t calls = g_provider_calls.exchange(0);
    const uint32_t waits = g_provider_waits.exchange(0);
    const uint64_t us = g_provider_wait_us.exchange(0);
    if (calls) {
      char b[96];
      std::snprintf(b, sizeof(b), "%son-demand readback %u x %.1f ms (%u touches)",
                    line.empty() ? "" : ", ", waits, us / 1000.0, calls);
      line += b;
    }
  }
  if (!line.empty()) REXLOG_INFO("[gpu] fence waits in {:.1f} s: {}", secs, line);
  g_fence_report_at = now;
}

}  // namespace

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFullPath(uint32_t total_size) {
  FenceReasonScope fence_reason(fence_reason_, "memexport readback");
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  ID3D12Resource* readback_buffer = RequestReadbackBuffer(total_size);
  if (!readback_buffer) {
    return true;
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_buffer_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback_buffer, readback_buffer_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_buffer_offset += memexport_range.size_bytes;
  }

  if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  D3D12_RANGE readback_range = {};
  readback_range.Begin = 0;
  readback_range.End = total_size;
  void* readback_mapping = nullptr;
  if (FAILED(readback_buffer->Map(0, &readback_range, &readback_mapping))) {
    return true;
  }

  const uint8_t* readback_bytes = reinterpret_cast<const uint8_t*>(readback_mapping);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }

  D3D12_RANGE readback_write_range = {};
  readback_buffer->Unmap(0, &readback_write_range);
  return true;
}

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFastPath(uint32_t total_size) {
  FenceReasonScope fence_reason(fence_reason_, "memexport fast readback");
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const uint64_t readback_key =
      MakeMemexportReadbackKey(memexport_ranges_.front().base_address_dwords, total_size);
  ReadbackBuffer& readback = memexport_readback_buffers_[readback_key];
  readback.last_used_frame = frame_current_;

  auto ensure_readback_slot = [&](uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] && readback.mapped_data[index] && size <= readback.sizes[index]) {
      return true;
    }

    const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::ngpu_d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::ngpu_d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      return false;
    }

    D3D12_RANGE read_range = {0, size};
    void* mapped_data = nullptr;
    if (FAILED(buffer->Map(0, &read_range, &mapped_data))) {
      buffer->Release();
      return false;
    }

    if (readback.buffers[index]) {
      if (!AwaitAllQueueOperationsCompletion()) {
        buffer->Unmap(0, nullptr);
        buffer->Release();
        return false;
      }
      if (readback.mapped_data[index]) {
        readback.buffers[index]->Unmap(0, nullptr);
      }
      readback.buffers[index]->Release();
    }

    readback.buffers[index] = buffer;
    readback.mapped_data[index] = mapped_data;
    readback.sizes[index] = size;
    readback.submission_written[index] = 0;
    readback.written_size[index] = 0;
    return true;
  };

  const uint32_t write_index = readback.current_index;
  const uint32_t read_index = 1 - write_index;
  const uint32_t readback_size = AlignReadbackBufferSize(total_size);
  if (!ensure_readback_slot(write_index, readback_size)) {
    return IssueDraw_MemexportReadbackFullPath(total_size);
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback.buffers[write_index], readback_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_offset += memexport_range.size_bytes;
  }
  readback.submission_written[write_index] = submission_current_;
  readback.written_size[write_index] = total_size;

  CheckSubmissionFence(0);
  bool previous_slot_ready = readback.buffers[read_index] && readback.mapped_data[read_index] &&
                             total_size <= readback.sizes[read_index] &&
                             total_size <= readback.written_size[read_index] &&
                             readback.submission_written[read_index] &&
                             readback.submission_written[read_index] <= submission_completed_;
  if (!previous_slot_ready) {
    IssueDraw_MemexportReadbackFullPath(total_size);
    readback.current_index = read_index;
    return true;
  }

  const uint8_t* readback_bytes = static_cast<const uint8_t*>(readback.mapped_data[read_index]);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }
  readback.current_index = read_index;
  return true;
}

bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  if (!BeginSubmission(true)) {
    return false;
  }
  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    uint32_t written_address, written_length;
    if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                       written_address, written_length)) {
      return false;
    }
    NoteFreshResolve(written_address, written_length);
    return true;
  }
  return IssueCopy_ReadbackResolvePath();
}

bool D3D12CommandProcessor::IssueCopy_ReadbackResolvePath() {
  GpuCatScope gpu_cat(*this, kGpuCatReadback);   // NATIVE PATCH: [gpu prof] (the resolve itself scopes RESOLVE)
  FenceReasonScope fence_reason(fence_reason_, "resolve readback");
  uint32_t written_address, written_length;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                     written_length)) {
    return false;
  }
  NoteFreshResolve(written_address, written_length);
  if (REXCVAR_GET(readback_resolve_uav_barrier)) {
    // [experiment] Make the resolve's writes visible to everything recorded
    // after it, whatever state tracking believes.
    PushUAVBarrier(shared_memory_->GetBuffer());
    PushUAVBarrier(render_target_cache_->edram_buffer());
    if (texture_cache_->IsDrawResolutionScaled()) {
      if (ID3D12Resource* scaled = texture_cache_->GetCurrentScaledResolveBufferResource())
        PushUAVBarrier(scaled);
    }
    SubmitBarriers();
  }

  if (!written_length) {
    return true;
  }

  if (!memory_->TranslatePhysical(written_address)) {
    return true;
  }

  bool is_scaled = texture_cache_->IsDrawResolutionScaled();
  uint64_t resolve_key = MakeReadbackResolveKey(written_address, written_length);
  ReadbackBuffer& rb = readback_buffers_[resolve_key];
  rb.last_used_frame = frame_current_;

  uint32_t write_index = rb.current_index;
  // [readback] The slot this resolve is about to fill may still hold a copy
  // that has not landed (the same target resolved again before it reached
  // guest memory - the impostor pool does this several times a frame). Never
  // overtake it: a copy whose submission has completed is landed now (a
  // memcpy); otherwise the resolve moves on to a fresh slot of the ring, grown
  // on demand; only a full ring waits for that one submission. Dropping the
  // copy left guest memory at the pool's fill, which the next invalidation
  // uploaded over the render; landing it later read a buffer the GPU was
  // rewriting: the white/violet impostor flash (0.2.11).
  if (GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve)) == ReadbackResolveMode::kSome &&
      REXCVAR_GET(readback_resolve_keep_superseded)) {
    for (uint32_t attempt = 0; attempt <= kReadbackSlots; ++attempt) {
      uint64_t await = 0;
      uint32_t held = 0;
      for (const PendingResolveReadback& p : pending_resolve_readbacks_) {
        if (p.key == resolve_key && p.index == write_index) {
          ++held;
          await = std::max(await, p.submission);
        }
      }
      if (!held) break;
      if (await > GetCompletedSubmission()) {
        if (rb.ring_size < kReadbackSlots) {
          ++rb.ring_size;
          write_index = rb.ring_size - 1;
          rb.current_index = write_index;
          g_ring_grown.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (await >= submission_current_ && submission_open_) EndSubmission(false);
        CheckSubmissionFence(std::min(await, submission_current_ - 1));
        g_superseded_waited.fetch_add(1, std::memory_order_relaxed);
      }
      size_t kept = 0;
      for (size_t i = 0; i < pending_resolve_readbacks_.size(); ++i) {
        const PendingResolveReadback p = pending_resolve_readbacks_[i];
        if (p.key != resolve_key || p.index != write_index) {
          pending_resolve_readbacks_[kept++] = p;
          continue;
        }
        shared_memory_->UnprotectGpuRange(p.address, p.length);
        if (REXCVAR_GET(readback_resolve_on_demand))
          ::ng2::ngpu::rtc::GuestDataProvidersDisable(memory_, /* NATIVE PATCH */ p.address, p.length);
        // Same drop as in the drain: the slot is about to be reused, so a copy
        // that cannot be landed here is lost and guest memory keeps the stale
        // fill. Counted rather than silent.
        if (rb.buffers[p.index] && rb.mapped_data[p.index] && p.length <= rb.sizes[p.index]) {
          CopyToGuestMemory(p.address, rb.mapped_data[p.index], p.length);
        } else {
          g_superseded_dropped.fetch_add(1, std::memory_order_relaxed);
        }
      }
      pending_resolve_readbacks_.resize(kept);
      g_superseded_kept.fetch_add(held, std::memory_order_relaxed);
      break;
    }
  }
  uint32_t size = AlignReadbackBufferSize(written_length);

  if (size > rb.sizes[write_index]) {
    const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::ngpu_d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::ngpu_d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return true;
    }
    if (rb.buffers[write_index]) {
      if (rb.mapped_data[write_index]) {
        rb.buffers[write_index]->Unmap(0, nullptr);
        rb.mapped_data[write_index] = nullptr;
      }
      // [readback] A copy into it may be in flight: release later.
      DropPendingResolveReadbacks(resolve_key, write_index);
      readback_buffers_to_release_.emplace_back(GetCurrentSubmission(),
                                                rb.buffers[write_index]);
    }
    rb.buffers[write_index] = buffer;
    rb.sizes[write_index] = size;
    D3D12_RANGE read_range = {0, size};
    if (FAILED(buffer->Map(0, &read_range, &rb.mapped_data[write_index]))) {
      REXGPU_ERROR("Failed to persistently map resolve readback buffer");
      rb.mapped_data[write_index] = nullptr;
    }
  }

  if (!rb.buffers[write_index]) {
    return true;
  }

  if (is_scaled) {
    if (!resolve_downscale_pipeline_ || !resolve_downscale_root_signature_) {
      return true;
    }

    reg::RB_COPY_DEST_INFO copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    const FormatInfo* format_info = FormatInfo::Get(uint32_t(copy_dest_info.copy_dest_format));
    uint32_t bits_per_pixel = format_info->bits_per_pixel;
    if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 32 &&
        bits_per_pixel != 64) {
      return true;
    }

    uint32_t pixel_size_log2;
    if (!rex::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2)) {
      return true;
    }
    uint32_t tile_size_1x = 32 * 32 * (uint32_t(1) << pixel_size_log2);
    uint32_t tile_count = written_length / tile_size_1x;
    if (!tile_count) {
      return true;
    }

    uint32_t scaled_length = uint32_t(texture_cache_->GetCurrentScaledResolveRangeLengthScaled());
    uint64_t scaled_address = texture_cache_->GetCurrentScaledResolveRangeStartScaled();
    if (!scaled_length) {
      return true;
    }

    uint32_t downscale_buffer_size = AlignReadbackBufferSize(written_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
      ID3D12Device* device = provider.GetDevice();
      D3D12_RESOURCE_DESC buffer_desc;
      ui::ngpu_d3d12::util::FillBufferResourceDesc(buffer_desc, downscale_buffer_size,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      ID3D12Resource* buffer = nullptr;
      if (FAILED(device->CreateCommittedResource(
              &ui::ngpu_d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
              &buffer_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
              IID_PPV_ARGS(&buffer)))) {
        REXGPU_ERROR("Failed to create a {} MB resolve downscale buffer",
                     downscale_buffer_size >> 20);
        return true;
      }
      if (resolve_downscale_buffer_) {
        resources_for_deletion_.emplace_back(GetCurrentSubmission(),
                                             resolve_downscale_buffer_.Detach());
      }
      resolve_downscale_buffer_.Attach(buffer);
      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }

    if (!resolve_downscale_buffer_) {
      return true;
    }

    ID3D12Resource* scaled_resolve_buffer = texture_cache_->GetCurrentScaledResolveBufferResource();
    size_t scaled_resolve_buffer_index = texture_cache_->GetCurrentScaledResolveBufferIndexPublic();
    if (!scaled_resolve_buffer) {
      return true;
    }
    uint64_t scaled_buffer_base = uint64_t(scaled_resolve_buffer_index) << 30;
    if (scaled_address < scaled_buffer_base) {
      return true;
    }
    uint64_t source_offset = scaled_address - scaled_buffer_base;

    ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair downscale_descriptors[2];
    if (!RequestOneUseSingleViewDescriptors(2, downscale_descriptors)) {
      return true;
    }

    const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    uint32_t aligned_scaled_length =
        rex::align(scaled_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::ngpu_d3d12::util::CreateBufferRawSRV(device, downscale_descriptors[0].first,
                                        scaled_resolve_buffer, aligned_scaled_length,
                                        source_offset);
    uint32_t aligned_written_length =
        rex::align(written_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::ngpu_d3d12::util::CreateBufferRawUAV(device, downscale_descriptors[1].first,
                                        resolve_downscale_buffer_.Get(), aligned_written_length, 0);

    PushUAVBarrier(scaled_resolve_buffer);
    texture_cache_->TransitionCurrentScaledResolveRange(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SubmitBarriers();

    SetExternalPipeline(resolve_downscale_pipeline_.Get());
    deferred_command_list_.D3DSetComputeRootSignature(resolve_downscale_root_signature_.Get());
    ResolveDownscaleConstants constants;
    constants.scale_x = texture_cache_->draw_resolution_scale_x();
    constants.scale_y = texture_cache_->draw_resolution_scale_y();
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.half_pixel_offset = (REXCVAR_GET(readback_resolve_half_pixel_offset) &&
                                   (constants.scale_x > 1 || constants.scale_y > 1))
                                      ? 1u
                                      : 0u;
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(ResolveDownscaleRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
        &constants, 0);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kSource), downscale_descriptors[0].second);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kDestination), downscale_descriptors[1].second);
    deferred_command_list_.D3DDispatch(tile_count, 1, 1);

    PushUAVBarrier(resolve_downscale_buffer_.Get());
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0,
                                               resolve_downscale_buffer_.Get(), 0, written_length);
    if (REXCVAR_GET(readback_resolve_mirror_unscaled)) {
      // [mirror] The same 1x data into the unscaled shared memory buffer, where
      // a scaled resolve never writes: an unscaled load of this range (a texture
      // object created before the range was resolved, or after a CPU write beside
      // the render cleared its scaled page bits) then shows the render instead of
      // the fill the CPU last uploaded here - the white/magenta impostor flash.
      shared_memory_->UseAsCopyDestination();
      SubmitBarriers();
      shared_memory_->TouchRange(written_address, written_length);   // NATIVE PATCH: [hoist]
      deferred_command_list_.D3DCopyBufferRegion(shared_memory_->GetBuffer(), written_address,
                                                 resolve_downscale_buffer_.Get(), 0, written_length);
      g_mirror_copies.fetch_add(1, std::memory_order_relaxed);
    }
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    texture_cache_->TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SubmitBarriers();
  } else {
    shared_memory_->UseAsCopySource();
    SubmitBarriers();
    ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0, shared_memory_buffer,
                                               written_address, written_length);
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve));
  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  uint32_t read_index = write_index;
  if (use_delayed_sync) {
    read_index = (write_index + rb.ring_size - 1) % rb.ring_size;
  } else if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  bool is_cache_miss = false;
  if (use_delayed_sync && (!rb.buffers[read_index] || written_length > rb.sizes[read_index] ||
                           !rb.mapped_data[read_index])) {
    is_cache_miss = true;
    read_index = write_index;
  }
  if (readback_mode == ReadbackResolveMode::kSome) {
    // [readback] "some" (2026-09-13): every resolve reaches guest memory,
    // exactly - the first at an address synchronously (budgeted, so the
    // CPU can read it this frame), later ones once their own submission
    // has completed, at the next frame's opening submission, never
    // waiting. It used to copy only the first resolve at an address, so a
    // texture re-rendered there (the cullis-gate swirl, the dog's coat)
    // stayed stale on the CPU and was uploaded over the fresh render
    // whenever the CPU touched the page: a white flash every few frames.
    const int32_t budget = REXCVAR_GET(readback_resolve_sync_budget);
    const int32_t drain_small_kb = REXCVAR_GET(readback_resolve_drain_small_kb);
    const int32_t drain_large_kb = REXCVAR_GET(readback_resolve_drain_large_kb);
    const int32_t drain_max_kb = REXCVAR_GET(readback_resolve_drain_max_kb);
    const int32_t wait_only_kb = REXCVAR_GET(readback_resolve_wait_only_kb);
    const bool sync_now =
        (is_cache_miss && (budget < 0 || int32_t(resolve_sync_misses_this_frame_) < budget)) ||
        (drain_small_kb > 0 && written_length <= uint32_t(drain_small_kb) * 1024u) ||
        (drain_large_kb > 0 && written_length >= uint32_t(drain_large_kb) * 1024u &&
         (drain_max_kb <= 0 || written_length <= uint32_t(drain_max_kb) * 1024u));
    if (!sync_now && wait_only_kb > 0 && written_length >= uint32_t(wait_only_kb) * 1024u) {
      // [experiment] The drain's wait without its copy: the copy stays asynchronous.
      ++g_frame_sync_readbacks;
      AwaitAllQueueOperationsCompletion();
    }
    if (sync_now) {
      ++resolve_sync_misses_this_frame_;
      ++g_frame_sync_readbacks;  // [hitch]
      if (AwaitAllQueueOperationsCompletion() && rb.mapped_data[write_index]) {
        CopyToGuestMemory(written_address, rb.mapped_data[write_index], written_length);
      }
    } else {
      pending_resolve_readbacks_.push_back(
          {resolve_key, GetCurrentSubmission(), write_index, written_address, written_length});
      shared_memory_->ProtectGpuRange(written_address, written_length);
      // The game's CPU may only touch this memory once the copy has landed:
      // no access until then, the provider lands it on demand (off by
      // default: the page protection alone costs two thirds of the frame
      // rate in a dense town).
      if (REXCVAR_GET(readback_resolve_on_demand))
        memory_->EnablePhysicalMemoryAccessCallbacks(written_address, written_length, false, true);
      // [experiment] The drain's cure without its wait: a submission
      // boundary, or only the render-target re-bind a boundary implies.
      const int32_t submit_kb = REXCVAR_GET(readback_resolve_submit_small_kb);
      const int32_t rebind_kb = REXCVAR_GET(readback_resolve_rebind_small_kb);
      {
        const uint32_t kb = written_length / 1024u;
        const size_t bucket = kb <= 64 ? 0 : kb <= 256 ? 1 : kb <= 1024 ? 2 : kb <= 4096 ? 3 : 4;
        g_resolve_size_buckets[bucket].fetch_add(1, std::memory_order_relaxed);
        if (submit_kb > 0 && written_length > uint32_t(submit_kb) * 1024u) {
          static uint32_t logged = 0;
          if (logged < 40) {
            ++logged;
            REXLOG_INFO("[readback] deferred resolve without boundary: {} KB at {:08X} (submission {})",
                        kb, written_address, GetCurrentSubmission());
          }
        }
      }
      if (submit_kb > 0 && written_length <= uint32_t(submit_kb) * 1024u) {
        if (submission_open_) {
          ++g_resolve_splits;
          EndSubmission(false);
        }
      } else if (rebind_kb > 0 && written_length <= uint32_t(rebind_kb) * 1024u) {
        ++g_resolve_splits;
        render_target_cache_->InvalidateCommandListRenderTargets();
      }
    }
    rb.current_index = (rb.current_index + 1) % rb.ring_size;
    return true;
  }
  if (is_cache_miss) {
    const int32_t budget = REXCVAR_GET(readback_resolve_sync_budget);
    if (budget < 0 || int32_t(resolve_sync_misses_this_frame_) < budget) {
      ++resolve_sync_misses_this_frame_;
      ++g_frame_sync_readbacks;  // [hitch]
      if (!AwaitAllQueueOperationsCompletion()) {
        return true;
      }
    } else {
      // Over budget: the copy is in the command list; hand it to guest
      // memory when the submission completes (BeginSubmission), not now.
      pending_resolve_readbacks_.push_back(
          {resolve_key, GetCurrentSubmission(), write_index, written_address, written_length});
      shared_memory_->ProtectGpuRange(written_address, written_length);
      if (REXCVAR_GET(readback_resolve_on_demand))
        memory_->EnablePhysicalMemoryAccessCallbacks(written_address, written_length, false, true);
      rb.current_index = (rb.current_index + 1) % rb.ring_size;
      return true;
    }
  }

  bool should_copy = true;
  if (should_copy && rb.buffers[read_index] && written_length <= rb.sizes[read_index] &&
      rb.mapped_data[read_index]) {
    CopyToGuestMemory(written_address, rb.mapped_data[read_index], written_length);
  }

  rb.current_index = (rb.current_index + 1) % rb.ring_size;
  return true;
}

void D3D12CommandProcessor::CheckSubmissionFence(uint64_t await_submission) {
  MaybeReportFenceWaits();
  if (await_submission >= submission_current_) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // Ending an open submission should result in queue operations done directly
    // (like UpdateTileMappings) to be tracked within the scope of that
    // submission, but just in case of a failure, or queue operations being done
    // outside of a submission, await explicitly.
    if (queue_operations_done_since_submission_signal_) {
      DrainSubmissions();   // NATIVE PATCH: [async submit]
      UINT64 fence_value = ++queue_operations_since_submission_fence_last_;
      ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
      if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value) &&
                    SUCCEEDED(queue_operations_since_submission_fence_->SetEventOnCompletion(
                        fence_value, fence_completion_event_)))) {
        PROFILE_CMD_BUFFER_STALL();
        {
          const auto t0 = std::chrono::steady_clock::now();
          WaitForSingleObject(fence_completion_event_, INFINITE);
          NoteFenceWait(fence_reason_, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                                     std::chrono::steady_clock::now() - t0)
                                                     .count()));
        }
        queue_operations_done_since_submission_signal_ = false;
      } else {
        REXGPU_ERROR(
            "Failed to await an out-of-submission queue operation completion "
            "Direct3D 12 fence");
      }
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = submission_current_ - 1;
  }

  uint64_t submission_completed_before = submission_completed_;
  submission_completed_ = submission_fence_->GetCompletedValue();
  if (submission_completed_ < await_submission) {
    if (SUCCEEDED(
            submission_fence_->SetEventOnCompletion(await_submission, fence_completion_event_))) {
      PROFILE_CMD_BUFFER_STALL();
      {
        const auto t0 = std::chrono::steady_clock::now();
        WaitForSingleObject(fence_completion_event_, INFINITE);
        NoteFenceWait(fence_reason_, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                                   std::chrono::steady_clock::now() - t0)
                                                   .count()));
      }
      submission_completed_ = submission_fence_->GetCompletedValue();
    }
  }
  if (submission_completed_ < await_submission) {
    REXGPU_ERROR("Failed to await a submission completion Direct3D 12 fence");
  }
  if (submission_completed_ <= submission_completed_before) {
    // Not updated - no need to reclaim or download things.
    return;
  }

  // Reclaim command allocators.
  while (command_allocator_submitted_first_) {
    if (command_allocator_submitted_first_->last_usage_submission > submission_completed_) {
      break;
    }
    if (command_allocator_writable_last_) {
      command_allocator_writable_last_->next = command_allocator_submitted_first_;
    } else {
      command_allocator_writable_first_ = command_allocator_submitted_first_;
    }
    command_allocator_writable_last_ = command_allocator_submitted_first_;
    command_allocator_submitted_first_ = command_allocator_submitted_first_->next;
    command_allocator_writable_last_->next = nullptr;
  }
  if (!command_allocator_submitted_first_) {
    command_allocator_submitted_last_ = nullptr;
  }

  // Release single-use bindless descriptors.
  while (!view_bindless_one_use_descriptors_.empty()) {
    if (view_bindless_one_use_descriptors_.front().second > submission_completed_) {
      break;
    }
    ReleaseViewBindlessDescriptorImmediately(view_bindless_one_use_descriptors_.front().first);
    view_bindless_one_use_descriptors_.pop_front();
  }

  // Delete transient resources marked for deletion.
  while (!resources_for_deletion_.empty()) {
    if (resources_for_deletion_.front().first > submission_completed_) {
      break;
    }
    resources_for_deletion_.front().second->Release();
    resources_for_deletion_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);
}

void D3D12CommandProcessor::LogDeviceRemovalDiagnostics(ID3D12Device* device, HRESULT reason) {
  const char* reason_str = "Unknown";
  switch (reason) {
    case DXGI_ERROR_DEVICE_HUNG:
      reason_str = "DEVICE_HUNG (TDR - GPU command took too long)";
      break;
    case DXGI_ERROR_DEVICE_REMOVED:
      reason_str = "DEVICE_REMOVED (driver internal error or hot-unplug)";
      break;
    case DXGI_ERROR_DEVICE_RESET:
      reason_str = "DEVICE_RESET (bad GPU command)";
      break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      reason_str = "DRIVER_INTERNAL_ERROR";
      break;
    case DXGI_ERROR_INVALID_CALL:
      reason_str = "INVALID_CALL";
      break;
  }
  REXGPU_ERROR("D3D12 device removed: HRESULT 0x{:08X} - {}", static_cast<unsigned>(reason),
               reason_str);

  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    return;
  }

  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node;
         node = node->pNext) {
      if (!node->pLastBreadcrumbValue || !node->pCommandHistory ||
          *node->pLastBreadcrumbValue == 0) {
        continue;
      }
      REXGPU_ERROR("DRED breadcrumb: completed {} of {} ops", *node->pLastBreadcrumbValue,
                   node->BreadcrumbCount);
      uint32_t last = std::min(*node->pLastBreadcrumbValue, node->BreadcrumbCount);
      uint32_t start = last > 3 ? last - 3 : 0;
      uint32_t end = std::min(last + 1, node->BreadcrumbCount);
      for (uint32_t i = start; i < end; i++) {
        REXGPU_ERROR("  [{}] op type {}{}", i, static_cast<int>(node->pCommandHistory[i]),
                     i == last ? " <-- FAULT" : "");
      }
      // [dred] The draws recorded around that op, by the deferred list's own
      // count of the same ops (DeferredCommandList::DrawRecord). The two most
      // recent submissions are the candidates for the hung list.
      {
        const auto& recs = deferred_command_list_.draw_records();
        const uint64_t newest = submission_current_;
        REXGPU_ERROR("  ops in the last list by our count: {} (DRED: {})",
                     deferred_command_list_.last_execute_op_count(), node->BreadcrumbCount);
        int printed = 0;
        for (const auto& r : recs) {
          if (r.submission == 0 || r.submission + 1 < newest) continue;
          if (r.op_index + 2 < last || r.op_index > last + 2) continue;
          REXGPU_ERROR("  draw at op {} (submission {}): vs {:016X} ps {:016X} indices {} prim {}{}",
                       r.op_index, r.submission, r.vertex_shader_hash, r.pixel_shader_hash,
                       r.index_count, r.primitive_type,
                       r.op_index == last ? " <-- THE HUNG DRAW" : "");
          if (++printed >= 12) break;
        }
        if (!printed)
          REXGPU_ERROR("  no draw record near op {} in the last two submissions", last);
      }
    }
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) && page_fault.PageFaultVA != 0) {
    REXGPU_ERROR("DRED page fault at VA 0x{:016X}", page_fault.PageFaultVA);
  }
}

bool D3D12CommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_removed_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check if the device is still available.
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  HRESULT device_removed_reason = device->GetDeviceRemovedReason();
  if (FAILED(device_removed_reason)) {
    device_removed_ = true;
    LogDeviceRemovalDiagnostics(device, device_removed_reason);
    if (graphics_system_) {
      graphics_system_->OnHostGpuLossFromAnyThread(device_removed_reason !=
                                                   DXGI_ERROR_DEVICE_REMOVED);
    }
    return false;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame.
  CheckSubmissionFence(is_opening_frame ? closed_frame_submissions_[frame_current_ % kQueueFrames]
                                        : 0);
  // [readback] Buffers replaced or evicted while a copy may have been in
  // flight are released once that submission has completed.
  if (!readback_buffers_to_release_.empty()) {
    const uint64_t completed = GetCompletedSubmission();
    size_t kept = 0;
    for (size_t i = 0; i < readback_buffers_to_release_.size(); ++i) {
      auto& entry = readback_buffers_to_release_[i];
      if (entry.first > completed) {
        readback_buffers_to_release_[kept++] = entry;
      } else if (entry.second) {
        entry.second->Release();
      }
    }
    readback_buffers_to_release_.resize(kept);
  }
  // [readback] Deferred resolve copies whose submission has completed go to
  // guest memory now - at a frame's opening submission only, so a copy
  // always lands before that frame's own resolves and never over a fresher
  // one; a buffer evicted meanwhile simply drops its copy.
  if (is_opening_frame && !pending_resolve_readbacks_.empty()) {
    const uint64_t completed = GetCompletedSubmission();
    size_t kept = 0;
    for (size_t i = 0; i < pending_resolve_readbacks_.size(); ++i) {
      const PendingResolveReadback& p = pending_resolve_readbacks_[i];
      if (p.submission > completed) {
        pending_resolve_readbacks_[kept++] = p;
        continue;
      }
      shared_memory_->UnprotectGpuRange(p.address, p.length);  // the copy lands now
      if (REXCVAR_GET(readback_resolve_on_demand))
        ::ng2::ngpu::rtc::GuestDataProvidersDisable(memory_, /* NATIVE PATCH */ p.address, p.length);
      bool landed_here = false;
      auto it = readback_buffers_.find(p.key);
      if (it != readback_buffers_.end()) {
        ReadbackBuffer& rb = it->second;
        if (rb.buffers[p.index] && rb.mapped_data[p.index] && p.length <= rb.sizes[p.index]) {
          CopyToGuestMemory(p.address, rb.mapped_data[p.index], p.length);
          landed_here = true;
        }
      }
      // Third of four sites with this shape: the entry leaves the list either
      // way, so a copy that cannot land is lost silently. Counted.
      if (!landed_here) g_superseded_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    pending_resolve_readbacks_.resize(kept);
  }
  // TODO(Triang3l): If failed to await (completed submission < awaited frame
  // submission), do something like dropping the draw command that wanted to
  // open the frame.
  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kQueueFrames)) - kQueueFrames;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kQueueFrames] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    // Start a new deferred command list - will submit it to the real one in the
    // end of the submission (when async pipeline creation requests are
    // fulfilled).
    deferred_command_list_.Reset();
    GpuProfBegin();   // NATIVE PATCH: [gpu prof]
    // NATIVE PATCH: [hoist] a fresh prologue; the buffer's state now is the state around it.
    prologue_list_.Reset();
    if (shared_memory_) {
      shared_memory_->ResetTouched();
      prologue_buffer_state_ = shared_memory_->GetBufferState();
    }

    // Reset cached state of the command list.
    ff_viewport_update_needed_ = true;
    ff_scissor_update_needed_ = true;
    ff_blend_factor_update_needed_ = true;
    ff_stencil_ref_update_needed_ = true;
    viewport_cache_valid_ = false;
    current_guest_pipeline_ = nullptr;
    current_external_pipeline_ = nullptr;
    current_graphics_root_signature_ = nullptr;
    current_graphics_root_up_to_date_ = 0;
    if (bindless_resources_used_) {
      deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                sampler_bindless_heap_current_);
    } else {
      view_bindful_heap_current_ = nullptr;
      sampler_bindful_heap_current_ = nullptr;
    }
    primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    render_target_cache_->BeginSubmission();

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(submission_current_);
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Reset bindings that depend on the data stored in the pools.
    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    cbuffer_binding_system_.up_to_date = false;
    cbuffer_binding_float_vertex_.up_to_date = false;
    cbuffer_binding_float_pixel_.up_to_date = false;
    cbuffer_binding_bool_loop_.up_to_date = false;
    cbuffer_binding_fetch_.up_to_date = false;
    current_shared_memory_binding_is_uav_.reset();
    if (bindless_resources_used_) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else {
      draw_view_bindful_heap_index_ = ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      draw_sampler_bindful_heap_index_ = ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    constant_buffer_pool_->Reclaim(frame_completed_);
    if (!bindless_resources_used_) {
      view_bindful_heap_pool_->Reclaim(frame_completed_);
      sampler_bindful_heap_pool_->Reclaim(frame_completed_);
    }
    EvictOldReadbackBuffers(readback_buffers_);
    EvictOldReadbackBuffers(memexport_readback_buffers_);

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

bool D3D12CommandProcessor::EndSubmission(bool is_swap) {
  const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
  fresh_resolve_ranges_.clear();  // [split] a boundary makes every resolve "old"
  struct SubmissionTimer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~SubmissionTimer() {
      g_submission_count.fetch_add(1, std::memory_order_relaxed);
      g_submission_us.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count()),
          std::memory_order_relaxed);
    }
  } submission_timer;

  // Make sure there is a command allocator to write commands to.
  if (submission_open_ && !command_allocator_writable_first_) {
    ID3D12CommandAllocator* command_allocator;
    if (FAILED(provider.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&command_allocator)))) {
      REXGPU_ERROR("Failed to create a command allocator");
      // Try to submit later. Completely dropping the submission is not
      // permitted because resources would be left in an undefined state.
      return false;
    }
    command_allocator_writable_first_ = new CommandAllocator;
    command_allocator_writable_first_->command_allocator = command_allocator;
    command_allocator_writable_first_->last_usage_submission = 0;
    command_allocator_writable_first_->next = nullptr;
    command_allocator_writable_last_ = command_allocator_writable_first_;
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    texture_cache_->EndFrame();

    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    if (active_occlusion_query_.valid && occlusion_query_heap_) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         active_occlusion_query_.host_index);
      active_occlusion_query_ = {};
    }

    {
      const auto t0 = std::chrono::steady_clock::now();
      pipeline_cache_->EndSubmission();
      const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count());
      if (us > 200) {  // [hitch] a real wait for pipeline creation
        ++g_frame_pipeline_waits;
        g_frame_pipeline_wait_us += us;
      }
    }

    // Submit barriers now because resources with the queued barriers may be
    // destroyed between frames.
    SubmitBarriers();

    ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

    // Submit the deferred command list.
    // Only one deferred command list must be executed in the same
    // ExecuteCommandLists - the boundaries of ExecuteCommandLists are a full
    // UAV and aliasing barrier, and subsystems of the emulator assume it
    // happens between Xenia submissions.
    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    deferred_command_list_.SetExecuteSubmission(submission_current_);
    if (async_submit_) {
      // [async submit] The recording moves to a pooled list the submit thread executes; this one starts empty.
      std::unique_ptr<DeferredCommandList> job_list;
      {
        std::lock_guard<std::mutex> lk(submit_mutex_);
        if (!submit_free_lists_.empty()) {
          job_list = std::move(submit_free_lists_.back());
          submit_free_lists_.pop_back();
        }
      }
      if (!job_list) job_list = std::make_unique<DeferredCommandList>(*this);
      uint32_t prof_used = 0;
      if (gpu_prof_ && gpu_prof_heap_ && gpu_prof_used_) {
        const uint32_t base = gpu_prof_slot_ * kGpuProfPerSlot;
        deferred_command_list_.D3DEndQuery(gpu_prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP, base + gpu_prof_used_);
        prof_used = gpu_prof_used_ + 1;
        deferred_command_list_.D3DResolveQueryData(gpu_prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP, base, prof_used,
                                                   gpu_prof_readback_, uint64_t(base) * sizeof(uint64_t));
      }
      job_list->SwapRecording(deferred_command_list_);
      std::unique_ptr<DeferredCommandList> prologue;
      if (hoist_uploads_ && !prologue_list_.IsEmpty()) {
        {
          std::lock_guard<std::mutex> lk(submit_mutex_);
          if (!submit_free_lists_.empty()) {
            prologue = std::move(submit_free_lists_.back());
            submit_free_lists_.pop_back();
          }
        }
        if (!prologue) prologue = std::make_unique<DeferredCommandList>(*this);
        prologue->SwapRecording(prologue_list_);
        prologue_list_.Reset();
      }
      deferred_command_list_.Reset();
      {
        std::lock_guard<std::mutex> lk(submit_mutex_);
        SubmitJob job;
        job.list = std::move(job_list);
        job.allocator = command_allocator;
        job.submission = submission_current_;
        job.prof_cats = std::move(gpu_prof_cats_);
        job.prof_slot = gpu_prof_slot_;
        job.prof_used = prof_used;
        job.prologue = std::move(prologue);
        job.prologue_state = prologue_buffer_state_;
        submit_jobs_.push_back(std::move(job));
        gpu_prof_cats_ = {};
        gpu_prof_used_ = 0;
        submit_queued_through_ = submission_current_;
      }
      submit_cv_.notify_one();
    } else {
      command_allocator->Reset();
      command_list_->Reset(command_allocator, nullptr);
      deferred_command_list_.Execute(command_list_, command_list_1_);
      command_list_->Close();
      ID3D12CommandList* execute_command_lists[] = {command_list_};
      direct_queue->ExecuteCommandLists(1, execute_command_lists);
    }
    command_allocator_writable_first_->last_usage_submission = submission_current_;
    if (command_allocator_submitted_last_) {
      command_allocator_submitted_last_->next = command_allocator_writable_first_;
    } else {
      command_allocator_submitted_first_ = command_allocator_writable_first_;
    }
    command_allocator_submitted_last_ = command_allocator_writable_first_;
    command_allocator_writable_first_ = command_allocator_writable_first_->next;
    command_allocator_submitted_last_->next = nullptr;
    if (!command_allocator_writable_first_) {
      command_allocator_writable_last_ = nullptr;
    }

    if (async_submit_) {
      ++submission_current_;   // the submit thread signals this value after executing the list
    } else {
      direct_queue->Signal(submission_fence_, submission_current_++);
    }

    submission_open_ = false;

    // Queue operations done directly (like UpdateTileMappings) will be awaited
    // alongside the last submission if needed.
    queue_operations_done_since_submission_signal_ = false;
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state) && shared_memory_) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kQueueFrames] = submission_current_ - 1;
    resolve_sync_misses_this_frame_ = 0;  // [readback] a fresh budget per frame

    if (cache_clear_requested_ &&
        (FenceReasonScope(fence_reason_, "cache clear"), AwaitAllQueueOperationsCompletion())) {
      cache_clear_requested_ = false;

      ClearCommandAllocatorCache();

      ui::ngpu_d3d12::util::ReleaseAndNull(scratch_buffer_);
      scratch_buffer_size_ = 0;

      if (bindless_resources_used_) {
        texture_cache_bindless_sampler_map_.clear();
        for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
          sampler_bindless_heap_overflowed.first->Release();
        }
        sampler_bindless_heaps_overflowed_.clear();
        sampler_bindless_heap_allocated_ = 0;
      } else {
        sampler_bindful_heap_pool_->ClearCache();
        view_bindful_heap_pool_->ClearCache();
      }
      constant_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      // Not clearing the root signatures as they're referenced by pipelines,
      // which are not destroyed.

      primitive_processor_->ClearCache();

      render_target_cache_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

bool D3D12CommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_->IsCreatingPipelines();
}

void D3D12CommandProcessor::ClearCommandAllocatorCache() {
  while (command_allocator_submitted_first_) {
    auto next = command_allocator_submitted_first_->next;
    command_allocator_submitted_first_->command_allocator->Release();
    delete command_allocator_submitted_first_;
    command_allocator_submitted_first_ = next;
  }
  command_allocator_submitted_last_ = nullptr;
  while (command_allocator_writable_first_) {
    auto next = command_allocator_writable_first_->next;
    command_allocator_writable_first_->command_allocator->Release();
    delete command_allocator_writable_first_;
    command_allocator_writable_first_ = next;
  }
  command_allocator_writable_last_ = nullptr;
}

void D3D12CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info, const draw_util::Scissor& scissor,
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Viewport.
  D3D12_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  D3D12_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.offset[0]);
  scissor_rect.top = LONG(scissor.offset[1]);
  scissor_rect.right = LONG(scissor.offset[0] + scissor.extent[0]);
  scissor_rect.bottom = LONG(scissor.offset[1] + scissor.extent[1]);
  SetScissorRect(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    const RegisterFile& regs = *register_file_;

    // Blend factor.
    float blend_factor[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    ff_blend_factor_update_needed_ |=
        std::memcmp(ff_blend_factor_, blend_factor, sizeof(float) * 4) != 0;
    if (ff_blend_factor_update_needed_) {
      std::memcpy(ff_blend_factor_, blend_factor, sizeof(float) * 4);
      deferred_command_list_.D3DOMSetBlendFactor(ff_blend_factor_);
      ff_blend_factor_update_needed_ = false;
    }

    // Stencil reference value. Per-face reference not supported by Direct3D 12,
    // choose the back face one only if drawing only back faces.
    Register stencil_ref_mask_reg;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (primitive_polygonal && normalized_depth_control.backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
    } else {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
    }
    uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
    ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
    if (ff_stencil_ref_update_needed_) {
      ff_stencil_ref_ = stencil_ref;
      deferred_command_list_.D3DOMSetStencilRef(ff_stencil_ref_);
      ff_stencil_ref_update_needed_ = false;
    }
  }
}

void D3D12CommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  bool edram_rov_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get the color info register values for each render target. Also, for ROV,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[4];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_rov_used) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_rov_used && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Whether shared memory is an SRV or a UAV. Because a resource can't be in a
  // read-write (UAV) and a read-only (SRV, IBV) state at once, if any shader in
  // the pipeline uses memexport, the shared memory buffer must be a UAV.
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal and SV_IsFrontFace matters.
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  if (REXCVAR_GET(diag_alpha_test_off)) {
    // DIAGNOSTIC: never reject a pixel on alpha, to find out whether the
    // alpha test is what removes geometry that is otherwise submitted.
    alpha_test_function = xenos::CompareFunction::kAlways;
  }
  flags |= uint32_t(alpha_test_function) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_rov_used && depth_stencil_enabled) {
    flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= DxbcShaderTranslator::kSysFlag_ROVDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfEqual |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Tessellation factor range, plus 1.0 according to the images in
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360
  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index (or 0 when drawing other primitives or using an
  // index buffer).
  dirty |= system_constants_.line_loop_closing_index != line_loop_closing_index;
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_index_offset != vgt_indx_offset;
  system_constants_.vertex_index_offset = vgt_indx_offset;

  // Vertex index range.
  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  // The shader knows only the total count - tightly packing the user clip
  // planes that are actually used.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Conversion to Direct3D 12 normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = (texture_index & 3) * 8;
    uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
    uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
    uint32_t texture_signs_mask = uint32_t(0b11111111) << texture_signs_shift;
    dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
    texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
    textures_resolution_scaled |=
        uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Log2 of sample count, for alpha to mask and with ROV, for EDRAM address
  // calculation with MSAA.
  uint32_t sample_count_log2_x = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  dirty |= system_constants_.sample_count_log2[0] != sample_count_log2_x;
  dirty |= system_constants_.sample_count_log2[1] != sample_count_log2_y;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test and alpha to coverage.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  uint32_t edram_tile_dwords_scaled = xenos::kEdramTileWidthSamples *
                                      xenos::kEdramTileHeightSamples *
                                      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for ROV writing.
  if (edram_rov_used) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and ROV render target writing.
  for (uint32_t i = 0; i < 4; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
          !render_target_cache_->IsFixed16TruncatedToMinus1To1()) {
        // Remap from -32...32 to -1...1 by dividing the output values by 32,
        // losing blending correctness, but getting the full range.
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale =
        rex::memory::Reinterpret<float>(int32_t(0x3F800000 + (color_exp_bias << 23)));
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_rov_used) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        uint32_t format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |=
            std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float));
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
      }
    }
  }

  if (edram_rov_used) {
    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor = xenos::kPolygonOffsetScaleSubpixelUnit *
                                     std::max(draw_resolution_scale_x, draw_resolution_scale_y);
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale != poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset != poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale != poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset != poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      dirty |= system_constants_.edram_stencil_front_reference != rb_stencilrefmask.stencilref;
      system_constants_.edram_stencil_front_reference = rb_stencilrefmask.stencilref;
      dirty |= system_constants_.edram_stencil_front_read_mask != rb_stencilrefmask.stencilmask;
      system_constants_.edram_stencil_front_read_mask = rb_stencilrefmask.stencilmask;
      dirty |=
          system_constants_.edram_stencil_front_write_mask != rb_stencilrefmask.stencilwritemask;
      system_constants_.edram_stencil_front_write_mask = rb_stencilrefmask.stencilwritemask;
      uint32_t stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        dirty |= system_constants_.edram_stencil_back_reference != rb_stencilrefmask_bf.stencilref;
        system_constants_.edram_stencil_back_reference = rb_stencilrefmask_bf.stencilref;
        dirty |= system_constants_.edram_stencil_back_read_mask != rb_stencilrefmask_bf.stencilmask;
        system_constants_.edram_stencil_back_read_mask = rb_stencilrefmask_bf.stencilmask;
        dirty |= system_constants_.edram_stencil_back_write_mask !=
                 rb_stencilrefmask_bf.stencilwritemask;
        system_constants_.edram_stencil_back_write_mask = rb_stencilrefmask_bf.stencilwritemask;
        uint32_t stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front, 4 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    4 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] != regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] != regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  cbuffer_binding_system_.up_to_date &= !dirty;
}

float D3D12CommandProcessor::C8QuadSpan(const D3D12Shader* vertex_shader, float& x_min,
                                        float& x_max) const {
  x_min = x_max = 0.0f;
  if (!uw_ppr_ || uw_ppr_->host_draw_vertex_count == 0 || uw_ppr_->host_draw_vertex_count > 6) {
    return -1.0f;
  }
  const auto& vbs = vertex_shader->vertex_bindings();
  if (vbs.empty() || vbs[0].attributes.empty()) return -1.0f;
  const auto& attr = vbs[0].attributes[0].fetch_instr.attributes;
  if (attr.data_format != xenos::VertexFormat::k_32_32_FLOAT &&
      attr.data_format != xenos::VertexFormat::k_32_32_32_FLOAT &&
      attr.data_format != xenos::VertexFormat::k_32_32_32_32_FLOAT) {
    return -1.0f;
  }
  const xenos::xe_gpu_vertex_fetch_t vf = register_file_->GetVertexFetch(vbs[0].fetch_constant);
  if (vf.type != xenos::FetchConstantType::kVertex || vf.size == 0) return -1.0f;
  const uint32_t stride = vbs[0].stride_words * 4;
  const uint32_t offset = uint32_t(attr.offset) * 4;
  if (stride == 0) return -1.0f;
  const uint8_t* base = memory_->TranslatePhysical(vf.address << 2);
  if (!base) return -1.0f;
  const uint8_t* indices = nullptr;
  bool indices_32 = false;
  if (uw_ppr_->index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
    indices = memory_->TranslatePhysical(uw_ppr_->guest_index_base);
    if (!indices) return -1.0f;
    indices_32 = uw_ppr_->host_index_format == xenos::IndexFormat::kInt32;
  } else if (uw_ppr_->index_buffer_type != PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    return -1.0f;
  }
  const uint64_t buffer_bytes = uint64_t(vf.size) * 4;
  bool first = true;
  for (uint32_t i = 0; i < uw_ppr_->host_draw_vertex_count; ++i) {
    uint32_t index = i;
    if (indices) {
      if (indices_32) {
        uint32_t raw;
        std::memcpy(&raw, indices + size_t(i) * 4, 4);
        index = xenos::GpuSwap(raw, uw_ppr_->host_shader_index_endian);
      } else {
        uint16_t raw;
        std::memcpy(&raw, indices + size_t(i) * 2, 2);
        index = xenos::GpuSwap(raw, uw_ppr_->host_shader_index_endian);
      }
    }
    const uint64_t at = uint64_t(index) * stride + offset;
    if (at + 4 > buffer_bytes) return -1.0f;
    uint32_t raw_x;
    std::memcpy(&raw_x, base + at, 4);
    raw_x = xenos::GpuSwap(raw_x, vf.endian);
    float x;
    std::memcpy(&x, &raw_x, 4);
    if (!(x == x) || x < -1e5f || x > 1e5f) return -1.0f;
    if (first) { x_min = x_max = x; first = false; }
    else { x_min = std::min(x_min, x); x_max = std::max(x_max, x); }
  }
  return x_max - x_min;
}

// [dd] One line per draw while a dump is open: shaders, primitive, vertex
// count, depth/blend/mask state, render target, viewport, scissor, the c0..c3
// projection block and c8 when the vertex shader uses them, the small-draw
// vertex x range, and the pixel shader's textures (size, format, address,
// GPU-written, readback pending). Written with buffered stdio; the game runs
// on while the file grows.
void D3D12CommandProcessor::DrawDumpLine(const D3D12Shader* vertex_shader,
                                         const D3D12Shader* pixel_shader,
                                         const PrimitiveProcessor::ProcessingResult& ppr,
                                         const draw_util::ViewportInfo& viewport,
                                         const draw_util::Scissor& scissor,
                                         reg::RB_DEPTHCONTROL depth_control) {
  if (!dd_file_ || !vertex_shader) return;
  const RegisterFile& regs = *register_file_;
  char line[1400];
  int n = std::snprintf(
      line, sizeof(line),
      "f%u d%u vs %016llX ps %016llX prim %u verts %u idx %u z %u/%u blend %08X cmask %X "
      "rt %08X vp %u,%u %ux%u sc %u,%u %ux%u",
      dd_frame_, dd_draw_++, (unsigned long long)vertex_shader->ucode_data_hash(),
      (unsigned long long)(pixel_shader ? pixel_shader->ucode_data_hash() : 0ull),
      uint32_t(ppr.guest_primitive_type), ppr.host_draw_vertex_count,
      uint32_t(ppr.index_buffer_type), uint32_t(depth_control.z_enable),
      uint32_t(depth_control.z_write_enable), regs[XE_GPU_REG_RB_BLENDCONTROL0],
      regs[XE_GPU_REG_RB_COLOR_MASK] & 0xFu, regs[XE_GPU_REG_RB_COLOR_INFO], viewport.xy_offset[0],
      viewport.xy_offset[1], viewport.xy_extent[0], viewport.xy_extent[1], scissor.offset[0],
      scissor.offset[1], scissor.extent[0], scissor.extent[1]);
  auto put = [&](const char* fmt, auto... args) {
    if (n < 0 || size_t(n) >= sizeof(line) - 2) return;
    const int m = std::snprintf(line + n, sizeof(line) - 2 - size_t(n), fmt, args...);
    if (m > 0) n += m;
  };
  const auto& bmu = vertex_shader->constant_register_map().float_bitmap;
  const float* c = reinterpret_cast<const float*>(&regs[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  if ((bmu[0] & 0xFull) == 0xFull) {
    put(" c0 %.4g,%.4g,%.4g,%.4g c1 %.4g,%.4g,%.4g,%.4g c2 %.4g,%.4g,%.4g,%.4g c3 %.4g,%.4g,%.4g,%.4g",
        c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13],
        c[14], c[15]);
  }
  if ((bmu[0] >> 8) & 1ull) put(" c8 %.5g,%.5g,%.4g,%.4g", c[32], c[33], c[34], c[35]);
  put(" vconst %016llX", (unsigned long long)bmu[0]);
  float x0 = 0.0f, x1 = 0.0f;
  const float span = C8QuadSpan(vertex_shader, x0, x1);
  if (span >= 0.0f) put(" x %.1f..%.1f", x0, x1);
  if (pixel_shader) {
    const auto& tb = pixel_shader->GetTextureBindingsAfterTranslation();
    for (size_t i = 0; i < tb.size() && i < 4; ++i) {
      xenos::xe_gpu_texture_fetch_t fetch;
      std::memcpy(&fetch,
                  &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + tb[i].fetch_constant * 6],
                  sizeof(fetch));
      const uint32_t base = uint32_t(fetch.base_address) << 12;
      put(" t%u:%ux%u f%u @%08X%s%s", tb[i].fetch_constant, uint32_t(fetch.size_2d.width) + 1,
          uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.format), base,
          (shared_memory_ && shared_memory_->AnyPageGpuWritten(base, 4096)) ? " gpuw" : "",
          HasPendingResolveReadback(base, 4096) ? " rbpend" : "");
    }
  }
  if (n < 0) n = 0;
  if (size_t(n) > sizeof(line) - 2) n = int(sizeof(line) - 2);
  line[n++] = '\n';
  line[n] = 0;
  std::fputs(line, dd_file_);
}

bool D3D12CommandProcessor::UpdateBindings(const D3D12Shader* vertex_shader,
                                           const D3D12Shader* pixel_shader,
                                           ID3D12RootSignature* root_signature,
                                           bool shared_memory_is_uav) {
  const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  const RegisterFile& regs = *register_file_;

#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Set the new root signature.
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    if (!bindless_resources_used_) {
      GetRootBindfulExtraParameterIndices(vertex_shader, pixel_shader,
                                          current_graphics_root_bindful_extras_);
    }
    // Changing the root signature invalidates all bindings.
    current_graphics_root_up_to_date_ = 0;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }

  // Select the root parameter indices depending on the used binding model.
  uint32_t root_parameter_fetch_constants = bindless_resources_used_
                                                ? kRootParameter_Bindless_FetchConstants
                                                : kRootParameter_Bindful_FetchConstants;
  uint32_t root_parameter_float_constants_vertex =
      bindless_resources_used_ ? kRootParameter_Bindless_FloatConstantsVertex
                               : kRootParameter_Bindful_FloatConstantsVertex;
  uint32_t root_parameter_float_constants_pixel = bindless_resources_used_
                                                      ? kRootParameter_Bindless_FloatConstantsPixel
                                                      : kRootParameter_Bindful_FloatConstantsPixel;
  uint32_t root_parameter_system_constants = bindless_resources_used_
                                                 ? kRootParameter_Bindless_SystemConstants
                                                 : kRootParameter_Bindful_SystemConstants;
  uint32_t root_parameter_bool_loop_constants = bindless_resources_used_
                                                    ? kRootParameter_Bindless_BoolLoopConstants
                                                    : kRootParameter_Bindful_BoolLoopConstants;
  uint32_t root_parameter_shared_memory_and_bindful_edram =
      bindless_resources_used_ ? kRootParameter_Bindless_SharedMemory
                               : kRootParameter_Bindful_SharedMemoryAndEdram;

  //
  // Update root constant buffers that are common for bindful and bindless.
  //

  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, we can reuse any buffer for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Write the constant buffer data.
  if (!cbuffer_binding_system_.up_to_date) {
    uint8_t* system_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(system_constants_), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_system_.address);
    if (system_constants == nullptr) {
      return false;
    }
    std::memcpy(system_constants, &system_constants_, sizeof(system_constants_));
    cbuffer_binding_system_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_system_constants);
  }
  if (!cbuffer_binding_float_vertex_.up_to_date) {
    // Even if the shader doesn't need any float constants, a valid binding must
    // still be provided, so if the first draw in the frame with the current
    // root signature doesn't have float constants at all, still allocate an
    // empty buffer.
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_vertex, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_vertex_.address);
    if (float_constants == nullptr) {
      return false;
    }
    const uint8_t* float_constants_begin = float_constants;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t float_constant_map_entry = float_constant_map_vertex.float_bitmap[i];
      uint32_t float_constant_index;
      while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
        float_constant_map_entry &= ~(1ull << float_constant_index);
        std::memcpy(
            float_constants,
            &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (float_constant_index << 2)],
            4 * sizeof(float));
        float_constants += 4 * sizeof(float);
      }
    }
    // [vsc-dump] NG2 FOV work (additive, NG2-only, off unless env is set): log
    // the vertex shader's float constants as vec4s with their register index,
    // for the first draws that carry a matrix's worth, to locate the projection
    // / view-projection among them. Read-only. Runs on the GPU thread only, so a
    // plain static counter is safe.
    {
      static const bool s_vsc_dump = std::getenv("NG2_DUMP_VSCONST") != nullptr;
      // Snapshot a few draws every couple of seconds (not just the first burst,
      // which is boot/2D), so gameplay's 3D matrices are captured too. Bounded
      // total so the log cannot run away.
      static int s_vsc_total = 0;
      static int s_vsc_window = 0;
      static auto s_vsc_last = std::chrono::steady_clock::now();
      const auto s_vsc_now = std::chrono::steady_clock::now();
      if (std::chrono::duration<double>(s_vsc_now - s_vsc_last).count() > 2.0) {
        s_vsc_last = s_vsc_now;
        s_vsc_window = 0;
      }
      if (s_vsc_dump && float_constant_count_vertex >= 4 && s_vsc_total < 50000 &&
          s_vsc_window < 6) {
        ++s_vsc_total;
        ++s_vsc_window;
        const size_t vc =
            size_t(float_constants - float_constants_begin) / sizeof(float);
        const float* vals = reinterpret_cast<const float*>(float_constants_begin);
        uint32_t idxs[256];
        uint32_t nidx = 0;
        for (uint32_t i = 0; i < 4 && nidx < 256; ++i) {
          uint64_t bits = float_constant_map_vertex.float_bitmap[i];
          uint32_t bit;
          while (rex::bit_scan_forward(bits, &bit) && nidx < 256) {
            bits &= ~(1ull << bit);
            idxs[nidx++] = (i << 6) + bit;
          }
        }
        REXLOG_INFO("[vsc] draw {} uses {} vec4 constants:", s_vsc_total, nidx);
        for (uint32_t k = 0; k < nidx && (k * 4 + 3) < vc; ++k) {
          REXLOG_INFO("[vsc]   c{}: {:.4f} {:.4f} {:.4f} {:.4f}", idxs[k],
                      vals[k * 4], vals[k * 4 + 1], vals[k * 4 + 2],
                      vals[k * 4 + 3]);
        }
      }
    }
    // [ng2-fov] Gated horizontal-FOV widen (Hor+), additive and NG2-only. Off
    // unless the live cvar ng2_fov_k is set to a factor k = render_aspect /
    // display_aspect (e.g. 1.7778/2.4 = 0.7407 for a 2.4:1 display). A vertex
    // shader's projection is a 4x4 whose column 0 equals sx * (its non-projection
    // columns), so scaling that column by k is mathematically exact: it widens
    // horizontal FOV while leaving vertical FOV and depth untouched. Different
    // NG2 shaders keep this matrix in different register slots (the world/env at
    // c21-c24, the skinned character elsewhere, etc.), so rather than hard-code a
    // slot we find the first four consecutive constants that form a perspective
    // matrix - each of the first three rows has its w-component tracking its
    // z-component, and the w column is not the 2D ortho [0,0,0,1] - and scale
    // that block's column 0. Ortho/2D draws (menus, videos, HUD) and directional
    // shadow projections have a [0,0,0,1] w column and are skipped, so they stay
    // 16:9. Runs on the GPU thread only, so plain statics are safe.
    // [uw-2d] Fable II HUD at ultrawide: compress draws that carry the
    // pixel-to-clip constant c8 = (2/W, -2/H, -1, 1) to a centred 16:9 band.
    // The values are tested in the REGISTER FILE (cached memory) and only the
    // scaled ones are written into the upload buffer: reading them back from
    // that write-combined mapping cost 3 fps in the market (s77 -> s78).
    {
      const double kd = REXCVAR_GET(fable2_uw_2d_k);
      if (kd > 0.05 && kd < 1.5 && std::fabs(kd - 1.0) > 1e-3) {
        const auto& bmu = float_constant_map_vertex.float_bitmap;
        if ((bmu[0] >> 8) & 1ull) {
          float c8[4];
          std::memcpy(c8, &register_file_->values[XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * 8],
                      sizeof(c8));
          const bool pixel_scale = c8[0] > 1e-3f && c8[0] < 2.5e-3f && c8[1] < -1.5e-3f &&
                                   c8[1] > -4e-3f && std::fabs(c8[2] + 1.0f) < 0.02f &&
                                   std::fabs(c8[3] - 1.0f) < 0.02f;
          // [uw-2d] c8 census: each (vs, ps) pair once, with its textures.
          if (pixel_scale) {
            static std::unordered_set<uint64_t> seen_pairs;
            const uint64_t ps_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0ull;
            const uint64_t pair = vertex_shader->ucode_data_hash() ^ (ps_hash * 0x9E3779B97F4A7C15ull);
            if (seen_pairs.size() < 80 && !seen_pairs.count(pair)) {
              seen_pairs.insert(pair);
              char tex_desc[160] = {0};
              int td = 0;
              uint32_t ntex = 0;
              if (pixel_shader) {
                const auto& tb = pixel_shader->GetTextureBindingsAfterTranslation();
                ntex = uint32_t(tb.size());
                for (size_t i = 0; i < tb.size() && i < 2; ++i) {
                  xenos::xe_gpu_texture_fetch_t fetch;
                  std::memcpy(&fetch,
                              &register_file_->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                                      tb[i].fetch_constant * 6],
                              sizeof(fetch));
                  td += std::snprintf(tex_desc + td, sizeof(tex_desc) - size_t(td), " t%u:%ux%u fmt %u",
                                      tb[i].fetch_constant, uint32_t(fetch.size_2d.width) + 1,
                                      uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.format));
                }
              }
              const auto& psb = pixel_shader ? pixel_shader->constant_register_map().float_bitmap : bmu;
              REXLOG_INFO("[uw-2d] c8 draw first seen: vs {:016X} ps {:016X} textures {}{} verts {} ps-consts {:016X}",
                          vertex_shader->ucode_data_hash(), ps_hash, ntex, tex_desc, uw_diag_vertex_count_,
                          pixel_shader ? psb[0] : 0ull);
            }
          }
          // A solid fill (no texture sampled) is a scene-transition fade or a
          // full-screen tint: it must cover the whole ultrawide picture, so it
          // keeps its width (the Ninja Gaiden II v1.0.20 rule).
          const bool solid_fill =
              pixel_shader != nullptr && pixel_shader->GetTextureBindingsAfterTranslation().empty();
          if (pixel_scale && solid_fill) {
            static uint32_t solid_count = 0;
            static std::chrono::steady_clock::time_point solid_logged{};
            ++solid_count;
            const auto now_sf = std::chrono::steady_clock::now();
            if (now_sf - solid_logged > std::chrono::seconds(1)) {
              solid_logged = now_sf;
              REXLOG_INFO("[uw-2d] solid-fill 2D draws left full width: {} since the last line (ps {:016X})",
                          solid_count, pixel_shader->ucode_data_hash());
              solid_count = 0;
            }
          }
          // [uw-2d] Geometry: a quad spanning the 2D space's full width (the
          // fade to black, a full-screen tint) keeps its width; the HUD's
          // shader pair is the same, only the extent tells them apart.
          bool full_width = false;
          if (pixel_scale && !solid_fill) {
            float x0 = 0.0f, x1 = 0.0f;
            const float span = C8QuadSpan(vertex_shader, x0, x1);
            full_width = span >= 1200.0f;
            static uint32_t span_logged = 0;
            if (span_logged < 40) {
              ++span_logged;
              REXLOG_INFO("[uw-2d] c8 quad span {:.0f}..{:.0f} ({:.0f} px) verts {} indexed {} -> {}",
                          x0, x1, span, uw_diag_vertex_count_,
                          uw_ppr_ && uw_ppr_->index_buffer_type !=
                                         PrimitiveProcessor::ProcessedIndexBufferType::kNone,
                          span < 0.0f ? "unknown, compress" : full_width ? "full width" : "compress");
            }
          }
          if (pixel_scale && !solid_fill && !full_width) {
            // c8's packed position = the number of used registers below 8.
            const uint32_t pos = rex::bit_count(bmu[0] & 0xFFull);
            float* out = reinterpret_cast<float*>(const_cast<uint8_t*>(float_constants_begin)) + pos * 4;
            const float k = static_cast<float>(kd);
            out[0] = c8[0] * k;  // x scale
            out[2] = c8[2] * k;  // x offset (-1 -> -k keeps the band centred)
          }
        }
        // [uw-2d] perspective HUD widgets: a 16:9 projection at c0..c3 with
        // the depth test off (the d-pad prompt's shaded buttons). The world's
        // projection carries the display aspect, so it never matches here.
        if ((bmu[0] & 0xFull) == 0xFull) {
          float c0[8];
          std::memcpy(c0, &register_file_->values[XE_GPU_REG_SHADER_CONSTANT_000_X], sizeof(c0));
          const float sx = c0[0];
          if (sx > 1e-3f && c0[1] == 0.0f && c0[2] == 0.0f && c0[3] == 0.0f) {
            // c1 holds the y scale in exactly one lane.
            float sy = 0.0f;
            int lanes = 0;
            for (int i = 4; i < 8; ++i) {
              if (c0[i] != 0.0f) { sy = std::fabs(c0[i]); ++lanes; }
            }
            const float ratio = lanes == 1 ? sy / sx : 0.0f;
            if (ratio > 1.751f && ratio < 1.805f &&
                !draw_util::GetNormalizedDepthControl(*register_file_).z_enable) {
              // [uw-menu] The pause-menu transition draws its dissolve layer and
              // the leather frame pieces with this same projection, depth off,
              // over the world for a few frames: quads that reach the 16:9 frame
              // edge (|x| >= 7.0 of 7.6). Compressing them put a 16:9 band of
              // menu over an edge-to-edge world; the steady menu (depth on) is
              // full width, so these stay full width too. HUD widgets are small.
              float ex0 = 0.0f, ex1 = 0.0f;
              const float espan = C8QuadSpan(vertex_shader, ex0, ex1);
              const bool frame_edge =
                  espan >= 0.0f && std::max(std::fabs(ex0), std::fabs(ex1)) >= 7.0f;
              if (!frame_edge) {
                // c0 is the first used register: packed position 0.
                float* out = reinterpret_cast<float*>(const_cast<uint8_t*>(float_constants_begin));
                out[0] = sx * static_cast<float>(kd);
              } else {
                static uint32_t edge_logged = 0;
                if (edge_logged < 12) {
                  ++edge_logged;
                  REXLOG_INFO("[uw-menu] transition layer quad kept full width: x {:.1f}..{:.1f} verts {}",
                              ex0, ex1, uw_diag_vertex_count_);
                }
              }
            }
          }
        }
      }
    }
    // [2d-census] Fable II: how do the UI shaders carry their transform?
    if (REXCVAR_GET(fable2_2d_census) > 0) {
      static std::unordered_map<const void*, int> s_seen;
      static int s_logged = 0;
      const void* skey = static_cast<const void*>(vertex_shader);
      if (s_logged < REXCVAR_GET(fable2_2d_census) && !s_seen.count(skey)) {
        s_seen.emplace(skey, 1);
        ++s_logged;
        const float* fb = reinterpret_cast<const float*>(float_constants_begin);
        const auto& bmc = float_constant_map_vertex.float_bitmap;
        const float* rp[512] = {nullptr};
        uint32_t used = 0, reg_lo = 512, reg_hi = 0;
        for (uint32_t i = 0; i < 4; ++i) {
          uint64_t bits = bmc[i];
          uint32_t bit;
          while (rex::bit_scan_forward(bits, &bit)) {
            bits &= ~(1ull << bit);
            const uint32_t reg = (i << 6) + bit;
            if (reg < 512) {
              rp[reg] = fb + used * 4;
              if (reg < reg_lo) reg_lo = reg;
              if (reg > reg_hi) reg_hi = reg;
            }
            ++used;
          }
        }
        int ortho_at = -1, persp_at = -1;
        float ox = 0, oy = 0, oox = 0, ooy = 0;
        for (uint32_t r = reg_lo; r + 3 <= reg_hi && r < 512; ++r) {
          const float *r0 = rp[r], *r1 = rp[r + 1], *r2 = rp[r + 2], *r3 = rp[r + 3];
          if (!r0 || !r1 || !r3) continue;
          const float w2 = r2 ? r2[3] : 0.0f;
          const bool ortho_w = std::fabs(r0[3]) < 1e-3f && std::fabs(r1[3]) < 1e-3f &&
                               std::fabs(w2) < 1e-3f && std::fabs(r3[3] - 1.0f) < 1e-3f;
          if (ortho_at < 0 && ortho_w && r0[0] > 1e-5f && r0[0] < 0.5f && std::fabs(r1[0]) < 1e-4f &&
              std::fabs(r1[1]) > 1e-5f && std::fabs(r1[1]) < 0.5f) {
            ortho_at = int(r); ox = r0[0]; oy = r1[1]; oox = r3[0]; ooy = r3[1];
          }
          bool close = true;
          const float* prows[3] = {r0, r1, r2};
          for (int j = 0; j < 3; ++j) {
            if (!prows[j]) continue;
            if (std::fabs(prows[j][2] - prows[j][3]) > 0.01f * (1.0f + std::fabs(prows[j][2]))) close = false;
          }
          const bool nontrivial = std::fabs(r0[3]) > 1e-3f || std::fabs(r1[3]) > 1e-3f || std::fabs(w2) > 1e-3f;
          if (persp_at < 0 && close && nontrivial && !ortho_w) persp_at = int(r);
        }
        const reg::RB_DEPTHCONTROL depth = draw_util::GetNormalizedDepthControl(*register_file_);
        const uint32_t ps_textures = pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0u;
        char first[160] = {0};
        int fn = 0;
        for (uint32_t r = reg_lo; r < 512 && r <= reg_hi && fn < 150; ++r) {
          if (!rp[r]) continue;
          fn += std::snprintf(first + fn, sizeof(first) - size_t(fn), " c%u=%.4g,%.4g,%.4g,%.4g", r,
                              rp[r][0], rp[r][1], rp[r][2], rp[r][3]);
          if (r >= reg_lo + 5) break;
        }
        REXLOG_INFO("[2d-census] vs {:016X} ps {:016X} depth {} ps_textures {} consts {} ({}..{}) "
                    "ortho_at {} (x {:.5f} y {:.5f} origin {:.3f},{:.3f}) persp_at {} first:{}",
                    vertex_shader ? vertex_shader->ucode_data_hash() : 0ull,
                    pixel_shader ? pixel_shader->ucode_data_hash() : 0ull, depth.z_enable ? 1 : 0,
                    ps_textures, used, reg_lo, reg_hi, ortho_at, ox, oy, oox, ooy, persp_at, first);
      }
    }
    {
      // One-time env seed (NG2_FOV_K) so the widen can be driven headless for
      // testing; the live cvar ng2_fov_k, set from the in-game FOV slider, is
      // the source of truth and is read fresh every draw for real-time control.
      static const bool s_fov_seeded = [] {
        if (const char* e = std::getenv("NG2_FOV_K")) {
          const double v = std::atof(e);
          if (v > 0.05 && v < 1.5) REXCVAR_SET(ng2_fov_k, v);
        }
        return true;
      }();
      (void)s_fov_seeded;
      const float s_fov_k = static_cast<float>(REXCVAR_GET(ng2_fov_k));
      if (s_fov_k > 0.05f && s_fov_k < 1.5f && std::fabs(s_fov_k - 1.0f) > 1e-3f) {
        float* fbase =
            reinterpret_cast<float*>(const_cast<uint8_t*>(float_constants_begin));
        const auto& bm = float_constant_map_vertex.float_bitmap;
        const bool gameplay = REXCVAR_GET(ng2_uw_mode) == 1;

        // The projection's register slot is fixed per vertex shader, so the
        // search for it runs ONCE per shader and is cached; every later draw with
        // that shader is O(1). Searching on every draw (~500-1100 of them per
        // frame) cost about a third of the frame rate. kind: 0 = nothing to
        // scale, 1 = a perspective projection (3D world), 2 = the screen-space 2D
        // UI ortho at c21-c24. base = the block's first register. Runs on the GPU
        // command thread only, so the static cache needs no lock.
        struct FovInfo {
          uint16_t base;
          uint8_t kind;
        };
        static std::unordered_map<const void*, FovInfo> s_fov_cache;
        const void* skey = static_cast<const void*>(vertex_shader);
        FovInfo info{0, 0};
        auto cit = s_fov_cache.find(skey);
        if (cit != s_fov_cache.end()) {
          info = cit->second;
        } else {
          // First draw of this shader: map used registers to their packed vec4
          // slots and find the projection (perspective, z-row optional) or the 2D
          // UI ortho at c21-c24.
          float* rp[512] = {nullptr};
          uint32_t reg_lo = 512, reg_hi = 0, wpos = 0;
          for (uint32_t i = 0; i < 4; ++i) {
            uint64_t bits = bm[i];
            uint32_t bit;
            while (rex::bit_scan_forward(bits, &bit)) {
              bits &= ~(1ull << bit);
              const uint32_t reg = (i << 6) + bit;
              if (reg < 512) {
                rp[reg] = fbase + wpos * 4;
                if (reg < reg_lo) reg_lo = reg;
                if (reg > reg_hi) reg_hi = reg;
              }
              ++wpos;
            }
          }
          for (uint32_t r = reg_lo; r + 3 <= reg_hi && info.kind == 0; ++r) {
            // Rows r (x-out), r+1 (y-out), r+2 (z-out, OPTIONAL - effect shaders
            // that ignore depth omit it, e.g. the sword-tip glow) and r+3 (w-out).
            float* r0 = rp[r];
            float* r1 = rp[r + 1];
            float* r2 = rp[r + 2];
            float* r3 = rp[r + 3];
            if (!r0 || !r1 || !r3) continue;
            // Perspective: each present output row's w tracks its z (the w column
            // follows the z column - the hallmark of a perspective divide), and
            // the w column is not the 2D ortho [0,0,0,1].
            bool close = true;
            const float* prows[3] = {r0, r1, r2};
            for (int j = 0; j < 3; ++j) {
              if (!prows[j]) continue;
              const float z = prows[j][2], w = prows[j][3];
              if (std::fabs(z - w) > 0.01f * (1.0f + std::fabs(z))) close = false;
            }
            const float w0 = r0[3], w1 = r1[3], w3 = r3[3];
            const float w2 = r2 ? r2[3] : 0.0f;
            const bool ortho = std::fabs(w0) < 1e-3f && std::fabs(w1) < 1e-3f &&
                               std::fabs(w2) < 1e-3f && std::fabs(w3 - 1.0f) < 1e-3f;
            const bool nontrivial = std::fabs(w0) > 1e-3f ||
                                    std::fabs(w1) > 1e-3f || std::fabs(w2) > 1e-3f;
            if (close && nontrivial && !ortho) {
              info.base = static_cast<uint16_t>(r);
              info.kind = 1;
            }
          }
          if (info.kind == 0 && rp[21] && rp[22] && rp[23] && rp[24]) {
            // Screen-space 2D UI ortho: top-left origin, small +x / -y scale,
            // constant w column. In gameplay its column 0 is scaled too, so the
            // HUD shrinks to a centred 16:9 band instead of stretching.
            float* r0 = rp[21];
            float* r1 = rp[22];
            float* r2 = rp[23];
            float* r3 = rp[24];
            const bool ortho_w = std::fabs(r0[3]) < 1e-3f && std::fabs(r1[3]) < 1e-3f &&
                                 std::fabs(r2[3]) < 1e-3f && std::fabs(r3[3] - 1.0f) < 1e-3f;
            const bool ui_scale = r0[0] > 1e-5f && r0[0] < 0.5f &&
                                  std::fabs(r0[1]) < 1e-4f && std::fabs(r0[2]) < 1e-4f &&
                                  std::fabs(r1[0]) < 1e-4f && r1[1] < -1e-5f;
            const bool ui_origin = std::fabs(r3[0] + 1.0f) < 0.01f &&
                                   std::fabs(r3[1] - 1.0f) < 0.01f;
            if (ortho_w && ui_scale && ui_origin) {
              info.base = 21;
              info.kind = 2;
            }
          }
          s_fov_cache.emplace(skey, info);
        }
        // Apply. The block's register positions come from a popcount of the used-
        // constant bitmap below each register, so no per-draw table is built.
        if (info.kind != 0) {
          // [ng2-fade] A 2D UI draw that samples no texture is a SOLID FILL - the
          // shape of NG2's full-screen fade-to-black (and full-screen colour
          // tints). Measured (2026-09-14): across gameplay, combat, menus, cards
          // and loading the ONLY textureless 2D UI draw is the fade - the HUD,
          // menus and cards are all textured - so this cleanly identifies the
          // fade without touching anything else.
          const bool is_solid_2d =
              info.kind == 2 &&
              (!pixel_shader ||
               pixel_shader->GetTextureBindingsAfterTranslation().empty());
          if (info.kind == 1) {
            ++g_ng2_persp_this_frame;
          } else {
            ++g_ng2_2d_this_frame;
            if (is_solid_2d) ++g_ng2_2d_solid_this_frame;
          }
          // Column-0 scale: widen the 3D world (kind 1) and COMPRESS the textured
          // 2D HUD (kind 2) to a centred 16:9 band, so both survive the
          // presenter's stretch to full width. SKIP a solid-fill 2D draw: leaving
          // the fade quad uncompressed lets it cover the whole 16:9 render, which
          // the presenter stretches to a flat full-width black - instead of a
          // centred 16:9 black band with the FOV-widened 3D leaking through on the
          // sides (the old "black centre, scene on the sides" fade).
          if (gameplay && !is_solid_2d) {
            const uint32_t B = info.base;
            uint32_t pos = 0;
            const uint32_t wrd = B >> 6, b = B & 63;
            for (uint32_t i = 0; i < wrd; ++i) pos += rex::bit_count(bm[i]);
            if (b) pos += rex::bit_count(bm[wrd] & ((1ull << b) - 1));
            for (uint32_t rr = B; rr <= B + 3; ++rr) {
              if ((bm[rr >> 6] >> (rr & 63)) & 1ull) {
                fbase[pos * 4] *= s_fov_k;  // column 0 (x-output) of this row
                ++pos;
              }
            }
          }
        }
      }
    }
    if (REXCVAR_GET(draw_census) > 0) {
      // Read back what was just written, so this reports what the GPU will see.
      size_t value_count = size_t(float_constants - float_constants_begin) / sizeof(float);
      const float* values = reinterpret_cast<const float*>(float_constants_begin);
      ++draw_census_.vs_const_uploads;
      draw_census_.vs_const_values += value_count;
      draw_census_.vs_const_max_count =
          std::max(draw_census_.vs_const_max_count, float_constant_count_vertex);
      // Rebuild the constant index for each uploaded vec4, in the same order
      // the loop above wrote them, so a NaN can be attributed to its constant.
      uint32_t uploaded_indices[256];
      uint32_t uploaded_count = 0;
      for (uint32_t i = 0; i < 4 && uploaded_count < 256; ++i) {
        uint64_t bits = float_constant_map_vertex.float_bitmap[i];
        uint32_t bit;
        while (rex::bit_scan_forward(bits, &bit) && uploaded_count < 256) {
          bits &= ~(1ull << bit);
          uploaded_indices[uploaded_count++] = (i << 6) + bit;
        }
      }
      bool draw_has_nan = false;
      bool draw_all_zero = value_count > 0;
      for (size_t vi = 0; vi < value_count; ++vi) {
        float v = values[vi];
        if (v != 0.0f) {
          draw_all_zero = false;
        }
        if (std::isnan(v)) {
          draw_has_nan = true;
          ++draw_census_.vs_const_nan;
          if (draw_census_.vs_const_nan_patterns.size() < 64) {
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            ++draw_census_.vs_const_nan_patterns[bits];
          }
          size_t vec4 = vi >> 2;
          if (vec4 < uploaded_count) {
            uint32_t ci = uploaded_indices[vec4];
            draw_census_.vs_const_nan_bitmap[ci >> 6] |= 1ull << (ci & 63);
            if (draw_census_.vs_const_first_nan_index < 0) {
              draw_census_.vs_const_first_nan_index = int32_t(ci);
            }
          }
        } else if (std::isinf(v)) {
          ++draw_census_.vs_const_inf;
        } else if (v == 0.0f) {
          ++draw_census_.vs_const_zero;
        } else if (std::fabs(v) > 1.0e9f) {
          ++draw_census_.vs_const_huge;
        }
      }
      ++draw_census_.draws_classified;
      draw_census_.draws_with_nan += draw_has_nan ? 1 : 0;
      draw_census_.draws_all_zero += draw_all_zero ? 1 : 0;
      if (shared_memory_is_uav) {
        ++draw_census_.mx_draws_classified;
        draw_census_.mx_draws_with_nan += draw_has_nan ? 1 : 0;
        draw_census_.mx_draws_all_zero += draw_all_zero ? 1 : 0;
      }
    }
    // CAUSALITY TEST for the NaN the census found in these constants. The
    // census proves NaN ARRIVES; it cannot prove the NaN is why nothing draws.
    // Substituting a finite value settles that: if the scene or the characters
    // change under either mode, the NaN is the cause rather than a symptom.
    // Applied after the census so the reported counts stay honest.
    const int32_t nan_fix = REXCVAR_GET(diag_vs_const_nan_fix);
    if (nan_fix > 0) {
      float* mutable_values = reinterpret_cast<float*>(const_cast<uint8_t*>(float_constants_begin));
      const size_t total = size_t(float_constants - float_constants_begin) / sizeof(float);
      uint32_t indices[256];
      uint32_t index_count = 0;
      for (uint32_t i = 0; i < 4 && index_count < 256; ++i) {
        uint64_t bits = float_constant_map_vertex.float_bitmap[i];
        uint32_t bit;
        while (rex::bit_scan_forward(bits, &bit) && index_count < 256) {
          bits &= ~(1ull << bit);
          indices[index_count++] = (i << 6) + bit;
        }
      }
      for (size_t vi = 0; vi < total; ++vi) {
        if (!std::isnan(mutable_values[vi])) {
          continue;
        }
        float replacement = 0.0f;
        if (nan_fix >= 2) {
          const size_t vec4 = vi >> 2;
          const uint32_t ci = vec4 < index_count ? indices[vec4] : uint32_t(vec4);
          // Row (ci % 4) of an identity matrix carries 1.0 in component ci % 4.
          replacement = ((ci & 3u) == (vi & 3u)) ? 1.0f : 0.0f;
        }
        mutable_values[vi] = replacement;
      }
    }
    cbuffer_binding_float_vertex_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_vertex);
  }
  if (!cbuffer_binding_float_pixel_.up_to_date) {
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_pixel, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_pixel_.address);
    if (float_constants == nullptr) {
      return false;
    }
    if (pixel_shader != nullptr) {
      const Shader::ConstantRegisterMap& float_constant_map_pixel =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry = float_constant_map_pixel.float_bitmap[i];
        uint32_t float_constant_index;
        while (rex::bit_scan_forward(float_constant_map_entry, &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(
              float_constants,
              &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (float_constant_index << 2)],
              4 * sizeof(float));
          float_constants += 4 * sizeof(float);
        }
      }
    }
    cbuffer_binding_float_pixel_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_pixel);
  }
  if (!cbuffer_binding_bool_loop_.up_to_date) {
    constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
    uint8_t* bool_loop_constants = constant_buffer_pool_->Request(
        frame_current_, kBoolLoopConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_bool_loop_.address);
    if (bool_loop_constants == nullptr) {
      return false;
    }
    std::memcpy(bool_loop_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    cbuffer_binding_bool_loop_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_bool_loop_constants);
  }
  if (!cbuffer_binding_fetch_.up_to_date) {
    constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
    uint8_t* fetch_constants = constant_buffer_pool_->Request(
        frame_current_, kFetchConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_fetch_.address);
    if (fetch_constants == nullptr) {
      return false;
    }
    std::memcpy(fetch_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
    cbuffer_binding_fetch_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_fetch_constants);
  }

  //
  // Update descriptors.
  //

  if (!current_shared_memory_binding_is_uav_.has_value() ||
      current_shared_memory_binding_is_uav_.value() != shared_memory_is_uav) {
    current_shared_memory_binding_is_uav_ = shared_memory_is_uav;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_shared_memory_and_bindful_edram);
  }

  // Get textures and samplers used by the vertex shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_vertex = vertex_shader->GetTextureBindingLayoutUserUID();
  size_t sampler_layout_uid_vertex = vertex_shader->GetSamplerBindingLayoutUserUID();
  const std::vector<D3D12Shader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  const std::vector<D3D12Shader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  size_t texture_count_vertex = textures_vertex.size();
  size_t sampler_count_vertex = samplers_vertex.size();
  if (sampler_count_vertex) {
    if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
      current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      bindful_samplers_written_vertex_ = false;
    }
    current_samplers_vertex_.resize(
        std::max(current_samplers_vertex_.size(), sampler_count_vertex));
    for (size_t i = 0; i < sampler_count_vertex; ++i) {
      D3D12TextureCache::SamplerParameters parameters =
          texture_cache_->GetSamplerParameters(samplers_vertex[i]);
      if (current_samplers_vertex_[i] != parameters) {
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
        bindful_samplers_written_vertex_ = false;
        current_samplers_vertex_[i] = parameters;
      }
    }
  }

  // Get textures and samplers used by the pixel shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_pixel, sampler_layout_uid_pixel;
  const std::vector<D3D12Shader::TextureBinding>* textures_pixel;
  const std::vector<D3D12Shader::SamplerBinding>* samplers_pixel;
  size_t texture_count_pixel, sampler_count_pixel;
  if (pixel_shader != nullptr) {
    texture_layout_uid_pixel = pixel_shader->GetTextureBindingLayoutUserUID();
    sampler_layout_uid_pixel = pixel_shader->GetSamplerBindingLayoutUserUID();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    texture_count_pixel = textures_pixel->size();
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    sampler_count_pixel = samplers_pixel->size();
    if (sampler_count_pixel) {
      if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
        current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
        bindful_samplers_written_pixel_ = false;
      }
      current_samplers_pixel_.resize(
          std::max(current_samplers_pixel_.size(), size_t(sampler_count_pixel)));
      for (uint32_t i = 0; i < sampler_count_pixel; ++i) {
        D3D12TextureCache::SamplerParameters parameters =
            texture_cache_->GetSamplerParameters((*samplers_pixel)[i]);
        if (current_samplers_pixel_[i] != parameters) {
          current_samplers_pixel_[i] = parameters;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          bindful_samplers_written_pixel_ = false;
        }
      }
    }
  } else {
    texture_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    sampler_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    textures_pixel = nullptr;
    texture_count_pixel = 0;
    samplers_pixel = nullptr;
    sampler_count_pixel = 0;
  }

  assert_true(sampler_count_vertex + sampler_count_pixel <= kSamplerHeapSize);

  if (bindless_resources_used_) {
    //
    // Bindless descriptors path.
    //

    // Check if need to write new descriptor indices.
    // Samplers have already been checked.
    if (texture_count_vertex && cbuffer_binding_descriptor_indices_vertex_.up_to_date &&
        (current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_vertex_.data(),
                                                          textures_vertex.data(),
                                                          texture_count_vertex))) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
    if (texture_count_pixel && cbuffer_binding_descriptor_indices_pixel_.up_to_date &&
        (current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_pixel_.data(),
                                                          textures_pixel->data(),
                                                          texture_count_pixel))) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }

    // Get sampler descriptor indices, write new samplers, and handle sampler
    // heap overflow if it happens.
    if ((sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) ||
        (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date)) {
      for (uint32_t i = 0; i < 2; ++i) {
        if (i) {
          // Overflow happened - invalidate sampler bindings because their
          // descriptor indices can't be used anymore (and even if heap creation
          // fails, because current_sampler_bindless_indices_#_ are in an
          // undefined state now) and switch to a new sampler heap.
          cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          ID3D12DescriptorHeap* sampler_heap_new;
          if (!sampler_bindless_heaps_overflowed_.empty() &&
              sampler_bindless_heaps_overflowed_.front().second <= submission_completed_) {
            sampler_heap_new = sampler_bindless_heaps_overflowed_.front().first;
            sampler_bindless_heaps_overflowed_.pop_front();
          } else {
            D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_new_desc;
            sampler_heap_new_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            sampler_heap_new_desc.NumDescriptors = kSamplerHeapSize;
            sampler_heap_new_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            sampler_heap_new_desc.NodeMask = 0;
            if (FAILED(device->CreateDescriptorHeap(&sampler_heap_new_desc,
                                                    IID_PPV_ARGS(&sampler_heap_new)))) {
              REXGPU_ERROR(
                  "Failed to create a new bindless sampler descriptor heap "
                  "after an overflow of the previous one");
              return false;
            }
          }
          // Only change the heap if a new heap was created successfully, not to
          // leave the values in an undefined state in case CreateDescriptorHeap
          // has failed.
          sampler_bindless_heaps_overflowed_.push_back(
              std::make_pair(sampler_bindless_heap_current_, submission_current_));
          sampler_bindless_heap_current_ = sampler_heap_new;
          sampler_bindless_heap_cpu_start_ =
              sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_gpu_start_ =
              sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_allocated_ = 0;
          // The only thing the heap is used for now is texture cache samplers -
          // invalidate all of them.
          texture_cache_bindless_sampler_map_.clear();
          deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                    sampler_bindless_heap_current_);
          current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_SamplerHeap);
        }
        bool samplers_overflowed = false;
        if (sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
          current_sampler_bindless_indices_vertex_.resize(std::max(
              current_sampler_bindless_indices_vertex_.size(), size_t(sampler_count_vertex)));
          for (uint32_t j = 0; j < sampler_count_vertex; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_vertex_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_vertex_[j] = sampler_index;
          }
        }
        if (samplers_overflowed) {
          continue;
        }
        if (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
          current_sampler_bindless_indices_pixel_.resize(std::max(
              current_sampler_bindless_indices_pixel_.size(), size_t(sampler_count_pixel)));
          for (uint32_t j = 0; j < sampler_count_pixel; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_pixel_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_pixel_[j] = sampler_index;
          }
        }
        if (!samplers_overflowed) {
          break;
        }
      }
    }

    if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_vertex + sampler_count_vertex, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_vertex_.address));
      if (!descriptor_indices) {
        DrawFailReason("descriptor indices cbuffer (vertex)");
        return false;
      }
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        const D3D12Shader::TextureBinding& texture = textures_vertex[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      if (texture_count_vertex) {
        current_texture_srv_keys_vertex_.resize(
            std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                  textures_vertex.data(), texture_count_vertex);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        descriptor_indices[samplers_vertex[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_vertex_[i];
      }
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesVertex);
    }

    if (!cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_pixel + sampler_count_pixel, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_pixel_.address));
      if (!descriptor_indices) {
        DrawFailReason("descriptor indices cbuffer (pixel)");
        return false;
      }
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        const D3D12Shader::TextureBinding& texture = (*textures_pixel)[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      if (texture_count_pixel) {
        current_texture_srv_keys_pixel_.resize(
            std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                  textures_pixel->data(), texture_count_pixel);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        descriptor_indices[(*samplers_pixel)[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_pixel_[i];
      }
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesPixel);
    }
  } else {
    //
    // Bindful descriptors path.
    //

    // See what descriptors need to be updated.
    // Samplers have already been checked.
    bool write_textures_vertex =
        texture_count_vertex && (!bindful_textures_written_vertex_ ||
                                 current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
                                 !texture_cache_->AreActiveTextureSRVKeysUpToDate(
                                     current_texture_srv_keys_vertex_.data(),
                                     textures_vertex.data(), texture_count_vertex));
    bool write_textures_pixel =
        texture_count_pixel &&
        (!bindful_textures_written_pixel_ ||
         current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_pixel_.data(), textures_pixel->data(), texture_count_pixel));
    bool write_samplers_vertex = sampler_count_vertex && !bindful_samplers_written_vertex_;
    bool write_samplers_pixel = sampler_count_pixel && !bindful_samplers_written_pixel_;
    bool edram_rov_used =
        render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

    // Allocate the descriptors.
    size_t view_count_partial_update = 0;
    if (write_textures_vertex) {
      view_count_partial_update += texture_count_vertex;
    }
    if (write_textures_pixel) {
      view_count_partial_update += texture_count_pixel;
    }
    // Shared memory SRV and null UAV + null SRV and shared memory UAV +
    // textures.
    size_t view_count_full_update = 4 + texture_count_vertex + texture_count_pixel;
    if (edram_rov_used) {
      // + EDRAM UAV in two tables (with the shared memory SRV and with the
      // shared memory UAV).
      view_count_full_update += 2;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE view_cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE view_gpu_handle;
    uint32_t descriptor_size_view = provider.GetViewDescriptorSize();
    uint64_t view_heap_index = RequestViewBindfulDescriptors(
        draw_view_bindful_heap_index_, uint32_t(view_count_partial_update),
        uint32_t(view_count_full_update), view_cpu_handle, view_gpu_handle);
    if (view_heap_index == ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      REXGPU_ERROR("Failed to allocate view descriptors");
      return false;
    }
    size_t sampler_count_partial_update = 0;
    if (write_samplers_vertex) {
      sampler_count_partial_update += sampler_count_vertex;
    }
    if (write_samplers_pixel) {
      sampler_count_partial_update += sampler_count_pixel;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_handle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_handle = {};
    uint32_t descriptor_size_sampler = provider.GetSamplerDescriptorSize();
    uint64_t sampler_heap_index = ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
    if (sampler_count_vertex != 0 || sampler_count_pixel != 0) {
      sampler_heap_index = RequestSamplerBindfulDescriptors(
          draw_sampler_bindful_heap_index_, uint32_t(sampler_count_partial_update),
          uint32_t(sampler_count_vertex + sampler_count_pixel), sampler_cpu_handle,
          sampler_gpu_handle);
      if (sampler_heap_index == ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
        REXGPU_ERROR("Failed to allocate sampler descriptors");
        return false;
      }
    }
    if (draw_view_bindful_heap_index_ != view_heap_index) {
      // Need to update all view descriptors.
      write_textures_vertex = texture_count_vertex != 0;
      write_textures_pixel = texture_count_pixel != 0;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      // If updating fully, write the shared memory SRV and UAV descriptors and,
      // if needed, the EDRAM descriptor.
      // SRV + null UAV + EDRAM.
      gpu_handle_shared_memory_srv_and_edram_ = view_gpu_handle;
      shared_memory_->WriteRawSRVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      ui::ngpu_d3d12::util::CreateBufferRawUAV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      // Null SRV + UAV + EDRAM.
      gpu_handle_shared_memory_uav_and_edram_ = view_gpu_handle;
      ui::ngpu_d3d12::util::CreateBufferRawSRV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      shared_memory_->WriteRawUAVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindful_SharedMemoryAndEdram);
    }
    if (sampler_heap_index != ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid &&
        draw_sampler_bindful_heap_index_ != sampler_heap_index) {
      write_samplers_vertex = sampler_count_vertex != 0;
      write_samplers_pixel = sampler_count_pixel != 0;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Write the descriptors.
    if (write_textures_vertex) {
      assert_true(current_graphics_root_bindful_extras_.textures_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_vertex_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV(textures_vertex[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      current_texture_srv_keys_vertex_.resize(
          std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                textures_vertex.data(), texture_count_vertex);
      bindful_textures_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_vertex);
    }
    if (write_textures_pixel) {
      assert_true(current_graphics_root_bindful_extras_.textures_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_pixel_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV((*textures_pixel)[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      current_texture_srv_keys_pixel_.resize(
          std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                textures_pixel->data(), texture_count_pixel);
      bindful_textures_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_pixel);
    }
    if (write_samplers_vertex) {
      assert_true(current_graphics_root_bindful_extras_.samplers_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_vertex_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        texture_cache_->WriteSampler(current_samplers_vertex_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_vertex);
    }
    if (write_samplers_pixel) {
      assert_true(current_graphics_root_bindful_extras_.samplers_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_pixel_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        texture_cache_->WriteSampler(current_samplers_pixel_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_pixel);
    }

    // Wrote new descriptors on the current page.
    draw_view_bindful_heap_index_ = view_heap_index;
    if (sampler_heap_index != ui::ngpu_d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      draw_sampler_bindful_heap_index_ = sampler_heap_index;
    }
  }

  // Update the root parameters.
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_fetch_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_fetch_constants,
                                                                cbuffer_binding_fetch_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_fetch_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_vertex))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_vertex, cbuffer_binding_float_vertex_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_vertex;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_pixel))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_pixel, cbuffer_binding_float_pixel_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_pixel;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_system_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_system_constants,
                                                                cbuffer_binding_system_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_system_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_bool_loop_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_bool_loop_constants,
                                                                cbuffer_binding_bool_loop_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_bool_loop_constants;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_shared_memory_and_bindful_edram))) {
    assert_true(current_shared_memory_binding_is_uav_.has_value());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_and_bindful_edram;
    if (bindless_resources_used_) {
      gpu_handle_shared_memory_and_bindful_edram = provider.OffsetViewDescriptor(
          view_bindless_heap_gpu_start_,
          uint32_t(current_shared_memory_binding_is_uav_.value()
                       ? SystemBindlessView ::kNullRawSRVAndSharedMemoryRawUAVStart
                       : SystemBindlessView ::kSharedMemoryRawSRVAndNullRawUAVStart));
    } else {
      gpu_handle_shared_memory_and_bindful_edram = current_shared_memory_binding_is_uav_.value()
                                                       ? gpu_handle_shared_memory_uav_and_edram_
                                                       : gpu_handle_shared_memory_srv_and_edram_;
    }
    deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
        root_parameter_shared_memory_and_bindful_edram, gpu_handle_shared_memory_and_bindful_edram);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_shared_memory_and_bindful_edram;
  }
  if (bindless_resources_used_) {
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesPixel))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesPixel,
          cbuffer_binding_descriptor_indices_pixel_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesPixel;
    }
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesVertex))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesVertex,
          cbuffer_binding_descriptor_indices_vertex_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesVertex;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_SamplerHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_SamplerHeap,
                                                               sampler_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_SamplerHeap;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_ViewHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_ViewHeap,
                                                               view_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_ViewHeap;
    }
  } else {
    uint32_t extra_index;
    extra_index = current_graphics_root_bindful_extras_.textures_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.textures_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
  }

  return true;
}

void D3D12CommandProcessor::ResolveDataProviderThunk(
    void* context, std::unique_lock<std::recursive_mutex>& global_lock, uint32_t physical_address,
    uint32_t length, bool is_write) {
  reinterpret_cast<D3D12CommandProcessor*>(context)->ResolveDataProvider(
      global_lock, physical_address, length, is_write);
}

void D3D12CommandProcessor::ResolveDataProvider(std::unique_lock<std::recursive_mutex>& global_lock,
                                                uint32_t physical_address, uint32_t length,
                                                bool is_write) {
  g_provider_calls.fetch_add(1, std::memory_order_relaxed);
  if (system::XThread::IsInThread(worker_thread_.get())) {
    LandPendingResolveReadbacks(physical_address, length);
    return;
  }
  // Another thread touched the memory: have the worker land the copies,
  // and wait for it with the global lock released (the worker needs it).
  struct Request {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
  };
  auto request = std::make_shared<Request>();
  const auto t0 = std::chrono::steady_clock::now();
  CallInThreadSafe([this, request, physical_address, length]() {
    LandPendingResolveReadbacks(physical_address, length);
    {
      std::lock_guard<std::mutex> lock(request->m);
      request->done = true;
    }
    request->cv.notify_all();
  });
  const bool was_locked = global_lock.owns_lock();
  if (was_locked) global_lock.unlock();
  bool timed_out = false;
  {
    std::unique_lock<std::mutex> lock(request->m);
    timed_out = !request->cv.wait_for(lock, std::chrono::seconds(3), [&] { return request->done; });
  }
  if (was_locked) global_lock.lock();
  const uint64_t us = uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
          .count());
  g_provider_waits.fetch_add(1, std::memory_order_relaxed);
  g_provider_wait_us.fetch_add(us, std::memory_order_relaxed);
  if (timed_out) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 5)
      REXGPU_WARN("On-demand readback: the GPU worker did not land 0x{:08X}+{} within 3 s",
                  physical_address, length);
  }
}

void D3D12CommandProcessor::LandPendingResolveReadbacks(uint32_t address, uint32_t length) {
  FenceReasonScope fence_reason(fence_reason_, "on-demand readback");
  const uint64_t end = uint64_t(address) + length;
  uint64_t await = 0;
  bool need_submit = false;
  for (const PendingResolveReadback& p : pending_resolve_readbacks_) {
    if (p.address >= end || uint64_t(p.address) + p.length <= address) continue;
    await = std::max(await, p.submission);
    if (p.submission >= GetCurrentSubmission()) need_submit = true;
  }
  if (await) {
    if (need_submit && submission_open_) {
      EndSubmission(false);  // the copy is queued in the open submission
    }
    await = std::min(await, submission_current_ - 1);
    CheckSubmissionFence(await);
    const uint64_t completed = GetCompletedSubmission();
    size_t kept = 0;
    for (size_t i = 0; i < pending_resolve_readbacks_.size(); ++i) {
      const PendingResolveReadback p = pending_resolve_readbacks_[i];
      const bool overlaps = !(p.address >= end || uint64_t(p.address) + p.length <= address);
      if (!overlaps || p.submission > completed) {
        pending_resolve_readbacks_[kept++] = p;
        continue;
      }
      shared_memory_->UnprotectGpuRange(p.address, p.length);
      bool landed_here = false;
      auto it = readback_buffers_.find(p.key);
      if (it != readback_buffers_.end()) {
        ReadbackBuffer& rb = it->second;
        if (rb.buffers[p.index] && rb.mapped_data[p.index] && p.length <= rb.sizes[p.index]) {
          CopyToGuestMemory(p.address, rb.mapped_data[p.index], p.length);
          landed_here = true;
        }
      }
      // Fourth of four sites with this shape. The comment below already admits a
      // dropped copy can be left here; now it is counted instead of assumed.
      if (!landed_here) g_superseded_dropped.fetch_add(1, std::memory_order_relaxed);
      ::ng2::ngpu::rtc::GuestDataProvidersDisable(memory_, /* NATIVE PATCH */ p.address, p.length);
    }
    pending_resolve_readbacks_.resize(kept);
  }
  // Whatever is left watched in the range (a dropped copy, a race) is
  // released: the faulting thread must be able to proceed.
  ::ng2::ngpu::rtc::GuestDataProvidersDisable(memory_, /* NATIVE PATCH */ address, length);
}

bool NgpuParallelCopy(void* dst, const void* src, size_t size);   // shared_memory.cpp (NATIVE PATCH)

bool D3D12CommandProcessor::CopyToGuestMemory(uint32_t address, const void* source,
                                              uint32_t length) {
  if (!length || !source) return false;
  uint8_t* destination = memory_->TranslatePhysical(address);
  if (!destination) return false;
  const auto landing_t0 = std::chrono::steady_clock::now();
  struct LandingTimer {
    std::chrono::steady_clock::time_point t0;
    ~LandingTimer() {
      g_landing_count.fetch_add(1, std::memory_order_relaxed);
      g_landing_us.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count()),
          std::memory_order_relaxed);
    }
  } landing_timer{landing_t0};
  // Still allocated? The physical parent heap's page table (an array
  // lookup; the release path clears it under the global critical region).
  // VirtualQuery was tried first: the guest views carry thousands of small
  // regions from the per-page write watches, so one query per region span
  // was hundreds of kernel calls per copy - 22 fps, then 4 fps chunked,
  // against 58-60 without a check. The copy itself runs without the lock.
  {
    auto global_lock = rex::thread::global_critical_region::AcquireDirect();
    g_landing_lock_us.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - landing_t0)
                     .count()),
        std::memory_order_relaxed);
    memory::BaseHeap* heap = ::ng2::ngpu::rtc::GuestPhysicalHeap(memory_);   // NATIVE PATCH: null unless the native backend owns guest memory
    const uint64_t end = uint64_t(address) + length;
    uint64_t cursor = address;
    bool allocated = heap != nullptr;
    while (allocated && cursor < end) {
      memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(uint32_t(cursor), &info) || !info.state) {
        allocated = false;
        break;
      }
      const uint64_t region_end = uint64_t(info.base_address) + info.region_size;
      if (region_end <= cursor) {
        allocated = false;
        break;
      }
      cursor = std::min(region_end, end);
    }
    if (!allocated) {
      // The game freed the target meanwhile (a save thumbnail read and
      // freed before the GPU had finished it, 16:20 and 16:35).
      g_guest_copy_faults.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  // Declared to the shared memory first when the pages are all valid and
  // GPU-written: the bytes below are the ones the GPU buffer holds, so the
  // write must not invalidate them or fire the texture watches (which made
  // the texture cache reload every resolved render target once a frame).
  const bool quiet = shared_memory_ && shared_memory_->BeginSelfCopy(address, length);
  if (!NgpuParallelCopy(destination, source, length))   // NATIVE PATCH: 1 MB+ split over the upload copy pool
    std::memcpy(destination, source, length);
  if (quiet) {
    shared_memory_->EndSelfCopy(address, length);
    g_guest_copy_quiet.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

void D3D12CommandProcessor::DropPendingResolveReadbacks(uint64_t key, uint32_t index) {
  size_t kept = 0;
  for (size_t i = 0; i < pending_resolve_readbacks_.size(); ++i) {
    const PendingResolveReadback& p = pending_resolve_readbacks_[i];
    if (p.key != key || p.index != index) {
      pending_resolve_readbacks_[kept++] = p;
    } else if (shared_memory_) {
      shared_memory_->UnprotectGpuRange(p.address, p.length);
      if (REXCVAR_GET(readback_resolve_on_demand))
        ::ng2::ngpu::rtc::GuestDataProvidersDisable(memory_, /* NATIVE PATCH */ p.address, p.length);
    }
  }
  pending_resolve_readbacks_.resize(kept);
}

void D3D12CommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  if (buffer_map.empty()) {
    return;
  }
  const uint64_t eviction_frame_floor = (frame_current_ > kReadbackBufferEvictionAgeFrames)
                                            ? (frame_current_ - kReadbackBufferEvictionAgeFrames)
                                            : 0;
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    ReadbackBuffer& readback = it->second;
    bool evict =
        buffer_map.size() > kMaxReadbackBuffers || readback.last_used_frame < eviction_frame_floor;
    if (!evict) {
      ++it;
      continue;
    }
    for (uint32_t i = 0; i < kReadbackSlots; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
        }
        // [readback] A copy into it may be in flight: release later.
        DropPendingResolveReadbacks(it->first, i);
        readback_buffers_to_release_.emplace_back(GetCurrentSubmission(), readback.buffers[i]);
      }
      readback.buffers[i] = nullptr;
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
    it = buffer_map.erase(it);
  }
}

ID3D12Resource* D3D12CommandProcessor::RequestReadbackBuffer(uint32_t size) {
  if (size == 0) {
    return nullptr;
  }
  size = rex::align(size, kReadbackBufferSizeIncrement);
  if (size > readback_buffer_size_) {
    const ui::ngpu_d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::ngpu_d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer;
    if (FAILED(device->CreateCommittedResource(
            &ui::ngpu_d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return nullptr;
    }
    if (readback_buffer_ != nullptr) {
      readback_buffer_->Release();
    }
    readback_buffer_ = buffer;
    readback_buffer_size_ = size;
  }
  return readback_buffer_;
}

bool D3D12CommandProcessor::InitializeOcclusionQueryResources() {
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
  occlusion_query_heap_.Reset();
  occlusion_query_readback_.Reset();
  occlusion_query_readback_mapping_ = nullptr;

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (!device) {
    return false;
  }

  D3D12_QUERY_HEAP_DESC heap_desc;
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = kMaxOcclusionQueries;
  heap_desc.NodeMask = 0;
  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&occlusion_query_heap_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to create occlusion query heap, using fake sample counts");
    return false;
  }

  D3D12_RESOURCE_DESC buffer_desc;
  ui::ngpu_d3d12::util::FillBufferResourceDesc(buffer_desc, sizeof(uint64_t) * kMaxOcclusionQueries,
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(&ui::ngpu_d3d12::util::kHeapPropertiesReadback,
                                             GetD3D12Provider().GetHeapFlagCreateNotZeroed(),
                                             &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&occlusion_query_readback_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to allocate occlusion query readback buffer, using fake "
        "sample counts");
    occlusion_query_heap_.Reset();
    return false;
  }

  D3D12_RANGE read_range = {0, sizeof(uint64_t) * kMaxOcclusionQueries};
  void* mapping = nullptr;
  if (FAILED(occlusion_query_readback_->Map(0, &read_range, &mapping))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to map occlusion query readback buffer, using fake sample "
        "counts");
    occlusion_query_readback_.Reset();
    occlusion_query_heap_.Reset();
    return false;
  }

  occlusion_query_readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);
  occlusion_query_resources_available_ = true;
  return true;
}

void D3D12CommandProcessor::ShutdownOcclusionQueryResources() {
  DisableHostOcclusionQueries();

  if (occlusion_query_readback_ && occlusion_query_readback_mapping_) {
    occlusion_query_readback_->Unmap(0, nullptr);
  }
  occlusion_query_readback_mapping_ = nullptr;
  occlusion_query_readback_.Reset();
  occlusion_query_heap_.Reset();
}

bool D3D12CommandProcessor::AcquireOcclusionQueryIndex(uint32_t& host_index_out) {
  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {
    occlusion_query_cursor_ = 0;
  }
  host_index_out = occlusion_query_cursor_++;
  return true;
}

void D3D12CommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_heap_) {
    uint32_t host_index = active_occlusion_query_.host_index;
    // Clear before EndSubmission to prevent the EndSubmission safety net from issuing a second
    // EndQuery for the same index.
    active_occlusion_query_ = {};
    if (BeginSubmission(true)) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         host_index);
      EndSubmission(false);
    }
  } else {
    active_occlusion_query_ = {};
  }
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
}

bool D3D12CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Occlusion query begin issued while another query is active");
    DisableHostOcclusionQueries();
    return false;
  }

  uint32_t host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                       host_index);
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.host_index = host_index;
  active_occlusion_query_.valid = true;
  return true;
}

bool D3D12CommandProcessor::EndGuestOcclusionQuery(
    uint32_t sample_count_address, xenos::xe_gpu_depth_sample_counts* sample_counts) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      !active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {
    return false;
  }

  uint32_t host_index = active_occlusion_query_.host_index;
  active_occlusion_query_ = {};

  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                     host_index);
  deferred_command_list_.D3DResolveQueryData(
      occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, host_index, 1,
      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);

  if (!EndSubmission(false)) {
    return false;
  }

  uint64_t query_submission = submission_current_ ? submission_current_ - 1 : 0;
  {
    FenceReasonScope fence_reason(fence_reason_, "query result");
    CheckSubmissionFence(query_submission);
  }
  if (submission_completed_ < query_submission) {
    return false;
  }
  if (!occlusion_query_readback_mapping_) {
    return false;
  }

  uint64_t samples = occlusion_query_readback_mapping_[host_index];
  samples = NormalizeOcclusionSamples(samples);
  WriteGuestOcclusionResult(sample_counts, samples);
  return true;
}

uint64_t D3D12CommandProcessor::NormalizeOcclusionSamples(uint64_t samples) const {
  if (samples == 0 || !texture_cache_) {
    return samples;
  }
  uint64_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint64_t scale_y = texture_cache_->draw_resolution_scale_y();
  uint64_t scale = scale_x * scale_y;
  if (scale <= 1) {
    return samples;
  }
  return (samples + (scale >> 1)) / scale;
}

void D3D12CommandProcessor::WriteGuestOcclusionResult(
    xenos::xe_gpu_depth_sample_counts* sample_counts, uint64_t samples) {
  if (!sample_counts) {
    return;
  }
  uint32_t clamped = samples > uint64_t(UINT32_MAX) ? UINT32_MAX : uint32_t(samples);
  sample_counts->Total_A = clamped;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = clamped;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

void D3D12CommandProcessor::WriteGammaRampSRV(bool is_pwl,
                                              D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  if (is_pwl) {
    desc.Format = DXGI_FORMAT_R16G16_UINT;
    desc.Buffer.FirstElement = 256 * 4 / 4;
    desc.Buffer.NumElements = 128 * 3;
  } else {
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = 256;
  }
  device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &desc, handle);
}

}  // namespace rex::graphics::ngpu_d3d12
