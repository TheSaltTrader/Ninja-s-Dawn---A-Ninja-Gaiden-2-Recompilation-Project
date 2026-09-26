P = r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b, count=1):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != count: raise SystemExit(f"anchor count {s.count(a2)} (want {count}): {a[:80]!r}")
    s = s.replace(a2, b2)

# 1. ShadowPresent: async hand-off.
rep("""void ShadowPresent() {
  if (!REXCVAR_GET(ngpu_shadow)) return;
  if (!Init()) return;
  FlushDeferredUP();
  if (!g_s.frame_open && !BeginFrame()) return;""",
"""// ASYNC REPLAY (ngpu_async_replay, 2026-09-26): the replay cost ~70 ms of every 75 ms guest frame at Bower Lake because
// ShadowPresent - and so the whole native replay - runs on the game's present path. With the switch on (read once at
// startup; bridge mode only), the game thread only signals and pumps the window; a native thread replays the latest
// handed-off frame. The bridge hand-off already drops frames the replay has not consumed, so a slow replay costs the
// native window frames, not the game.
std::mutex g_async_mu;
std::condition_variable g_async_cv;
uint64_t g_async_requested = 0, g_async_done = 0, g_async_skipped = 0;
std::thread g_async_thread;
bool g_async_on = false;
void AsyncReplayWorker() {
  uint64_t seen = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(g_async_mu);
      g_async_cv.wait(lk, [&] { return g_async_requested != seen; });
      if (g_async_requested - seen > 1) g_async_skipped += g_async_requested - seen - 1;
      seen = g_async_requested;
    }
    if (!g_s.frame_open && !BeginFrame()) continue;
    const double t0 = Now();
    EndFrame();
    ++g_async_done;
    static double acc = 0; static uint32_t n = 0;
    acc += (Now() - t0) / 1000.0;
    if (++n == 300) {
      REXLOG_INFO("[ngpu] ASYNC REPLAY: mean {:.2f} ms per replayed frame over {}; present requests {}, replayed {}, coalesced {}", acc / n, n, g_async_requested, g_async_done, g_async_skipped);
      acc = 0; n = 0;
    }
  }
}
inline bool AsyncHooksOff() { return g_async_on; }   // bridge mode: the guest-thread hooks do nothing useful, and must not touch g_s

void ShadowPresent() {
  if (!REXCVAR_GET(ngpu_shadow)) return;
  if (!Init()) return;
  static const bool want_async = REXCVAR_GET(ngpu_async_replay) && REXCVAR_GET(ngpu_bridge) && REXCVAR_GET(ngpu_bridge_draws);
  if (want_async) {
    if (!g_async_on) { g_async_on = true; g_async_thread = std::thread(AsyncReplayWorker); g_async_thread.detach(); REXLOG_INFO("[ngpu] ASYNC REPLAY: native replay moved off the guest present path"); }
    Pump();
    { std::lock_guard<std::mutex> lk(g_async_mu); ++g_async_requested; }
    g_async_cv.notify_one();
    return;
  }
  FlushDeferredUP();
  if (!g_s.frame_open && !BeginFrame()) return;""")

# 2. guest-thread hooks are no-ops in async mode.
rep("""void ShadowDrawIndexed(uint32_t dev, uint32_t prim, uint32_t base_vertex, uint32_t start, uint32_t count) {
  ShadowDrawIndexedImpl(dev, prim, base_vertex, start, count, true);""",
"""void ShadowDrawIndexed(uint32_t dev, uint32_t prim, uint32_t base_vertex, uint32_t start, uint32_t count) {
  if (AsyncHooksOff()) return;
  ShadowDrawIndexedImpl(dev, prim, base_vertex, start, count, true);""")
rep("""void ShadowDrawVertices(uint32_t dev, uint32_t prim, uint32_t start, uint32_t count) {
  if (!REXCVAR_GET(ngpu_draw_vertices)) { NgpuDropAt(__LINE__); return; }""",
"""void ShadowDrawVertices(uint32_t dev, uint32_t prim, uint32_t start, uint32_t count) {
  if (AsyncHooksOff()) return;
  if (!REXCVAR_GET(ngpu_draw_vertices)) { NgpuDropAt(__LINE__); return; }""")
rep("""void ShadowDrawUP(uint32_t dev, uint32_t prim, uint32_t num_vertices, uint32_t stride, uint32_t vdata, uint32_t index_count) {
  (void)num_vertices; (void)stride; (void)vdata;""",
"""void ShadowDrawUP(uint32_t dev, uint32_t prim, uint32_t num_vertices, uint32_t stride, uint32_t vdata, uint32_t index_count) {
  if (AsyncHooksOff()) return;
  (void)num_vertices; (void)stride; (void)vdata;""")
rep("""void DeferUP(uint32_t dev, uint32_t prim, uint32_t min_index, uint32_t num_vertices, uint32_t index_count, uint32_t stride, uint32_t vptr, uint32_t iptr) {
  if (!REXCVAR_GET(ngpu_shadow) || !REXCVAR_GET(ngpu_native_draws) || !REXCVAR_GET(ngpu_draw_up)) return;""",
"""void DeferUP(uint32_t dev, uint32_t prim, uint32_t min_index, uint32_t num_vertices, uint32_t index_count, uint32_t stride, uint32_t vptr, uint32_t iptr) {
  if (AsyncHooksOff()) return;
  if (!REXCVAR_GET(ngpu_shadow) || !REXCVAR_GET(ngpu_native_draws) || !REXCVAR_GET(ngpu_draw_up)) return;""")
rep("""void FlushDeferredUP() {
  if (g_flushing_up || g_deferred_up.empty()) return;""",
"""void FlushDeferredUP() {
  if (AsyncHooksOff()) return;
  if (g_flushing_up || g_deferred_up.empty()) return;""")
rep("""void NoteResolveDest(uint32_t base, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags, uint32_t surf, uint32_t color) {""",
"""void NoteResolveDest(uint32_t base, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags, uint32_t surf, uint32_t color) {
  if (AsyncHooksOff()) return;   // bridge mode: the replay performs and registers its resolves itself""")

rep("REXCVAR_DEFINE_BOOL(ngpu_sdk_shm_gpu,",
    "REXCVAR_DEFINE_BOOL(ngpu_async_replay, false, \"GPU\", \"Native-GPU (bridge mode, read at startup): replay the handed-off frame on a native thread instead of on the guest's present path - the game no longer waits for the native renderer\");\nREXCVAR_DEFINE_BOOL(ngpu_sdk_shm_gpu,")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
