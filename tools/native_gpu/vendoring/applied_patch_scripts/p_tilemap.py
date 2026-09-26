P = r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit(f"anchor count {s.count(a2)}: {a[:80]!r}")
    s = s.replace(a2, b2, 1)

rep("""  g_s.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.tex.get()), RenderTextureCopyLocation::Subresource(src), 0, 0, 0, &box);
  g_s.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(r.tex.get(), RenderTextureLayout::SHADER_READ));""",
"""  g_s.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.tex.get()), RenderTextureCopyLocation::Subresource(src), 0, 0, 0, &box);
  // EDRAM DEPTH TILE MAP (ngpu_depth_tilemap, diagnostic, 2026-09-26 town): a depth resolve whose own source had no
  // depth-tested draw reads, on the console, whatever depth OTHER surfaces left in its EDRAM tiles. Where another
  // native depth (drawn this frame) covers those tiles with the same pitch in tiles, reproduce the console's
  // reinterpretation row by row: destination tile row T (1x: 16 pixel rows per tile) holds source tile row
  // T + (dst_base - src_base) / pitch, whose samples are 8 pixel rows at 2x MSAA (each read as two 1x rows) or 16 at 1x.
  if (depth && REXCVAR_GET(ngpu_depth_tilemap)) {
    auto key_of = [](NativeRT* x) -> int32_t { if (!x->cur_dep) return -1; for (const auto& kv : x->deps) if (&kv.second == x->cur_dep) return int32_t(kv.first); return -1; };
    const uint32_t own = srt->cur_dep ? srt->cur_dep->greater + srt->cur_dep->less : srt->greater + srt->less;
    const int32_t dkey = key_of(srt);
    if (!own && dkey >= 0) {
      const uint32_t dmsaa = (srt->surf >> 16) & 3u, dpt = std::max(1u, ((srt->surf & 0x3FFFu) * (dmsaa == 2u ? 2u : 1u) + 79u) / 80u);
      const uint32_t drows = dmsaa ? 8u : 16u;   // pixel rows per tile row of the DESTINATION surface (natively drawn 1x)
      const uint32_t dbase = uint32_t(dkey) & 0x7FFu, dtr = (srt->h + drows - 1) / drows;
      NativeRT* best = nullptr; uint32_t best_v = 0; int32_t best_key = -1;
      for (auto& kv : g_rts) {
        NativeRT* r2 = &kv.second;
        if (r2 == srt || r2->last_frame != g_s.frames || !DepTex(r2)) continue;
        const uint32_t v = r2->cur_dep ? r2->cur_dep->greater + r2->cur_dep->less : r2->greater + r2->less;
        const uint32_t m2 = (r2->surf >> 16) & 3u, pt2 = std::max(1u, ((r2->surf & 0x3FFFu) * (m2 == 2u ? 2u : 1u) + 79u) / 80u);
        if (!v || pt2 != dpt) continue;
        if (v > best_v) { best_v = v; best = r2; best_key = key_of(r2); }
      }
      if (best && best_key >= 0) {
        const uint32_t smsaa = (best->surf >> 16) & 3u, srows = smsaa ? 8u : 16u, sbase = uint32_t(best_key) & 0x7FFu;
        const uint32_t sext = EdramExtent(best->surf, best->h), str = sext / dpt;
        RenderTexture* s2 = DepTex(best);
        g_s.cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(s2, RenderTextureLayout::COPY_SOURCE));
        uint32_t rows_copied = 0;
        for (uint32_t T = 0; T < dtr; ++T) {
          const uint32_t tile = (dbase + T * dpt) % kEdramTiles;
          const uint32_t rel = (tile + kEdramTiles - sbase) % kEdramTiles;   // tiles past the source base (wrapping)
          if (rel >= sext) continue;
          const uint32_t sT = rel / dpt;
          if (sT >= str) continue;
          for (uint32_t y = 0; y < 16u; ++y) {
            const uint32_t dy = T * drows + (drows == 16u ? y : y / 2u), sy = sT * srows + (srows == 16u ? y : y / 2u);
            if (drows != 16u && (y & 1u)) continue;   // an 8-row destination tile row takes every other sample row
            if (dy >= srt->h || sy >= best->h || dy >= h) continue;
            const RenderBox rb(0, int32_t(sy), int32_t(std::min(srt->w, best->w)), int32_t(sy + 1), 0, 1);
            g_s.cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(r.tex.get()), RenderTextureCopyLocation::Subresource(s2), 0, dy, 0, &rb);
            ++rows_copied;
          }
        }
        g_s.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(s2, RenderTextureLayout::DEPTH_WRITE));
        ++g_depth_tilemapped;
        static uint32_t tlog = 0;
        if (tlog < 6) { ++tlog; REXLOG_INFO("[ngpu] DEPTH TILE MAP {:08X}: {:08X} key {:X} (base {} pitch {} tiles, {} rows/tile) <- {:08X} key {:X} (base {} extent {} tiles, {} rows/tile): {} pixel rows copied", base, srt->surf, dkey, dbase, dpt, drows, best->surf, best_key, sbase, sext, srows, rows_copied); }
      }
    }
  }
  g_s.cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(r.tex.get(), RenderTextureLayout::SHADER_READ));""")
rep("uint64_t g_depth_borrowed = 0;", "uint64_t g_depth_borrowed = 0, g_depth_tilemapped = 0;")
rep("REXCVAR_DEFINE_BOOL(ngpu_depth_resolve_borrow,",
    "REXCVAR_DEFINE_BOOL(ngpu_depth_tilemap, false, \"GPU\", \"Native-GPU diagnostic: a depth resolve whose source had no depth-tested draw reproduces the console's EDRAM reinterpretation of another surface's depth tiles, row by row (same pitch in tiles only)\");\nREXCVAR_DEFINE_BOOL(ngpu_depth_resolve_borrow,")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
