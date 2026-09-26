// The plugin's generated geometry shaders (vendored: pipeline_gs_vendored.cpp).
// Taken from Fable II's src/native_gpu_sdk_gs.h (wt-fable2-nativegpu) with the
// namespace renamed, like the rest of this tree - see ORIGIN.txt.
#pragma once

#include <bit>
#include <cstdint>
#include <vector>

namespace ng2::ngpu::sdk {

enum class PipelineGeometryShader : uint32_t { kNone, kPointList, kRectangleList, kQuadList };

union GeometryShaderKey {
  uint32_t key;
  struct {
    PipelineGeometryShader type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
    uint32_t point_ps_ucp_mode : 2;
  };
  GeometryShaderKey() : key(0) {}
};

bool GetGeometryShaderKey(PipelineGeometryShader type, uint64_t vs_modification, uint64_t ps_modification, GeometryShaderKey& key_out);
void CreateDxbcGeometryShader(GeometryShaderKey key, std::vector<uint32_t>& shader_out);

}  // namespace ng2::ngpu::sdk
