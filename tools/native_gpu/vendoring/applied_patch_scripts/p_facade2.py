D = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_xlat/rtc_d3d12/"
h = open(D + "facade.h", encoding="utf-8").read()

# 1. the real SystemBindlessView enum, verbatim.
a = h.index("  enum class SystemBindlessView : uint32_t {")
b = h.index("  };", a) + 4
h = h[:a] + """  enum class SystemBindlessView : uint32_t {   // verbatim from command_processor.h at 23ace0b
    kSharedMemoryRawSRVAndNullRawUAVStart,
    kSharedMemoryRawSRV = kSharedMemoryRawSRVAndNullRawUAVStart,
    kNullRawUAV,
    kNullRawSRVAndSharedMemoryRawUAVStart,
    kNullRawSRV = kNullRawSRVAndSharedMemoryRawUAVStart,
    kSharedMemoryRawUAV,
    kSharedMemoryR32UintSRV,
    kSharedMemoryR32G32UintSRV,
    kSharedMemoryR32G32B32A32UintSRV,
    kSharedMemoryR32UintUAV,
    kSharedMemoryR32G32UintUAV,
    kSharedMemoryR32G32B32A32UintUAV,
    kEdramRawSRV,
    kEdramR32UintSRV,
    kEdramR32G32UintSRV,
    kEdramR32G32B32A32UintSRV,
    kEdramRawUAV,
    kEdramR32UintUAV,
    kEdramR32G32UintUAV,
    kEdramR32G32B32A32UintUAV,
    kGammaRampTableSRV,
    kGammaRampPWLSRV,
    kUnboundedSRVsStart,
    kNullTexture2DArray = kUnboundedSRVsStart,
    kNullTexture3D,
    kNullTextureCube,
    kCount,
  };
""" + h[b:]

# 2. provider members the shared memory / texture cache use.
h = h.replace("""  bool AreRasterizerOrderedViewsSupported() const { return false; }""",
"""  bool AreRasterizerOrderedViewsSupported() const { return false; }
  ID3D12CommandQueue* GetDirectQueue() const { return ::fable2::ngpu::rtc::Native().queue; }
  // PIX programmatic capture interface - never attached natively.
  void* GetGraphicsAnalysis() const { return nullptr; }
  D3D12_TILED_RESOURCES_TIER GetTiledResourcesTier() const;
  uint32_t GetVirtualAddressBitsPerResource() const;""")
h = h.replace("""  ID3D12GraphicsCommandList* list = nullptr;   // the native frame's open command list""",
"""  ID3D12GraphicsCommandList* list = nullptr;   // the native frame's open command list
  ID3D12CommandQueue* queue = nullptr;         // plume's direct queue (tiled-resource mapping updates)""")

# 3. command-processor members; the fork's readback hooks are no-ops natively (the PLUGIN keeps guest memory right).
h = h.replace("""  // Native: replay what the cache recorded into the native command list, in order, and start a new recording.""",
"""  void PushAliasingBarrier(ID3D12Resource* old_resource, ID3D12Resource* new_resource);
  void ClearCaches() {}
  void NotifyQueueOperationsDoneDirectly() {}
  // Scratch GPU buffer (texture loads of large textures) - one, grown on demand, as the plugin does.
  ID3D12Resource* RequestScratchGPUBuffer(uint32_t size, D3D12_RESOURCE_STATES state);
  void ReleaseScratchGPUBuffer(ID3D12Resource* buffer, D3D12_RESOURCE_STATES new_state);
  rex::ui::ngpu_d3d12::D3D12UploadBufferPool& GetConstantBufferPool() const;
  // Bindless-only (never reached bindful).
  D3D12_CPU_DESCRIPTOR_HANDLE GetViewBindlessHeapCPUStart() const { return {}; }
  uint32_t RequestPersistentViewBindlessDescriptor() { return UINT32_MAX; }
  void ReleaseViewBindlessDescriptorImmediately(uint32_t) {}
  rex::ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair GetSharedMemoryUintPow2BindlessSRVHandlePair(uint32_t element_size_bytes_pow2) const;
  // THE FORK'S GPU->CPU RESOLVE READBACK HOOKS ([readback], [split], [diag] in command_processor.h). The native renderer
  // never owns guest memory - the plugin, still running, lands its readbacks there - so these are no-ops natively.
  void LandOrAwaitReadbackForUpload(uint32_t, uint32_t) {}
  bool RangeResolvedInOpenSubmission(uint32_t, uint32_t) const { return false; }
  bool SplitSubmissionForFreshResolve() { return false; }
  bool HasPendingResolveReadback(uint32_t, uint32_t) const { return false; }
  void LandImpostorReadbackBeforeUpload(uint32_t, uint32_t) {}
  void NoteTextureLoad(uint64_t) {}
  void NoteSharedMemoryUpload(uint64_t) {}
  uint32_t FrameTextureLoads() const { return 0; }

  // Native: replay what the cache recorded into the native command list, in order, and start a new recording.""")
