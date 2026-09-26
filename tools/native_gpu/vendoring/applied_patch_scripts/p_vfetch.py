p=r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s=open(p,encoding='utf-8',newline='').read()
nl='\r\n' if '\r\n' in s else '\n'
def rep(a,b):
    global s
    a=a.replace('\n',nl); b=b.replace('\n',nl)
    assert s.count(a)==1,a[:60]
    s=s.replace(a,b,1)
rep('''uint64_t g_ucode_snap_hashed = 0, g_ucode_snap_unreadable = 0;
''','''uint64_t g_ucode_snap_hashed = 0, g_ucode_snap_unreadable = 0;
// The vertex fetch constants each VS reads ({fetch constant, stride dwords}), by microcode hash - filled by the SDK
// path once it has translated the shader, read by the record-time vertex snapshot (under g_ucode_lock).
std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> g_vs_fetch_by_hash;
uint64_t g_vsnap_nonvertex = 0;
''')
rep('''      for (uint32_t i = 0; i < 96; ++i) {
        const uint32_t d0 = d->regs[0x4800 + i * 2], d1 = d->regs[0x4800 + i * 2 + 1];
        if ((d0 & 3u) != 3u) continue;
        const uint32_t phys = (d0 & ~3u) & 0x1FFFFFFFu, bytes = ((d1 >> 2) & 0xFFFFFFu) * 4;
''','''      // The slots THIS VS reads when the SDK path has already translated it - whatever their type: the lake's
      // reflection blit (frame 1500, VS 8A0AE541) fetches its 3 vertices through vf0 while slot 0 holds the PS's
      // TEXTURE constant (82024802); the GPU reads the vertex address from dword 0 regardless, and the plugin's copy
      // of that blit is right (F3402000 means 0.143/0.174/0.222 = its source F3582000). Else every type-3 slot.
      std::vector<std::pair<uint32_t, uint32_t>> used;
      if (rec.vs_hash) {
        std::lock_guard<std::mutex> lk(g_ucode_lock);
        auto it = g_vs_fetch_by_hash.find(rec.vs_hash);
        if (it != g_vs_fetch_by_hash.end()) used = it->second;
      }
      const bool known = !used.empty();
      if (!known) for (uint32_t i = 0; i < 96; ++i) used.push_back({i, 0});
      const bool auto_idx = ((d->draw_initiator >> 6) & 3u) == 2u;
      for (const auto& [i, stride] : used) {
        if (i >= 96) continue;
        const uint32_t d0 = d->regs[0x4800 + i * 2], d1 = d->regs[0x4800 + i * 2 + 1];
        const bool vtx = (d0 & 3u) == 3u;
        if (!vtx && !(known && auto_idx && stride)) continue;
        const uint32_t phys = (d0 & ~3u) & 0x1FFFFFFFu, bytes = vtx ? ((d1 >> 2) & 0xFFFFFFu) * 4 : count * stride * 4;
        if (!vtx) ++g_vsnap_nonvertex;
''')
rep('''  if (pt->vs.memexport || pt->ps.memexport) {''','''  if (rec.vs_hash) {
    std::lock_guard<std::mutex> lk(g_ucode_lock);
    auto& e = g_vs_fetch_by_hash[rec.vs_hash];
    if (e.empty()) for (const auto& vb : pt->vs.vertex) e.push_back({vb.fetch_constant, vb.stride_words});
  }
  if (pt->vs.memexport || pt->ps.memexport) {''')
rep('''vertex snapshots taken {} applied {}", g_resolves_from_regs, g_rt_grown, g_bridge_vsnap_taken, g_st.vsnap_applied);''',
    '''vertex snapshots taken {} applied {} (streams through a NON-vertex fetch constant {})", g_resolves_from_regs, g_rt_grown, g_bridge_vsnap_taken, g_st.vsnap_applied, g_vsnap_nonvertex);''')
open(p,'w',encoding='utf-8',newline='').write(s)
print('ok')
