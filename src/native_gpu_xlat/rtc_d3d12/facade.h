#pragma once
// BACKEND TRANSPLANT: NATIVE stand-ins for the plugin pieces the vendored D3D12 backend needs from outside it
// (d3d12/render_target_cache.cpp at rexglue-src 23ace0b). Same class names and signatures, in the renamed
// namespaces (vendor_rtc_d3d12.py), built on plume's ID3D12Device and the native frame's command list. Only the
// members the vendored code actually calls exist here; everything is routed through ng2::ngpu::rtc::Native().
#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <rex/assert.h>
#include <rex/ui/graphics_provider.h>
#include <rex/graphics/flags.h>
#include <rex/ui/d3d12/d3d12_api.h>
#include <rex/logging.h>

namespace rex::memory { class Memory; class BaseHeap; }
namespace ng2::ngpu::backend { class Driver; }   // the replay driver (friend of the vendored command processor)

namespace ng2::ngpu::rtc {
// What the stand-ins need from the native renderer, filled by native_gpu_rtc.cpp before any cache call.
struct NativeContext {
  ID3D12Device* device = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;   // the native frame's open command list
  ID3D12CommandQueue* queue = nullptr;         // plume's direct queue (tiled-resource mapping updates)
  uint64_t submission_current = 1, submission_completed = 0;
  // One-use shader-visible CBV/SRV/UAV descriptors from the native frame's heap (contiguous).
  bool (*alloc_views)(uint32_t count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu) = nullptr;
  // Called after the cache changed pipeline / root signature / render targets on the list, so the native draw path
  // re-binds its own state instead of trusting a cache.
  void (*state_clobbered)() = nullptr;
};
NativeContext& Native();
// OWNERSHIP OF GUEST MEMORY (T3, 2026-09-26). While the plugin renders, it owns guest memory and the native backend
// never writes it (every readback forced off, the calls below no-ops). With the plugin's gpu_offload_to_native on,
// the plugin skips its own GPU work and the native backend becomes the GPU: its readbacks take the plugin's settings
// and land in guest memory, as the plugin's did. The data-provider / physical-heap API exists only in the fork's
// newer runtime, so it is bound at run time (the exe must still start with the older runtime pair).
bool NativeOwnsGuestMemory();
using GuestDataProviderFn = void (*)(void*, std::unique_lock<std::recursive_mutex>&, uint32_t, uint32_t, bool);
void* GuestDataProviderRegister(rex::memory::Memory* memory, GuestDataProviderFn fn, void* context);
void GuestDataProvidersDisable(rex::memory::Memory* memory, uint32_t address, uint32_t length);
void GuestDataProviderUnregister(rex::memory::Memory* memory, void* handle);
rex::memory::BaseHeap* GuestPhysicalHeap(rex::memory::Memory* memory);
// [async submit] ngpu_backend_async_submit (read once). The backend's swap notes its submission; the native frame
// waits until the submit thread has executed it before sampling the guest output on the same queue.
bool AsyncSubmitEnabled();
bool GpuProfEnabled();   // ngpu_gpu_prof (read once)
bool HoistUploadsEnabled();   // ngpu_backend_hoist_uploads (read once)
void NoteSubmissionExecuted(uint64_t submission);
void NoteSwapSubmission(uint64_t submission);
uint64_t SwapSubmissionsNoted();   // how many swaps have been noted (frames, for per-frame reports)
bool WaitSwapSubmitted(uint32_t timeout_ms);
bool WaitSubmitted(uint64_t want, uint32_t timeout_ms);   // [gs present thread]
uint64_t SwapSubmission();
}  // namespace ng2::ngpu::rtc

namespace rex::ui::ngpu_d3d12 {
namespace util { using DescriptorCpuGpuHandlePair = std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE>; }

class D3D12Provider {
 public:
  ID3D12Device* GetDevice() const { return ::ng2::ngpu::rtc::Native().device; }
  uint32_t GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE type) const { return GetDevice()->GetDescriptorHandleIncrementSize(type); }
  uint32_t GetViewDescriptorSize() const { return GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); }
  uint32_t GetSamplerDescriptorSize() const { return GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER); }
  uint32_t GetRTVDescriptorSize() const { return GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV); }
  uint32_t GetDSVDescriptorSize() const { return GetDescriptorSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV); }
  template <typename T>
  T OffsetDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type, T start, uint32_t index) const {
    start.ptr += index * GetDescriptorSize(type);
    return start;
  }
  template <typename T>
  T OffsetViewDescriptor(T start, uint32_t index) const {
    start.ptr += index * GetViewDescriptorSize();
    return start;
  }
  // The plugin's answers on this machine: an NVIDIA adapter, the HOST RENDER TARGET path (never ROV - the plugin's
  // owner trace shows RTV-path transfers), PS-specified stencil reference as the device reports it.
  rex::ui::GraphicsProvider::GpuVendorID GetAdapterVendorID() const { return rex::ui::GraphicsProvider::GpuVendorID::kNvidia; }
  D3D12_HEAP_FLAGS GetHeapFlagCreateNotZeroed() const { return D3D12_HEAP_FLAG_CREATE_NOT_ZEROED; }
  bool IsPSSpecifiedStencilReferenceSupported() const;
  bool AreRasterizerOrderedViewsSupported() const { return false; }
  ID3D12CommandQueue* GetDirectQueue() const { return ::ng2::ngpu::rtc::Native().queue; }
  // PIX programmatic capture interface - never attached natively.
  void* GetGraphicsAnalysis() const { return nullptr; }
  D3D12_TILED_RESOURCES_TIER GetTiledResourcesTier() const;
  uint32_t GetVirtualAddressBitsPerResource() const;
  bool AreUnalignedBlockTexturesSupported() const;
  D3D12_RESOURCE_BINDING_TIER GetResourceBindingTier() const;
  template <typename T>
  T OffsetSamplerDescriptor(T start, uint32_t index) const {
    start.ptr += index * GetSamplerDescriptorSize();
    return start;
  }
  // Shader disassembly / DXBC->DXIL conversion are diagnostics (d3d12_dxbc_disasm*): not loaded natively.
  HRESULT Disassemble(const void*, size_t, UINT, const char*, ID3DBlob**) const { return E_NOINTERFACE; }
  HRESULT DxbcConverterCreateInstance(const CLSID&, const IID&, void**) const { return E_NOINTERFACE; }
  HRESULT DxcCreateInstance(const CLSID&, const IID&, void**) const { return E_NOINTERFACE; }
  HRESULT SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version,
                                 ID3DBlob** blob_out, ID3DBlob** error_blob_out) const {
    return D3D12SerializeRootSignature(desc, version, blob_out, error_blob_out);
  }
};
D3D12Provider& NativeProvider();
}  // namespace rex::ui::ngpu_d3d12

// (The plugin's D3D12CommandProcessor, DeferredCommandList, shared memory, texture cache and D3D12Shader are now
// VENDORED - rtc_d3d12/command_processor.h etc. - and replace the earlier stand-ins.)
