// VENDORED from rexglue-src 23ace0b:src/graphics/flags.cpp - systematic renames only (see vendor_rtc_d3d12.py / ORIGIN.txt):
// namespaces d3d12 -> ngpu_d3d12, plugin headers -> rtc_d3d12/facade.h, cvars -> plugin registry reads (13 bool, 1 string, 3 int).
#include <string>
#include <cstdint>
#include <rex/logging.h>
namespace ng2::ngpu::xlat { bool PluginBool(const char*, bool); std::string PluginString(const char*, const char*); int32_t PluginInt(const char*, int32_t); double PluginDouble(const char*, double); }
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/flags.h>
#include <rex/logging.h>
// NATIVE PATCH: <rex/ui/renderdoc_api.h> not included (no RenderDoc detection)

bool& FLAGS_gpu_allow_invalid_fetch_constants_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("gpu_allow_invalid_fetch_constants", false); return s; }
bool& FLAGS_native_2x_msaa_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("native_2x_msaa", true); return s; }
bool& FLAGS_diag_alpha_test_off_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("diag_alpha_test_off", false); return s; }
bool& FLAGS_diag_cull_none_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("diag_cull_none", false); return s; }
bool& FLAGS_diag_depth_always_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("diag_depth_always", false); return s; }
int32_t& FLAGS_diag_vs_const_nan_fix_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("diag_vs_const_nan_fix", 0); return s; }
int32_t& FLAGS_draw_census_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("draw_census", 0); return s; }
double& FLAGS_ng2_fov_k_storage_() { static double s = ::ng2::ngpu::xlat::PluginDouble("ng2_fov_k", 1.0); return s; }
int32_t& FLAGS_ng2_uw_mode_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("ng2_uw_mode", 0); return s; }
int32_t& FLAGS_ng2_uw_fade_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("ng2_uw_fade", 0); return s; }
int32_t& FLAGS_ng2_uw_fade_frames_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("ng2_uw_fade_frames", 12); return s; }
bool& FLAGS_spirv_disable_rounding_mode_rte_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("spirv_disable_rounding_mode_rte", false); return s; }
bool& FLAGS_force_depth_clamp_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("force_depth_clamp", false); return s; }
bool& FLAGS_depth_float24_round_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("depth_float24_round", false); return s; }
bool& FLAGS_depth_float24_convert_in_pixel_shader_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("depth_float24_convert_in_pixel_shader", false); return s; }
bool& FLAGS_depth_transfer_not_equal_test_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("depth_transfer_not_equal_test", true); return s; }
bool& FLAGS_gamma_render_target_as_unorm16_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("gamma_render_target_as_unorm16", true); return s; }
std::string& FLAGS_dump_shaders_storage_() { static std::string s = ::ng2::ngpu::xlat::PluginString("dump_shaders", ""); return s; }
bool& FLAGS_use_fuzzy_alpha_epsilon_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("use_fuzzy_alpha_epsilon", false); return s; }
bool& FLAGS_gpu_debug_markers_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("gpu_debug_markers", false); return s; }

bool IsGpuDebugMarkersEnabled() {
  static bool cached = false;
  static bool result = false;
  if (!cached) {
    cached = true;
    if (REXCVAR_GET(gpu_debug_markers)) {
      result = true;
      REXLOG_INFO("GPU debug markers enabled via CVar");
    }   // NATIVE PATCH: no RenderDoc detection
  }
  return result;
}
