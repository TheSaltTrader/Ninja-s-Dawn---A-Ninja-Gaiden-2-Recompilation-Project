R = r'C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/'
def patch(p, old, new, count=1):
    s = open(R + p, encoding='utf-8').read()
    n = s.count(old)
    assert n == count, (p, old[:70], n)
    open(R + p, 'w', encoding='utf-8').write(s.replace(old, new))
X = 'native_gpu_xlat/rtc_d3d12/'

# ---- SharedMemory base: pages touched by the GPU in the open submission -----------------------------------------
patch(X + 'shared_memory_base.h', '''  void RangeWrittenByGpu(uint32_t start, uint32_t length);''', '''  void RangeWrittenByGpu(uint32_t start, uint32_t length);
  // [hoist] (NATIVE PATCH) pages read or written by GPU work recorded in the OPEN submission. An upload into pages
  // nothing has touched yet may be moved to the submission's prologue (one transition for all of them).
  void TouchRange(uint32_t start, uint32_t length);
  bool AnyTouched(uint32_t start, uint32_t length) const;
  void ResetTouched();''')
patch(X + 'shared_memory_base.cpp', '''void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }''', '''namespace {
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
  TouchRange(start, length);   // NATIVE PATCH: [hoist]''')
# RequestRanges: touch what the draw reads - after its own uploads were decided, on both return paths
patch(X + 'shared_memory_base.cpp', '''    if (all_valid) return true;
  }''', '''    if (all_valid) {
      for (const auto& range : merged_ranges) TouchRange(range.first, range.second);   // NATIVE PATCH: [hoist]
      return true;
    }
  }''')
patch(X + 'shared_memory_base.cpp', '''  if (upload_ranges_.empty()) {
    return true;
  }

  return UploadRanges(upload_ranges_);
}''', '''  bool ok = true;
  if (!upload_ranges_.empty()) ok = UploadRanges(upload_ranges_);
  for (const auto& range : merged_ranges) TouchRange(range.first, range.second);   // NATIVE PATCH: [hoist]
  return ok;
}''')

# ---- UploadRanges: hoist when nothing in this submission touched the pages -------------------------------------
patch(X + 'shared_memory.cpp', '''  D3D12CommandProcessor::GpuCatScope gpu_cat(command_processor_, D3D12CommandProcessor::kGpuCatUpload);   // NATIVE PATCH
  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  auto& command_list = command_processor_.GetDeferredCommandList();''', '''  D3D12CommandProcessor::GpuCatScope gpu_cat(command_processor_, D3D12CommandProcessor::kGpuCatUpload);   // NATIVE PATCH
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
  auto& command_list = hoist ? command_processor_.GetPrologueList() : command_processor_.GetDeferredCommandList();''')

# ---- command processor: prologue list, reset per submission, execution ------------------------------------------
H = X + 'command_processor.h'
patch(H, '''  uint8_t GpuCatSet(uint8_t cat);''', '''  uint8_t GpuCatSet(uint8_t cat);
  // [hoist] (NATIVE PATCH) the submission's upload prologue (see D3D12SharedMemory::UploadRanges).
  bool PrologueAvailable() const { return hoist_uploads_ && submission_open_; }
  DeferredCommandList& GetPrologueList() { return prologue_list_; }
  void NoteHoistedUpload() { ++hoisted_uploads_; }''')
patch(H, '''    std::vector<uint8_t> prof_cats;   // [gpu prof] category of each timestamp interval''', '''    std::vector<uint8_t> prof_cats;   // [gpu prof] category of each timestamp interval
    std::unique_ptr<DeferredCommandList> prologue;   // [hoist] hoisted upload copies, run first
    D3D12_RESOURCE_STATES prologue_state = D3D12_RESOURCE_STATE_COMMON;   // buffer state around the prologue''')
patch(H, '''  // [async submit]
  struct SubmitJob {''', '''  // [hoist]
  bool hoist_uploads_ = false;
  DeferredCommandList prologue_list_{*this};
  D3D12_RESOURCE_STATES prologue_buffer_state_ = D3D12_RESOURCE_STATE_COMMON;
  uint64_t hoisted_uploads_ = 0, inline_uploads_ = 0;
  // [async submit]
  struct SubmitJob {''')

C = X + 'command_processor.cpp'
patch(C, '''  gpu_prof_ = async_submit_ && ::fable2::ngpu::rtc::GpuProfEnabled();   // NATIVE PATCH: [gpu prof]''', '''  gpu_prof_ = async_submit_ && ::fable2::ngpu::rtc::GpuProfEnabled();   // NATIVE PATCH: [gpu prof]
  hoist_uploads_ = async_submit_ && ::fable2::ngpu::rtc::HoistUploadsEnabled();   // NATIVE PATCH: [hoist]''')
