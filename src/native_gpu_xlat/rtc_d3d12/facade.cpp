// EDRAM PORT phase 2: the native stand-ins declared in facade.h. Behaviour follows the plugin's D3D12CommandProcessor
// (d3d12/command_processor.cpp at rexglue-src 23ace0b) where the vendored cache depends on it: barriers are batched
// and written INTO the deferred list at SubmitBarriers (so they keep their order relative to the cache's commands),
// fixed-function state goes through the deferred list, and FlushDeferred replays the recording into the native
// frame's command list.
#include "rtc_d3d12/facade.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include <rex/logging.h>

namespace ng2::ngpu::xlat { bool PluginBool(const char*, bool); }

namespace ng2::ngpu::rtc {
bool NativeOwnsGuestMemory() {
  static const bool owns = ::ng2::ngpu::xlat::PluginBool("gpu_offload_to_native", false);
  return owns;
}
namespace {
HMODULE Runtime() { static HMODULE m = GetModuleHandleA("rexruntime.dll"); return m; }
template <typename T>
T Bind(const char* mangled) {
  HMODULE m = Runtime();
  return m ? reinterpret_cast<T>(GetProcAddress(m, mangled)) : nullptr;
}
}  // namespace
// x64: a non-virtual member function is called like a free function with `this` first.
void* GuestDataProviderRegister(rex::memory::Memory* memory, GuestDataProviderFn fn, void* context) {
  if (!NativeOwnsGuestMemory()) return nullptr;
  using F = void* (*)(rex::memory::Memory*, GuestDataProviderFn, void*);
  static F f = Bind<F>("?RegisterPhysicalMemoryDataProvider@Memory@memory@rex@@QEAAPEAXP6AXPEAXAEAV?$unique_lock@Vrecursive_mutex@std@@@std@@II_N@Z0@Z");
  static bool said = false;
  if (!said) { said = true; REXLOG_INFO("[ngpu] BACKEND owns guest memory: runtime data-provider API {}", f ? "bound" : "MISSING (older runtime) - readbacks land only at their usual points"); }
  return f ? f(memory, fn, context) : nullptr;
}
void GuestDataProvidersDisable(rex::memory::Memory* memory, uint32_t address, uint32_t length) {
  if (!NativeOwnsGuestMemory()) return;
  using F = void (*)(rex::memory::Memory*, uint32_t, uint32_t);
  static F f = Bind<F>("?DisablePhysicalMemoryDataProviders@Memory@memory@rex@@QEAAXII@Z");
  if (f) f(memory, address, length);
}
void GuestDataProviderUnregister(rex::memory::Memory* memory, void* handle) {
  if (!NativeOwnsGuestMemory() || !handle) return;
  using F = void (*)(rex::memory::Memory*, void*);
  static F f = Bind<F>("?UnregisterPhysicalMemoryDataProvider@Memory@memory@rex@@QEAAXPEAX@Z");
  if (f) f(memory, handle);
}
rex::memory::BaseHeap* GuestPhysicalHeap(rex::memory::Memory* memory) {
  if (!NativeOwnsGuestMemory()) return nullptr;
  using F = void* (*)(rex::memory::Memory*);
  static F f = Bind<F>("?GetPhysicalHeap@Memory@memory@rex@@QEAAPEAVVirtualHeap@23@XZ");
  // VirtualHeap derives from BaseHeap first (single inheritance), so the pointer is the BaseHeap.
  return f ? static_cast<rex::memory::BaseHeap*>(f(memory)) : nullptr;
}
bool NgpuBackendFastValidCvar();
bool FastValidChecks() {
  static const bool on = NgpuBackendFastValidCvar();
  return on;
}
bool NgpuHoistUploadsCvar();
bool HoistUploadsEnabled() {
  static const bool on = NgpuHoistUploadsCvar();
  return on;
}
bool NgpuGpuProfCvar();
bool GpuProfEnabled() {
  static const bool on = NgpuGpuProfCvar();
  return on;
}
bool NgpuBackendAsyncSubmitCvar();   // native_gpu_present.cpp (the cvar lives with the other ngpu_backend ones)
bool AsyncSubmitEnabled() {
  static const bool on = NgpuBackendAsyncSubmitCvar();
  return on;
}
namespace {
std::atomic<uint64_t> g_executed{0}, g_swap_submission{0};
std::mutex g_exec_mu;
std::condition_variable g_exec_cv;
}  // namespace
void NoteSubmissionExecuted(uint64_t submission) {
  { std::lock_guard<std::mutex> lk(g_exec_mu); g_executed.store(submission); }
  g_exec_cv.notify_all();
}
std::atomic<uint64_t> g_swaps_noted{0};
void NoteSwapSubmission(uint64_t submission) { g_swap_submission.store(submission); g_swaps_noted.fetch_add(1); }
uint64_t SwapSubmissionsNoted() { return g_swaps_noted.load(); }
bool WaitSwapSubmitted(uint32_t timeout_ms) {
  if (!AsyncSubmitEnabled()) return true;
  const uint64_t want = g_swap_submission.load();
  std::unique_lock<std::mutex> lk(g_exec_mu);
  return g_exec_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [want] { return g_executed.load() >= want; });
}
NativeContext& Native() {
  static NativeContext c;
  return c;
}
}  // namespace ng2::ngpu::rtc

namespace rex::ui::ngpu_d3d12 {
D3D12Provider& NativeProvider() {
  static D3D12Provider p;
  return p;
}
bool D3D12Provider::IsPSSpecifiedStencilReferenceSupported() const {
  static int cached = -1;
  if (cached < 0) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    cached = (GetDevice() && SUCCEEDED(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) && o.PSSpecifiedStencilRefSupported) ? 1 : 0;
  }
  return cached == 1;
}
D3D12_TILED_RESOURCES_TIER D3D12Provider::GetTiledResourcesTier() const {
  D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
  return (GetDevice() && SUCCEEDED(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)))) ? o.TiledResourcesTier : D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
}
uint32_t D3D12Provider::GetVirtualAddressBitsPerResource() const {
  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT v = {};
  return (GetDevice() && SUCCEEDED(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &v, sizeof(v)))) ? v.MaxGPUVirtualAddressBitsPerResource : 0;
}
bool D3D12Provider::AreUnalignedBlockTexturesSupported() const {
  D3D12_FEATURE_DATA_D3D12_OPTIONS8 o = {};
  return GetDevice() && SUCCEEDED(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &o, sizeof(o))) && o.UnalignedBlockTexturesSupported;
}
D3D12_RESOURCE_BINDING_TIER D3D12Provider::GetResourceBindingTier() const {
  D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
  return (GetDevice() && SUCCEEDED(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)))) ? o.ResourceBindingTier : D3D12_RESOURCE_BINDING_TIER_1;
}
}  // namespace rex::ui::ngpu_d3d12

