// VENDORED from rexglue-src 23ace0b:src/graphics/d3d12/shared_memory.cpp - systematic renames only (see vendor_rtc_d3d12.py / ORIGIN.txt):
// namespaces d3d12 -> ngpu_d3d12, plugin headers -> rtc_d3d12/facade.h, cvars -> plugin registry reads (1 bool, 0 string, 1 int).
#include <string>
#include <cstdint>
#include <rex/logging.h>
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

#include <cstring>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace {
uint64_t g_upload_pages_skipped = 0;
// [reach] See the probe below. At file scope because the report must not live
// inside the loop it measures: a counter that prints nothing when its loop
// never runs cannot tell "no dirty runs" from "this build has no probe".
std::atomic<uint64_t> g_reach_max_run{0}, g_reach_adds{0}, g_reach_runs{0}, g_reach_calls{0};
}  // namespace
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include "xxhash.h"
#include <rex/cvar.h>
#include "rtc_d3d12/command_processor.h"
#include "rtc_d3d12/shared_memory.h"
#include <rex/logging.h>
#include <rex/math.h>
#include "rtc_d3d12/d3d12_util.h"

bool& FLAGS_d3d12_tiled_shared_memory_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("d3d12_tiled_shared_memory", true); return s; }

int32_t& FLAGS_shared_memory_upload_threads_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("shared_memory_upload_threads", 3); return s; }

namespace rex::graphics::ngpu_d3d12 {

namespace {
// [perf] A pool for the upload copies: jobs are added while the command
// list is recorded, then run on the helpers AND the caller, and joined.
class UploadCopyPool {
 public:
  explicit UploadCopyPool(int threads) {
    for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { Run(); });
  }
  ~UploadCopyPool() {
    {
      std::lock_guard<std::mutex> lock(m_);
      quit_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) w.join();
  }
  // Under the lock. It was not, and workers read jobs_ and jobs_.size() while
  // holding it - a lock only one side takes is not a lock. push_back can
  // reallocate under a worker still indexing the old buffer, and it grows
  // jobs_.size() where a worker is about to compare it against a next_ left
  // over from the previous batch.
  void Add(void* dst, const void* src, size_t size) {
    std::lock_guard<std::mutex> lock(m_);
    jobs_.push_back({dst, src, size});
  }
  bool Empty() const {
    std::lock_guard<std::mutex> lock(m_);
    return jobs_.empty();
  }
  void RunAll() {
    uint64_t gen;
    {
      std::lock_guard<std::mutex> lock(m_);
      if (jobs_.empty()) return;
      next_ = 0;
      end_ = jobs_.size();
      pending_ = end_;
      gen = ++generation_;
    }
    cv_.notify_all();
    Work(gen);
    std::unique_lock<std::mutex> lock(m_);
    done_cv_.wait(lock, [this] { return pending_ == 0; });
    jobs_.clear();
  }

 private:
  struct Job {
    void* dst;
    const void* src;
    size_t size;
  };
  // Every worker is pinned to the batch it woke for. Without that, a worker
  // still inside this loop when the NEXT batch is queued could claim a job
  // from it and decrement a counter that is not its own - and pending_ is a
  // size_t, so one extra decrement wraps it to SIZE_MAX, "--pending_ == 0" is
  // never true again, done_cv_ is never signalled, and RunAll blocks forever
  // on the GPU worker thread. No error, no failed call: a hang that looks like
  // the GPU simply stopped.
  //
  // The job is also copied out under the lock rather than indexed after it, so
  // no memcpy reads a Job out of a vector another thread may reallocate.
  void Work(uint64_t gen) {
    for (;;) {
      Job job;
      {
        std::lock_guard<std::mutex> lock(m_);
        // end_, not jobs_.size(): the vector grows when Add() queues the NEXT
        // batch, and that can happen while this worker is still draining the
        // current one. Pinning the generation alone left exactly that gap, and
        // the assertion below fired on it under the other title's load.
        if (generation_ != gen || next_ >= end_) return;
        job = jobs_[next_++];
      }
      std::memcpy(job.dst, job.src, job.size);
      std::lock_guard<std::mutex> lock(m_);
      if (generation_ != gen) return;
      // An assertion, not a safety net. With the generation pinning above, a
      // decrement with nothing pending is impossible; if it ever happens the
      // pinning is broken and the two outcomes are a wrap to SIZE_MAX (hang) or
      // an early return (the GPU copies a buffer a worker is still writing).
      // Silence would leave the second one undetectable, so say it loudly and
      // do not decrement.
      if (pending_ == 0) {
        static bool once = false;
        if (!once) {
          once = true;
          REXGPU_ERROR(
              "[upload] a copy worker finished a job with nothing pending - the batch pinning is "
              "broken. This is the corruption case, not just the hang case: the GPU may copy an "
              "upload buffer while a worker is still writing it");
        }
        continue;
      }
      if (--pending_ == 0) done_cv_.notify_all();
    }
  }
  void Run() {
    uint64_t seen = 0;
    for (;;) {
      std::unique_lock<std::mutex> lock(m_);
      cv_.wait(lock, [&] { return quit_ || generation_ != seen; });
      if (quit_) return;
      seen = generation_;
      lock.unlock();
      Work(seen);
    }
  }
  std::vector<Job> jobs_;
  std::vector<std::thread> workers_;
  mutable std::mutex m_;
  std::condition_variable cv_, done_cv_;
  size_t next_ = 0, end_ = 0, pending_ = 0;
  uint64_t generation_ = 0;
  bool quit_ = false;
};
UploadCopyPool* g_upload_pool = nullptr;
}  // namespace

