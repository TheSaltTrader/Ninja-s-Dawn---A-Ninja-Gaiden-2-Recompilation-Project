R = r'C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/'
def patch(p, old, new, count=1):
    s = open(R + p, encoding='utf-8').read()
    n = s.count(old)
    assert n == count, (p, old[:70], n)
    open(R + p, 'w', encoding='utf-8').write(s.replace(old, new))
X = 'native_gpu_xlat/rtc_d3d12/'

patch(X + 'command_processor.h', '''  uint64_t submit_queued_through_ = 0, submit_done_through_ = 0;''', '''  uint64_t submit_queued_through_ = 0, submit_done_through_ = 0;
  // [gpu time] timestamp pairs around every submission (submit thread only): the TRUE GPU time per frame, which the
  // pmon "SM active" figure is not (it counts any kernel running).
  static constexpr uint32_t kGpuTimeSlots = 256;
  ID3D12QueryHeap* gpu_time_heap_ = nullptr;
  ID3D12Resource* gpu_time_readback_ = nullptr;
  const uint64_t* gpu_time_mapped_ = nullptr;
  std::deque<uint64_t> gpu_time_pending_;
  uint64_t gpu_time_ticks_ = 0, gpu_time_submissions_ = 0, gpu_time_frequency_ = 0;
  uint64_t gpu_time_next_report_ms_ = 0, gpu_time_swaps_at_report_ = 0;
  void GpuTimeCollect(bool all);''')

C = X + 'command_processor.cpp'
patch(C, '''void D3D12CommandProcessor::SubmitThreadMain() {
  ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();''', '''void D3D12CommandProcessor::GpuTimeCollect(bool all) {
  if (!gpu_time_mapped_) return;
  const uint64_t completed = submission_fence_->GetCompletedValue();
  while (!gpu_time_pending_.empty() && (all || gpu_time_pending_.front() <= completed)) {
    if (gpu_time_pending_.front() > completed) break;
    const uint32_t slot = uint32_t(gpu_time_pending_.front() % kGpuTimeSlots);
    gpu_time_pending_.pop_front();
    const uint64_t b = gpu_time_mapped_[slot * 2], e = gpu_time_mapped_[slot * 2 + 1];
    if (e > b) gpu_time_ticks_ += e - b;
    ++gpu_time_submissions_;
  }
  const uint64_t now_ms = GetTickCount64();
  if (!gpu_time_next_report_ms_) gpu_time_next_report_ms_ = now_ms + 5000;
  if (now_ms >= gpu_time_next_report_ms_ && gpu_time_frequency_) {
    const uint64_t swaps = ::fable2::ngpu::rtc::SwapSubmissionsNoted();
    const uint64_t frames = swaps - gpu_time_swaps_at_report_;
    const double ms = double(gpu_time_ticks_) * 1000.0 / double(gpu_time_frequency_);
    REXGPU_INFO("[ngpu] GPU TIME: {:.2f} ms of backend GPU work per frame ({} frames, {} submissions, {:.1f} ms total in 5 s)",
                frames ? ms / double(frames) : 0.0, frames, gpu_time_submissions_, ms);
    gpu_time_ticks_ = 0;
    gpu_time_submissions_ = 0;
    gpu_time_swaps_at_report_ = swaps;
    gpu_time_next_report_ms_ = now_ms + 5000;
  }
}

void D3D12CommandProcessor::SubmitThreadMain() {
  ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
  {
    ID3D12Device* device = GetD3D12Provider().GetDevice();
    D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kGpuTimeSlots * 2, 0};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kGpuTimeSlots * 2 * sizeof(uint64_t);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr;
    if (SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&gpu_time_heap_))) &&
        SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&gpu_time_readback_))) &&
        SUCCEEDED(gpu_time_readback_->Map(0, nullptr, &mapped)) &&
        SUCCEEDED(direct_queue->GetTimestampFrequency(&gpu_time_frequency_))) {
      gpu_time_mapped_ = static_cast<const uint64_t*>(mapped);
    }
  }''')
patch(C, '''    job.allocator->Reset();
    command_list_->Reset(job.allocator, nullptr);
    job.list->Execute(command_list_, command_list_1_);
    command_list_->Close();''', '''    job.allocator->Reset();
    command_list_->Reset(job.allocator, nullptr);
    const uint32_t ts_slot = uint32_t(job.submission % kGpuTimeSlots);
    if (gpu_time_mapped_) command_list_->EndQuery(gpu_time_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ts_slot * 2);
    job.list->Execute(command_list_, command_list_1_);
    if (gpu_time_mapped_) {
      command_list_->EndQuery(gpu_time_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ts_slot * 2 + 1);
      command_list_->ResolveQueryData(gpu_time_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ts_slot * 2, 2, gpu_time_readback_,
                                      uint64_t(ts_slot) * 2 * sizeof(uint64_t));
      gpu_time_pending_.push_back(job.submission);
    }
    command_list_->Close();''')
patch(C, '''    ::fable2::ngpu::rtc::NoteSubmissionExecuted(job.submission);
    submit_done_cv_.notify_all();''', '''    ::fable2::ngpu::rtc::NoteSubmissionExecuted(job.submission);
    submit_done_cv_.notify_all();
    GpuTimeCollect(false);''')

# facade: count swap submissions
patch(X + 'facade.h', '''void NoteSwapSubmission(uint64_t submission);''', '''void NoteSwapSubmission(uint64_t submission);
uint64_t SwapSubmissionsNoted();   // how many swaps have been noted (frames, for per-frame reports)''')
patch(X + 'facade.cpp', '''void NoteSwapSubmission(uint64_t submission) { g_swap_submission.store(submission); }''', '''std::atomic<uint64_t> g_swaps_noted{0};
void NoteSwapSubmission(uint64_t submission) { g_swap_submission.store(submission); g_swaps_noted.fetch_add(1); }
uint64_t SwapSubmissionsNoted() { return g_swaps_noted.load(); }''')
print('ok')
