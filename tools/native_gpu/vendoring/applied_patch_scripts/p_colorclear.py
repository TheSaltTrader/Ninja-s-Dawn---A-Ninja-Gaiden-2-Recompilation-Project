P = r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit(f"anchor count {s.count(a2)}: {a[:80]!r}")
    s = s.replace(a2, b2, 1)

rep("  float clear_depth_value = -1.0f;        // RB_DEPTH_CLEAR of the resolve that asked for the pending depth clear (< 0 = unknown)",
"""  float clear_depth_value = -1.0f;        // RB_DEPTH_CLEAR of the resolve that asked for the pending depth clear (< 0 = unknown)
  bool clear_color_known = false;         // clear_color_value holds the resolve's RB_COLOR_CLEAR, decoded for this target's format
  float clear_color_value[4] = {0, 0, 0, 0};""")

rep("""      rt->clear_depth_value = (REXCVAR_GET(ngpu_clear_depth_from_reg) && LiveCopyFor((base & 0x1FFFFFFFu) + 0x1000u, own))
                                  ? float(own.depth_clear >> 8) / 16777215.0f : -1.0f;
    }""",
"""      rt->clear_depth_value = (REXCVAR_GET(ngpu_clear_depth_from_reg) && LiveCopyFor((base & 0x1FFFFFFFu) + 0x1000u, own))
                                  ? float(own.depth_clear >> 8) / 16777215.0f : -1.0f;
    }
    if ((cc >> 8) & 1) {
      // THE COLOUR SIBLING (peer's point, 2026-09-26): the pending colour clear wrote BLACK always, but the scene's
      // clearing resolves carry RB_COLOR_CLEAR 0, FFFFFFFF and D8060180 (logged). Decoded here for the target's colour
      // format (RB_COLOR_INFO bits 16..19); formats not handled keep black and are counted.
      rt->clear_color_known = false;
      LiveCopyRegs own;
      if (REXCVAR_GET(ngpu_clear_color_from_reg) && LiveCopyFor((base & 0x1FFFFFFFu) + 0x1000u, own)) {
        const uint32_t v = own.color_clear, f = (rt->color >> 16) & 0xFu;
        float* o = rt->clear_color_value;
        auto f7e3 = [](uint32_t x) {   // 10-bit 7e3 float: 3-bit exponent (bias 3), 7-bit mantissa
          const uint32_t e = (x >> 7) & 7u, m = x & 0x7Fu;
          return e ? std::ldexp(1.0f + float(m) / 128.0f, int(e) - 3) : std::ldexp(float(m) / 128.0f, -2);
        };
        if (v == 0) { o[0] = o[1] = o[2] = o[3] = 0.0f; rt->clear_color_known = true; }
        else if (f == 0 || f == 1) { o[0] = float((v >> 16) & 255) / 255.0f; o[1] = float((v >> 8) & 255) / 255.0f; o[2] = float(v & 255) / 255.0f; o[3] = float(v >> 24) / 255.0f; rt->clear_color_known = true; }
        else if (f == 2 || f == 10) { o[0] = float(v & 1023) / 1023.0f; o[1] = float((v >> 10) & 1023) / 1023.0f; o[2] = float((v >> 20) & 1023) / 1023.0f; o[3] = float(v >> 30) / 3.0f; rt->clear_color_known = true; }
        else if (f == 3 || f == 12) { o[0] = f7e3(v & 1023); o[1] = f7e3((v >> 10) & 1023); o[2] = f7e3((v >> 20) & 1023); o[3] = float(v >> 30) / 3.0f; rt->clear_color_known = true; }
        else ++g_clear_color_unknown_fmt;
        static uint32_t cvlog = 0;
        if (cvlog < 12) { ++cvlog; REXLOG_INFO("[ngpu] RESOLVE COLOUR CLEAR {:08X} on {:08X}/{:08X}: RB_COLOR_CLEAR {:08X} -> ({:.4g} {:.4g} {:.4g} {:.4g}){}", base, rt->surf, rt->color, v, o[0], o[1], o[2], o[3], rt->clear_color_known ? "" : " (format not decoded: black)"); }
      }
    }""")

rep("    if (rt->clear_color) g_s.cmd->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 0.0f));",
"""    if (rt->clear_color) {
      const float* c = rt->clear_color_value;
      g_s.cmd->clearColor(0, rt->clear_color_known ? RenderColor(c[0], c[1], c[2], c[3]) : RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
      if (rt->clear_color_known) ++g_clear_color_from_reg;
    }""")

rep("uint64_t g_clear_depth_from_reg = 0;", "uint64_t g_clear_depth_from_reg = 0, g_clear_color_from_reg = 0, g_clear_color_unknown_fmt = 0;")
rep("REXCVAR_DEFINE_BOOL(ngpu_clear_depth_from_reg,",
    "REXCVAR_DEFINE_BOOL(ngpu_clear_color_from_reg, false, \"GPU\", \"Native-GPU: a resolve's colour clear writes the resolve's own RB_COLOR_CLEAR decoded for the target format (8888, 2_10_10_10, 7e3), not black\");\nREXCVAR_DEFINE_BOOL(ngpu_clear_depth_from_reg,")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
