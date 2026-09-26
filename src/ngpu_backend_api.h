// ngpu_backend.dll - the Xenos plugin's own D3D12 backend, transplanted and game-agnostic (2026-09-26).
//
// What it is: a copy of rexgpu-xenos.dll's D3D12 backend (command processor, pipeline cache, render-target cache,
// texture cache, shared memory, primitive processor - namespace ngpu_d3d12) plus the LOCKSTEP glue that feeds it from
// the plugin's own draw/swap callbacks on the plugin's GPU thread. With the plugin's gpu_offload_to_native=true the
// plugin keeps parsing PM4 and this DLL does all of the GPU work, on the host's or its own D3D12 device.
//
// Requirements on the host process: a ReXGlue runtime (rexruntime.dll) with the kernel state up, and a rexgpu-xenos.dll
// that exports RexNgpuSetDrawCallback / RexNgpuSetSwapCallback (and, for full speed, RexNgpuDirtyRegs and
// RexNgpuGetGammaRamp). Start it after the plugin is loaded (the first frame is a safe point).
//
// Two ways to use it:
//   SELF-CONTAINED (least work):   NgpuBackendOptions o; NgpuBackendDefaultOptions(&o); NgpuBackendStart(&o, 0, 0);
//     -> creates its own device, queue and window, registers the plugin callbacks, presents every guest frame.
//   MANUAL (keep your presenter):   o.own_window = 0; o.register_callbacks = 0; NgpuBackendStart(&o, dev, queue);
//     -> forward the plugin's callbacks to NgpuBackendOnDraw / NgpuBackendOnSwap; after each OnSwap, if
//        NgpuBackendShouldPresent(), wait NgpuBackendWaitSwapSubmitted() and sample NgpuBackendGuestOutput()
//        (R10G10B10A2, gamma applied, PIXEL_SHADER_RESOURCE) on the SAME queue.
// Call NgpuBackendRevealAfterLoad() when the game finishes a load (its loading screen gives way to the world): the
// last frame is kept until the stage is complete (no draw skipped for a compiling pipeline, steady draw count).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
#define NGPU_EXTERN_C extern "C"
#else
typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12CommandQueue ID3D12CommandQueue;
typedef struct ID3D12Resource ID3D12Resource;
#define NGPU_EXTERN_C
#endif

#ifdef NGPU_BACKEND_BUILD
#define NGPU_API NGPU_EXTERN_C __declspec(dllexport)
#else
#define NGPU_API NGPU_EXTERN_C __declspec(dllimport)
#endif

#define NGPU_BACKEND_ABI 1

typedef struct NgpuBackendOptions {
  uint32_t size;                // sizeof(NgpuBackendOptions)
  int32_t own_window;           // 1: own device + queue + window + present (self-contained); 0: manual
  int32_t register_callbacks;   // 1: register the plugin's draw/swap callbacks itself; 0: host forwards them
  int32_t async_submit;         // 1: deferred command lists executed + submitted on a submit thread (default 1)
  int32_t upload_skip;          // 1: skip re-uploading 4 KB guest pages whose XXH3 matches the last upload (default 1)
  int32_t hoist_uploads;        // 1: uploads into untouched pages go to a per-submission prologue (default 1)
  int32_t fast_valid;           // 1: lock-free common-case validity checks (default 1)
  int32_t gpu_prof;             // 1: GPU time by category via timestamps (diagnostic, default 0)
  int32_t reveal_hold;          // 1: hold the last frame after a load until the stage is complete (default 1)
  int32_t reveal_hold_frames;   // complete frames that end the hold (default 6)
  int32_t reveal_hold_max_ms;   // cap (default 2500)
  int32_t selfcheck_every;      // register-sync self-check every Nth draw; 1 = every draw (correctness runs); 0 off
  const wchar_t* window_title;  // own_window: the window title (default L"Native renderer")
} NgpuBackendOptions;

typedef struct NgpuBackendStatsT {
  uint64_t draws, draw_failed, swaps, selfcheck_runs, selfcheck_mismatches, frames_presented, frames_held;
} NgpuBackendStatsT;

NGPU_API uint32_t NgpuBackendAbiVersion(void);
NGPU_API void NgpuBackendDefaultOptions(NgpuBackendOptions* options);
// Returns 1 on success, 0 (with a log line) on failure - including when called before the runtime's kernel state
// exists (call it from OnPostSetup or later). device/queue: required in manual mode, ignored with own_window=1.
// Works with the plugin's gpu_offload_to_native OFF too (lockstep beside the plugin; the backend then never writes
// guest memory) - verified on Ninja Gaiden II, 2026-09-26: 791,138 draws, 0 failed, 0 self-check mismatches.
NGPU_API int NgpuBackendStart(const NgpuBackendOptions* options, ID3D12Device* device, ID3D12CommandQueue* queue);
NGPU_API void NgpuBackendOnDraw(const void* rex_ngpu_draw);                    // the plugin's RexNgpuDraw*
NGPU_API void NgpuBackendOnSwap(uint32_t frontbuffer, uint32_t width, uint32_t height);
NGPU_API void NgpuBackendRevealAfterLoad(void);
NGPU_API int NgpuBackendShouldPresent(void);                                   // manual mode, after OnSwap
NGPU_API int NgpuBackendWaitSwapSubmitted(uint32_t timeout_ms);
NGPU_API ID3D12Resource* NgpuBackendGuestOutput(uint32_t* width, uint32_t* height);
NGPU_API void NgpuBackendStats(NgpuBackendStatsT* out);
// The present mode the backend's own scene detection chose this frame (ng2_uw_mode: 0 = user setting, 1 = fill,
// 2 = pillarbox) and its FOV factor (ng2_fov_k). Under offload they are also published into the runtime registry at
// every swap, so code that reads them by name keeps working.
NGPU_API void NgpuBackendPresentMode(int32_t* uw_mode, double* fov_k);
// Every setting of the vendored backend by name (its own copies - they are read from the runtime registry ONCE, so
// a live change the host makes, e.g. ng2_fov_k when the user toggles Ultrawide, must be pushed here; and values the
// backend computes, e.g. ng2_uw_mode, are read here). Values as text ("true"/"false", decimal). Returns 1 if the
// name exists. bool / int / double are safe to set at any time; set string settings before NgpuBackendStart.
// The table: src/ngpu_backend_dll/ngpu_setting_table.inc (generated by tools/native_gpu/gen_setting_table.py).
NGPU_API int NgpuBackendGetSetting(const char* name, char* buffer, uint32_t buffer_size);
NGPU_API int NgpuBackendSetSetting(const char* name, const char* value);
