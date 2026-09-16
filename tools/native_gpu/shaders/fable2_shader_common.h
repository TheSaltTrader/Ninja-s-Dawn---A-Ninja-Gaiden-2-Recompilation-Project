#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#define g_Booleans                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272)

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c32.x); \
    uint g_SwappedTexcoords : packoffset(c32.y); \
    float2 g_HalfPixelOffset : packoffset(c32.z); \
    float g_AlphaThreshold : packoffset(c33.x); \
    uint g_SpecFlags : packoffset(c33.y); \
    uint4 g_BoolConstants[2] : packoffset(c34); \
    uint4 g_LoopConstants[8] : packoffset(c36); \
    uint4 g_StreamSlots[4] : packoffset(c44);

// Fable II: the spec constants come per draw from the shared constants
// (c33.y: SPEC_CONSTANT_ALPHA_TEST when RB_COLORCONTROL enables the alpha
// test) - a plain function keeps the vs_6_0 / ps_6_0 profiles.
#define g_SpecConstants() g_SpecFlags

#endif

// Fable II: the boolean constants (registers 0x4900..0x4907, 256 bits: b0..b127
// vertex, b128..b255 pixel) and the loop constants (0x4908..0x4927, one dword
// each: count in bits 0..7, start 8..15, step 16..23) come per draw from the
// shared constants at c34..c35 and c36..c43. The recompiler names an unnamed
// boolean by its absolute index (b132) and a loop by its id (i0.x = count);
// translate_all.sh's fix_hlsl.py rewrites those to NGPU_BOOL(n) / NGPU_LOOP(n)
// (plain bN defines would break the register(bN) bindings).
#define NGPU_BOOL(n) ((g_BoolConstants[(n) >> 7][((n) >> 5) & 3] >> ((n) & 31)) & 1u)
#define NGPU_LOOPW(n) (g_LoopConstants[(n) >> 2][(n) & 3])
#define NGPU_LOOP(n) int4(int(NGPU_LOOPW(n) & 0xFFu), int((NGPU_LOOPW(n) >> 8) & 0xFFu), int((NGPU_LOOPW(n) >> 16) & 0xFFu), 0)

// Fable II: vertex shaders fetch textures too (displacement, instancing
// data); implicit-LOD Sample is a pixel-stage opcode, so the vertex stage
// samples level 0.
#if __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX
#define XSAMPLE(s, c) SampleLevel(s, c, 0)
#define XSAMPLE3(s, c, o) SampleLevel(s, c, 0, o)
#else
#define XSAMPLE(s, c) Sample(s, c)
#endif
// Fable II: signed 2_10_10_10 vertex attributes arrive as the raw dword in a
// float input (R32_FLOAT keeps the bits); D3D12 has no R10G10B10A2_SNORM.
float4 unpack2_10_10_10_snorm(uint v)
{
    int3 i = int3(v << 22, v << 12, v << 2) >> 22;
    return float4(max(float3(i) / 511.0, -1.0), float((v >> 30) & 3) / 3.0);
}

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
// Fable II: the vertex streams as raw dword buffers (space 4, the runtime's
// descriptor set 4) for fetches whose index the shader computes; c44..c47
// carry the descriptor index per stream.
StructuredBuffer<uint> g_VertexStreamHeap[] : register(t0, space4);
#define NGPU_STREAM(s) g_VertexStreamHeap[g_StreamSlots[(s) >> 2][(s) & 3]]

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.XSAMPLE(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.XSAMPLE(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.XSAMPLE(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.XSAMPLE(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.XSAMPLE(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].XSAMPLE(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].XSAMPLE(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

float4 tfetchR11G11B10(uint4 value)
{
    // Fable II: the recompiler calls this only for 11_11_10 fetch formats, so the decode always applies.
    if (true)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

// A vertex fetch from a stream buffer at a dword address: the formats the
// runtime's input layouts also know (the cache stores the streams already
// byte-swapped per their fetch constant, so dwords read as little-endian).
// Reads past the view return 0 (D3D12 structured-buffer bounds).
float4 ngpu_vload(StructuredBuffer<uint> b, uint a, uint fmt, uint sgn, uint integer)
{
    uint w0 = b[a], w1 = b[a + 1], w2 = b[a + 2], w3 = b[a + 3];
    switch (fmt)
    {
    case 57: return float4(asfloat(w0), asfloat(w1), asfloat(w2), 1.0);
    case 38: return float4(asfloat(w0), asfloat(w1), asfloat(w2), asfloat(w3));
    case 37: return float4(asfloat(w0), asfloat(w1), 0.0, 1.0);
    case 36: return float4(asfloat(w0), 0.0, 0.0, 1.0);
    case 32: return float4(f16tof32(w0 >> 16), f16tof32(w0), f16tof32(w1 >> 16), f16tof32(w1));
    case 31: return float4(f16tof32(w0 >> 16), f16tof32(w0), 0.0, 1.0);
    case 6:
    {
        uint4 u = uint4(w0 & 0xFF, (w0 >> 8) & 0xFF, (w0 >> 16) & 0xFF, w0 >> 24);
        int4 i = (int4(u << 24)) >> 24;
        if (integer) return sgn ? float4(i) : float4(u);
        return sgn ? max(float4(i) / 127.0, -1.0) : float4(u) / 255.0;
    }
    case 26:
    {
        uint4 u = uint4(w0 >> 16, w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF);
        int4 i = (int4(u << 16)) >> 16;
        if (integer) return sgn ? float4(i) : float4(u);
        return sgn ? max(float4(i) / 32767.0, -1.0) : float4(u) / 65535.0;
    }
    case 25:
    {
        uint2 u = uint2(w0 >> 16, w0 & 0xFFFF);
        int2 i = (int2(u << 16)) >> 16;
        float2 v = integer ? (sgn ? float2(i) : float2(u)) : (sgn ? max(float2(i) / 32767.0, -1.0) : float2(u) / 65535.0);
        return float4(v, 0.0, 1.0);
    }
    case 7:
    {
        uint3 u = uint3(w0 & 0x3FF, (w0 >> 10) & 0x3FF, (w0 >> 20) & 0x3FF);
        uint w = w0 >> 30;
        if (sgn) return float4(max(float3((int3(u << 22)) >> 22) / 511.0, -1.0), max(float((int(w << 30)) >> 30), -1.0));
        return float4(float3(u) / 1023.0, float(w) / 3.0);
    }
    case 16:
    case 17:
        return tfetchR11G11B10(uint4(w0, 0, 0, 0));
    default:
        return float4(0.0, 0.0, 0.0, 1.0);
    }
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
