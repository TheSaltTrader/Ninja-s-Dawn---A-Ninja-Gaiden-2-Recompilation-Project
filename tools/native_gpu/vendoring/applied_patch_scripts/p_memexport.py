import re
P = r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src"
def patch(path, pairs):
    s = open(path, encoding='utf-8', newline='').read()
    nl = '\r\n' if '\r\n' in s else '\n'
    for a, b in pairs:
        a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
        if s.count(a2) != 1:
            raise SystemExit(f"{path}: anchor count {s.count(a2)}: {a[:80]!r}")
        s = s.replace(a2, b2, 1)
    open(path, 'w', encoding='utf-8', newline='').write(s)

# 1. xlat: expose the memexport stream constants of each stage.
patch(P + r"\native_gpu_sdk_xlat.h", [(
"  bool memexport = false;",
"  bool memexport = false;\n  std::vector<uint32_t> memexport_consts;   // float constants holding the eA stream descriptors")])
patch(P + r"\native_gpu_sdk_xlat.cpp", [(
"  s.memexport = sh.memexport_eM_written() != 0;",
"  s.memexport = sh.memexport_eM_written() != 0;\n  s.memexport_consts.assign(sh.memexport_stream_constants().begin(), sh.memexport_stream_constants().end());")])

F = P + r"\native_gpu_present.cpp"
patch(F, [
# 2. globals + cvar
("constexpr uint64_t kMirrorScratch = 32ull << 20;",
"""constexpr uint64_t kMirrorScratch = 32ull << 20;
// GPU SHARED MEMORY (ngpu_sdk_shm_gpu, 2026-09-26): memexport writes guest memory from a shader - 45,138 draws a leg
// (the point-list passes of VS 0D7251C2, PS 60E8DEF9) were refused because the mirror is an UPLOAD heap the GPU can only
// read. With the switch on, a DEFAULT-heap copy (UAV-capable) is what the shaders read and write: every mirror store is
// queued and copied into it before the next SDK draw, and pages a memexport wrote are GPU-owned (not re-uploaded from
// the stale CPU copy, not overlaid by the vertex snapshot) until the CPU writes them again.
ID3D12Resource* g_shm_gpu = nullptr;
D3D12_RESOURCE_STATES g_shm_state = D3D12_RESOURCE_STATE_COPY_DEST;
std::vector<std::pair<uint64_t, uint64_t>> g_shm_pending;
std::vector<uint8_t> g_gpu_written;   // per page: last written by a memexport
bool g_shm_gpu_on = false;
uint64_t g_memexport_drawn = 0, g_memexport_ranges = 0, g_memexport_noranges = 0, g_shm_copies = 0, g_shm_copy_bytes = 0, g_vsnap_gpu_skipped = 0;
inline void ShmPending(uint64_t off, uint64_t n) { if (g_shm_gpu_on && n) g_shm_pending.push_back({off, n}); }"""),
# 3. create the default buffer with the mirror
("  g_mirror.up_tick.assign(kPhysPages, 0);",
"""  g_mirror.up_tick.assign(kPhysPages, 0);
  if (g_mirror.map.valid() && REXCVAR_GET(ngpu_sdk_shm_gpu)) {
    ID3D12Device* dev = static_cast<D3D12Device*>(g_s.device.get())->d3d;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = (uint64_t(kPhysPages) << 12) + kMirrorScratch; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_shm_gpu)))) {
      g_shm_gpu_on = true; g_shm_state = D3D12_RESOURCE_STATE_COPY_DEST; g_gpu_written.assign(kPhysPages, 0);
    }
    REXLOG_INFO("[ngpu] SDK path: GPU shared memory (default heap, UAV) {}", g_shm_gpu_on ? "created - memexport draws enabled" : "NOT created");
  }"""),
# 4. queue the mirror stores
("    g_mirror.map.store(size_t(a), PBase() + a, size_t(n));",
"    g_mirror.map.store(size_t(a), PBase() + a, size_t(n));\n    ShmPending(a, n);"),
("      g_mirror.map.store(size_t(at), g_replay_vsnap_data->data() + v.data_off, size_t(v.bytes));",
"      g_mirror.map.store(size_t(at), g_replay_vsnap_data->data() + v.data_off, size_t(v.bytes));\n      ShmPending(at, v.bytes);"),
# 5. SRV/UAV on the GPU copy
("  dev->CreateShaderResourceView(static_cast<D3D12Buffer*>(g_mirror.buf.get())->d3d, &s, pd->viewHeapAllocator->getCPUHandleAt(g_h.shm));",
"  dev->CreateShaderResourceView(g_shm_gpu_on ? g_shm_gpu : static_cast<D3D12Buffer*>(g_mirror.buf.get())->d3d, &s, pd->viewHeapAllocator->getCPUHandleAt(g_h.shm));"),
("  u.Buffer.NumElements = 1;\n  u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;\n  dev->CreateUnorderedAccessView(nullptr, nullptr, &u, pd->viewHeapAllocator->getCPUHandleAt(g_h.shm + 1));",
"  u.Buffer.NumElements = g_shm_gpu_on ? UINT(((uint64_t(kPhysPages) << 12) + kMirrorScratch) >> 2) : 1;\n  u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;\n  dev->CreateUnorderedAccessView(g_shm_gpu_on ? g_shm_gpu : nullptr, nullptr, &u, pd->viewHeapAllocator->getCPUHandleAt(g_h.shm + 1));"),
# 6. memexport gate
("  if (pt->vs.memexport || pt->ps.memexport) { ++g_st.memexport; ++g_pair_census[pkey][\"memexport\"]; return false; }",
"""  const bool memexport = pt->vs.memexport || pt->ps.memexport;
  if (memexport && !g_shm_gpu_on) { ++g_st.memexport; ++g_pair_census[pkey]["memexport"]; return false; }
  if (memexport) {
    // draw_util AddMemExportRanges: each eA stream constant (float constants, VS bank at 0x4000 / PS bank at 0x4400)
    // names a base (dwords), an element count and a colour format; the surrounding memory is loaded first, then the
    // pages become GPU-owned.
    uint32_t nr = 0;
    for (int stage = 0; stage < 2; ++stage) {
      const auto& consts = stage ? pt->ps.memexport_consts : pt->vs.memexport_consts;
      const uint32_t bank = stage ? 0x4400u : 0x4000u;
      for (uint32_t c : consts) {
        if (c >= 256) continue;
        const uint32_t* w = &regs[bank + c * 4];
        const uint32_t base_dw = w[0] & 0x3FFFFFFFu, c1 = w[0] >> 30, fmt = (w[2] >> 8) & 0x3Fu, c4b0 = w[2] >> 20, count = w[3] & 0x7FFFFFu, c96 = w[3] >> 23;
        if (c1 != 1 || c4b0 != 0x4B0 || c96 != 0x96 || !count) continue;
        const auto* fi = rex::graphics::FormatInfo::Get(static_cast<rex::graphics::xenos::TextureFormat>(fmt));
        const uint32_t bpe = fi ? std::max<uint32_t>(fi->bits_per_pixel >> 3, 1u) : 4u;
        const uint32_t phys = (base_dw << 2) & 0x1FFFFFFFu, bytes = count * bpe;
        if (!SdkMirrorRequest(phys, bytes)) continue;
        const uint32_t tick = g_tick.load(std::memory_order_relaxed);
        for (uint64_t pg = phys >> 12; (pg << 12) < uint64_t(phys) + bytes && pg < kPhysPages; ++pg) { g_mirror.up_tick[size_t(pg)] = tick + 1; g_gpu_written[size_t(pg)] = 1; }
        ++nr;
      }
    }
    g_memexport_ranges += nr;
    if (!nr) ++g_memexport_noranges;
    ++g_memexport_drawn;
    ++g_pair_census[pkey]["memexport (GPU shared memory)"];
  }"""),
# 7. no vertex-snapshot redirect into GPU-written pages
("      if (addr >= r.phys && addr < r.phys + r.bytes) { d0 = (r.scratch + (addr - r.phys)) | (d0 & 3u); ++g_vsnap_redirected; break; }",
"""      if (addr >= r.phys && addr < r.phys + r.bytes) {
        if (g_shm_gpu_on && (addr >> 12) < g_gpu_written.size() && g_gpu_written[addr >> 12]) { ++g_vsnap_gpu_skipped; break; }   // the GPU's copy is the truth
        d0 = (r.scratch + (addr - r.phys)) | (d0 & 3u); ++g_vsnap_redirected; break;
      }"""),
# 8. flush + state before recording
("  cl->checkDescriptorHeaps();\n  d->SetGraphicsRootSignature(rs);",
"""  cl->checkDescriptorHeaps();
  if (g_shm_gpu_on) {
    auto trans = [&](D3D12_RESOURCE_STATES to) {
      if (g_shm_state == to) return;
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = g_shm_gpu; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = g_shm_state; b.Transition.StateAfter = to;
      d->ResourceBarrier(1, &b); g_shm_state = to;
    };
    if (!g_shm_pending.empty()) {
      trans(D3D12_RESOURCE_STATE_COPY_DEST);
      std::sort(g_shm_pending.begin(), g_shm_pending.end());
      ID3D12Resource* up = static_cast<D3D12Buffer*>(g_mirror.buf.get())->d3d;
      uint64_t o = g_shm_pending[0].first, e = o + g_shm_pending[0].second;
      auto emit = [&]() { d->CopyBufferRegion(g_shm_gpu, o, up, o, e - o); ++g_shm_copies; g_shm_copy_bytes += e - o; };
      for (size_t k = 1; k < g_shm_pending.size(); ++k) {
        const auto& [po, pn] = g_shm_pending[k];
        if (po <= e) { e = std::max(e, po + pn); continue; }
        emit(); o = po; e = po + pn;
      }
      emit();
      g_shm_pending.clear();
    }
    trans(memexport ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                    : D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER));
  }
  d->SetGraphicsRootSignature(rs);"""),
# 9. index buffer from the GPU copy
("    D3D12_INDEX_BUFFER_VIEW ibv = {ib_va_snapshot ? ib_va_snapshot : static_cast<D3D12Buffer*>(g_mirror.buf.get())->d3d->GetGPUVirtualAddress() + ib_phys, ib_bytes,",
"    D3D12_INDEX_BUFFER_VIEW ibv = {ib_va_snapshot ? ib_va_snapshot : (g_shm_gpu_on ? g_shm_gpu : static_cast<D3D12Buffer*>(g_mirror.buf.get())->d3d)->GetGPUVirtualAddress() + ib_phys, ib_bytes,"),
# 10. UAV barrier after a memexport draw
("  if (q_on) QueryEnd(d);\n  // Hand the list back to plume",
"""  if (q_on) QueryEnd(d);
  if (memexport && g_shm_gpu_on) {
    D3D12_RESOURCE_BARRIER ub = {}; ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = g_shm_gpu;
    d->ResourceBarrier(1, &ub);
  }
  // Hand the list back to plume"""),
# 11. report
("REXLOG_INFO(\"[ngpu] SDK PATH IDENTITY: pair names confirmed by microcode {}, STALE {}, microcode not registered {}\", g_st.name_ok, g_st.name_stale, g_st.name_unknown);",
"""REXLOG_INFO("[ngpu] SDK PATH IDENTITY: pair names confirmed by microcode {}, STALE {}, microcode not registered {}", g_st.name_ok, g_st.name_stale, g_st.name_unknown);
  if (g_shm_gpu_on)
    REXLOG_INFO("[ngpu] SDK PATH GPU SHARED MEMORY: memexport draws {} (export ranges {}, draws with no valid range {}); copies {} ({} MB); vertex snapshots NOT applied over GPU-written pages {}",
                g_memexport_drawn, g_memexport_ranges, g_memexport_noranges, g_shm_copies, g_shm_copy_bytes >> 20, g_vsnap_gpu_skipped);"""),
# 12. cvar
("REXCVAR_DEFINE_INT32(ngpu_tex_nosettle_small_bytes,",
"REXCVAR_DEFINE_BOOL(ngpu_sdk_shm_gpu, false, \"GPU\", \"Native-GPU (SDK path, read at startup): shaders read guest memory from a DEFAULT-heap copy fed from the upload mirror by copies, and MEMEXPORT draws write it through a UAV (else they are refused, as before)\");\nREXCVAR_DEFINE_INT32(ngpu_tex_nosettle_small_bytes,"),
])
print("ok")
