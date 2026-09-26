P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)
rep("""    const size_t cap = size_t(std::max(0, REXCVAR_GET(ngpu_bridge_accumulate_cap)));""",
"""    // ASYNC: the game is no longer held to the replay's pace, so an uncapped merge snowballs (ASYNCP3: 130k-176k draws
    // per replay, 5-10 s each). The async cap merges only SMALL frames (load-time one-shot passes) and keeps the latest
    // frame otherwise.
    const size_t cap = size_t(std::max(0, g_async_on ? REXCVAR_GET(ngpu_async_accumulate_cap) : REXCVAR_GET(ngpu_bridge_accumulate_cap)));""")
rep("REXCVAR_DEFINE_BOOL(ngpu_async_replay, false,", "REXCVAR_DEFINE_INT32(ngpu_async_accumulate_cap, 4000, \"GPU\", \"Native-GPU: ngpu_bridge_accumulate_cap while ngpu_async_replay is on (a lake frame is ~2500-3500 draws, so gameplay frames are not merged)\");\nREXCVAR_DEFINE_BOOL(ngpu_async_replay, true,")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