h = h.replace("""  std::unique_ptr<DeferredCommandList> deferred_;""",
"""  std::unique_ptr<DeferredCommandList> deferred_;
  ID3D12Resource* scratch_ = nullptr;
  uint32_t scratch_size_ = 0;
  D3D12_RESOURCE_STATES scratch_state_ = D3D12_RESOURCE_STATE_COMMON;
  mutable std::unique_ptr<rex::ui::ngpu_d3d12::D3D12UploadBufferPool> constant_buffer_pool_;""")
h = h.replace("""class D3D12SharedMemory;
class D3D12TextureCache;
""", """class D3D12SharedMemory;
class D3D12TextureCache;
// The plugin's D3D12Shader adds pipeline bookkeeping to DxbcShader; the texture cache uses only DxbcShader's binding
// types through it.
using D3D12Shader = ::rex::graphics::DxbcShader;
""")
h = h.replace("#include <rex/ui/graphics_provider.h>", "#include <rex/ui/graphics_provider.h>\n#include <rex/graphics/pipeline/shader/dxbc.h>\n#include <rex/logging.h>\n\nnamespace rex::ui::ngpu_d3d12 { class D3D12UploadBufferPool; }")
h = h.rstrip() + "\n\n// The plugin's command_processor.h brought these in; the vendored sources rely on it.\n#include \"rtc_d3d12/deferred_command_list.h\"\n#include \"rtc_d3d12/d3d12_upload_buffer_pool.h\"\n"
open(D + "facade.h", "w", encoding="utf-8", newline="\n").write(h)

c = open(D + "facade.cpp", encoding="utf-8").read()
c = c.replace("""}  // namespace rex::ui::ngpu_d3d12""", """D3D12_TILED_RESOURCES_TIER D3D12Provider::GetTiledResourcesTier() const {
  D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
  return (GetDevice() && SUCCEEDED(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)))) ? o.TiledResourcesTier : D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
}
uint32_t D3D12Provider::GetVirtualAddressBitsPerResource() const {
  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT v = {};
  return (GetDevice() && SUCCEEDED(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &v, sizeof(v)))) ? v.MaxGPUVirtualAddressBitsPerResource : 0;
}
}  // namespace rex::ui::ngpu_d3d12""", 1)
c = c.replace("""void D3D12CommandProcessor::FlushDeferred() {""", """void D3D12CommandProcessor::PushAliasingBarrier(ID3D12Resource* old_resource, ID3D12Resource* new_resource) {
  if (barrier_count_ >= kMaxBarriers) SubmitBarriers();
  D3D12_RESOURCE_BARRIER& b = barriers_[barrier_count_++];
  b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  b.Aliasing.pResourceBefore = old_resource;
  b.Aliasing.pResourceAfter = new_resource;
}

// command_processor.cpp RequestScratchGPUBuffer / ReleaseScratchGPUBuffer: one buffer, grown to the next power of two.
ID3D12Resource* D3D12CommandProcessor::RequestScratchGPUBuffer(uint32_t size, D3D12_RESOURCE_STATES state) {
  if (size <= scratch_size_ && scratch_) {
    PushTransitionBarrier(scratch_, scratch_state_, state);
    scratch_state_ = state;
    return scratch_;
  }
  uint32_t n = 1;
  while (n < size) n <<= 1;
  if (scratch_) { scratch_->Release(); scratch_ = nullptr; }   // the previous frame's work is complete (native fence)
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, n, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  if (FAILED(::fable2::ngpu::rtc::Native().device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&scratch_)))) {
    scratch_ = nullptr; scratch_size_ = 0; return nullptr;
  }
  scratch_size_ = n;
  scratch_state_ = state;
  return scratch_;
}
void D3D12CommandProcessor::ReleaseScratchGPUBuffer(ID3D12Resource* buffer, D3D12_RESOURCE_STATES new_state) {
  if (buffer == scratch_) scratch_state_ = new_state;
}
rex::ui::ngpu_d3d12::D3D12UploadBufferPool& D3D12CommandProcessor::GetConstantBufferPool() const {
  if (!constant_buffer_pool_) constant_buffer_pool_ = std::make_unique<rex::ui::ngpu_d3d12::D3D12UploadBufferPool>(rex::ui::ngpu_d3d12::NativeProvider(), 1024 * 1024);
  return *constant_buffer_pool_;
}
rex::ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessSRVHandlePair(uint32_t) const { return NoBindless("GetSharedMemoryUintPow2BindlessSRVHandlePair"); }

void D3D12CommandProcessor::FlushDeferred() {""")
open(D + "facade.cpp", "w", encoding="utf-8", newline="\n").write(c)

# 4. every vendored file gets <rex/logging.h> in its prologue (the plugin sources got it transitively).
p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
if "#include <rex/logging.h>" not in s:
    s = s.replace('"#include <string>\\n#include <cstdint>\\n"', '"#include <string>\\n#include <cstdint>\\n#include <rex/logging.h>\\n"')
open(p, "w", encoding="utf-8").write(s)
print("ok", "#include <rex/logging.h>" in s)
