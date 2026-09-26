P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)

# 1. owner entries can be COLOUR owners too.
rep("""  NativeRT* rt = nullptr; NativeDepth* nd = nullptr;
  uint32_t base = 0, pitch_tiles = 0, msaa = 0, fmt = 0;   // fmt: RB_DEPTH_INFO bit 16 (0 D24S8, 1 D24FS8)
};""", """  NativeRT* rt = nullptr; NativeDepth* nd = nullptr;
  uint32_t base = 0, pitch_tiles = 0, msaa = 0, fmt = 0;   // fmt: RB_DEPTH_INFO bit 16 (0 D24S8, 1 D24FS8)
  bool color = false;   // rule 3: a COLOUR target owns the tile (nd == nullptr)
};""")

# 2. the plugin's rule (render_target/cache.cpp RenderTargetCache::Update + ChangeOwnership).
rep("""inline uint32_t TilesFor(uint32_t w, uint32_t h) {""",
"""// RULE 3 = THE PLUGIN'S OWN OWNERSHIP RULE (render_target/cache.cpp Update, 2026-09-26): every draw hands the EDRAM
// range of each target it USES to that target - depth when z or stencil is enabled, colour when its write mask is
// non-zero (edram mode colour+depth) - over the rows the draw can reach (the plugin estimates the max Y; the scissor
// bottom here), clamped so a target ends where the next bound target's base begins (with wrapping). A depth resolve
// then reads each tile from whichever target owns it: another DEPTH owner is copied row by row, a COLOUR owner or
// none keeps the resolve's own contents (the plugin would reinterpret colour bytes as depth - not modelled).
uint64_t g_owner3_claims = 0, g_owner3_color_claims = 0;
void ClaimTiles3(NativeRT* rt, NativeDepth* nd, bool color, uint32_t base, uint32_t fmt, uint32_t len_tiles) {
  const uint32_t msaa = (rt->surf >> 16) & 3u;
  const uint32_t pitch_tiles = ((rt->w << (msaa >= 2 ? 1 : 0)) + 79) / 80;
  const uint32_t n = std::min<uint32_t>(len_tiles, 2048u);
  if (!n) return;
  auto same = [&](const DepthOwner& o) { return o.rt == rt && o.nd == nd && o.color == color && o.base == base && o.fmt == fmt; };
  if (same(g_edram_depth_owner[base & 2047u]) && same(g_edram_depth_owner[(base + n - 1) & 2047u])) return;
  for (uint32_t i = 0; i < n; ++i) {
    DepthOwner& o = g_edram_depth_owner[(base + i) & 2047u];
    o.rt = rt; o.nd = nd; o.color = color; o.base = base; o.pitch_tiles = pitch_tiles; o.msaa = msaa; o.fmt = fmt;
  }
  ++(color ? g_owner3_color_claims : g_owner3_claims);
}
void ClaimDrawTiles3(NativeRT* rt, const uint32_t* regs, uint32_t sc_bottom) {
  const uint32_t mode = regs[0x2208] & 7u;          // RB_MODECONTROL edram mode: 4 colour+depth, 5 depth only
  if (mode != 4u && mode != 5u) return;
  const uint32_t dcr = regs[0x2200];
  const bool depth_used = (dcr & 3u) != 0 && rt->cur_dep;                 // stencil_enable | z_enable
  const bool color_used = mode == 4u && (regs[0x2104] & 0xFu) != 0;        // RB_COLOR_MASK, target 0
  if (!depth_used && !color_used) return;
  const uint32_t msaa = (rt->surf >> 16) & 3u;
  const uint32_t pitch_tiles = ((rt->w << (msaa >= 2 ? 1 : 0)) + 79) / 80;
  const uint32_t rows_px = std::min(rt->h, sc_bottom ? sc_bottom : rt->h);
  const uint32_t len32 = ((rows_px << (msaa >= 1 ? 1 : 0)) + 15) / 16 * pitch_tiles;
  uint32_t dkey = UINT32_MAX;
  if (depth_used) for (auto& kv : rt->deps) if (&kv.second == rt->cur_dep) { dkey = kv.first; break; }
  const uint32_t cinfo = regs[0x2001];
  const uint32_t cbase = cinfo & 0xFFFu, cfmt = (cinfo >> 16) & 0xFu;
  const bool c64 = cfmt == 5u || cfmt == 7u || cfmt == 15u;
  const bool d_on = depth_used && dkey != UINT32_MAX;
  const uint32_t dbase = d_on ? (dkey & 0x7FFu) : 0u;
  auto dist = [](uint32_t from, uint32_t to) { return to > from ? to - from : 2048u + to - from; };
  if (d_on) {
    uint32_t len = len32;
    if (color_used && cbase != dbase) len = std::min(len, dist(dbase, cbase));
    ClaimTiles3(rt, rt->cur_dep, false, dbase, (dkey >> 11) & 1u, len);
  }
  if (color_used) {
    uint32_t len = len32 << (c64 ? 1 : 0);
    if (d_on && cbase != dbase) len = std::min(len, dist(cbase, dbase));
    ClaimTiles3(rt, nullptr, true, cbase, cfmt, len);
  }
}
inline uint32_t TilesFor(uint32_t w, uint32_t h) {""")