// NATIVE PATCH (2026-09-26): readback landings (CopyToGuestMemory) of 1 MB or more are split over the upload copy
// pool - ~1.7 ms of single-threaded memcpy per town frame (PROFT3). Same thread as the uploads (the GPU thread), so
// the pool is never used by two batches at once. Returns false when the copy is left to the caller.
bool NgpuParallelCopy(void* dst, const void* src, size_t size) {
  if (size < (size_t(1) << 20)) return false;
  if (!g_upload_pool) {
    const int threads = REXCVAR_GET(shared_memory_upload_threads);
    if (threads <= 0) return false;
    g_upload_pool = new UploadCopyPool(std::min(threads, 8));
  }
  const size_t kChunk = 256 * 1024;
  for (size_t off = 0; off < size; off += kChunk)
    g_upload_pool->Add(static_cast<uint8_t*>(dst) + off, static_cast<const uint8_t*>(src) + off, std::min(kChunk, size - off));
  g_upload_pool->RunAll();
  return true;
}

D3D12SharedMemory::D3D12SharedMemory(D3D12CommandProcessor& command_processor,
                                     memory::Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

D3D12SharedMemory::~D3D12SharedMemory() {
  Shutdown(true);
}

bool D3D12SharedMemory::Initialize() {
  InitializeCommon();

  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_RESOURCE_DESC buffer_desc;
  ui::ngpu_d3d12::util::FillBufferResourceDesc(buffer_desc, kBufferSize,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (REXCVAR_GET(d3d12_tiled_shared_memory) &&
      provider.GetTiledResourcesTier() != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
      !provider.GetGraphicsAnalysis()) {
    if (FAILED(device->CreateReservedResource(&buffer_desc, buffer_state_, nullptr,
                                              IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB tiled buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    static_assert(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == (1 << 16));
    InitializeSparseHostGpuMemory(
        std::max(kHostGpuMemoryOptimalSparseAllocationLog2, uint32_t(16)));
  } else {
    REXGPU_INFO(
        "Direct3D 12 tiled resources are not used for shared memory "
        "emulation - video memory usage may increase significantly "
        "because a full {} MB buffer will be created",
        kBufferSize >> 20);
    if (provider.GetGraphicsAnalysis()) {
      // As of October 8th, 2018, PIX doesn't support tiled buffers.
      // FIXME(Triang3l): Re-enable tiled resources with PIX once fixed.
      REXGPU_INFO(
          "This is caused by PIX being attached, which doesn't support tiled "
          "resources yet.");
    }
    if (FAILED(device->CreateCommittedResource(&ui::ngpu_d3d12::util::kHeapPropertiesDefault,
                                               provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                               buffer_state_, nullptr, IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
  }
  buffer_gpu_address_ = buffer_->GetGPUVirtualAddress();
  buffer_uav_writes_commit_needed_ = false;

  D3D12_DESCRIPTOR_HEAP_DESC buffer_descriptor_heap_desc;
  buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  buffer_descriptor_heap_desc.NumDescriptors = uint32_t(BufferDescriptorIndex::kCount);
  buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&buffer_descriptor_heap_)))) {
    REXGPU_ERROR("Shared memory: Failed to create the descriptor heap for buffer views");
    Shutdown();
    return false;
  }
  buffer_descriptor_heap_start_ = buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  ui::ngpu_d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      buffer_, kBufferSize);
  ui::ngpu_d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintSRV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::ngpu_d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::ngpu_d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);
  ui::ngpu_d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      buffer_, kBufferSize);
  ui::ngpu_d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintUAV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::ngpu_d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::ngpu_d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);

  upload_buffer_pool_ = std::make_unique<ui::ngpu_d3d12::D3D12UploadBufferPool>(
      provider, rex::align(ui::ngpu_d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           size_t(1) << page_size_log2()));

  return true;
}

