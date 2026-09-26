P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
def rep(a, b):
    global s
    a2 = a.replace("\n", nl); b2 = b.replace("\n", nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2)
rep("void BackendLockstepSync(const uint32_t* regs);   // defined with OnBridgeDraw",
    "void BackendLockstepSync(const uint32_t* regs);   // defined with OnBridgeDraw\nvoid RequestNativePresent();   // defined with ShadowPresent")
rep("""      fable2::ngpu::backend::Swap(b.fb, b.w, b.h, b.fetch0, b.gamma ? b.table : nullptr, b.gamma ? b.pwl : nullptr);
      b.valid = false;   // consumed here, not by the replay""", """      fable2::ngpu::backend::Swap(b.fb, b.w, b.h, b.fetch0, b.gamma ? b.table : nullptr, b.gamma ? b.pwl : nullptr);
      b.valid = false;   // consumed here, not by the replay
      // PRESENT AT THE SWAP (WALKL1: the native window ran frames behind while walking and a whole menu screen behind
      // in the pause menu). The guest's present hook fires when the CPU submits the frame - ahead of the GPU thread
      // reaching this swap - so presenting from there showed an older output. The native frame is requested here,
      // right after the backend's own IssueSwap, and ShadowPresent no longer requests one in lockstep.
      RequestNativePresent();""")
rep("""    { std::lock_guard<std::mutex> lk(g_async_mu); ++g_async_requested; }
    g_async_cv.notify_one();
    return;
  }
  FlushDeferredUP();""", """    if (!g_backend_lockstep) RequestNativePresent();   // lockstep: requested at the backend's swap instead
    return;
  }
  FlushDeferredUP();""")
rep("""void ShadowPresent() {
  if (!REXCVAR_GET(ngpu_shadow)) return;
  if (!Init()) return;
  static const bool want_async""", """void RequestNativePresent() {
  if (!g_async_on) return;
  { std::lock_guard<std::mutex> lk(g_async_mu); ++g_async_requested; }
  g_async_cv.notify_one();
}

void ShadowPresent() {
  if (!REXCVAR_GET(ngpu_shadow)) return;
  if (!Init()) return;
  static const bool want_async""")
open(P, "w", encoding="utf-8", newline="").write(s)
print("ok")
