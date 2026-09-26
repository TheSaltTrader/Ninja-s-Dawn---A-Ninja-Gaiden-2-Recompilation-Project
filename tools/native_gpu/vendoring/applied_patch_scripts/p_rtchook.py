P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)

rep("""    g_ring.ps_ptr = rec.ps_addr; g_ring.ps_dwords = rec.ps_dwords; g_ring.ps_inline = false;
    RingSnapshot();
""", """    g_ring.ps_ptr = rec.ps_addr; g_ring.ps_dwords = rec.ps_dwords; g_ring.ps_inline = false;
    RingSnapshot();
    // EDRAM PORT phase 1 (ngpu_rtc_shadow): the vendored render-target cache sees EVERY draw record, as the plugin's
    // Update does - not only the draws the native path issues - with the same microcode the SDK path would use.
    if (REXCVAR_GET(ngpu_rtc_shadow)) {
      const uint8_t* vsn = UcodeSnapGet(rec.vs_hash, rec.vs_dwords);
      const uint8_t* psn = UcodeSnapGet(rec.ps_hash, rec.ps_dwords);
      const uint8_t* vc = vsn ? vsn : (rec.vs_addr && rec.vs_dwords ? Phys(rec.vs_addr) : nullptr);
      const uint8_t* pc = psn ? psn : (rec.ps_addr && rec.ps_dwords ? Phys(rec.ps_addr) : nullptr);
      if (vc && (vsn || (PageReadable(vc) && PageReadable(vc + rec.vs_dwords * 4 - 1)))) {
        const rex::graphics::Shader* vsh = fable2::ngpu::sdk::AnalyzedShader(false, XXH3_64bits(vc, rec.vs_dwords * 4), reinterpret_cast<const uint32_t*>(vc), rec.vs_dwords);
        uint32_t pw = 0;
        if (pc && (psn || (PageReadable(pc) && PageReadable(pc + rec.ps_dwords * 4 - 1))))
          if (const rex::graphics::Shader* psh = fable2::ngpu::sdk::AnalyzedShader(true, XXH3_64bits(pc, rec.ps_dwords * 4), reinterpret_cast<const uint32_t*>(pc), rec.ps_dwords))
            pw = psh->writes_color_targets();
        const uint32_t sprim = rec.draw_initiator & 0x3Fu;
        if (vsh) fable2::ngpu::rtc::ShadowOnDraw(RingRegsShadow(), *vsh, pw, sprim != 1u && sprim != 2u && sprim != 3u);
        else ++g_rtc_shadow_no_vs;
      } else ++g_rtc_shadow_no_vs;
    }
""")
rep("uint32_t g_tess_text_logs = 0;", "uint32_t g_tess_text_logs = 0;\nuint64_t g_rtc_shadow_no_vs = 0;   // EDRAM port phase 1: draw records the shadow cache could not be given a VS for")
rep("REXCVAR_DEFINE_BOOL(ngpu_tex_reupload_in_place,", "REXCVAR_DEFINE_BOOL(ngpu_rtc_shadow, false, \"GPU\", \"Native-GPU EDRAM port phase 1: run the vendored SDK render-target cache beside the replay (ownership only, stub targets) - compare with the plugin via REX_OWNER_TRACE_TILE\");\nREXCVAR_DEFINE_BOOL(ngpu_tex_reupload_in_place,")
rep('#include "plume_d3d12.h"', '#include "plume_d3d12.h"\n#include "native_gpu_rtc.h"')
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
