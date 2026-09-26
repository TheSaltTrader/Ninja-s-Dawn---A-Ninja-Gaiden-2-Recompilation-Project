p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
if "SOURCE_PATCHES" not in s:
    s = s.replace("bytecode = set()", r'''# SOURCE_PATCHES (surgical, each one documented): (vendored file, old text, new text, expected count).
SOURCE_PATCHES = [
    # The fork's CPU-readback data providers hook GUEST memory; the native backend never touches guest memory.
    ("command_processor.cpp", "memory_->RegisterPhysicalMemoryDataProvider(ResolveDataProviderThunk, this)",
     "nullptr /* NATIVE PATCH: no guest-memory data provider */", 1),
    ("command_processor.cpp", "memory_->DisablePhysicalMemoryDataProviders(",
     "::fable2::ngpu::rtc::NoGuestDataProviders(/* NATIVE PATCH */ ", None),
    ("command_processor.cpp", "memory_->UnregisterPhysicalMemoryDataProvider(resolve_data_provider_handle_);",
     "/* NATIVE PATCH: no guest-memory data provider */", 1),
    # IssueSwap: the native renderer presents; the plugin's gamma / FXAA pass writes into a NATIVE guest-output texture.
    ("command_processor.cpp", """  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("IssueSwap: presenter is null");
    return;
  }
""", "  // NATIVE PATCH: no presenter check - NativeRefreshGuestOutput (below) stands in for the presenter.\n", 1),
    ("command_processor.cpp", "presenter->RefreshGuestOutput(", "::fable2::ngpu::rtc::NativeRefreshGuestOutput(", 1),
]
bytecode = set()''')
    s = s.replace("    for m in re.finditer(r'#include \"\\.\\./shaders", """    for (pf, old, new, cnt) in SOURCE_PATCHES:
        if pf == dst:
            c = t.count(old)
            if c == 0 or (cnt is not None and c != cnt):
                raise SystemExit("SOURCE PATCH anchor count %d in %s: %r" % (c, pf, old[:60]))
            t = t.replace(old, new)
    for m in re.finditer(r'#include \"\\.\\./shaders""", 1)
open(p, "w", encoding="utf-8").write(s)

D = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_xlat/rtc_d3d12/"
h = open(D + "facade.h", encoding="utf-8").read()
if "GetResourceBindingTier" not in h:
    h = h.replace("  bool AreUnalignedBlockTexturesSupported() const;", """  bool AreUnalignedBlockTexturesSupported() const;
  D3D12_RESOURCE_BINDING_TIER GetResourceBindingTier() const;
  template <typename T>
  T OffsetSamplerDescriptor(T start, uint32_t index) const {
    start.ptr += index * GetSamplerDescriptorSize();
    return start;
  }
  // Shader disassembly / DXBC->DXIL conversion are diagnostics (d3d12_dxbc_disasm*): not loaded natively.
  HRESULT Disassemble(const void*, size_t, UINT, const char*, ID3DBlob**) const { return E_NOINTERFACE; }
  HRESULT DxbcConverterCreateInstance(const CLSID&, const IID&, void**) const { return E_NOINTERFACE; }
  HRESULT DxcCreateInstance(const CLSID&, const IID&, void**) const { return E_NOINTERFACE; }""")
    h = h.replace("#include <rex/graphics/flags.h>", "#include <rex/graphics/flags.h>\n#include <rex/ui/d3d12/d3d12_api.h>", 1)
    h = h.replace("NativeContext& Native();\n}  // namespace fable2::ngpu::rtc", """NativeContext& Native();
// NATIVE PATCH targets in the vendored command processor (vendor_rtc_d3d12.py SOURCE_PATCHES).
inline void NoGuestDataProviders(uint32_t, uint32_t) {}
}  // namespace fable2::ngpu::rtc""")
    open(D + "facade.h", "w", encoding="utf-8", newline="\n").write(h)
c = open(D + "facade.cpp", encoding="utf-8").read()
if "GetResourceBindingTier" not in c:
    c = c.replace("}  // namespace rex::ui::ngpu_d3d12", """D3D12_RESOURCE_BINDING_TIER D3D12Provider::GetResourceBindingTier() const {
  D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
  return (GetDevice() && SUCCEEDED(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)))) ? o.ResourceBindingTier : D3D12_RESOURCE_BINDING_TIER_1;
}
}  // namespace rex::ui::ngpu_d3d12""", 1)
    open(D + "facade.cpp", "w", encoding="utf-8", newline="\n").write(c)

g = open(D + "graphics_system_standin.h", encoding="utf-8").read()
if "D3D12Presenter" not in g:
    g = g.replace("namespace rex::graphics::ngpu_d3d12 {", """// The plugin's D3D12Presenter pieces the swap lambda uses (d3d12_presenter.h): the guest-output format / state and the
// refresh context carrying the UAV-capable output texture.
namespace rex::ui::ngpu_d3d12 {
class D3D12Presenter {
 public:
  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr D3D12_RESOURCE_STATES kGuestOutputInternalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  class D3D12GuestOutputRefreshContext final : public ::rex::ui::Presenter::GuestOutputRefreshContext {
   public:
    D3D12GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D12Resource* resource)
        : GuestOutputRefreshContext(is_8bpc_out_ref), resource_(resource) {}
    ID3D12Resource* resource_uav_capable() const { return resource_; }
   private:
    ID3D12Resource* resource_;
  };
};
}  // namespace rex::ui::ngpu_d3d12

namespace fable2::ngpu::rtc {
// Stands in for Presenter::RefreshGuestOutput: provides a native guest-output texture (kGuestOutputFormat, UAV, in
// kGuestOutputInternalState) of the requested size and runs the plugin's refresher on it.
bool NativeRefreshGuestOutput(uint32_t frontbuffer_width, uint32_t frontbuffer_height, uint32_t display_width,
                              uint32_t display_height,
                              const std::function<bool(::rex::ui::Presenter::GuestOutputRefreshContext& context)>& refresher);
}  // namespace fable2::ngpu::rtc

namespace rex::graphics::ngpu_d3d12 {""", 1)
    g = g.replace("#include <cstdint>", "#include <cstdint>\n#include <functional>", 1)
    open(D + "graphics_system_standin.h", "w", encoding="utf-8", newline="\n").write(g)
print("ok")
