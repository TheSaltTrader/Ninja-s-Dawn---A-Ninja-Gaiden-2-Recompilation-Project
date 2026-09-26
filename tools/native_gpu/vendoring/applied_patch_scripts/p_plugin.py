p=r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\rexglue-src\src\graphics\command_processor.cpp"
s=open(p,encoding='utf-8',newline='').read()
nl='\r\n' if '\r\n' in s else '\n'
def rep(a,b):
    global s
    a=a.replace('\n',nl); b=b.replace('\n',nl)
    assert s.count(a)==1,a[:70]; s=s.replace(a,b,1)
rep('''bool g_vs_immediate = false, g_ps_immediate = false;
''','''bool g_vs_immediate = false, g_ps_immediate = false;
// The microcode of the last IM_LOAD_IMMEDIATE of each type, copied out of the ring as the guest wrote it (big-endian
// dwords): an inline shader has no guest address, and g_cur_vs/g_cur_ps still name the PREVIOUS address-loaded
// shader - the Fable II lake's reflection blit was handed off as the sky cube's VS it followed.
std::vector<uint8_t> g_imm_vs, g_imm_ps;
''')
rep('''  if (opcode == PM4_IM_LOAD_IMMEDIATE && count >= 2) {
    (At(reader, offset, 0) & 0x3) ? (g_ps_immediate = true) : (g_vs_immediate = true);
    return;
  }
  if (opcode == PM4_IM_LOAD && count >= 2) {
    const uint32_t addr_type = At(reader, offset, 0);
    const uint32_t dwords = At(reader, offset, 1) & 0xFFFF;
    if ((addr_type & 0x3) == 0) {''','''  if (opcode == PM4_IM_LOAD_IMMEDIATE && count >= 2) {
    const bool ps = (At(reader, offset, 0) & 0x3) != 0;
    (ps ? g_ps_immediate : g_vs_immediate) = true;
    const uint32_t dwords = std::min<uint32_t>(At(reader, offset, 1) & 0xFFFF, count - 2);
    std::vector<uint8_t>& dst = ps ? g_imm_ps : g_imm_vs;
    dst.resize(size_t(dwords) * 4);
    for (uint32_t k = 0; k < dwords; ++k)   // raw bytes, the ring may wrap
      std::memcpy(dst.data() + k * 4, reader->buffer() + ((offset + (2 + k) * sizeof(uint32_t)) % reader->capacity()), 4);
    return;
  }
  if (opcode == PM4_IM_LOAD && count >= 2) {
    const uint32_t addr_type = At(reader, offset, 0);
    const uint32_t dwords = At(reader, offset, 1) & 0xFFFF;
    if ((addr_type & 0x3) == 0) {''')
rep('''  uint64_t bin_mask, bin_select;    // the tiling predicate in force at this draw
};''','''  uint64_t bin_mask, bin_select;    // the tiling predicate in force at this draw
  // (size >= offsetof end) the microcode of an INLINE shader (vs_inline/ps_inline), big-endian dwords as the guest
  // wrote them, valid for the duration of the callback; null when the shader was loaded from an address.
  const uint8_t* vs_code;
  const uint8_t* ps_code;
  uint32_t vs_code_dwords, ps_code_dwords;
};''')
rep('''      d.ps_inline = ngpu_pm4::g_ps_immediate ? 1u : 0u;
''','''      d.ps_inline = ngpu_pm4::g_ps_immediate ? 1u : 0u;
      if (d.vs_inline && !ngpu_pm4::g_imm_vs.empty()) { d.vs_code = ngpu_pm4::g_imm_vs.data(); d.vs_code_dwords = uint32_t(ngpu_pm4::g_imm_vs.size() / 4); }
      if (d.ps_inline && !ngpu_pm4::g_imm_ps.empty()) { d.ps_code = ngpu_pm4::g_imm_ps.data(); d.ps_code_dwords = uint32_t(ngpu_pm4::g_imm_ps.size() / 4); }
''')
open(p,'w',encoding='utf-8',newline='').write(s); print('ok')
