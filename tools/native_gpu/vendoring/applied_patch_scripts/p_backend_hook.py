P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
def rep(a, b, cnt=1):
    global s
    a2 = a.replace("\n", nl); b2 = b.replace("\n", nl)
    if s.count(a2) != cnt: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:90]))
    s = s.replace(a2, b2)

# 0. includes, cvar, the per-frame swap snapshot.
rep('#include "native_gpu_rtc.h"', '#include "native_gpu_rtc.h"\n#include "native_gpu_backend.h"')
rep("REXCVAR_DEFINE_BOOL(ngpu_rtc_shadow,", 'REXCVAR_DEFINE_BOOL(ngpu_backend, false, "GPU", "Native-GPU BACKEND TRANSPLANT (read at startup): the replay drives the plugin\'s own D3D12 backend, vendored in-app (native_gpu_backend.cpp) - every draw record, copy draws as resolves, the swap with the plugin\'s gamma ramp - and the window shows its gamma-applied output; the native draw path is bypassed");\nREXCVAR_DEFINE_BOOL(ngpu_rtc_shadow,')
rep("uint64_t g_bridge_frames_unconsumed = 0,", """// BACKEND TRANSPLANT: what the swap callback saw - fetch constant 0 (the front buffer the swap samples) and the gamma
// ramp - handed off with the frame and consumed by the replay's IssueSwap.
struct BackendSwap {
  bool valid = false, gamma = false;
  uint32_t fb = 0, w = 0, h = 0;
  uint32_t fetch0[6] = {};
  uint32_t table[256] = {};
  uint32_t pwl[128 * 3] = {};
};
BackendSwap g_bswap_pending, g_bswap_ready;
bool g_backend_on = false;   // ngpu_backend, latched when the backend initialises
uint64_t g_bridge_frames_unconsumed = 0,""")

# 1. hand-off: the ready frame carries the latest swap.
rep("""void BridgeLogHandOff() {
  std::lock_guard<std::mutex> lk(g_bridge_ready_lock);""", """void BridgeLogHandOff() {
  std::lock_guard<std::mutex> lk(g_bridge_ready_lock);
  if (g_bswap_pending.valid) { g_bswap_ready = g_bswap_pending; g_bswap_pending.valid = false; }""")

# 2. swap callback: snapshot fetch 0 + gamma ramp (valid only now - RexNgpuGetGammaRamp).
rep("""void OnBridgeSwap(uint32_t, uint32_t, uint32_t) {""", """void OnBridgeSwap(uint32_t fb, uint32_t fb_w, uint32_t fb_h) {
  if (REXCVAR_GET(ngpu_backend) && g_bridge_live_regs) {
    BackendSwap& b = g_bswap_pending;
    b.valid = true; b.fb = fb; b.w = fb_w; b.h = fb_h;
    for (uint32_t i = 0; i < 6; ++i) b.fetch0[i] = g_bridge_live_regs[0x4800 + i];
    using GetGammaFn = bool (*)(uint32_t*, uint32_t*);
    static GetGammaFn get_gamma = [] {
      HMODULE m = GetModuleHandleA("rexgpu-xenos.dll");
      auto f = m ? reinterpret_cast<GetGammaFn>(GetProcAddress(m, "RexNgpuGetGammaRamp")) : nullptr;
      REXLOG_INFO("[ngpu] BACKEND: plugin gamma-ramp export {}", f ? "found" : "MISSING - the swap keeps the linear default ramp");
      return f;
    }();
    b.gamma = get_gamma && get_gamma(b.table, b.pwl);
  }""")

# 3. the replay takes the frame's swap; with the backend on, resolves are the copy draws' job.
rep("""  std::vector<VSnap> vsnap;
  std::vector<uint8_t> vsnap_data;
  {
    std::lock_guard<std::mutex> lk(g_bridge_ready_lock);
    if (!g_bridge_ready) return;""", """  std::vector<VSnap> vsnap;
  std::vector<uint8_t> vsnap_data;
  static BackendSwap bswap;
  {
    std::lock_guard<std::mutex> lk(g_bridge_ready_lock);
    if (!g_bridge_ready) return;
    bswap = g_bswap_ready; g_bswap_ready.valid = false;""")
rep("""    vsnap_data.swap(g_bridge_ready_vsnap_data);
    g_bridge_ready = false;
  }
  {
    std::lock_guard<std::mutex> lk(g_bridge_resolve_lock);
    resolves.swap(g_bridge_ready_resolves);""", """    vsnap_data.swap(g_bridge_ready_vsnap_data);
    g_bridge_ready = false;
  }
  // BACKEND TRANSPLANT: initialise once (plume's own device and direct queue); from then on the replay feeds it.
  if (REXCVAR_GET(ngpu_backend) && !g_backend_on) {
    g_backend_on = fable2::ngpu::backend::Init(static_cast<D3D12Device*>(g_s.device.get())->d3d,
                                               static_cast<D3D12CommandQueue*>(g_s.queue.get())->d3d);
    if (!g_backend_on) { static bool said = false; if (!said) { said = true; REXLOG_ERROR("[ngpu] BACKEND: initialisation failed - the native draw path stays in charge"); } }
  }
  {
    std::lock_guard<std::mutex> lk(g_bridge_resolve_lock);
    resolves.swap(g_bridge_ready_resolves);
    if (g_backend_on) resolves.clear();   // the copy-mode draw records perform the resolves (IssueDraw -> IssueCopy)""")

