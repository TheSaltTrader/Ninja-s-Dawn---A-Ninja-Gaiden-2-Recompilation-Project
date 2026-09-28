#include <rex/cvar.h>
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

// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(gpu_allow_invalid_fetch_constants, false, "GPU",
                    "Allow invalid fetch constants");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(native_2x_msaa, true, "GPU", "Enable native 2x MSAA");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(diag_alpha_test_off, false, "GPU",
                    "DIAGNOSTIC: never reject a pixel on the alpha test. Makes alpha-tested "
                    "geometry render solid - the point is to find out whether the alpha test "
                    "is what removes geometry that is otherwise submitted.");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(diag_cull_none, false, "GPU",
                    "DIAGNOSTIC: force the rasterizer cull mode to NONE, ignoring the "
                    "guest's face culling. Renders back faces that should be hidden - the "
                    "point is to find out whether geometry is being culled away.");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(diag_depth_always, false, "GPU",
                    "DIAGNOSTIC: force the depth comparison to ALWAYS, so nothing is "
                    "rejected by the depth test. Breaks occlusion - the point is to find "
                    "out whether geometry is being depth-rejected.");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_INT32(diag_vs_const_nan_fix, 0, "GPU",
                     "DIAGNOSTIC: substitute for NaN in vertex shader float constants. "
                     "1 replaces NaN with 0; 2 replaces it with the matching row of an "
                     "identity matrix (assuming 4-register-aligned matrices). The census "
                     "proved NaN reaches these constants; this establishes whether that "
                     "NaN CAUSES the flat-blue scene or merely accompanies it.");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_INT32(draw_census, 0, "GPU",
                     "Log a summary of draws issued and draws dropped (and why) every N "
                     "seconds. 0 disables. Diagnostic: four of the five ways a draw can be "
                     "dropped return silently, so nothing else distinguishes 'issued but "
                     "invisible' from 'never issued'.");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_DOUBLE(ng2_fov_k, 1.0, "GPU",
                      "NG2 horizontal-FOV (Hor+) scale. Multiplies column 0 of the "
                      "c21-c24 world-view-projection on perspective draws, which widens "
                      "(k<1) or narrows (k>1) horizontal FOV while leaving vertical FOV, "
                      "depth, and 2D/ortho draws (menus, videos, HUD) untouched. 1.0 = "
                      "off. Hot-reloadable: set live from the game's FOV slider via "
                      "rex::cvar::SetFlagByName(\"ng2_fov_k\", ...).");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_INT32(ng2_uw_mode, 0, "GPU",
                     "NG2 ultrawide present mode, set by the GPU scene detector and read "
                     "by the presenter across the DLL boundary: 0 = off (user's aspect "
                     "setting), 1 = gameplay (fill + FOV widen), 2 = menu/video "
                     "(pillarbox 16:9). Internal; do not set by hand.");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_INT32(ng2_uw_fade, 0, "GPU",
                     "NG2 ultrawide: the black fade level (0..1000) the scene detector publishes while it "
                     "switches between the fill and the pillarbox; the app paints it. Internal.");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_INT32(ng2_uw_fade_frames, 12, "GPU",
                     "NG2 ultrawide: swaps for the fade to black before a fill / pillarbox switch, and "
                     "again for the fade back (12 = 200 ms at 60 fps)")
    .range(1, 120)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(spirv_disable_rounding_mode_rte, false, "GPU",
                    "Disable RoundingModeRTE capability in SPIR-V shaders. Enable this to "
                    "allow shader debugging in RenderDoc, which doesn't support this "
                    "capability.");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(force_depth_clamp, false, "GPU",
                    "Use host depth clamping instead of near and far plane clipping when "
                    "guest clipping is enabled. X/Y/W clipping is unaffected. On Vulkan, "
                    "this requires depthClamp support.");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(depth_float24_round, false, "GPU", "Round float24 depth values");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(depth_float24_convert_in_pixel_shader, false, "GPU",
                    "Convert float24 depth in pixel shader");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(depth_transfer_not_equal_test, true, "GPU",
                    "Use not-equal test for depth transfer");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(gamma_render_target_as_unorm16, true, "GPU",
                    "Use R16G16B16A16_UNORM for gamma render targets (more accurate than sRGB)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Path to dump shaders to");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(use_fuzzy_alpha_epsilon, false, "GPU",
                    "Use approximate compare for alpha test values to prevent "
                    "flickering on NVIDIA graphics cards");
// [no-dll] registered here since rexgpu-xenos.dll is gone (gen_gpu_cvars.py, verbatim from the fork)
REXCVAR_DEFINE_BOOL(gpu_debug_markers, false, "GPU",
                    "Insert debug markers into GPU command streams for tools "
                    "like PIX and RenderDoc. Automatically enabled when "
                    "RenderDoc is detected.");

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
