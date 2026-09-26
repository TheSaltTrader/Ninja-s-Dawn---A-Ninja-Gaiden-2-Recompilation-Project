P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)
rep("void ClaimTiles3(NativeRT* rt, NativeDepth* nd, bool color, uint32_t base, uint32_t fmt, uint32_t len_tiles) {",
    "void ClaimTiles3(NativeRT* rt, NativeDepth* nd, bool color, uint32_t base, uint32_t fmt, uint32_t len_tiles, bool transfer = true) {")
rep("  if (!color && nd && nd->tex && g_s.cmd) {\n    const uint32_t drows = msaa ? 8u : 16u;",
    "  if (transfer && !color && nd && nd->tex && g_s.cmd) {\n    const uint32_t drows = msaa ? 8u : 16u;")
rep("""    static uint32_t cc_logs = 0;
    if (cc_logs < 6 && g_s.frames > 300) { ++cc_logs; REXLOG_INFO("[ngpu] resolve copy control {:08X}: clear colour {} depth {}", cc, (cc >> 8) & 1, (cc >> 9) & 1); }""",
"""    // RULE 3: a clearing resolve hands the cleared range to the target it cleared, with NO transfer (cache.cpp
    // PrepareHostRenderTargetsResolveClear -> ChangeOwnership). The native clear itself is applied at the next bind.
    if (REXCVAR_GET(ngpu_depth_owner_rule) == 3 && ((cc >> 8) & 3u)) {
      const uint32_t cmsaa = (rt->surf >> 16) & 3u;
      const uint32_t cpitch = ((rt->w << (cmsaa >= 2 ? 1 : 0)) + 79) / 80;
      const uint32_t clen = ((std::min(h, rt->h) << (cmsaa >= 1 ? 1 : 0)) + 15) / 16 * cpitch;
      if (((cc >> 9) & 1u) && rt->cur_dep) {
        for (auto& kv : rt->deps) if (&kv.second == rt->cur_dep) { ClaimTiles3(rt, rt->cur_dep, false, kv.first & 0x7FFu, (kv.first >> 11) & 1u, clen, false); break; }
      }
      if ((cc >> 8) & 1u) {
        const uint32_t cf = (rt->color >> 16) & 0xFu;
        ClaimTiles3(rt, nullptr, true, rt->color & 0xFFFu, cf, clen << ((cf == 5u || cf == 7u || cf == 15u) ? 1 : 0), false);
      }
    }
    static uint32_t cc_logs = 0;
    if (cc_logs < 6 && g_s.frames > 300) { ++cc_logs; REXLOG_INFO("[ngpu] resolve copy control {:08X}: clear colour {} depth {}", cc, (cc >> 8) & 1, (cc >> 9) & 1); }""")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
