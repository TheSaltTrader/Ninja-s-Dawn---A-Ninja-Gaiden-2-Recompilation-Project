P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
def rep(a, b):
    global s
    a2 = a.replace("\n", nl); b2 = b.replace("\n", nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2)

rep("""LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
      ShowWindow(hwnd, SW_HIDE);  // the game owns the process; hiding is enough
      return 0;
    case WM_SIZE:
      if (g_s.swap && wp != SIZE_MINIMIZED && !g_s.frame_open) {
        g_s.fbs.clear();
        g_s.swap->resize();
        MakeFramebuffers();
      }
      return 0;
  }""", """// THE WINDOW HAS ITS OWN THREAD (2026-09-26, user report: clicking the native window made it "Not Responding").
// It used to be created by, and pumped only from, the guest's present path - any time that thread was busy (a
// load, a GPU wait) the window stopped answering and Windows flagged it. Now a dedicated thread creates it and runs
// a blocking message loop; WM_SIZE only raises a flag the RENDER thread acts on at its next frame, so the swap chain
// is never resized underneath a frame.
std::atomic<bool> g_resize_pending{false};
bool g_window_thread = false;
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
      ShowWindow(hwnd, SW_HIDE);  // the game owns the process; hiding is enough
      return 0;
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) g_resize_pending.store(true);
      return 0;
  }""")
rep("""void Pump() {
  MSG msg;""", """void Pump() {
  if (g_window_thread) return;   // the window's own thread pumps its messages
  MSG msg;""")
rep("""  RECT wr = {0, 0, 1280, 720};
  AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
  g_s.hwnd = CreateWindowExA(0, wc.lpszClassName, "Fable II - native shadow (Plume D3D12)", WS_OVERLAPPEDWINDOW,
                             40, 40, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, inst, nullptr);""", """  RECT wr = {0, 0, 1280, 720};
  AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
  {
    auto created = std::make_shared<std::promise<HWND>>();
    auto ready = created->get_future();
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    std::thread([created, inst, ww, wh]() {
      HWND h = CreateWindowExA(0, "Fable2NativeShadow", "Fable II - native shadow (Plume D3D12)", WS_OVERLAPPEDWINDOW,
                               40, 40, ww, wh, nullptr, nullptr, inst, nullptr);
      created->set_value(h);
      if (!h) return;
      MSG msg;
      while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
      }
    }).detach();
    g_s.hwnd = ready.get();
    g_window_thread = g_s.hwnd != nullptr;
  }""")
# the render thread applies a pending resize at the start of a frame.
rep("""bool BeginFrame() {
  if (g_s.frame_open) return true;
  Pump();""", """bool BeginFrame() {
  if (g_s.frame_open) return true;
  Pump();
  if (g_resize_pending.exchange(false) && g_s.swap) {
    g_s.fbs.clear();
    g_s.swap->resize();
    MakeFramebuffers();
  }""")
if "#include <future>" not in s:
    s = s.replace("#include <atomic>", "#include <atomic>" + nl + "#include <future>", 1)
open(P, "w", encoding="utf-8", newline="").write(s)
print("ok")
