// VENDORED from rexglue-src 23ace0b:src/graphics/shared_memory.cpp - systematic renames only (see vendor_rtc_d3d12.py / ORIGIN.txt):
// namespaces d3d12 -> ngpu_d3d12, plugin headers -> rtc_d3d12/facade.h, cvars -> plugin registry reads (0 bool, 0 string, 0 int).
#include <string>
#include <cstdint>
#include <rex/logging.h>
namespace ng2::ngpu::rtc { bool FastValidChecks(); }   // NATIVE PATCH: ngpu_backend_fast_valid (facade.cpp)

namespace ng2::ngpu::xlat { bool PluginBool(const char*, bool); std::string PluginString(const char*, const char*); int32_t PluginInt(const char*, int32_t); double PluginDouble(const char*, double); }
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstring>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/dbg.h>
#include "rtc_d3d12/shared_memory_base.h"
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::graphics {

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), uint64_t(0));
  std::fill(system_page_flags_valid_and_gpu_written_.begin(),
            system_page_flags_valid_and_gpu_written_.end(), uint64_t(0));
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire();

  // Pages that are valid only because the CPU uploaded them lose their valid
  // bit here, so the next frame re-reads them from guest memory.
  system_page_flags_valid_ = system_page_flags_valid_and_gpu_written_;
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

namespace {
uint64_t g_touched[SharedMemory::kBufferSize >> 12 >> 6];   // [hoist] one bit per 4 KB page
std::vector<uint32_t> g_touched_words;                      // words to clear at the next submission
}  // namespace
void SharedMemory::TouchRange(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) return;
  const uint32_t last = std::min<uint64_t>(uint64_t(start) + length, kBufferSize) - 1;
  for (uint32_t p = start >> 12; p <= (last >> 12); ++p) {
    uint64_t& w = g_touched[p >> 6];
    if (!w) g_touched_words.push_back(p >> 6);
    w |= uint64_t(1) << (p & 63);
  }
}
bool SharedMemory::AnyTouched(uint32_t start, uint32_t length) const {
  if (!length || start >= kBufferSize) return false;
  const uint32_t last = std::min<uint64_t>(uint64_t(start) + length, kBufferSize) - 1;
  for (uint32_t p = start >> 12; p <= (last >> 12); ++p)
    if (g_touched[p >> 6] & (uint64_t(1) << (p & 63))) return true;
  return false;
}
void SharedMemory::ResetTouched() {
  for (uint32_t w : g_touched_words) g_touched[w] = 0;
  g_touched_words.clear();
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  TouchRange(start, length);   // NATIVE PATCH: [hoist]
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

namespace {
// [ring] Where the GPU reads its commands from.
uint32_t g_ring_start = 0, g_ring_length = 0;
// [skip] Declared here because MakeRangeValid evicts, and it is the
// first user in the file.
std::atomic<uint64_t> g_page_upload_hashes_evicted{0};
std::atomic<uint64_t> g_gpu_written_pages_seen{0};
}  // namespace

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  {
    auto global_lock = global_critical_region_.Acquire();

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      system_page_flags_valid_[i] |= valid_bits;
      uint64_t& gpu_written = system_page_flags_valid_and_gpu_written_[i];
      gpu_written = written_by_gpu ? (gpu_written | valid_bits) : (gpu_written & ~valid_bits);
    }
  }

  if (written_by_gpu) {
    g_gpu_written_pages_seen.fetch_add(valid_page_last - valid_page_first + 1,
                                       std::memory_order_relaxed);
    // [skip] The buffer no longer holds what the CPU last uploaded for these
    // pages, so any recorded hash is now a lie. Evicting HERE, at the GPU
    // write, rather than checking something at the upload, means the skip
    // cannot see a stale entry whatever the valid bits are doing.
    EvictPageUploadHashes(valid_page_first, valid_page_last);
  }

  if (memory_invalidation_callback_handle_) {
    // A page that isn't writable here gets no watch. A later guest
    // protect-to-writable invalidates it so its writes are still caught.
    // Canary aed81ca93.
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}

bool SharedMemory::BeginSelfCopy(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return false;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  {
    auto global_lock = global_critical_region_.Acquire();
    if (self_copy_length_) {
      return false;   // one at a time; the caller is the single readback thread
    }
    for (uint32_t i = page_first >> 6; i <= (page_last >> 6); ++i) {
      uint64_t bits = UINT64_MAX;
      if (i == (page_first >> 6)) {
        bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
      }
      if (i == (page_last >> 6) && (page_last & 63) != 63) {
        bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
      }
      if ((system_page_flags_valid_and_gpu_written_[i] & bits) != bits) {
        return false;   // the CPU wrote here since the resolve: keep the normal path
      }
    }
    self_copy_start_ = start;
    self_copy_length_ = length;
  }
  // Unwatched in one call rather than one fault per page: the callback above
  // would say the same thing for each of the ~900 pages of a 720p target.
  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        page_first << page_size_log2_, (page_last - page_first + 1) << page_size_log2_, false,
        false);
  }
  return true;
}

