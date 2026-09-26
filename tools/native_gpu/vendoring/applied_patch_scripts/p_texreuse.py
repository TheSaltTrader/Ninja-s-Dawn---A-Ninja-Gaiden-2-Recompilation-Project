P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)

rep("""  auto it = g_xtextures.find(key);
  uint32_t reuse_index = 0, uploads = 0;""",
"""  auto it = g_xtextures.find(key);
  uint32_t reuse_index = 0, uploads = 0;
  // RE-UPLOAD IN PLACE (ngpu_tex_reupload_in_place, 2026-09-26): a re-upload used to retire the texture and create a
  // new one - ~6 lake LUTs every frame, 9% of the async replay thread in createTexture/D3D12MA (PROF2). The same key
  // means the same size and format, and the previous frame's GPU work is complete (EndFrame waits on its fence), so
  // the resource is kept and the new texels are copied into it. The command list orders the copy after this frame's
  // earlier draws. Anything not reused is retired on the way out, exactly as before.
  struct KeepTex { std::unique_ptr<RenderTexture> tex; uint32_t levels = 0; ~KeepTex() { if (tex) g_s.retired.push_back(std::move(tex)); } } keep;""")
rep("""    g_s.retired.push_back(std::move(old.tex));
    g_s.retired_views.push_back(std::move(old.view));
    reuse_index = old.index;""",
"""    if (REXCVAR_GET(ngpu_tex_reupload_in_place)) { keep.tex = std::move(old.tex); keep.levels = old.levels; }
    else g_s.retired.push_back(std::move(old.tex));
    g_s.retired_views.push_back(std::move(old.view));
    reuse_index = old.index;""")
rep("""  t.tex = faces == 6 ? g_s.device->createTexture(RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, t.w, t.h, 1, 1, 6, rf))
                     : g_s.device->createTexture(RenderTextureDesc::Texture2D(t.w, t.h, levels, rf));""",
"""  if (keep.tex && keep.levels == (faces == 6 ? 1u : levels)) { t.tex = std::move(keep.tex); ++g_tex_reused_in_place; }
  else
  t.tex = faces == 6 ? g_s.device->createTexture(RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, t.w, t.h, 1, 1, 6, rf))
                     : g_s.device->createTexture(RenderTextureDesc::Texture2D(t.w, t.h, levels, rf));""")
rep("XTexture* GetTexture(const uint32_t fc[6]) {", "uint64_t g_tex_reused_in_place = 0;\nXTexture* GetTexture(const uint32_t fc[6]) {")
rep("REXCVAR_DEFINE_INT32(ngpu_async_accumulate_cap,", "REXCVAR_DEFINE_BOOL(ngpu_tex_reupload_in_place, true, \"GPU\", \"Native-GPU: a texture re-upload copies into the existing resource (same key = same size/format) instead of creating a new one\");\nREXCVAR_DEFINE_INT32(ngpu_async_accumulate_cap,")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
