P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
a = ("bool g_async_on = false;   // ngpu_async_replay: the replay runs on its own thread" + nl +
     "inline bool AsyncHooksOff() { return g_async_on; }   // the guest-thread draw hooks must not touch g_s; NoteResolveDest still QUEUES (it feeds the replay's resolves)" + nl)
assert s.count(a) == 1
s = s.replace(a, "")
b = "uint64_t g_bridge_frames_unconsumed = 0,"
assert s.count(b) == 1
s = s.replace(b, a + b)
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
