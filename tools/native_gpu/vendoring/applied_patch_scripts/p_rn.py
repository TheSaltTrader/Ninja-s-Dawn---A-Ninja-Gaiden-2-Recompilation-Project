P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)
rep("""  ++g_rn_entered;
""", """  ++g_rn_entered;
  if (g_async_on) { g_resolve_pages.insert(base >> 12); ++g_resolve_hist[std::make_tuple(base, w, h, fmt, flags)]; }   // NoteResolveDest only queues in async mode
""")
rep("inline bool AsyncHooksOff() { return g_async_on; }   // bridge mode: the guest-thread hooks do nothing useful, and must not touch g_s",
    "inline bool AsyncHooksOff() { return g_async_on; }   // the guest-thread draw hooks must not touch g_s; NoteResolveDest still QUEUES (it feeds the replay's resolves)")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
