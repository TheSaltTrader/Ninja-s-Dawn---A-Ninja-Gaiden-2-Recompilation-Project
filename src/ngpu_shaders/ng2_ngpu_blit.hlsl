// NG2 native window: the present blit. Samples the transplanted backend's
// gamma-applied guest output (R10G10B10A2) onto the swap chain, through a
// viewport the window sets per frame (fill / pillarbox / letterbox - the
// ultrawide presenter half that used to live in rexruntime's presenter).
//
// Built once into DXBC headers with the Windows Kit fxc (SM 5.1), the command
// recorded in ng2_ngpu_blit_build.txt beside this file; raw D3D12 accepts DXBC.
Texture2D<float4> g_source : register(t0);
SamplerState g_sampler : register(s0);

struct VsOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

// A full-screen triangle from the vertex id; no vertex buffer.
VsOut VsMain(uint id : SV_VertexID) {
  VsOut o;
  const float2 uv = float2((id << 1) & 2, id & 2);
  o.uv = uv;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}

float4 PsMain(VsOut i) : SV_Target {
  return float4(g_source.Sample(g_sampler, i.uv).rgb, 1.0);
}
