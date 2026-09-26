P = r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit(f"anchor count {s.count(a2)}: {a[:80]!r}")
    s = s.replace(a2, b2, 1)

rep("""Queries g_q;
bool QueryBegin(ID3D12GraphicsCommandList* d, const char* what) {""",
"""Queries g_q;

// PIXEL PROBE (ngpu_probe_x/_y/_rt, at ngpu_sdk_query_frame): after every SDK draw on the probed target, copy the probe
// pixel's colour and depth into a readback slot; after the frame, log each draw that CHANGED either - the native
// last-writer list for one pixel (the WEDGE-NEAR question: which draw writes the hill's depth, and does its colour twin
// ever touch the pixel?). A value-change detector: a draw writing the same value is invisible (stated in the log).
struct Probe {
  static constexpr uint32_t kN = 8192, kSlot = 512;
  ID3D12Resource* rb = nullptr;
  uint32_t used = 0, frame = 0;
  bool reported = false;
  struct Meta { uint32_t seq, vs, ps, surf; } meta[kN];
};
Probe g_probe;
void ProbeAfterDraw(ID3D12GraphicsCommandList* d, NativeRT* rt, uint32_t vs, uint32_t ps) {
  const int qf = REXCVAR_GET(ngpu_sdk_query_frame), px = REXCVAR_GET(ngpu_probe_x), py = REXCVAR_GET(ngpu_probe_y);
  if (qf <= 0 || g_s.frames != uint32_t(qf) || px < 0 || py < 0 || !rt || !rt->col) return;
  if (REXCVAR_GET(ngpu_probe_rt) && uint32_t(REXCVAR_GET(ngpu_probe_rt)) != rt->surf) return;
  if (uint32_t(px) >= rt->w || uint32_t(py) >= rt->h) return;
  ID3D12Device* dev = PlumeDev()->d3d;
  if (!g_probe.rb) {
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, uint64_t(Probe::kN) * Probe::kSlot, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_probe.rb));
    if (!g_probe.rb) return;
  }
  if (g_probe.frame != g_s.frames) { g_probe.frame = g_s.frames; g_probe.used = 0; }
  if (g_probe.used >= Probe::kN) return;
  const uint32_t i = g_probe.used++;
  g_probe.meta[i] = {g_replay_seq, vs, ps, rt->surf};
  ID3D12Resource* col = static_cast<D3D12Texture*>(rt->col.get())->d3d;
  RenderTexture* dt = DepTex(rt);
  ID3D12Resource* dep = dt ? static_cast<D3D12Texture*>(dt)->d3d : nullptr;
  D3D12_RESOURCE_BARRIER b[2] = {};
  auto tr = [](D3D12_RESOURCE_BARRIER& x, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES c) {
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a; x.Transition.StateAfter = c;
  };
  tr(b[0], col, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (dep) tr(b[1], dep, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  d->ResourceBarrier(dep ? 2 : 1, b);
  D3D12_BOX box = {UINT(px), UINT(py), 0, UINT(px) + 1, UINT(py) + 1, 1};
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = col; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
  dst.pResource = g_probe.rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Offset = uint64_t(i) * Probe::kSlot; dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 1, 256};
  d->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  if (dep) {
    src.pResource = dep;
    dst.PlacedFootprint.Offset = uint64_t(i) * Probe::kSlot + 256; dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_TYPELESS, 1, 1, 1, 256};
    d->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  }
  std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
  std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
  d->ResourceBarrier(dep ? 2 : 1, b);
}
void ProbeReport() {
  if (!g_probe.used || g_probe.reported || g_s.frames == g_probe.frame) return;
  g_probe.reported = true;
  void* p = nullptr;
  D3D12_RANGE r = {0, uint64_t(g_probe.used) * Probe::kSlot};
  if (FAILED(g_probe.rb->Map(0, &r, &p)) || !p) return;
  const uint8_t* b = static_cast<const uint8_t*>(p);
  auto h2f = [](uint16_t h) { const uint32_t e = (h >> 10) & 31, m = h & 1023; const float sgn = (h >> 15) ? -1.0f : 1.0f;
    if (e == 0) return sgn * std::ldexp(float(m), -24); if (e == 31) return sgn * 65504.0f; return sgn * std::ldexp(float(m | 1024), int(e) - 25); };
  float pc[4] = {-1, -1, -1, -1}, pd = -1.0f;
  uint32_t changes = 0;
  for (uint32_t i = 0; i < g_probe.used; ++i) {
    uint16_t hv[4]; std::memcpy(hv, b + uint64_t(i) * Probe::kSlot, 8);
    float c[4]; for (int k = 0; k < 4; ++k) c[k] = h2f(hv[k]);
    float dz; std::memcpy(&dz, b + uint64_t(i) * Probe::kSlot + 256, 4);
    const bool cc = std::memcmp(c, pc, sizeof(c)) != 0, dc = dz != pd;
    if (cc || dc) {
      ++changes;
      const auto& m = g_probe.meta[i];
      REXLOG_INFO("[ngpu] PROBE ({},{}) frame {} draw #{} seq {} real VS {:08X} PS {:08X} rt {:08X}:{}{} colour ({:.3f} {:.3f} {:.3f} {:.3f}) depth {:.6f}", REXCVAR_GET(ngpu_probe_x), REXCVAR_GET(ngpu_probe_y), g_probe.frame, i, m.seq,
                  m.vs, m.ps, m.surf, cc ? " COLOUR" : "", dc ? " DEPTH" : "", c[0], c[1], c[2], c[3], dz);
    }
    std::memcpy(pc, c, sizeof(c)); pd = dz;
  }
  REXLOG_INFO("[ngpu] PROBE ({},{}) frame {}: {} SDK draws on the probed target, {} changed the pixel (a draw writing the SAME value is not visible here)", REXCVAR_GET(ngpu_probe_x), REXCVAR_GET(ngpu_probe_y), g_probe.frame, g_probe.used, changes);
  D3D12_RANGE none = {0, 0};
  g_probe.rb->Unmap(0, &none);
}

bool QueryBegin(ID3D12GraphicsCommandList* d, const char* what) {""")

rep("""  if (q_on) QueryEnd(d);
  if (memexport && g_shm_gpu_on) {""",
"""  if (q_on) QueryEnd(d);
  ProbeAfterDraw(d, g_cur_rt, uint32_t(vh >> 32), ps_hi);
  if (memexport && g_shm_gpu_on) {""")

rep("""void SdkQueryReport() {
  using namespace sdkdraw;""",
"""void SdkQueryReport() {
  using namespace sdkdraw;
  ProbeReport();""")

rep("REXCVAR_DEFINE_BOOL(ngpu_sdk_shm_gpu,",
"""REXCVAR_DEFINE_INT32(ngpu_probe_x, -1, "GPU", "Native-GPU diagnostic: probe pixel x (see ngpu_probe_y/_rt; at ngpu_sdk_query_frame)");
REXCVAR_DEFINE_INT32(ngpu_probe_y, -1, "GPU", "Native-GPU diagnostic: probe pixel y");
REXCVAR_DEFINE_INT32(ngpu_probe_rt, 0, "GPU", "Native-GPU diagnostic: probe only draws on this RB_SURFACE_INFO (decimal); 0 = any target");
REXCVAR_DEFINE_BOOL(ngpu_sdk_shm_gpu,""")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
