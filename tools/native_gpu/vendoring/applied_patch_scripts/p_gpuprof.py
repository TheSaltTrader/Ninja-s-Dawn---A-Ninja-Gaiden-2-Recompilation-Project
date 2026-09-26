R = r'C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/'
def patch(p, old, new, count=1):
    s = open(R + p, encoding='utf-8').read()
    n = s.count(old)
    assert n == count, (p, old[:70], n)
    open(R + p, 'w', encoding='utf-8').write(s.replace(old, new))
X = 'native_gpu_xlat/rtc_d3d12/'
H = X + 'command_processor.h'
C = X + 'command_processor.cpp'

# ---- header: public API + members ------------------------------------------------------------------------------
patch(H, '''  // [async submit] (NATIVE PATCH) see SubmitThreadMain; queue-order sites call DrainSubmissions first.''', '''  // [gpu prof] (NATIVE PATCH, ngpu_gpu_prof) GPU time by category: a timestamp is recorded into the deferred list
  // whenever the category changes; the submit thread resolves them and reports ms per frame per category.
  enum GpuCat : uint8_t { kGpuCatOther, kGpuCatDraw, kGpuCatTexLoad, kGpuCatUpload, kGpuCatResolve, kGpuCatRtXfer,
                          kGpuCatReadback, kGpuCatOutput, kGpuCatCount };
  uint8_t GpuCatSet(uint8_t cat);
  struct GpuCatScope {
    D3D12CommandProcessor& cp;
    uint8_t prev;
    GpuCatScope(D3D12CommandProcessor& p, uint8_t cat) : cp(p), prev(p.GpuCatSet(cat)) {}
    ~GpuCatScope() { cp.GpuCatSet(prev); }
  };
  // [async submit] (NATIVE PATCH) see SubmitThreadMain; queue-order sites call DrainSubmissions first.''')
patch(H, '''    uint64_t submission = 0;
  };''', '''    uint64_t submission = 0;
    std::vector<uint8_t> prof_cats;   // [gpu prof] category of each timestamp interval
    uint32_t prof_slot = 0, prof_used = 0;
  };''')
patch(H, '''  void GpuTimeCollect(bool all);''', '''  void GpuTimeCollect(bool all);
  static constexpr uint32_t kGpuProfSlots = 8, kGpuProfPerSlot = 8192;
  bool gpu_prof_ = false;
  ID3D12QueryHeap* gpu_prof_heap_ = nullptr;
  ID3D12Resource* gpu_prof_readback_ = nullptr;
  const uint64_t* gpu_prof_mapped_ = nullptr;
  uint8_t gpu_prof_cat_ = 0;
  std::vector<uint8_t> gpu_prof_cats_;
  uint32_t gpu_prof_slot_ = 0, gpu_prof_used_ = 0, gpu_prof_overflow_ = 0;
  struct GpuProfPending { uint64_t submission; uint32_t slot, used; std::vector<uint8_t> cats; };
  std::deque<GpuProfPending> gpu_prof_pending_;
  uint64_t gpu_prof_ticks_[kGpuCatCount] = {};
  void GpuProfBegin();''')

# ---- cpp: set/begin/end ---------------------------------------------------------------------------------------
patch(C, '''void D3D12CommandProcessor::GpuTimeCollect(bool all) {''', '''uint8_t D3D12CommandProcessor::GpuCatSet(uint8_t cat) {
  const uint8_t prev = gpu_prof_cat_;
  if (cat == prev) return prev;
  gpu_prof_cat_ = cat;
  if (gpu_prof_ && submission_open_ && gpu_prof_heap_) {
    if (gpu_prof_used_ + 1 < kGpuProfPerSlot) {
      deferred_command_list_.D3DEndQuery(gpu_prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP,
                                         gpu_prof_slot_ * kGpuProfPerSlot + gpu_prof_used_);
      gpu_prof_cats_.push_back(cat);
      ++gpu_prof_used_;
    } else {
      ++gpu_prof_overflow_;
    }
  }
  return prev;
}

void D3D12CommandProcessor::GpuProfBegin() {
  if (!gpu_prof_ || !gpu_prof_heap_) return;
  gpu_prof_slot_ = uint32_t(submission_current_ % kGpuProfSlots);
  gpu_prof_used_ = 0;
  gpu_prof_cats_.clear();
  deferred_command_list_.D3DEndQuery(gpu_prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP, gpu_prof_slot_ * kGpuProfPerSlot);
  gpu_prof_cats_.push_back(gpu_prof_cat_);
  gpu_prof_used_ = 1;
}

void D3D12CommandProcessor::GpuTimeCollect(bool all) {
  if (gpu_prof_mapped_) {
    const uint64_t done = submission_fence_->GetCompletedValue();
    while (!gpu_prof_pending_.empty() && gpu_prof_pending_.front().submission <= done) {
      const GpuProfPending& p = gpu_prof_pending_.front();
      const uint64_t* ts = gpu_prof_mapped_ + size_t(p.slot) * kGpuProfPerSlot;
      for (uint32_t i = 0; i + 1 < p.used && i < p.cats.size(); ++i)
        if (ts[i + 1] > ts[i]) gpu_prof_ticks_[p.cats[i] < kGpuCatCount ? p.cats[i] : 0] += ts[i + 1] - ts[i];
      gpu_prof_pending_.pop_front();
    }
  }''')