void D3D12SharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();

  ui::ngpu_d3d12::util::ReleaseAndNull(buffer_descriptor_heap_);

  // First free the buffer to detach it from the heaps.
  ui::ngpu_d3d12::util::ReleaseAndNull(buffer_);

  for (ID3D12Heap* heap : buffer_tiled_heaps_) {
    heap->Release();
  }
  buffer_tiled_heaps_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12SharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void D3D12SharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void D3D12SharedMemory::BeginSubmission() {
  // ExecuteCommandLists is a full UAV barrier.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state) {
  if (buffer_state_ == new_state) {
    if (new_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && buffer_uav_writes_commit_needed_) {
      command_processor_.PushUAVBarrier(buffer_);
      buffer_uav_writes_commit_needed_ = false;
    }
    return;
  }
  command_processor_.PushTransitionBarrier(buffer_, buffer_state_, new_state);
  buffer_state_ = new_state;
  // "UAV -> anything" transition commits the writes implicitly.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                         uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  uint32_t offset_bytes = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  uint32_t length_bytes = length_allocations << host_gpu_memory_sparse_granularity_log2();

  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();
  command_processor_.DrainSubmissions();   // NATIVE PATCH: [async submit] keep queue order with earlier submissions

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = length_bytes;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed();
  ID3D12Heap* heap;
  if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    REXGPU_ERROR("Shared memory: Failed to create a tile heap");
    return false;
  }
  buffer_tiled_heaps_.push_back(heap);

  D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
  region_start_coordinates.X = offset_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_start_coordinates.Y = 0;
  region_start_coordinates.Z = 0;
  region_start_coordinates.Subresource = 0;
  D3D12_TILE_REGION_SIZE region_size;
  region_size.NumTiles = length_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_size.UseBox = FALSE;
  D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_range_start_offset = 0;
  direct_queue->UpdateTileMappings(buffer_, 1, &region_start_coordinates, &region_size, heap, 1,
                                   &range_flags, &heap_range_start_offset, &region_size.NumTiles,
                                   D3D12_TILE_MAPPING_FLAG_NONE);
  command_processor_.NotifyQueueOperationsDoneDirectly();
  return true;
}

