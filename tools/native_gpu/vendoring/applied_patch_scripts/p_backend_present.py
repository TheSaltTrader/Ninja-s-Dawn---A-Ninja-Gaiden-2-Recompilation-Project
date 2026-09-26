P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
a = "    if (!post && REXCVAR_GET(ngpu_present_post) && (g_swap_front || g_swap_resolve) && g_swap_resolve_frame + 2 >= g_s.frames) {"
assert s.count(a.replace("\n", nl)) == 1
b = """    // BACKEND TRANSPLANT: the window shows the transplanted backend's own gamma-applied guest output (what the
    // plugin's presenter would show), through the same blit, from the LAST descriptor slot (the native texture path
    // never runs in this mode, so nothing else writes it).
    if (!post && g_backend_on) {
      uint32_t ow = 0, oh = 0;
      if (ID3D12Resource* out = fable2::ngpu::backend::GuestOutput(ow, oh)) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        static_cast<D3D12DescriptorSet*>(g_s.xs_sets[0].get())->setSRV(kTexSlots - 1, out, &sd);
        post = true;
        post_index = kTexSlots - 1;
        static uint32_t logs = 0;
        if (logs < 3) { ++logs; REXLOG_INFO("[ngpu] BACKEND: presenting the transplanted backend's {}x{} guest output", ow, oh); }
      }
    }
""" + a
s = s.replace(a.replace("\n", nl), b.replace("\n", nl), 1)
open(P, "w", encoding="utf-8", newline="").write(s)
print("ok")
