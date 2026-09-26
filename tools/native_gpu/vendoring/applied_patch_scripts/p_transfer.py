P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)

# 1. ClaimTiles3 performs the plugin's OWNERSHIP TRANSFER for depth: rows whose previous owner is another depth
#    target are copied into the new owner before the draw (cache.cpp ChangeOwnership + PerformTransfersAndResolveClears).
rep("""  auto same = [&](const DepthOwner& o) { return o.rt == rt && o.nd == nd && o.color == color && o.base == base && o.fmt == fmt; };
  if (same(g_edram_depth_owner[base & 2047u]) && same(g_edram_depth_owner[(base + n - 1) & 2047u])) return;
  for (uint32_t i = 0; i < n; ++i) {""", """  auto same = [&](const DepthOwner& o) { return o.rt == rt && o.nd == nd && o.color == color && o.base == base && o.fmt == fmt; };
  if (same(g_edram_depth_owner[base & 2047u]) && same(g_edram_depth_owner[(base + n - 1) & 2047u])) return;
  // THE TRANSFER (depth -> depth only): for each tile row of the new owner's range whose first tile another DEPTH
  // target owns (same pitch in tiles), that owner's samples are copied into this depth before the draw - the plugin's
  // render-target cache does this on every ownership change. Sample rows: 16 per tile; pixel rows per tile 8 at
  // 2x MSAA (natively drawn 1x, each pixel row = two sample rows) or 16 at 1x.
  if (!color && nd && nd->tex && g_s.cmd) {
    const uint32_t drows = msaa ? 8u : 16u;
    std::vector<RenderTexture*> touched;
    bool dest_open = false;
    for (uint32_t T = 0; T * pitch_tiles < n; ++T) {
      const uint32_t tile = (base + T * pitch_tiles) & 2047u;
      const DepthOwner& o = g_edram_depth_owner[tile];
      if (!o.rt || o.color || !o.nd || o.nd == nd || !o.nd->tex || o.pitch_tiles != pitch_tiles) continue;
      RenderTexture* s2 = o.nd->tex.get();
      if (!dest_open) { dest_open = true; g_s.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(nd->tex.get(), RenderTextureLayout::COPY_DEST)); }
      if (std::find(touched.begin(), touched.end(), s2) == touched.end()) { touched.push_back(s2); g_s.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(s2, RenderTextureLayout::COPY_SOURCE)); }
      const uint32_t srows = o.msaa ? 8u : 16u;
      const uint32_t rel = (tile + 2048u - o.base) & 2047u, sT = rel / pitch_tiles;
      for (uint32_t y = 0; y < 16u; ++y) {
        if (drows != 16u && (y & 1u)) continue;
        const uint32_t dy = T * drows + (drows == 16u ? y : y / 2u), sy = sT * srows + (srows == 16u ? y : y / 2u);
        if (dy >= rt->h || sy >= o.rt->h) continue;
        const RenderBox rb(0, int32_t(sy), int32_t(std::min(rt->w, o.rt->w)), int32_t(sy + 1), 0, 1);
        g_s.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(nd->tex.get()), RenderTextureCopyLocation::Subresource(s2), 0, dy, 0, &rb);
        ++g_owner3_rows_transferred;
      }
    }
    for (RenderTexture* s2 : touched) g_s.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(s2, RenderTextureLayout::DEPTH_WRITE));
    if (dest_open) { g_s.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(nd->tex.get(), RenderTextureLayout::DEPTH_WRITE)); ++g_owner3_transfers; }
  }
  for (uint32_t i = 0; i < n; ++i) {""")
rep("uint64_t g_owner3_claims = 0, g_owner3_color_claims = 0;", "uint64_t g_owner3_claims = 0, g_owner3_color_claims = 0, g_owner3_transfers = 0, g_owner3_rows_transferred = 0;")
# 2. claim BEFORE the draw (the transfer must land before the draw tests against it).
rep("""  if (const int rule = REXCVAR_GET(ngpu_depth_owner_rule); rule == 3 && g_cur_rt) ClaimDrawTiles3(g_cur_rt, regs, dc.sc_y + dc.sc_h);
  else if (rule""", """  if (const int rule = REXCVAR_GET(ngpu_depth_owner_rule); rule == 3) {}   // claimed before the draw (transfer)
  else if (rule""")
rep("""  const bool q_on = QueryBegin(d, qwhat);""", """  if (REXCVAR_GET(ngpu_depth_owner_rule) == 3 && g_cur_rt) ClaimDrawTiles3(g_cur_rt, regs, dc.sc_y + dc.sc_h);   // + ownership transfer
  const bool q_on = QueryBegin(d, qwhat);""")
rep("""(claims so far: depth {}, colour {})", base, g_s.frames, g_replay_seq, dtr, dtr - rows_color - rows_self - rows_none, rows_copied, rows_color, rows_self, rows_none, g_owner3_claims, g_owner3_color_claims); }""",
    """(claims so far: depth {}, colour {}; transfers {}, {} pixel rows)", base, g_s.frames, g_replay_seq, dtr, dtr - rows_color - rows_self - rows_none, rows_copied, rows_color, rows_self, rows_none, g_owner3_claims, g_owner3_color_claims, g_owner3_transfers, g_owner3_rows_transferred); }""")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
