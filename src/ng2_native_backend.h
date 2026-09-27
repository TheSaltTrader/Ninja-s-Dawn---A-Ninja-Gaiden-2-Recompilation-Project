#pragma once
// BACKEND TRANSPLANT (user decision 2026-09-26): the plugin's own D3D12 backend - command processor draw/copy/swap
// path, pipeline cache, primitive processor, render-target cache (EDRAM), texture cache, shared memory - vendored
// under src/native_gpu_xlat/rtc_d3d12 and driven here from the bridge replay on plume's device and queue. The same
// code as the Xenos plugin, fed the same register stream, so the picture comes from identical logic.
//
// Behind ngpu_backend (default off). Nothing here writes guest memory: every readback is forced off in the vendored
// sources (vendor_rtc_d3d12.py NATIVE_FORCED / SOURCE_PATCHES) - the plugin, still running, owns guest memory.
#include <cstdint>
#include <filesystem>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace ng2::ngpu::backend {

bool Init(ID3D12Device* device, ID3D12CommandQueue* queue);
// The pipeline storage (cache root + title id as the runtime gave them to the plugin, RexNgpuGetShaderStorage):
// load it synchronously (on the GPU thread, right after Init) and append this run's pipelines; Shutdown writes
// the tail out and closes the files.
void InitShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id);
void ShutdownShaderStorage();
// The swap post effect (F10 Antialiasing: 0 none, 1 fxaa, 2 fxaa_extreme), set on the GPU thread; the plugin's
// graphics system did this for the plugin's own command processor, nothing did it for this copy.
void SetSwapPostEffect(int effect);
int SwapPostEffect();
bool Ready();

// Register write, exactly as the PM4 parser would perform it (the command processor tracks dirty constants here).
void WriteRegister(uint32_t index, uint32_t value);

struct DrawRecord {
  uint32_t draw_initiator;     // VGT_DRAW_INITIATOR as the packet gave it
  uint32_t index_addr;         // VGT_DMA_BASE (kDMA)
  uint32_t index_size;         // VGT_DMA_SIZE (kDMA)
  uint32_t vs_addr, vs_dwords, ps_addr, ps_dwords;
  const uint32_t* vs_code;     // big-endian microcode (a record-time snapshot or guest memory), nullptr = none
  const uint32_t* ps_code;
};
// One recorded draw (including copy-mode draws, which the command processor turns into resolves).
bool Draw(const DrawRecord& d);

// The frame's swap: fetch constant 0 (6 dwords) as the swap saw it, the gamma ramp tables (nullptr = unchanged),
// the front buffer the XE_SWAP packet named.
void Swap(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height, const uint32_t* fetch0,
          const uint32_t* gamma_table_256, const uint32_t* gamma_pwl_rgb);
// End of a replayed frame that had no swap (submits the pending work).
void EndFrameNoSwap();

// The latest gamma-applied guest output (R10G10B10A2, PIXEL_SHADER_RESOURCE) and its size; nullptr before the first.
ID3D12Resource* GuestOutput(uint32_t& width, uint32_t& height);
bool GuestOutputIs8bpc();   // what the last refresh told the presenter context (gamma table, no FXAA)

struct Stats { uint64_t draws = 0, draw_failed = 0, swaps = 0, shader_loads = 0, shader_load_failed = 0; };
Stats GetStats();
// Readiness for the reveal hold: draws skipped so far because their pipeline was still being compiled (async shader
// compilation), and whether the pipeline cache is compiling right now.
struct Readiness { uint64_t pipeline_not_ready_draws = 0; bool creating_pipelines = false; };
Readiness GetReadiness();

}  // namespace ng2::ngpu::backend