# the report: add the category line where GPU TIME is reported
patch(C, '''    gpu_time_ticks_ = 0;
    gpu_time_submissions_ = 0;''', '''    if (gpu_prof_mapped_ && frames) {
      auto cat_ms = [&](int c) { return double(gpu_prof_ticks_[c]) * 1000.0 / double(gpu_time_frequency_) / double(frames); };
      REXGPU_INFO("[ngpu] GPU PROF per frame: draws {:.2f} ms, texture loads {:.2f}, uploads {:.2f}, resolves {:.2f}, "
                  "render-target transfers/clears {:.2f}, readback copies {:.2f}, output {:.2f}, other {:.2f} "
                  "({} timestamp overflows)",
                  cat_ms(kGpuCatDraw), cat_ms(kGpuCatTexLoad), cat_ms(kGpuCatUpload), cat_ms(kGpuCatResolve),
                  cat_ms(kGpuCatRtXfer), cat_ms(kGpuCatReadback), cat_ms(kGpuCatOutput), cat_ms(kGpuCatOther),
                  gpu_prof_overflow_);
      for (auto& t : gpu_prof_ticks_) t = 0;
    }
    gpu_time_ticks_ = 0;
    gpu_time_submissions_ = 0;''')
# submit thread: prof readback + pending
patch(C, '''      gpu_time_mapped_ = static_cast<const uint64_t*>(mapped);
    }
  }''', '''      gpu_time_mapped_ = static_cast<const uint64_t*>(mapped);
    }
    if (gpu_prof_ && gpu_prof_readback_) {
      void* pm = nullptr;
      if (SUCCEEDED(gpu_prof_readback_->Map(0, nullptr, &pm))) gpu_prof_mapped_ = static_cast<const uint64_t*>(pm);
    }
  }''')
patch(C, '''      gpu_time_pending_.push_back(job.submission);
    }''', '''      gpu_time_pending_.push_back(job.submission);
    }
    if (job.prof_used > 1) gpu_prof_pending_.push_back({job.submission, job.prof_slot, job.prof_used, std::move(job.prof_cats)});''')
# EndSubmission: final timestamp + resolve, hand the categories to the job
patch(C, '''      if (!job_list) job_list = std::make_unique<DeferredCommandList>(*this);
      job_list->SwapRecording(deferred_command_list_);''', '''      if (!job_list) job_list = std::make_unique<DeferredCommandList>(*this);
      uint32_t prof_used = 0;
      if (gpu_prof_ && gpu_prof_heap_ && gpu_prof_used_) {
        const uint32_t base = gpu_prof_slot_ * kGpuProfPerSlot;
        deferred_command_list_.D3DEndQuery(gpu_prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP, base + gpu_prof_used_);
        prof_used = gpu_prof_used_ + 1;
        deferred_command_list_.D3DResolveQueryData(gpu_prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP, base, prof_used,
                                                   gpu_prof_readback_, uint64_t(base) * sizeof(uint64_t));
      }
      job_list->SwapRecording(deferred_command_list_);''')
patch(C, '''        submit_jobs_.push_back(SubmitJob{std::move(job_list), command_allocator, submission_current_});''', '''        submit_jobs_.push_back(SubmitJob{std::move(job_list), command_allocator, submission_current_,
                                         std::move(gpu_prof_cats_), gpu_prof_slot_, prof_used});
        gpu_prof_cats_ = {};
        gpu_prof_used_ = 0;''')
# BeginSubmission: first timestamp once the new deferred list is reset
patch(C, '''    deferred_command_list_.Reset();

    // Reset cached state of the command list.''', '''    deferred_command_list_.Reset();
    GpuProfBegin();   // NATIVE PATCH: [gpu prof]

    // Reset cached state of the command list.''')