void SharedMemory::EndSelfCopy(uint32_t start, uint32_t length) {
  {
    auto global_lock = global_critical_region_.Acquire();
    self_copy_length_ = 0;
    self_copy_start_ = 0;
  }
  // Valid, GPU-written, watched again - the state the resolve left them in.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllPagesValid(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return false;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = page_first >> 6; i <= (page_last >> 6); ++i) {
    uint64_t bits = UINT64_MAX;
    if (i == (page_first >> 6)) {
      bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == (page_last >> 6) && (page_last & 63) != 63) {
      bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if ((system_page_flags_valid_[i] & bits) != bits) {
      return false;
    }
  }
  return true;
}

namespace {
// [skip] One entry per page. 0 means "nothing recorded" - an FNV-1a of a page
// is essentially never 0, and treating the collision as "upload anyway" is the
// safe direction.
std::vector<uint64_t> g_page_upload_hash;
}  // namespace

bool SharedMemory::PageUploadHashMatches(uint32_t page, uint64_t hash) const {
  if (!hash || page >= g_page_upload_hash.size()) return false;
  return g_page_upload_hash[page] == hash;
}

void SharedMemory::SetPageUploadHash(uint32_t page, uint64_t hash) {
  if (g_page_upload_hash.empty()) {
    g_page_upload_hash.assign(size_t(kBufferSize >> page_size_log2_), 0);
  }
  if (page < g_page_upload_hash.size()) g_page_upload_hash[page] = hash;
}

void SharedMemory::EvictPageUploadHashes(uint32_t page_first, uint32_t page_last) {
  if (g_page_upload_hash.empty()) return;
  const size_t last = std::min<size_t>(page_last, g_page_upload_hash.size() - 1);
  uint64_t n = 0;
  for (size_t p = page_first; p <= last; ++p) {
    if (g_page_upload_hash[p]) {
      g_page_upload_hash[p] = 0;
      ++n;
    }
  }
  if (n) g_page_upload_hashes_evicted.fetch_add(n, std::memory_order_relaxed);
}

uint64_t SharedMemory::PageUploadHashesEvicted() {
  return g_page_upload_hashes_evicted.load(std::memory_order_relaxed);
}

void SharedMemory::SetRingRange(uint32_t start, uint32_t length) {
  g_ring_start = start;
  g_ring_length = length;
}

uint64_t SharedMemory::GpuWrittenPagesSeen() {
  return g_gpu_written_pages_seen.load(std::memory_order_relaxed);
}

bool SharedMemory::AllPagesGpuWritten(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return false;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = page_first >> 6; i <= (page_last >> 6); ++i) {
    uint64_t bits = UINT64_MAX;
    if (i == (page_first >> 6)) {
      bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == (page_last >> 6) && (page_last & 63) != 63) {
      bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if ((system_page_flags_valid_and_gpu_written_[i] & bits) != bits) {
      return false;
    }
  }
  return true;
}

