P = r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit(f"anchor count {s.count(a2)}: {a[:80]!r}")
    s = s.replace(a2, b2, 1)

# 1. SDK draws claim depth tiles too (rule selectable: 1 = z write, 2 = z test use).
rep("""  if (q_on) QueryEnd(d);
  ProbeAfterDraw(d, g_cur_rt, uint32_t(vh >> 32), ps_hi);""",
"""  if (q_on) QueryEnd(d);
  ProbeAfterDraw(d, g_cur_rt, uint32_t(vh >> 32), ps_hi);
  // EDRAM DEPTH OWNERSHIP from SDK draws too (only the old path claimed: 0 claims all night). Rule 1 = the draw WRITES
  // depth (the console's truth), rule 2 = the draw USES depth (z test or write - the plugin's render-target cache hands
  // tiles to the depth target a pass binds, written or not).
  if (const int rule = REXCVAR_GET(ngpu_depth_owner_rule); rule && g_cur_rt && g_cur_rt->cur_dep) {
    const uint32_t dc = regs[0x2200];
    if ((rule == 1 && (dc & 6u) == 6u) || (rule == 2 && (dc & 2u))) ClaimDepthTiles(g_cur_rt);
  }""")

# 2. the tile map copies each destination tile row from its recorded OWNER when the owner table is in use.
rep("""      NativeRT* best = nullptr; uint32_t best_v = 0; int32_t best_key = -1;
      for (auto& kv : g_rts) {""",
"""      NativeRT* best = nullptr; uint32_t best_v = 0; int32_t best_key = -1;
      if (REXCVAR_GET(ngpu_depth_owner_rule)) {
        // OWNER MODE: the recorded owner of the destination's FIRST covered tile row decides (one source per resolve,
        // same pitch in tiles only); an owner that is the resolve's own source, or none, leaves the copy as it is.
        for (uint32_t T = 0; T < dtr && !best; ++T) {
          const DepthOwner& o = g_edram_depth_owner[(dbase + T * dpt) & 2047u];
          if (o.rt && o.nd && o.rt != srt && o.pitch_tiles == dpt && o.rt->cur_dep == o.nd) { best = o.rt; best_key = int32_t(o.base | (o.fmt << 11)); best_v = 1; }
        }
        if (!best) ++g_depth_owner_none;
      } else
      for (auto& kv : g_rts) {""")
rep("uint64_t g_depth_borrowed = 0, g_depth_tilemapped = 0;", "uint64_t g_depth_borrowed = 0, g_depth_tilemapped = 0, g_depth_owner_none = 0;")
rep("REXCVAR_DEFINE_BOOL(ngpu_depth_tilemap,",
    "REXCVAR_DEFINE_INT32(ngpu_depth_owner_rule, 0, \"GPU\", \"Native-GPU diagnostic: SDK draws claim EDRAM depth tiles - 0 off, 1 when they WRITE depth, 2 when they USE it (z test); with ngpu_depth_tilemap the recorded owner is the copy source\");\nREXCVAR_DEFINE_BOOL(ngpu_depth_tilemap,")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