patch(C, '''    deferred_command_list_.Reset();
    GpuProfBegin();   // NATIVE PATCH: [gpu prof]''', '''    deferred_command_list_.Reset();
    GpuProfBegin();   // NATIVE PATCH: [gpu prof]
    // NATIVE PATCH: [hoist] a fresh prologue; the buffer's state now is the state around it.
    prologue_list_.Reset();
    if (shared_memory_) {
      shared_memory_->ResetTouched();
      prologue_buffer_state_ = shared_memory_->GetBufferState();
    }''')
patch(C, '''      job_list->SwapRecording(deferred_command_list_);''', '''      job_list->SwapRecording(deferred_command_list_);
      std::unique_ptr<DeferredCommandList> prologue;
      if (hoist_uploads_ && !prologue_list_.IsEmpty()) {
        {
          std::lock_guard<std::mutex> lk(submit_mutex_);
          if (!submit_free_lists_.empty()) {
            prologue = std::move(submit_free_lists_.back());
            submit_free_lists_.pop_back();
          }
        }
        if (!prologue) prologue = std::make_unique<DeferredCommandList>(*this);
        prologue->SwapRecording(prologue_list_);
        prologue_list_.Reset();
      }''')
patch(C, '''        submit_jobs_.push_back(SubmitJob{std::move(job_list), command_allocator, submission_current_,
                                         std::move(gpu_prof_cats_), gpu_prof_slot_, prof_used});''', '''        submit_jobs_.push_back(SubmitJob{std::move(job_list), command_allocator, submission_current_,
                                         std::move(gpu_prof_cats_), gpu_prof_slot_, prof_used, std::move(prologue),
                                         prologue_buffer_state_});''')
patch(C, '''    if (gpu_time_mapped_) command_list_->EndQuery(gpu_time_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ts_slot * 2);
    job.list->Execute(command_list_, command_list_1_);''', '''    if (gpu_time_mapped_) command_list_->EndQuery(gpu_time_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ts_slot * 2);
    if (job.prologue) {
      // [hoist] every hoisted upload under ONE transition pair, before the submission's own commands.
      ID3D12Resource* buffer = shared_memory_->GetBuffer();
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = buffer;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      const bool transition = job.prologue_state != D3D12_RESOURCE_STATE_COPY_DEST;
      if (transition) {
        b.Transition.StateBefore = job.prologue_state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        command_list_->ResourceBarrier(1, &b);
      }
      job.prologue->Execute(command_list_, command_list_1_);
      if (transition) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = job.prologue_state;
        command_list_->ResourceBarrier(1, &b);
      }
      job.prologue->Reset();
      std::lock_guard<std::mutex> lk(submit_mutex_);
      submit_free_lists_.push_back(std::move(job.prologue));
    }
    job.list->Execute(command_list_, command_list_1_);''')
# report the split beside BARRIERS
patch(C, '''      g_prof_barrier_batches = g_prof_barriers = g_prof_uav_barriers = 0;''', '''      REXGPU_INFO("[ngpu] HOIST: {:.0f} upload batches per frame moved to the prologue", double(hoisted_uploads_) / frames);
      hoisted_uploads_ = 0;
      g_prof_barrier_batches = g_prof_barriers = g_prof_uav_barriers = 0;''')
# the resolve's mirror copy writes the buffer outside RangeWrittenByGpu
patch(C, '''      shared_memory_->UseAsCopyDestination();
      SubmitBarriers();
      deferred_command_list_.D3DCopyBufferRegion(shared_memory_->GetBuffer(), written_address,''', '''      shared_memory_->UseAsCopyDestination();
      SubmitBarriers();
      shared_memory_->TouchRange(written_address, written_length);   // NATIVE PATCH: [hoist]
      deferred_command_list_.D3DCopyBufferRegion(shared_memory_->GetBuffer(), written_address,''')
# facade + cvar
patch(X + 'facade.h', '''bool GpuProfEnabled();   // ngpu_gpu_prof (read once)''', '''bool GpuProfEnabled();   // ngpu_gpu_prof (read once)
bool HoistUploadsEnabled();   // ngpu_backend_hoist_uploads (read once)''')
patch(X + 'facade.cpp', '''bool NgpuGpuProfCvar();''', '''bool NgpuHoistUploadsCvar();
bool HoistUploadsEnabled() {
  static const bool on = NgpuHoistUploadsCvar();
  return on;
}
bool NgpuGpuProfCvar();''')
patch('native_gpu_present.cpp', '''namespace fable2::ngpu::rtc { bool NgpuGpuProfCvar() { return REXCVAR_GET(ngpu_gpu_prof); } }''', '''namespace fable2::ngpu::rtc { bool NgpuGpuProfCvar() { return REXCVAR_GET(ngpu_gpu_prof); } }
REXCVAR_DEFINE_BOOL(ngpu_backend_hoist_uploads, false, "GPU", "Native-GPU BACKEND (read once): uploads into guest pages no GPU work of the open submission has touched are copied in a prologue under one transition, instead of a copy-dest/shader-resource round trip (a GPU drain) per upload");
namespace fable2::ngpu::rtc { bool NgpuHoistUploadsCvar() { return REXCVAR_GET(ngpu_backend_hoist_uploads); } }''')
print('ok')
