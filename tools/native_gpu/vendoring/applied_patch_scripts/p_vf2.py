p=r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s=open(p,encoding='utf-8',newline='').read()
def rep(a,b):
    global s
    assert s.count(a)==1,a[:60]; s=s.replace(a,b,1)
rep("const bool auto_idx = ((d->draw_initiator >> 6) & 3u) == 2u;","const bool auto_idx = ((d->draw_initiator >> 6) & 3u) != 0u;   // as the replay's auto_index")
rep("      const bool known = !used.empty();","      const bool known = !used.empty();\n      if (known) ++g_vsnap_known; else if (rec.vs_hash) ++g_vsnap_unknown; else ++g_vsnap_nohash;".replace('\n','\r\n' if '\r\n' in s else '\n'))
rep("uint64_t g_vsnap_nonvertex = 0;","uint64_t g_vsnap_nonvertex = 0, g_vsnap_known = 0, g_vsnap_unknown = 0, g_vsnap_nohash = 0;")
rep("(streams through a NON-vertex fetch constant {})\", g_resolves_from_regs, g_rt_grown, g_bridge_vsnap_taken, g_st.vsnap_applied, g_vsnap_nonvertex);",
    "(streams through a NON-vertex fetch constant {}; small draws whose VS slots were known {}, unknown {}, no microcode hash {}; VS entries {})\", g_resolves_from_regs, g_rt_grown, g_bridge_vsnap_taken, g_st.vsnap_applied, g_vsnap_nonvertex, g_vsnap_known, g_vsnap_unknown, g_vsnap_nohash, g_vs_fetch_by_hash.size());")
open(p,'w',encoding='utf-8',newline='').write(s); print('ok')