# create heap+readback in SetupContext (next to the async switch)
patch(C, '''  async_submit_ = ::fable2::ngpu::rtc::AsyncSubmitEnabled();   // NATIVE PATCH''', '''  async_submit_ = ::fable2::ngpu::rtc::AsyncSubmitEnabled();   // NATIVE PATCH
  gpu_prof_ = async_submit_ && ::fable2::ngpu::rtc::GpuProfEnabled();   // NATIVE PATCH: [gpu prof]
  if (gpu_prof_) {
    D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kGpuProfSlots * kGpuProfPerSlot, 0};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = uint64_t(kGpuProfSlots) * kGpuProfPerSlot * sizeof(uint64_t);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&gpu_prof_heap_))) ||
        FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&gpu_prof_readback_)))) {
      gpu_prof_ = false;
    } else {
      REXGPU_INFO("[ngpu] GPU PROF: timestamping GPU work by category (ngpu_gpu_prof)");
    }
  }''')
# scopes at the category entry points
patch(C, '''bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {''', '''bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  GpuCatScope gpu_cat(*this, kGpuCatDraw);   // NATIVE PATCH: [gpu prof]''')
patch(C, '''bool D3D12CommandProcessor::IssueCopy_ReadbackResolvePath() {''', '''bool D3D12CommandProcessor::IssueCopy_ReadbackResolvePath() {
  GpuCatScope gpu_cat(*this, kGpuCatReadback);   // NATIVE PATCH: [gpu prof] (the resolve itself scopes RESOLVE)''')
patch(C, '''void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {''', '''void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  GpuCatScope gpu_cat(*this, kGpuCatOutput);   // NATIVE PATCH: [gpu prof]''')
patch(X + 'render_target_cache.cpp', '''                                     uint32_t& written_address_out, uint32_t& written_length_out) {
  written_address_out = 0;''', '''                                     uint32_t& written_address_out, uint32_t& written_length_out) {
  D3D12CommandProcessor::GpuCatScope gpu_cat(command_processor_, D3D12CommandProcessor::kGpuCatResolve);   // NATIVE PATCH
  written_address_out = 0;''')
patch(X + 'render_target_cache.cpp', '''    const Transfer::Rectangle* resolve_clear_rectangle) {''', '''    const Transfer::Rectangle* resolve_clear_rectangle) {
  D3D12CommandProcessor::GpuCatScope gpu_cat(command_processor_, D3D12CommandProcessor::kGpuCatRtXfer);   // NATIVE PATCH''')
patch(X + 'texture_cache.cpp', '''                                                              bool load_mips) {
  command_processor_.NoteTextureLoad(  // [hitch]''', '''                                                              bool load_mips) {
  D3D12CommandProcessor::GpuCatScope gpu_cat(command_processor_, D3D12CommandProcessor::kGpuCatTexLoad);   // NATIVE PATCH
  command_processor_.NoteTextureLoad(  // [hitch]''')
patch(X + 'shared_memory.cpp', '''  if (upload_page_ranges.empty()) {
    return true;
  }
  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);''', '''  if (upload_page_ranges.empty()) {
    return true;
  }
  D3D12CommandProcessor::GpuCatScope gpu_cat(command_processor_, D3D12CommandProcessor::kGpuCatUpload);   // NATIVE PATCH
  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);''')
# facade + cvar
patch(X + 'facade.h', '''bool AsyncSubmitEnabled();''', '''bool AsyncSubmitEnabled();
bool GpuProfEnabled();   // ngpu_gpu_prof (read once)''')
patch(X + 'facade.cpp', '''bool NgpuBackendAsyncSubmitCvar();''', '''bool NgpuGpuProfCvar();
bool GpuProfEnabled() {
  static const bool on = NgpuGpuProfCvar();
  return on;
}
bool NgpuBackendAsyncSubmitCvar();''')
patch('native_gpu_present.cpp', '''namespace fable2::ngpu::rtc { bool NgpuBackendAsyncSubmitCvar() { return REXCVAR_GET(ngpu_backend_async_submit); } }''', '''namespace fable2::ngpu::rtc { bool NgpuBackendAsyncSubmitCvar() { return REXCVAR_GET(ngpu_backend_async_submit); } }
REXCVAR_DEFINE_BOOL(ngpu_gpu_prof, false, "GPU", "Native-GPU BACKEND (read once): time the backend's GPU work by category (draws, texture loads, uploads, resolves, render-target transfers, readback copies, output) with timestamp queries - reported every 5 s beside GPU TIME");
namespace fable2::ngpu::rtc { bool NgpuGpuProfCvar() { return REXCVAR_GET(ngpu_gpu_prof); } }''')
print('ok')