bool SharedMemory::AnyPageGpuWritten(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return false;
  }
  length = std::min(length, kBufferSize - start);
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = page_first >> 6; i <= (page_last >> 6); ++i) {
    uint64_t bits = UINT64_MAX;
    if (i == (page_first >> 6)) {
      bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == (page_last >> 6) && (page_last & 63) != 63) {
      bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (system_page_flags_valid_and_gpu_written_[i] & bits) {
      return true;
    }
  }
  return false;
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

void SharedMemory::ProtectGpuRange(uint32_t start, uint32_t length) {
  if (!length) return;
  gpu_protected_ranges_.emplace_back(start, length);
}

void SharedMemory::UnprotectGpuRange(uint32_t start, uint32_t length) {
  for (size_t i = 0; i < gpu_protected_ranges_.size(); ++i) {
    if (gpu_protected_ranges_[i].first == start && gpu_protected_ranges_[i].second == length) {
      gpu_protected_ranges_[i] = gpu_protected_ranges_.back();
      gpu_protected_ranges_.pop_back();
      return;
    }
  }
}

bool SharedMemory::IsGpuRangeProtected(uint32_t start, uint32_t length) const {
  if (!length) return false;
  const uint64_t end = uint64_t(start) + length;
  for (const auto& r : gpu_protected_ranges_) {
    if (r.first < end && uint64_t(r.first) + r.second > start) return true;
  }
  return false;
}

void SharedMemory::SplitAroundProtected(
    uint32_t start, uint32_t length,
    std::vector<std::pair<uint32_t, uint32_t>>& pieces) const {
  if (!length) return;
  const uint64_t end = uint64_t(start) + length;
  // The protected ranges overlapping this one, in address order (few).
  std::pair<uint32_t, uint32_t> hits[16];
  size_t hit_count = 0;
  for (const auto& r : gpu_protected_ranges_) {
    const uint64_t r_end = uint64_t(r.first) + r.second;
    if (r.first >= end || r_end <= start) continue;
    if (hit_count < 16) hits[hit_count++] = r;
  }
  if (!hit_count) {
    pieces.emplace_back(start, length);
    return;
  }
  std::sort(hits, hits + hit_count);
  uint64_t cursor = start;
  for (size_t i = 0; i < hit_count; ++i) {
    const uint64_t h_start = std::max<uint64_t>(hits[i].first, start);
    const uint64_t h_end = std::min<uint64_t>(uint64_t(hits[i].first) + hits[i].second, end);
    if (h_start > cursor) pieces.emplace_back(uint32_t(cursor), uint32_t(h_start - cursor));
    if (h_end > cursor) cursor = h_end;
  }
  if (cursor < end) pieces.emplace_back(uint32_t(cursor), uint32_t(end - cursor));
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  if (ranges == nullptr || !count) {
    return true;
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  // NATIVE PATCH (2026-09-26, PROFT4): reused per thread - a fresh vector per call was a heap allocation per vertex
  // buffer per draw (~5,200 draws a frame in town).
  thread_local std::vector<std::pair<uint32_t, uint32_t>> merged_ranges;
  merged_ranges.clear();
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  // NATIVE PATCH (2026-09-26, PROFT4): the common case - every page already valid - is answered from the validity
  // bitmap without the global critical region (its lock was ~4.5% of the GPU thread, shared with the CPU threads'
  // write-watch faults). Read as volatile 64-bit words: a bit cleared concurrently is the same outcome as the CPU
  // write landing just after a locked check. Anything not valid falls through to the locked path unchanged.
  if (::ng2::ngpu::rtc::FastValidChecks()) {
    const volatile uint64_t* valid = system_page_flags_valid_.data();
    bool all_valid = true;
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      const uint32_t page_first = range.first >> page_size_log2_;
      const uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      for (uint32_t i = page_first >> 6; all_valid && i <= (page_last >> 6); ++i) {
        uint64_t need = ~uint64_t(0);
        if (i == (page_first >> 6)) need &= ~((uint64_t(1) << (page_first & 63)) - 1);
        if (i == (page_last >> 6) && (page_last & 63) != 63) need &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
        if ((valid[i] & need) != need) all_valid = false;
      }
      if (!all_valid) break;
    }
    if (all_valid) {
      for (const auto& range : merged_ranges) TouchRange(range.first, range.second);   // NATIVE PATCH: [hoist]
      return true;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    auto global_lock = global_critical_region_.Acquire();
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = system_page_flags_valid_[i];
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  bool ok = true;
  if (!upload_ranges_.empty()) ok = UploadRanges(upload_ranges_);
  for (const auto& range : merged_ranges) TouchRange(range.first, range.second);   // NATIVE PATCH: [hoist]
  return ok;
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  // The plugin's own readback copy landing (BeginSelfCopy): the GPU buffer
  // already holds these bytes. The heap lifts the page protection on return;
  // the pages stay valid and GPU-written and no watch fires, so the texture
  // cache keeps its copy of the resolved render target instead of reloading
  // it - which it did five times a frame. EndSelfCopy re-arms the watch.
  if (self_copy_length_ &&
      physical_address_start < uint64_t(self_copy_start_) + self_copy_length_ &&
      uint64_t(physical_address_start) + length > self_copy_start_) {
    return std::make_pair(page_first << page_size_log2_,
                          (page_last - page_first + 1) << page_size_log2_);
  }

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
  }

  uint32_t gpu_written_hit = 0;
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    gpu_written_hit += uint32_t(rex::bit_count(system_page_flags_valid_and_gpu_written_[i] & invalidate_bits));
    system_page_flags_valid_[i] &= ~invalidate_bits;
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
  }
  if (gpu_written_hit) {
    // [diag] A CPU write over GPU-rendered pages: whatever texture lives
    // there is re-uploaded from CPU memory next - the readback copy, older
    // than the render or never landed. The event behind a white impostor.
    static std::atomic<uint32_t> n{0};
    static std::atomic<uint64_t> bucket_second{0};
    static std::atomic<uint32_t> bucket_count{0};
    const uint32_t k = ++n;
    // Up to 40 lines a second (each flash needs its own line to be matched
    // with a recording), then a count.
    const uint64_t now_s = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    if (bucket_second.exchange(now_s) != now_s) bucket_count.store(0);
    const bool hits_ring =
        g_ring_length && physical_address_start < g_ring_start + g_ring_length &&
        g_ring_start < physical_address_start + length;
    if (++bucket_count <= 40 || k % 1000 == 0)
      REXLOG_INFO("[diag] gpu-written pages invalidated by a CPU write: {} page(s) at {:08X} len {} "
                  "exact {} ({} so far){}", gpu_written_hit, physical_address_start, length,
                  exact_range ? 1 : 0, k,
                  hits_ring ? " - THIS RANGE OVERLAPS THE GPU COMMAND RING" : "");
  }

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