bool D3D12SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  D3D12CommandProcessor::GpuCatScope gpu_cat(command_processor_, D3D12CommandProcessor::kGpuCatUpload);   // NATIVE PATCH
  // NATIVE PATCH [hoist] (BAR1: ~1,000 shared-memory transitions to copy-dest and back per town frame, each one a GPU
  // drain): an upload into pages no GPU work of the open submission has read or written goes to the submission's
  // PROLOGUE - copied before any of it runs, all under one transition pair. Anything else stays inline.
  bool hoist = command_processor_.PrologueAvailable();
  for (const auto& r : upload_page_ranges) {
    if (!hoist) break;
    if (AnyTouched(r.first << page_size_log2(), r.second << page_size_log2())) hoist = false;
  }
  if (hoist) {
    command_processor_.NoteHoistedUpload();
  } else {
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
    command_processor_.SubmitBarriers();
  }
  auto& command_list = hoist ? command_processor_.GetPrologueList() : command_processor_.GetDeferredCommandList();
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length != 0) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), &upload_buffer, &upload_buffer_offset, &upload_buffer_size,
          nullptr);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
        if (g_upload_pool) g_upload_pool->RunAll();
        return false;
      }
      // [readback] Before the bytes below are read from guest memory: any resolve
      // copy still owed to this range lands now (or is awaited, an earlier
      // submission only) so the upload carries the render, not the stale fill.
      command_processor_.LandOrAwaitReadbackForUpload(upload_range_start << page_size_log2(),
                                                      uint32_t(upload_buffer_size));
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      command_processor_.NoteSharedMemoryUpload(uint64_t(upload_buffer_size));  // [hitch]
      // [churn] Is this re-upload doing work, or repeating itself?
      //
      // With clear_memory_page_state on, every CPU-uploaded page is invalidated
      // at frame close, so the next frame re-reads whatever it touches - 17 MB
      // a frame on Ninja Gaiden II against 820 KB with the cvar off. Whether
      // that is fixable depends on a question nothing here has asked: are those
      // bytes actually DIFFERENT from last time?
      //
      // If they are, the CPU really is rewriting its working set every frame,
      // the refresh is doing necessary work, and the cost is inherent. If they
      // are not, the refresh is re-reading bytes the GPU already had correct,
      // the write-watch was adequate, and the volume is pure waste.
      //
      // Sampled - the first 4 KB of each range, FNV-1a - because hashing 17 MB
      // a frame to measure the cost of moving 17 MB a frame would be its own
      // answer.
      if (REXCVAR_GET(shared_memory_upload_churn)) {
        const uint8_t* probe = static_cast<const uint8_t*>(
            memory().TranslatePhysical(upload_range_start << page_size_log2()));
        if (probe) {
          // Head AND tail, because a head-only sample calls a range unchanged
          // when its head is stable and its tail churns - harmless in a
          // diagnostic, a corruption bug in anything that SKIPS on the answer.
          uint64_t h = 1469598103934665603ull;
          auto mix = [&h](const uint8_t* p, size_t n) {
            for (size_t i = 0; i < n; ++i) {
              h ^= p[i];
              h *= 1099511628211ull;
            }
          };
          // WHOLE range, not a sample. Every widening of the window lowered the
          // unchanged figure - 94.4% at 4 KB, 71.0% at 8 KB - because a byte
          // that is not hashed cannot disagree, so any sample is an UPPER BOUND
          // on redundancy and never the number. The production skip path has to
          // hash everything to be safe, so hashing everything here measures the
          // real redundancy and the real hashing cost at the same time.
          mix(probe, upload_buffer_size);
          static std::unordered_map<uint32_t, uint64_t> s_last;
          static uint64_t s_same_bytes = 0, s_diff_bytes = 0, s_same_n = 0, s_diff_n = 0;
          static std::chrono::steady_clock::time_point s_next;
          auto it = s_last.find(upload_range_start);
          if (it != s_last.end() && it->second == h) {
            s_same_bytes += upload_buffer_size;
            ++s_same_n;
          } else {
            s_diff_bytes += upload_buffer_size;
            ++s_diff_n;
          }
          s_last[upload_range_start] = h;
          const auto now = std::chrono::steady_clock::now();
          if (s_next.time_since_epoch().count() == 0) s_next = now + std::chrono::seconds(5);
          if (now >= s_next) {
            const uint64_t total = s_same_bytes + s_diff_bytes;
            REXGPU_INFO(
                "[churn] re-uploads in 5 s: {} KB unchanged in {} ranges, {} KB changed in {} "
                "ranges - {}% of the upload volume is bytes the GPU already had",
                s_same_bytes >> 10, s_same_n, s_diff_bytes >> 10, s_diff_n,
                total ? (s_same_bytes * 100 / total) : 0);
            REXGPU_INFO(
                "[skip] pages skipped {}, hashes evicted by a GPU write {} of {} GPU-written "
                "pages seen - evictions plateauing is benign only if the second number plateaued too; if GPU writes keep climbing while evictions do not, eviction is wired wrong and late skips are unprotected",
                g_upload_pages_skipped, SharedMemory::PageUploadHashesEvicted(),
                SharedMemory::GpuWrittenPagesSeen());
            s_same_bytes = s_diff_bytes = s_same_n = s_diff_n = 0;
            s_next = now + std::chrono::seconds(5);
          }
        }
      }
      // [race] Three knobs that separate WHY adding work to this path suppresses
      // the NG2 freeze. The churn probe was found to hide it - probe off froze
      // 3/3, probe on survived 3/3 - and the freeze got harder to reproduce as
      // the probe got more expensive, which is a dose-response curve and not a
      // volume story.
      //
      //   spin   costs TIME, touches nothing
      //   touch  touches the pages, costs almost no time
      //   churn  does both
      //
      // If spin alone suppresses it, it is a timing race. If touch does but spin
      // does not, reading those pages has a side effect - paging, TLB, or the
      // write-watch. If only full hashing does, it is the whole read.
      if (const int32_t spin_ns = REXCVAR_GET(shared_memory_upload_spin_ns)) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(spin_ns);
        while (std::chrono::steady_clock::now() < until) {
        }
      }
      // [race] Bandwidth as a SWEEP, at constant residency and near-zero time.
      // The original touch read one byte per 4 KB page - one cache line in
      // sixty-four, about 1.5% of the churn probe's traffic - so it was never
      // "the memory half at full strength" and the gap between it and churn was
      // never tested. This reads N bytes per page for any N: every leg touches
      // every page, so residency is held constant while traffic varies 4000x.
      //
      // Padding legs with spin to equalise wall-time was the alternative and it
      // does not work: spin has its own boot-time failure, so a padded leg loses
      // runs to something unrelated to the question and loses them before
      // reaching the phase being measured.
      if (const int32_t touch_bytes = REXCVAR_GET(shared_memory_upload_touch_bytes)) {
        const volatile uint8_t* t = static_cast<const volatile uint8_t*>(
            memory().TranslatePhysical(upload_range_start << page_size_log2()));
        if (t) {
          const size_t page = size_t(1) << page_size_log2();
          const size_t per_page = std::min<size_t>(size_t(touch_bytes), page);
          volatile uint64_t sink = 0;
          for (size_t base = 0; base < upload_buffer_size; base += page) {
            const size_t n = std::min(per_page, upload_buffer_size - base);
            for (size_t i = 0; i < n; ++i) sink += t[base + i];
          }
          (void)sink;
        }
      }
      // [skip] Per PAGE, not per range: 93.9% of ranges are unchanged but only
      // 71.2% of bytes, so the volume lives in large ranges with a few dirty
      // pages. Range granularity would forfeit most of what is recoverable.
      const uint32_t chunk_byte_start = upload_range_start << page_size_log2();
      const uint32_t chunk_page_bytes = 1u << page_size_log2();
      const uint32_t chunk_pages = uint32_t(upload_buffer_size >> page_size_log2());
      thread_local std::vector<std::pair<uint32_t, uint32_t>> dirty_runs;   // NATIVE PATCH: reused, no alloc per chunk
      dirty_runs.clear();
      if (REXCVAR_GET(shared_memory_upload_skip_unchanged) && chunk_pages) {
        const uint8_t* chunk_src = static_cast<const uint8_t*>(
            memory().TranslatePhysical(chunk_byte_start));
        if (chunk_src) {
          uint32_t run_first = UINT32_MAX;
          for (uint32_t i = 0; i < chunk_pages; ++i) {
            // NATIVE PATCH (2026-09-26): XXH3 instead of byte-wise FNV-1a (~4 cycles a byte - the town's 560 MB/s of
            // re-uploads cost more to hash than to copy). 64-bit, per 4 KB page.
            const uint8_t* q = chunk_src + size_t(i) * chunk_page_bytes;
            const uint64_t h = XXH3_64bits(q, chunk_page_bytes);
            const uint32_t page = upload_range_start + i;
            const bool same = PageUploadHashMatches(page, h);
            SetPageUploadHash(page, h);
            if (!same) {
              if (run_first == UINT32_MAX) run_first = i;
            } else if (run_first != UINT32_MAX) {
              dirty_runs.emplace_back(chunk_byte_start + run_first * chunk_page_bytes,
                                      (i - run_first) * chunk_page_bytes);
              run_first = UINT32_MAX;
            }
          }
          if (run_first != UINT32_MAX) {
            dirty_runs.emplace_back(chunk_byte_start + run_first * chunk_page_bytes,
                                    (chunk_pages - run_first) * chunk_page_bytes);
          }
          g_upload_pages_skipped += chunk_pages;
          for (const auto& r : dirty_runs) g_upload_pages_skipped -= r.second >> page_size_log2();
        } else {
          dirty_runs.emplace_back(chunk_byte_start, uint32_t(upload_buffer_size));
        }
      } else {
        dirty_runs.emplace_back(chunk_byte_start, uint32_t(upload_buffer_size));
      }

      {
        const int threads = REXCVAR_GET(shared_memory_upload_threads);
        if (threads > 0 && !g_upload_pool) g_upload_pool = new UploadCopyPool(std::min(threads, 8));
        const uint8_t* src = static_cast<const uint8_t*>(
            memory().TranslatePhysical(chunk_byte_start));
        for (const auto& run : dirty_runs) {
          const size_t run_off = run.first - chunk_byte_start;
          const size_t run_len = run.second;
          // [reach] Is the copy pool reachable in THIS title, or only argued to
          // be? The race needs Add() to be called at all, and Add() is only
          // called for a single dirty run of 256 KB or more. A title whose runs
          // never reach it never constructs a job, so RunAll returns on an empty
          // vector and no worker ever wakes.
          //
          // "Dormant" and "safe" are different claims and only a counter can
          // tell them apart: the largest run this title actually produces says
          // how much headroom there is, and the Add count says whether the
          // threshold has been crossed even once.
          {
            g_reach_runs.fetch_add(1, std::memory_order_relaxed);
            uint64_t prev = g_reach_max_run.load(std::memory_order_relaxed);
            while (run_len > prev && !g_reach_max_run.compare_exchange_weak(prev, run_len)) {}
            if (threads > 0 && run_len >= (256u << 10))
              g_reach_adds.fetch_add(1, std::memory_order_relaxed);
          }
          if (threads > 0 && run_len >= (256u << 10)) {
            // Big chunks are split in 256 KB pieces so every helper gets a share.
            for (size_t off = 0; off < run_len; off += (256u << 10)) {
              const size_t piece = std::min(size_t(256u << 10), run_len - off);
              g_upload_pool->Add(upload_buffer_mapping + run_off + off, src + run_off + off, piece);
            }
          } else {
            std::memcpy(upload_buffer_mapping + run_off, src + run_off, run_len);
          }
        }
      }
      {
        // [readback] Not over a resolve whose CPU copy is still pending: the
        // GPU keeps its fresh render there, the rest of the page is uploaded.
        const uint32_t byte_start = upload_range_start << page_size_log2();
        std::vector<std::pair<uint32_t, uint32_t>> pieces;
        SplitAroundProtected(byte_start, uint32_t(upload_buffer_size), pieces);
        for (const auto& piece : pieces) {
          // Intersected with the dirty runs: a clean page is already correct in
          // the buffer and must not be copied over.
          for (const auto& run : dirty_runs) {
            const uint32_t lo = std::max(piece.first, run.first);
            const uint32_t hi = std::min(piece.first + piece.second, run.first + run.second);
            if (lo >= hi) continue;
            command_list.D3DCopyBufferRegion(
                buffer_, lo, upload_buffer,
                UINT64(upload_buffer_offset) + (lo - byte_start), UINT64(hi - lo));
          }
        }
      }
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
  }
  if (g_upload_pool) g_upload_pool->RunAll();
  // [reach] Reported HERE, on a path that runs whether or not there were any
  // dirty runs, so "0 runs" is a reading rather than a silence.
  g_reach_calls.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(shared_memory_upload_reach)) {
    static std::chrono::steady_clock::time_point s_next;
    const auto now = std::chrono::steady_clock::now();
    if (s_next.time_since_epoch().count() == 0) s_next = now + std::chrono::seconds(5);
    if (now >= s_next) {
      s_next = now + std::chrono::seconds(5);
      REXGPU_INFO(
          "[reach] upload copy pool in 5 s: {} UploadRanges calls, {} dirty runs, largest {} KB, "
          "{} reached the 256 KB threshold and were handed to the pool. Zero handed over means the "
          "pool never ran, so the race is unreachable in this title rather than merely unobserved",
          g_reach_calls.exchange(0, std::memory_order_relaxed),
          g_reach_runs.exchange(0, std::memory_order_relaxed),
          g_reach_max_run.exchange(0, std::memory_order_relaxed) >> 10,
          g_reach_adds.exchange(0, std::memory_order_relaxed));
    }
  }
  return true;
}

}  // namespace rex::graphics::ngpu_d3d12
