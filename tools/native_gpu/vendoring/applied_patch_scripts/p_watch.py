P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)
rep("""std::thread g_async_thread;
void AsyncReplayWorker() {""", """std::thread g_async_thread;
std::atomic<const char*> g_async_phase{"start"};
std::atomic<double> g_async_phase_t{0.0};
inline void AsyncPhase(const char* p) { g_async_phase.store(p); g_async_phase_t.store(Now()); }
void AsyncReplayWorker() {""")
rep("""      std::unique_lock<std::mutex> lk(g_async_mu);
      g_async_cv.wait""", """      AsyncPhase("waiting for a present");
      std::unique_lock<std::mutex> lk(g_async_mu);
      g_async_cv.wait""")
rep("""    if (!InitDraws()) continue;
    if (!g_s.frame_open && !BeginFrame()) continue;
    const double t0 = Now();
    EndFrame();
    ++g_async_done;""", """    AsyncPhase("InitDraws");
    if (!InitDraws()) continue;
    AsyncPhase("BeginFrame");
    if (!g_s.frame_open && !BeginFrame()) continue;
    const double t0 = Now();
    AsyncPhase("EndFrame");
    EndFrame();
    ++g_async_done;""")
rep("""    Pump();
    { std::lock_guard<std::mutex> lk(g_async_mu); ++g_async_requested; }""", """    Pump();
    // WATCHDOG: the worker stalled near replay frame ~1000 in both first legs (ASYNCP2, ASYNCM2) while the game ran on.
    if (const double pt = g_async_phase_t.load(); pt > 0 && Now() - pt > 3000.0) {
      static double last = 0;
      if (Now() - last > 5000.0) { last = Now(); REXLOG_INFO("[ngpu] ASYNC WATCHDOG: the replay thread has been in '{}' for {:.1f} s (replayed {}, frame {}, draw seq {})", g_async_phase.load(), (Now() - pt) / 1000.0, g_async_done, g_s.frames, g_replay_seq); }
    }
    { std::lock_guard<std::mutex> lk(g_async_mu); ++g_async_requested; }""")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
