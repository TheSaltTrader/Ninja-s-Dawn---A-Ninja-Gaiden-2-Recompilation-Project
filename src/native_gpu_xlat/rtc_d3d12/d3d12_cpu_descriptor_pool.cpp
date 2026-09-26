// VENDORED from rexglue-src 23ace0b:src/ui/d3d12/d3d12_cpu_descriptor_pool.cpp - systematic renames only (see vendor_rtc_d3d12.py / ORIGIN.txt):
// namespaces d3d12 -> ngpu_d3d12, plugin headers -> rtc_d3d12/facade.h, cvars -> plugin registry reads (0 bool, 0 string, 0 int).
#include <string>
#include <cstdint>
#include <rex/logging.h>
namespace ng2::ngpu::xlat { bool PluginBool(const char*, bool); std::string PluginString(const char*, const char*); int32_t PluginInt(const char*, int32_t); double PluginDouble(const char*, double); }
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>
#include <memory>

#include <rex/logging.h>
#include "rtc_d3d12/d3d12_cpu_descriptor_pool.h"

namespace rex::ui::ngpu_d3d12 {

D3D12CpuDescriptorPool::Descriptor D3D12CpuDescriptorPool::AllocateDescriptor() {
  if (!freed_indices_.empty()) {
    size_t index = freed_indices_.back();
    freed_indices_.pop_back();
    return Descriptor(shared_from_this(), index);
  }
  uint32_t heap_size = uint32_t(1) << heap_size_log2_;
  if (!heaps_.empty() && last_heap_allocated_ < heap_size) {
    return Descriptor(shared_from_this(), (heaps_.size() - 1) * heap_size + last_heap_allocated_++);
  }
  D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc;
  descriptor_heap_desc.Type = type_;
  descriptor_heap_desc.NumDescriptors = heap_size;
  descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  descriptor_heap_desc.NodeMask = 0;
  ID3D12DescriptorHeap* descriptor_heap;
  if (FAILED(provider_.GetDevice()->CreateDescriptorHeap(&descriptor_heap_desc,
                                                         IID_PPV_ARGS(&descriptor_heap)))) {
    REXLOG_ERROR(
        "Failed to create a non-shader-visible descriptor heap for {} "
        "descriptors",
        heap_size);
    return Descriptor();
  }
  heaps_.push_back(descriptor_heap);
  last_heap_allocated_ = 1;
  return Descriptor(shared_from_this(), (heaps_.size() - 1) * heap_size);
}

}  // namespace rex::ui::ngpu_d3d12
