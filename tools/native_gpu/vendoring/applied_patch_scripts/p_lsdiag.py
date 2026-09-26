P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
def rep(a, b):
    global s
    a2 = a.replace("\n", nl); b2 = b.replace("\n", nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:90]))
    s = s.replace(a2, b2)
rep("uint64_t g_ls_draws = 0, g_ls_regs_written = 0;",
    "uint64_t g_ls_draws = 0, g_ls_regs_written = 0;\n// why a lockstep draw had no microcode: [vs|ps][inline mismatch | inline null | unreadable | no address]\nuint64_t g_ls_nocode[2][4] = {};\nuint64_t g_ls_fail_nocode = 0, g_ls_fail_withcode = 0;")
rep("""  auto code = [](uint32_t addr, uint32_t dwords, bool inl, const uint8_t* inline_code, uint32_t inline_dwords) -> const uint32_t* {
    if (inl) return (inline_code && inline_dwords == dwords) ? reinterpret_cast<const uint32_t*>(inline_code) : nullptr;
    if (!addr || !dwords) return nullptr;
    const uint8_t* p = Phys(addr);
    return (p && PageReadable(p) && PageReadable(p + dwords * 4 - 1)) ? reinterpret_cast<const uint32_t*>(p) : nullptr;
  };""", """  auto code = [](int stage, uint32_t addr, uint32_t& dwords, bool inl, const uint8_t* inline_code, uint32_t inline_dwords) -> const uint32_t* {
    if (inl) {
      if (!inline_code) { ++g_ls_nocode[stage][1]; return nullptr; }
      // The packet's own copy is authoritative; its length is the one to hand the pipeline cache.
      if (inline_dwords != dwords) ++g_ls_nocode[stage][0];
      dwords = inline_dwords;
      return reinterpret_cast<const uint32_t*>(inline_code);
    }
    if (!addr || !dwords) { ++g_ls_nocode[stage][3]; return nullptr; }
    const uint8_t* p = Phys(addr);
    if (!(p && PageReadable(p) && PageReadable(p + dwords * 4 - 1))) { ++g_ls_nocode[stage][2]; return nullptr; }
    return reinterpret_cast<const uint32_t*>(p);
  };""")
rep("""  br.vs_code = code(d->vs_addr, d->vs_dwords, d->vs_inline != 0, has_inline ? d->vs_code : nullptr, has_inline ? d->vs_code_dwords : 0);
  br.ps_code = code(d->ps_addr, d->ps_dwords, d->ps_inline != 0, has_inline ? d->ps_code : nullptr, has_inline ? d->ps_code_dwords : 0);
  fable2::ngpu::backend::Draw(br);""", """  br.vs_code = code(0, d->vs_addr, br.vs_dwords, d->vs_inline != 0, has_inline ? d->vs_code : nullptr, has_inline ? d->vs_code_dwords : 0);
  br.ps_code = code(1, d->ps_addr, br.ps_dwords, d->ps_inline != 0, has_inline ? d->ps_code : nullptr, has_inline ? d->ps_code_dwords : 0);
  if (!fable2::ngpu::backend::Draw(br)) ++(br.vs_code ? g_ls_fail_withcode : g_ls_fail_nocode);""")
rep("""        REXLOG_INFO("[ngpu] BACKEND LOCKSTEP: {} draws ({} failed), {} swaps, {} shader loads ({} failed), {} register writes", st.draws, st.draw_failed, st.swaps, st.shader_loads, st.shader_load_failed, g_ls_regs_written);""",
"""        REXLOG_INFO("[ngpu] BACKEND LOCKSTEP: {} draws ({} failed: {} with no VS microcode, {} with it), {} swaps, {} shader loads ({} failed), {} register writes; no microcode VS inline-len {} inline-null {} unreadable {} no-addr {} | PS {} {} {} {}",
                    st.draws, st.draw_failed, g_ls_fail_nocode, g_ls_fail_withcode, st.swaps, st.shader_loads, st.shader_load_failed, g_ls_regs_written,
                    g_ls_nocode[0][0], g_ls_nocode[0][1], g_ls_nocode[0][2], g_ls_nocode[0][3], g_ls_nocode[1][0], g_ls_nocode[1][1], g_ls_nocode[1][2], g_ls_nocode[1][3]);""")
open(P, "w", encoding="utf-8", newline="").write(s)
print("ok")
