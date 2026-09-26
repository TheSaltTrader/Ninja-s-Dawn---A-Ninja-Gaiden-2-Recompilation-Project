// VENDORED from rexglue-src 23ace0b:include/rex/ui/d3d12/d3d12_upload_buffer_pool.h - systematic renames only (see vendor_rtc_d3d12.py / ORIGIN.txt):
// namespaces d3d12 -> ngpu_d3d12, plugin headers -> rtc_d3d12/facade.h, cvars -> plugin registry reads (0 bool, 0 string, 0 int).
#include <string>
#include <cstdint>
#include <rex/logging.h>
namespace ng2::ngpu::xlat { bool PluginBool(const char*, bool); std::string PluginString(const char*, const char*); int32_t PluginInt(const char*, int32_t); double PluginDouble(const char*, double); }
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include "rtc_d3d12/facade.h"
#include <rex/ui/graphics_upload_buffer_pool.h>

namespace rex::ui::ngpu_d3d12 {

class D3D12UploadBufferPool : public GraphicsUploadBufferPool {
 public:
  D3D12UploadBufferPool(const D3D12Provider& provider, size_t page_size = kDefaultPageSize);

  uint8_t* Request(uint64_t submission_index, size_t size, size_t alignment,
                   ID3D12Resource** buffer_out, size_t* offset_out,
                   D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out);
  uint8_t* RequestPartial(uint64_t submission_index, size_t size, size_t alignment,
                          ID3D12Resource** buffer_out, size_t* offset_out, size_t* size_out,
                          D3D12_GPU_VIRTUAL_ADDRESS* gpu_address_out);

 protected:
  Page* CreatePageImplementation() override;

 private:
  struct D3D12Page : public Page {
    // Creates a reference to the buffer. It must not be unmapped until this
    // D3D12Page is deleted.
    D3D12Page(ID3D12Resource* buffer, void* mapping);
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    void* mapping_;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address_;
  };

  const D3D12Provider& provider_;
};

}  // namespace rex::ui::ngpu_d3d12
