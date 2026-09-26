P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)

# 1. decide once per draw whether anything will READ the note's text.
rep("""      const uint32_t patches = (adaptive || prim == 13) ? count / 4u : count;
      char tb[300];
      std::snprintf(tb, sizeof(tb), "; TESSELLATED""",
"""      const uint32_t patches = (adaptive || prim == 13) ? count / 4u : count;
      // THE NOTE'S TEXT ONLY WHEN SOMETHING READS IT (PROF2, xperf 2026-09-26: the tessellation note's snprintf calls
      // were 12% of the async replay thread, built for every patch draw and read only by the first dozen log lines,
      // ngpu_dump_tess_draws, and dump frames). Otherwise the note is a non-empty marker: "!tess_note.empty()" is how
      // the rest of this loop knows the draw is a patch draw.
      const int tess_de = REXCVAR_GET(ngpu_dump_rts);
      tess_text = REXCVAR_GET(ngpu_dump_tess_draws) || g_tess_text_logs < 12 || (tess_de > 0 && g_s.frames && (g_s.frames % uint32_t(tess_de)) == 0);
      char tb[300];
      if (!tess_text) std::strcpy(tb, "; T");
      else std::snprintf(tb, sizeof(tb), "; TESSELLATED""")
rep("""      tess_note = tb;
      if (prim == 18) {""", """      tess_note = tb;
      if (prim == 18 && tess_text) {""")
rep("""    bool tess_line_wanted = false;""", """    bool tess_text = false;   // whether anything reads the note's text this draw (set below for patch draws)
    bool tess_line_wanted = false;""")
# 2. the factor / raw listing reads WRITE-COMBINED memory (slow_uncached_readback) - text only.
rep("""          if (adaptive && count <= 24 && pf_base && pe->bytes >= count * 4u) {
            const float* pf""", """          if (tess_text && adaptive && count <= 24 && pf_base && pe->bytes >= count * 4u) {
            const float* pf""")
rep("""      static uint32_t tess_logs = 0;
      if (tess_logs < 12 && g_s.frames > 300) {
        ++tess_logs;""", """      if (g_tess_text_logs < 12 && g_s.frames > 300) {
        ++g_tess_text_logs;""")
rep("uint32_t g_replay_drawn = 0,", "uint32_t g_tess_text_logs = 0;   // the first dozen tessellation lines (was a static inside the replay loop)\nuint32_t g_replay_drawn = 0,")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
