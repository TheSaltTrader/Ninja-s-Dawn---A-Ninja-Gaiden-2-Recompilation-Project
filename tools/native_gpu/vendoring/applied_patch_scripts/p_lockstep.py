P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
def rep(a, b, cnt=1):
    global s
    a2 = a.replace("\n", nl); b2 = b.replace("\n", nl)
    if s.count(a2) != cnt: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:90]))
    s = s.replace(a2, b2)

rep("REXCVAR_DEFINE_BOOL(ngpu_rtc_shadow,", 'REXCVAR_DEFINE_BOOL(ngpu_backend_lockstep, true, "GPU", "Native-GPU BACKEND TRANSPLANT: feed the transplanted backend INSIDE the plugin\'s draw / swap callbacks (its GPU thread, the same instant) instead of from the replay - guest memory (CPU-written resolve rectangles, dynamic vertex buffers, microcode) is exactly what the plugin reads. The async replay read it a frame or more later: 39k empty resolve rectangles in town (BET1)");\nREXCVAR_DEFINE_BOOL(ngpu_rtc_shadow,')

rep("""bool g_backend_on = false;   // ngpu_backend, latched when the backend initialises""", """bool g_backend_on = false;   // ngpu_backend, latched when the backend initialises
bool g_backend_lockstep = false;   // ngpu_backend_lockstep in effect (the replay does not feed the backend)
// LOCKSTEP: the backend's view of the plugin's register file, diffed per draw in 64-bit chunks over the ranges the
// transplanted IssueDraw / IssueCopy / IssueSwap read (0x2000-0x23FF state, 0x4000-0x4927 constants).
uint32_t g_ls_prev[0x5003];
bool g_ls_prev_valid = false;
uint64_t g_ls_draws = 0, g_ls_regs_written = 0;""")

# the per-draw hook, first thing in OnBridgeDraw after the struct check.
rep("""  if (!d || (d->size != sizeof(RexNgpuDraw) && d->size != offsetof(RexNgpuDraw, vs_code))) { ++g_bridge_struct_bad; return; }
  g_bridge_draws.fetch_add(1, std::memory_order_relaxed);""", """  if (!d || (d->size != sizeof(RexNgpuDraw) && d->size != offsetof(RexNgpuDraw, vs_code))) { ++g_bridge_struct_bad; return; }
  if (REXCVAR_GET(ngpu_backend) && REXCVAR_GET(ngpu_backend_lockstep) && d->regs && d->reg_count >= 0x4928) {
    BackendLockstepDraw(d);
    return;   // the replay does not run in lockstep - the native frame only presents the backend's output
  }
  g_bridge_draws.fetch_add(1, std::memory_order_relaxed);""")