# 4. every delta goes to the backend too.
rep("""        g_slot_last_seq = g_replay_seq;
      }
      RingSetReg(kv.first, kv.second);
    }""", """        g_slot_last_seq = g_replay_seq;
      }
      RingSetReg(kv.first, kv.second);
      if (g_backend_on) fable2::ngpu::backend::WriteRegister(kv.first, kv.second);
    }""")

# 5. the record goes to the backend; the native draw path is bypassed.
rep("""      } else ++g_rtc_shadow_no_vs;
    }
""", """      } else ++g_rtc_shadow_no_vs;
    }
    if (g_backend_on) {
      // The microcode as the SDK path would use it: the record-time snapshot, else guest memory.
      const uint8_t* vsn = UcodeSnapGet(rec.vs_hash, rec.vs_dwords);
      const uint8_t* psn = UcodeSnapGet(rec.ps_hash, rec.ps_dwords);
      const uint8_t* vc = vsn ? vsn : (rec.vs_addr && rec.vs_dwords ? Phys(rec.vs_addr) : nullptr);
      const uint8_t* pc = psn ? psn : (rec.ps_addr && rec.ps_dwords ? Phys(rec.ps_addr) : nullptr);
      if (vc && !vsn && !(PageReadable(vc) && PageReadable(vc + rec.vs_dwords * 4 - 1))) vc = nullptr;
      if (pc && !psn && !(PageReadable(pc) && PageReadable(pc + rec.ps_dwords * 4 - 1))) pc = nullptr;
      fable2::ngpu::backend::DrawRecord br;
      br.draw_initiator = rec.draw_initiator;
      br.index_addr = rec.index_addr;
      br.index_size = rec.index_size;
      br.vs_addr = rec.vs_addr; br.vs_dwords = rec.vs_dwords;
      br.ps_addr = rec.ps_addr; br.ps_dwords = rec.ps_dwords;
      br.vs_code = reinterpret_cast<const uint32_t*>(vc);
      br.ps_code = reinterpret_cast<const uint32_t*>(pc);
      if (fable2::ngpu::backend::Draw(br)) ++g_replay_drawn;
      ++g_replay_seq;
      continue;
    }
""")

# 6. after the replay: the frame's swap (or a plain submission).
rep("""void EndFrame() {
  if (!g_s.frame_open) return;
  BridgeLogReplay();""", """void EndFrame() {
  if (!g_s.frame_open) return;
  BridgeLogReplay();
  BackendAfterReplay();""")
rep("""bool DrawAtExit() { return REXCVAR_GET(ngpu_draw_at_exit); }""", """bool DrawAtExit() { return REXCVAR_GET(ngpu_draw_at_exit); }

// BACKEND TRANSPLANT: the frame the replay just fed ends with its swap - IssueSwap with the fetch constant 0 and the
// gamma ramp the swap callback saw - or, when the handed-off frame had none, a plain submission.
BackendSwap g_bswap_for_frame;
void BackendAfterReplay() {
  if (!g_backend_on) return;
  if (g_bswap_for_frame.valid) {
    const BackendSwap& b = g_bswap_for_frame;
    fable2::ngpu::backend::Swap(b.fb, b.w, b.h, b.fetch0, b.gamma ? b.table : nullptr, b.gamma ? b.pwl : nullptr);
    g_bswap_for_frame.valid = false;
  } else {
    fable2::ngpu::backend::EndFrameNoSwap();
  }
  static uint32_t n = 0;
  if ((++n % 300) == 1) {
    const auto st = fable2::ngpu::backend::GetStats();
    REXLOG_INFO("[ngpu] BACKEND: {} draws issued ({} failed), {} swaps, {} shader loads ({} failed)", st.draws, st.draw_failed, st.swaps, st.shader_loads, st.shader_load_failed);
  }
}""")
# the replay hands its swap to BackendAfterReplay.
rep("""    bswap = g_bswap_ready; g_bswap_ready.valid = false;""", """    bswap = g_bswap_ready; g_bswap_ready.valid = false;
    if (bswap.valid) g_bswap_for_frame = bswap;""")
rep("struct BackendSwap {", "void BackendAfterReplay();\nstruct BackendSwap {")
open(P, "w", encoding="utf-8", newline="").write(s)
print("ok")
