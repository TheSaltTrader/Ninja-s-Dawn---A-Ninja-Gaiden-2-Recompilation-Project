p=r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s=open(p,encoding='utf-8',newline='').read()
nl='\r\n' if '\r\n' in s else '\n'
def rep(a,b):
    global s
    a=a.replace('\n',nl); b=b.replace('\n',nl)
    assert s.count(a)==1,a[:70]; s=s.replace(a,b,1)
rep('''  uint32_t vs_inline, ps_inline;
  uint32_t predicated;
  uint64_t bin_mask, bin_select;
};
using RexNgpuDrawFn''','''  uint32_t vs_inline, ps_inline;
  uint32_t predicated;
  uint64_t bin_mask, bin_select;
  // Newer plugins (size covers them): an INLINE shader's microcode, big-endian, valid during the callback.
  const uint8_t* vs_code;
  const uint8_t* ps_code;
  uint32_t vs_code_dwords, ps_code_dwords;
};
using RexNgpuDrawFn''')
rep('''uint64_t UcodeSnapTake(uint32_t addr, uint32_t dwords) {
  if (!addr || !dwords) return 0;
  const uint8_t* p = Phys(addr);
  if (!PageReadable(p) || !PageReadable(p + dwords * 4 - 1)) { ++g_ucode_snap_unreadable; return 0; }
''','''uint64_t g_ucode_snap_inline = 0, g_ucode_inline_missing = 0;
uint64_t UcodeSnapBytes(const uint8_t* p, uint32_t dwords) {
''')
rep('''  const uint64_t h = XXH3_64bits(p, dwords * 4);
  ++g_ucode_snap_hashed;''','''  const uint64_t h = XXH3_64bits(p, dwords * 4);''')
rep('''  if (!e) e = std::make_unique<std::vector<uint8_t>>(p, p + dwords * 4);
  return h;
}''','''  if (!e) e = std::make_unique<std::vector<uint8_t>>(p, p + dwords * 4);
  return h;
}
uint64_t UcodeSnapTake(uint32_t addr, uint32_t dwords) {
  if (!addr || !dwords) return 0;
  const uint8_t* p = Phys(addr);
  if (!PageReadable(p) || !PageReadable(p + dwords * 4 - 1)) { ++g_ucode_snap_unreadable; return 0; }
  ++g_ucode_snap_hashed;
  return UcodeSnapBytes(p, dwords);
}''')
rep('''  if (REXCVAR_GET(ngpu_bridge_ucode_snapshot)) { rec.vs_hash = UcodeSnapTake(d->vs_addr, d->vs_dwords); rec.ps_hash = UcodeSnapTake(d->ps_addr, d->ps_dwords); }''',
'''  if (REXCVAR_GET(ngpu_bridge_ucode_snapshot)) {
    // An INLINE shader (IM_LOAD_IMMEDIATE) has no address: vs_addr still names the previous address-loaded shader.
    // Take the plugin's copy of the packet's microcode when it provides one; else count the draw as missing it.
    const bool has_code = d->size >= offsetof(RexNgpuDraw, ps_code_dwords) + sizeof(uint32_t);
    if (d->vs_inline && has_code && d->vs_code && d->vs_code_dwords) { rec.vs_hash = UcodeSnapBytes(d->vs_code, d->vs_code_dwords); rec.vs_dwords = d->vs_code_dwords; ++g_ucode_snap_inline; }
    else { if (d->vs_inline) ++g_ucode_inline_missing; rec.vs_hash = UcodeSnapTake(d->vs_addr, d->vs_dwords); }
    if (d->ps_inline && has_code && d->ps_code && d->ps_code_dwords) { rec.ps_hash = UcodeSnapBytes(d->ps_code, d->ps_code_dwords); rec.ps_dwords = d->ps_code_dwords; ++g_ucode_snap_inline; }
    else { if (d->ps_inline) ++g_ucode_inline_missing; rec.ps_hash = UcodeSnapTake(d->ps_addr, d->ps_dwords); }
  }''')
rep('''shaders snapshotted at record {} (unreadable {}, distinct {}); draws whose address held OTHER microcode by replay time {}", g_ucode_snap_hashed, g_ucode_snap_unreadable, g_ucode_by_hash.size(), g_st.ucode_moved);''',
'''shaders snapshotted at record {} (unreadable {}, distinct {}; INLINE shaders taken from the packet {}, inline with no copy from the plugin {}); draws whose address held OTHER microcode by replay time {}", g_ucode_snap_hashed, g_ucode_snap_unreadable, g_ucode_by_hash.size(), g_ucode_snap_inline, g_ucode_inline_missing, g_st.ucode_moved);''')
open(p,'w',encoding='utf-8',newline='').write(s); print('ok')