rep("""void OnBridgeDraw(const RexNgpuDraw* d) {""", """// BACKEND LOCKSTEP (plugin GPU thread, inside the draw callback): the register diff, the microcode as it sits in
// guest memory NOW (or the packet's inline copy), then IssueDraw - what the plugin itself does at this instant.
void BackendLockstepSync(const uint32_t* regs) {
  static const uint32_t kRanges[][2] = {{0x2000, 0x2400}, {0x4000, 0x4928}};
  for (const auto& r : kRanges) {
    for (uint32_t i = r[0]; i < r[1]; i += 2) {
      uint64_t now, was;
      std::memcpy(&now, &regs[i], 8);
      std::memcpy(&was, &g_ls_prev[i], 8);
      if (g_ls_prev_valid && now == was) continue;
      for (uint32_t k = i; k < i + 2 && k < r[1]; ++k) {
        if (g_ls_prev_valid && g_ls_prev[k] == regs[k]) continue;
        g_ls_prev[k] = regs[k];
        fable2::ngpu::backend::WriteRegister(k, regs[k]);
        ++g_ls_regs_written;
      }
    }
  }
  g_ls_prev_valid = true;
}
bool BackendLockstepReady() {
  if (g_backend_on) return true;
  if (!g_s.device || !g_s.queue) return false;   // the native window is not up yet
  static bool tried = false;
  if (tried) return false;
  tried = true;
  g_backend_on = fable2::ngpu::backend::Init(static_cast<D3D12Device*>(g_s.device.get())->d3d,
                                             static_cast<D3D12CommandQueue*>(g_s.queue.get())->d3d);
  g_backend_lockstep = g_backend_on;
  REXLOG_INFO("[ngpu] BACKEND LOCKSTEP: {}", g_backend_on ? "the plugin's draw callback feeds the transplanted backend directly" : "initialisation FAILED");
  return g_backend_on;
}
void BackendLockstepDraw(const RexNgpuDraw* d) {
  if (!BackendLockstepReady()) return;
  BackendLockstepSync(d->regs);
  auto code = [](uint32_t addr, uint32_t dwords, bool inl, const uint8_t* inline_code, uint32_t inline_dwords) -> const uint32_t* {
    if (inl) return (inline_code && inline_dwords == dwords) ? reinterpret_cast<const uint32_t*>(inline_code) : nullptr;
    if (!addr || !dwords) return nullptr;
    const uint8_t* p = Phys(addr);
    return (p && PageReadable(p) && PageReadable(p + dwords * 4 - 1)) ? reinterpret_cast<const uint32_t*>(p) : nullptr;
  };
  const bool has_inline = d->size >= sizeof(RexNgpuDraw);
  fable2::ngpu::backend::DrawRecord br;
  br.draw_initiator = d->draw_initiator;
  br.index_addr = d->index_addr;
  br.index_size = d->index_size;
  br.vs_addr = d->vs_addr; br.vs_dwords = d->vs_dwords;
  br.ps_addr = d->ps_addr; br.ps_dwords = d->ps_dwords;
  br.vs_code = code(d->vs_addr, d->vs_dwords, d->vs_inline != 0, has_inline ? d->vs_code : nullptr, has_inline ? d->vs_code_dwords : 0);
  br.ps_code = code(d->ps_addr, d->ps_dwords, d->ps_inline != 0, has_inline ? d->ps_code : nullptr, has_inline ? d->ps_code_dwords : 0);
  fable2::ngpu::backend::Draw(br);
  ++g_ls_draws;
}

void OnBridgeDraw(const RexNgpuDraw* d) {""")

# swap: in lockstep the backend swaps right here, on the plugin thread.
rep("""    b.gamma = get_gamma && get_gamma(b.table, b.pwl);
  }""", """    b.gamma = get_gamma && get_gamma(b.table, b.pwl);
    if (REXCVAR_GET(ngpu_backend_lockstep) && g_backend_lockstep) {
      BackendLockstepSync(g_bridge_live_regs);
      fable2::ngpu::backend::Swap(b.fb, b.w, b.h, b.fetch0, b.gamma ? b.table : nullptr, b.gamma ? b.pwl : nullptr);
      b.valid = false;   // consumed here, not by the replay
      static uint32_t n = 0;
      if ((++n % 300) == 1) {
        const auto st = fable2::ngpu::backend::GetStats();
        REXLOG_INFO("[ngpu] BACKEND LOCKSTEP: {} draws ({} failed), {} swaps, {} shader loads ({} failed), {} register writes", st.draws, st.draw_failed, st.swaps, st.shader_loads, st.shader_load_failed, g_ls_regs_written);
      }
    }
  }""")
# the replay path stays for ngpu_backend with lockstep off.
rep("""  if (REXCVAR_GET(ngpu_backend) && !g_backend_on) {""", """  if (REXCVAR_GET(ngpu_backend) && !REXCVAR_GET(ngpu_backend_lockstep) && !g_backend_on) {""")
rep("""void BackendAfterReplay() {
  if (!g_backend_on) return;""", """void BackendAfterReplay() {
  if (!g_backend_on || g_backend_lockstep) return;""")
open(P, "w", encoding="utf-8", newline="").write(s)

# the driver: load the shader every draw (the plugin hashes on every shader packet); the pointer cache was unsafe
# once the code comes straight from guest memory at its address.
d = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_backend.cpp"
t = open(d, encoding="utf-8").read()
old = "    if (code == last_code && dwords == last_dwords && last) return last;\n"
assert t.count(old) == 1
t = t.replace(old, "    // No pointer cache: the pipeline cache hashes the microcode itself (as on every shader packet in the plugin); a\n    // pointer into guest memory can hold different code later.\n")
open(d, "w", encoding="utf-8").write(t)
print("ok")
