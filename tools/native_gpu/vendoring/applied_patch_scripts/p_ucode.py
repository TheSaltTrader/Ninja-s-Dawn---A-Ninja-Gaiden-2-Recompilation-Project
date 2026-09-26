import re,sys
p=r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s=open(p,encoding='utf-8',newline='').read()
nl='\r\n' if '\r\n' in s else '\n'
a="  uint32_t vsnap_first = 0, vsnap_count = 0;"
assert s.count(a)==1
s=s.replace(a,a+nl+"  // SHADER MICROCODE hashes at RECORD time (UcodeSnapGet): 0 = not taken."+nl+"  uint64_t vs_hash = 0, ps_hash = 0;",1)
b="void BridgeLogRecord(const RexNgpuDraw* d) {"
assert s.count(b)==1
blk='''// SHADER MICROCODE, copied at RECORD time and kept by content hash (never erased; a few hundred shaders).
// The replay used to read the microcode at the draw's address a frame later: a shader loaded INLINE lives in the
// command ring, and a streamed one's memory is reused - measured on the lake reflection blit (frame 1500 seq 610):
// the bytes found there were a world-space VS fetching its vertices from vf0, which at that draw is the PS's
// TEXTURE fetch constant (82024802), so every vertex was (0,0,0) with clip w = 0 and the blit drew nothing.
std::mutex g_ucode_lock;
std::unordered_map<uint64_t, std::unique_ptr<std::vector<uint8_t>>> g_ucode_by_hash;
uint64_t g_ucode_snap_hashed = 0, g_ucode_snap_unreadable = 0;
uint64_t UcodeSnapTake(uint32_t addr, uint32_t dwords) {
  if (!addr || !dwords) return 0;
  const uint8_t* p = Phys(addr);
  if (!PageReadable(p) || !PageReadable(p + dwords * 4 - 1)) { ++g_ucode_snap_unreadable; return 0; }
  const uint64_t h = XXH3_64bits(p, dwords * 4);
  ++g_ucode_snap_hashed;
  std::lock_guard<std::mutex> lk(g_ucode_lock);
  auto& e = g_ucode_by_hash[h];
  if (!e) e = std::make_unique<std::vector<uint8_t>>(p, p + dwords * 4);
  return h;
}
const uint8_t* UcodeSnapGet(uint64_t h, uint32_t dwords) {
  if (!h) return nullptr;
  std::lock_guard<std::mutex> lk(g_ucode_lock);
  auto it = g_ucode_by_hash.find(h);
  return it != g_ucode_by_hash.end() && it->second->size() == size_t(dwords) * 4 ? it->second->data() : nullptr;
}

'''
s=s.replace(b,blk.replace('\n',nl)+b,1)
c="  rec.ps_dwords = d->ps_dwords;"
assert s.count(c)==1
s=s.replace(c,c+nl+"  if (REXCVAR_GET(ngpu_bridge_ucode_snapshot)) { rec.vs_hash = UcodeSnapTake(d->vs_addr, d->vs_dwords); rec.ps_hash = UcodeSnapTake(d->ps_addr, d->ps_dwords); }",1)
d='''  const uint8_t* vcode = rec.vs_addr && rec.vs_dwords ? Phys(rec.vs_addr) : nullptr;
  const uint8_t* pcode = rec.ps_addr && rec.ps_dwords ? Phys(rec.ps_addr) : nullptr;'''
d=d.replace('\n',nl)
assert s.count(d)==1
e='''  // The RECORD-TIME microcode when it was snapshotted (UcodeSnapTake), else what the address holds now.
  const uint8_t* vsnapc = UcodeSnapGet(rec.vs_hash, rec.vs_dwords);
  const uint8_t* psnapc = UcodeSnapGet(rec.ps_hash, rec.ps_dwords);
  const uint8_t* vcode = vsnapc ? vsnapc : (rec.vs_addr && rec.vs_dwords ? Phys(rec.vs_addr) : nullptr);
  const uint8_t* pcode = psnapc ? psnapc : (rec.ps_addr && rec.ps_dwords ? Phys(rec.ps_addr) : nullptr);
  if (vsnapc && XXH3_64bits(Phys(rec.vs_addr), rec.vs_dwords * 4) != rec.vs_hash) ++g_st.ucode_moved;
  if (psnapc && XXH3_64bits(Phys(rec.ps_addr), rec.ps_dwords * 4) != rec.ps_hash) ++g_st.ucode_moved;'''
s=s.replace(d,e.replace('\n',nl),1)
f="name_unknown = 0, desc_reused = 0,"
assert s.count(f)==1
s=s.replace(f,"name_unknown = 0, ucode_moved = 0, desc_reused = 0,",1)
g='''REXLOG_INFO("[ngpu] SDK PATH IDENTITY: pair names confirmed by microcode {}, STALE {}, microcode not registered {}", g_st.name_ok, g_st.name_stale, g_st.name_unknown);'''
assert s.count(g)==1
s=s.replace(g,g+nl+'''  REXLOG_INFO("[ngpu] SDK PATH MICROCODE: shaders snapshotted at record {} (unreadable {}, distinct {}); draws whose address held OTHER microcode by replay time {}", g_ucode_snap_hashed, g_ucode_snap_unreadable, g_ucode_by_hash.size(), g_st.ucode_moved);''',1)
h="REXCVAR_DEFINE_INT32(ngpu_guest_raw_addr"
assert s.count(h)==1
s=s.replace(h,'REXCVAR_DEFINE_BOOL(ngpu_bridge_ucode_snapshot, true, "GPU", "Native-GPU: the bridge copies each draw\'s VS/PS microcode at RECORD time (by hash) and the SDK path translates that copy, not what the address holds a frame later");'+nl+h,1)
open(p,'w',encoding='utf-8',newline='').write(s)
print("ok")
