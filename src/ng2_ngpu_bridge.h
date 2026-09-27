/**
 * @file        ng2_ngpu_bridge.h
 * @brief       NG2's side of the native-GPU BACKEND TRANSPLANT: the plugin's
 *              draw / swap callbacks fed, in LOCKSTEP, into the plugin's own
 *              D3D12 backend - EITHER the game-agnostic ngpu_backend.dll the
 *              Fable II team delivered (2026-09-26 evening; manual mode, NG2
 *              keeps its own window and presenter half) OR the same backend
 *              vendored in-app (src/native_gpu_xlat/rtc_d3d12, driven by
 *              src/ng2_native_backend.cpp) as the fallback - and the native
 *              window that presents its output (src/ng2_ngpu_window.cpp).
 *
 * The design and every number behind it are Fable II's
 * (claudecode/NATIVE_GPU_MIGRATION_KIT/MIGRATION_GUIDE.md, 2026-09-26):
 * lockstep - calling the backend INSIDE the plugin's GPU-thread callbacks -
 * sees exactly the guest memory the plugin would have seen; an async replay
 * read it a frame late (39,000 empty resolve rectangles). Under the plugin's
 * gpu_offload_to_native the plugin keeps its PM4 parser and the bridge and
 * skips its own GPU work: the native backend is then the only GPU, and the
 * plugin's own window stays black - expected.
 *
 * Off by default. Switched on by the exe cvar ngpu_backend (test lever
 * NG2_NATIVE_GPU=1 through ng2_tuning.h), and made the ONLY GPU by the
 * plugin cvar gpu_offload_to_native (NG2_NATIVE_OFFLOAD=1). ngpu_backend_dll
 * (default on) picks the DLL when ngpu_backend.dll sits beside the exe and
 * answers ABI 1; otherwise the in-exe copy. Without the plugin exports (an
 * older plugin) it logs and does nothing - a missing native path must never
 * stop the game running.
 */

#pragma once

#include <cstdint>

#include "ng2_ngpu_window.h"

struct ID3D12Resource;

namespace ng2::ngpu {

// [p3 draw] What the front end hands the bridge at a DRAW packet it decoded (the plugin's RexNgpuDraw, minus the
// register-file pointer, which comes as the front end's own file plus a dirty bitmap of what it wrote).
struct FeDrawInfo {
  uint32_t draw_initiator, index_addr, index_size;
  uint32_t vs_addr, vs_dwords, ps_addr, ps_dwords;
  bool vs_inline, ps_inline;
  const uint8_t* vs_code;   // inline microcode (big-endian dwords), valid for the call
  const uint8_t* ps_code;
  uint32_t vs_code_dwords, ps_code_dwords;
};
void FrontEndDraw(const uint32_t* regs, uint64_t* dirty, const FeDrawInfo& d);
void FrontEndSwap(uint32_t fb, uint32_t w, uint32_t h, const uint32_t* regs, uint64_t* dirty);

// Binds the plugin's RexNgpu* exports and installs the lockstep consumer;
// starts the native window when ngpu_backend is on. Call once the plugin is
// loaded (OnPostSetup).
void Start(const render::WindowSpec& window);

// Removes the consumer (the plugin holds raw pointers into this module) and
// stops the native window. Before the runtime tears down.
void Stop();

// THE ULTRAWIDE VALUES HAVE TWO HOMES under the transplant. The backend
// carries its own copies of the plugin's cvars (accessor-only statics, read from
// the runtime registry ONCE at first use; a REXCVAR_SET inside the vendored code
// writes only that copy). ng2_fov_k is set by the app (ApplyFov, live from the
// menu): push it into the backend's copy too, or a toggle never reaches it.
// ng2_uw_mode is written by the backend's IssueSwap at every swap: the native
// window reads that copy, which is right whether or not the plugin's own swap
// path still runs its detection under offload (it does not: it returns first).
// With the DLL these go through NgpuBackendSetSetting / NgpuBackendPresentMode.
void SetFovK(double k);
int UltrawideMode();   // the backend's ng2_uw_mode: 0 off, 1 gameplay (fill), 2 menu/video (pillarbox)

// For the native window: the backend's latest gamma-applied guest output
// (R10G10B10A2, PIXEL_SHADER_RESOURCE, on the window's queue) and the wait
// that makes its swap submission precede the window's blit in queue order.
bool BackendWaitSwapSubmitted(uint32_t timeout_ms);
ID3D12Resource* BackendGuestOutput(uint32_t& width, uint32_t& height);

}  // namespace ng2::ngpu
