R = r'C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/'
def patch(p, old, new, count=1):
    s = open(R + p, encoding='utf-8').read()
    n = s.count(old)
    assert n == count, (p, old[:60], n)
    open(R + p, 'w', encoding='utf-8').write(s.replace(old, new))

patch('native_gpu_xlat/rtc_d3d12/cp_base.cpp', '''  // The register-table lookup only feeds a debug message; it cost ~2% of the GPU thread per write (PROFT1).
  if (auto* lp = ::rex::GetLoggerRaw(::rex::log::gpu());
      lp && lp->should_log(spdlog::level::debug) && !regs.GetRegisterInfo(index)) {''',
'''  // The register-table lookup only feeds a debug message; it cost ~2% of the GPU thread per write (PROFT1).
  // The level is read once (the cross-module logger lookup per write was itself 0.6%, PROFT2).
  static const bool gpu_debug = [] {
    auto* lp = ::rex::GetLoggerRaw(::rex::log::gpu());
    return lp && lp->should_log(spdlog::level::debug);
  }();
  if (gpu_debug && !regs.GetRegisterInfo(index)) {''')

p = 'native_gpu_present.cpp'
patch(p, '''  static const uint32_t kRanges[][2] = {{0x2000, 0x2400}, {0x4000, 0x4928}};
  // SELF-CHECK, every 1024th draw''', '''  static const uint32_t kRanges[][2] = {{0x2000, 0x2400}, {0x4000, 0x4928}};
  // DIRTY BITMAP (plugin export RexNgpuDirtyRegs, rexglue-src after 17309f4): the plugin marks every register it
  // writes; only marked registers are compared. The self-check below compares against the full scan, and a single
  // miss switches this run back to the full scan for good (logged).
  using DirtyFn = uint64_t* (*)(uint32_t*);
  static uint32_t dirty_words = 0;
  static uint64_t* dirty = [] {
    HMODULE m = GetModuleHandleA("rexgpu-xenos.dll");
    auto f = m ? reinterpret_cast<DirtyFn>(GetProcAddress(m, "RexNgpuDirtyRegs")) : nullptr;
    uint64_t* d = f ? f(&dirty_words) : nullptr;
    if (d && dirty_words * 64 < 0x4928) d = nullptr;
    REXLOG_INFO("[ngpu] BACKEND LOCKSTEP: plugin dirty-register bitmap {}", d ? "found - only written registers are compared" : "MISSING - full register scan per draw");
    return d;
  }();
  static bool dirty_ok = true;
  // SELF-CHECK, every 1024th draw''')
patch(p, '''  if (check)
    for (const auto& r : kRanges)
      for (uint32_t k = r[0]; k < r[1]; ++k) expect += g_ls_prev[k] != regs[k];
  for (const auto& r : kRanges) {''', '''  if (check)
    for (const auto& r : kRanges)
      for (uint32_t k = r[0]; k < r[1]; ++k) expect += g_ls_prev[k] != regs[k];
  if (dirty && dirty_ok && g_ls_prev_valid) {
    for (const auto& r : kRanges) {
      for (uint32_t w = r[0] >> 6; w <= (r[1] - 1) >> 6; ++w) {
        uint64_t bits = dirty[w];
        if (!bits) continue;
        dirty[w] = 0;
        while (bits) {
          unsigned long b; _BitScanForward64(&b, bits);
          const uint32_t k = (w << 6) + uint32_t(b);
          bits &= bits - 1;
          if (k < r[0] || k >= r[1] || g_ls_prev[k] == regs[k]) continue;
          g_ls_prev[k] = regs[k];
          fable2::ngpu::backend::WriteRegister(k, regs[k]);
          ++g_ls_regs_written;
        }
      }
    }
  } else {
  if (dirty) std::memset(dirty, 0, dirty_words * sizeof(uint64_t));
  for (const auto& r : kRanges) {''')
patch(p, '''  g_ls_prev_valid = true;
  if (check) {
    ++check_runs;
    if (g_ls_regs_written - written_before != expect) ++check_bad;''', '''  }
  g_ls_prev_valid = true;
  if (check) {
    ++check_runs;
    if (g_ls_regs_written - written_before != expect) {
      ++check_bad;
      if (dirty_ok && dirty) {
        dirty_ok = false;
        REXLOG_INFO("[ngpu] BACKEND LOCKSTEP: the dirty bitmap MISSED a register write ({} written, {} differ) - back to the full scan for this run",
                    g_ls_regs_written - written_before, expect);
      }
    }''')
print('ok')