# 3. SDK draws apply it.
rep("""  if (const int rule = REXCVAR_GET(ngpu_depth_owner_rule); rule && g_cur_rt && g_cur_rt->cur_dep) {
    const uint32_t dc = regs[0x2200];
    if ((rule == 1 && (dc & 6u) == 6u) || (rule == 2 && (dc & 2u))) ClaimDepthTiles(g_cur_rt);
  }""", """  if (const int rule = REXCVAR_GET(ngpu_depth_owner_rule); rule == 3 && g_cur_rt) ClaimDrawTiles3(g_cur_rt, regs, dc.sc_y + dc.sc_h);
  else if (rule && g_cur_rt && g_cur_rt->cur_dep) {
    const uint32_t dcr = regs[0x2200];
    if ((rule == 1 && (dcr & 6u) == 6u) || (rule == 2 && (dcr & 2u))) ClaimDepthTiles(g_cur_rt);
  }""")

# 4. the resolve: rule 3 copies each tile row from ITS OWN depth owner.
rep("""      NativeRT* best = nullptr; uint32_t best_v = 0; int32_t best_key = -1;
      if (REXCVAR_GET(ngpu_depth_owner_rule)) {""", """      NativeRT* best = nullptr; uint32_t best_v = 0; int32_t best_key = -1;
      if (REXCVAR_GET(ngpu_depth_owner_rule) == 3) {
        uint32_t rows_copied = 0, rows_color = 0, rows_none = 0, rows_self = 0;
        std::vector<RenderTexture*> touched;
        for (uint32_t T = 0; T < dtr; ++T) {
          const uint32_t tile = (dbase + T * dpt) % kEdramTiles;
          const DepthOwner& o = g_edram_depth_owner[tile];
          if (!o.rt) { ++rows_none; continue; }
          if (o.color) { ++rows_color; continue; }
          if (o.rt == srt && o.nd == srt->cur_dep) { ++rows_self; continue; }
          if (!o.nd || !o.nd->tex || o.pitch_tiles != dpt) { ++rows_none; continue; }
          RenderTexture* s2 = o.nd->tex.get();
          if (std::find(touched.begin(), touched.end(), s2) == touched.end()) {
            touched.push_back(s2);
            g_s.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(s2, RenderTextureLayout::COPY_SOURCE));
          }
          const uint32_t srows = o.msaa ? 8u : 16u;
          const uint32_t rel = (tile + kEdramTiles - o.base) % kEdramTiles, sT = rel / dpt;
          for (uint32_t y = 0; y < 16u; ++y) {
            if (drows != 16u && (y & 1u)) continue;
            const uint32_t dy = T * drows + (drows == 16u ? y : y / 2u), sy = sT * srows + (srows == 16u ? y : y / 2u);
            if (dy >= srt->h || sy >= o.rt->h || dy >= h) continue;
            const RenderBox rb(0, int32_t(sy), int32_t(std::min(srt->w, o.rt->w)), int32_t(sy + 1), 0, 1);
            g_s.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.tex.get()), RenderTextureCopyLocation::Subresource(s2), 0, dy, 0, &rb);
            ++rows_copied;
          }
        }
        for (RenderTexture* s2 : touched) g_s.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(s2, RenderTextureLayout::DEPTH_WRITE));
        if (rows_copied) ++g_depth_tilemapped;
        static uint32_t olog = 0;
        if (olog < 12 && g_s.frames > 300) { ++olog; REXLOG_INFO("[ngpu] DEPTH OWNERS (rule 3) {:08X} frame {} seq {}: {} tile rows - {} copied from another depth owner ({} pixel rows), {} colour-owned, {} self, {} no owner (claims so far: depth {}, colour {})", base, g_s.frames, g_replay_seq, dtr, dtr - rows_color - rows_self - rows_none, rows_copied, rows_color, rows_self, rows_none, g_owner3_claims, g_owner3_color_claims); }
      } else if (REXCVAR_GET(ngpu_depth_owner_rule)) {""")
rep("\"Native-GPU diagnostic: SDK draws claim EDRAM depth tiles - 0 off, 1 when they WRITE depth, 2 when they USE it (z test); with ngpu_depth_tilemap the recorded owner is the copy source\"",
    "\"Native-GPU diagnostic: SDK draws claim EDRAM depth tiles - 0 off, 1 when they WRITE depth, 2 when they USE it (z test), 3 the plugin's rule (depth and COLOUR targets claim the rows a draw reaches; a depth resolve copies each tile row from its own depth owner); with ngpu_depth_tilemap the recorded owner is the copy source\"")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
