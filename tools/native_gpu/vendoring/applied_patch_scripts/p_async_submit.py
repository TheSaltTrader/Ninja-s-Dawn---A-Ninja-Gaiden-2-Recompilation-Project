R = r'C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/'
def patch(p, old, new, count=1):
    s = open(R + p, encoding='utf-8').read()
    n = s.count(old)
    assert n == count, (p, old[:70], n)
    open(R + p, 'w', encoding='utf-8').write(s.replace(old, new))

X = 'native_gpu_xlat/rtc_d3d12/'

# --- DeferredCommandList: hand a recording to another instance --------------------------------------------------
patch(X + 'deferred_command_list.h', '''  void Reset();
  void Execute(ID3D12GraphicsCommandList* command_list, ID3D12GraphicsCommandList1* command_list_1);
''', '''  void Reset();
  void Execute(ID3D12GraphicsCommandList* command_list, ID3D12GraphicsCommandList1* command_list_1);
  // [async submit] Exchanges the recorded stream with another list (capacities travel with it), so a finished
  // submission can be executed on the submit thread while this one records the next.
  void SwapRecording(DeferredCommandList& other) {
    command_stream_.swap(other.command_stream_);
    std::swap(execute_submission_, other.execute_submission_);
  }
''')

# --- command processor header -------------------------------------------------------------------------------------
H = X + 'command_processor.h'
patch(H, '#include <atomic>\n#include <deque>\n', '#include <atomic>\n#include <condition_variable>\n#include <deque>\n#include <mutex>\n#include <thread>\n')
patch(H, '''  bool AwaitAllQueueOperationsCompletion() {''', '''  // [async submit] (NATIVE PATCH, 2026-09-26) EndSubmission hands the recorded deferred list to a submit thread that
  // replays it into the D3D12 command list and submits (12% of the GPU thread in town, PROFT3). Direct-queue work
  // that must stay ordered after earlier submissions calls DrainSubmissions first.
  void DrainSubmissions();
  uint64_t LastQueuedSubmission() const { return submission_current_ ? submission_current_ - 1 : 0; }

  bool AwaitAllQueueOperationsCompletion() {''')
s = open(R + H, encoding='utf-8').read()
anchor = '  DeferredCommandList deferred_command_list_;'
assert s.count(anchor) == 1, 'deferred member'
s = s.replace(anchor, anchor + '''
  // [async submit]
  struct SubmitJob {
    std::unique_ptr<DeferredCommandList> list;
    ID3D12CommandAllocator* allocator = nullptr;
    uint64_t submission = 0;
  };
  void SubmitThreadMain();
  bool async_submit_ = false;
  std::thread submit_thread_;
  std::mutex submit_mutex_;
  std::condition_variable submit_cv_, submit_done_cv_;
  std::deque<SubmitJob> submit_jobs_;
  std::vector<std::unique_ptr<DeferredCommandList>> submit_free_lists_;
  uint64_t submit_queued_through_ = 0, submit_done_through_ = 0;
  bool submit_quit_ = false;''')
open(R + H, 'w', encoding='utf-8').write(s)

# --- command processor: thread, EndSubmission, shutdown -----------------------------------------------------------
C = X + 'command_processor.cpp'
patch(C, '''  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  // Optional - added in Creators Update (SDK 10.0.15063.0).
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));
''', '''  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  // Optional - added in Creators Update (SDK 10.0.15063.0).
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));
  async_submit_ = ::fable2::ngpu::rtc::AsyncSubmitEnabled();   // NATIVE PATCH
  if (async_submit_) {
    submit_quit_ = false;
    submit_thread_ = std::thread([this] { SubmitThreadMain(); });
    REXGPU_INFO("[ngpu] BACKEND: submissions are executed on their own thread (ngpu_backend_async_submit)");
  }
''')
patch(C, '''void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();''', '''void D3D12CommandProcessor::SubmitThreadMain() {
  ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
  for (;;) {
    SubmitJob job;
    {
      std::unique_lock<std::mutex> lk(submit_mutex_);
      submit_cv_.wait(lk, [this] { return submit_quit_ || !submit_jobs_.empty(); });
      if (submit_jobs_.empty()) return;
      job = std::move(submit_jobs_.front());
      submit_jobs_.pop_front();
    }
    // The allocator and command_list_ belong to this thread from EndSubmission until the fence value is signalled;
    // the GPU thread reuses an allocator only once GetCompletedSubmission has passed its submission.
    job.allocator->Reset();
    command_list_->Reset(job.allocator, nullptr);
    job.list->Execute(command_list_, command_list_1_);
    command_list_->Close();
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
    direct_queue->Signal(submission_fence_, job.submission);
    job.list->Reset();
    {
      std::lock_guard<std::mutex> lk(submit_mutex_);
      submit_free_lists_.push_back(std::move(job.list));
      submit_done_through_ = job.submission;
    }
    ::fable2::ngpu::rtc::NoteSubmissionExecuted(job.submission);
    submit_done_cv_.notify_all();
  }
}

void D3D12CommandProcessor::DrainSubmissions() {
  if (!async_submit_) return;
  std::unique_lock<std::mutex> lk(submit_mutex_);
  submit_done_cv_.wait(lk, [this] { return submit_done_through_ >= submit_queued_through_; });
}

void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  if (submit_thread_.joinable()) {
    DrainSubmissions();
    {
      std::lock_guard<std::mutex> lk(submit_mutex_);
      submit_quit_ = true;
    }
    submit_cv_.notify_all();
    submit_thread_.join();
  }''')
patch(C, '''    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    command_allocator->Reset();
    command_list_->Reset(command_allocator, nullptr);
    deferred_command_list_.SetExecuteSubmission(submission_current_);
    deferred_command_list_.Execute(command_list_, command_list_1_);
    command_list_->Close();
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
''', '''    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    deferred_command_list_.SetExecuteSubmission(submission_current_);
    if (async_submit_) {
      // [async submit] The recording moves to a pooled list the submit thread executes; this one starts empty.
      std::unique_ptr<DeferredCommandList> job_list;
      {
        std::lock_guard<std::mutex> lk(submit_mutex_);
        if (!submit_free_lists_.empty()) {
          job_list = std::move(submit_free_lists_.back());
          submit_free_lists_.pop_back();
        }
      }
      if (!job_list) job_list = std::make_unique<DeferredCommandList>(*this);
      job_list->SwapRecording(deferred_command_list_);
      deferred_command_list_.Reset();
      {
        std::lock_guard<std::mutex> lk(submit_mutex_);
        submit_jobs_.push_back(SubmitJob{std::move(job_list), command_allocator, submission_current_});
        submit_queued_through_ = submission_current_;
      }
      submit_cv_.notify_one();
    } else {
      command_allocator->Reset();
      command_list_->Reset(command_allocator, nullptr);
      deferred_command_list_.Execute(command_list_, command_list_1_);
      command_list_->Close();
      ID3D12CommandList* execute_command_lists[] = {command_list_};
      direct_queue->ExecuteCommandLists(1, execute_command_lists);
    }
''')
patch(C, '''    direct_queue->Signal(submission_fence_, submission_current_++);
''', '''    if (async_submit_) {
      ++submission_current_;   // the submit thread signals this value after executing the list
    } else {
      direct_queue->Signal(submission_fence_, submission_current_++);
    }
''')
# Queue operations that the queue orders against submissions: wait for queued submissions first.
patch(X + 'shared_memory.cpp', '''  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();''', '''  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();
  command_processor_.DrainSubmissions();   // NATIVE PATCH: [async submit] keep queue order with earlier submissions''')
patch(X + 'texture_cache.cpp', '''    auto direct_queue = provider.GetDirectQueue();''', '''    auto direct_queue = provider.GetDirectQueue();
    command_processor_.DrainSubmissions();   // NATIVE PATCH: [async submit] keep queue order with earlier submissions''')
patch(C, '''    if (queue_operations_done_since_submission_signal_) {
      UINT64 fence_value = ++queue_operations_since_submission_fence_last_;''', '''    if (queue_operations_done_since_submission_signal_) {
      DrainSubmissions();   // NATIVE PATCH: [async submit]
      UINT64 fence_value = ++queue_operations_since_submission_fence_last_;''')

# --- facade: switch + the present's wait --------------------------------------------------------------------------
patch(X + 'facade.h', 'rex::memory::BaseHeap* GuestPhysicalHeap(rex::memory::Memory* memory);', '''rex::memory::BaseHeap* GuestPhysicalHeap(rex::memory::Memory* memory);
// [async submit] ngpu_backend_async_submit (read once). The backend's swap notes its submission; the native frame
// waits until the submit thread has executed it before sampling the guest output on the same queue.
bool AsyncSubmitEnabled();
void NoteSubmissionExecuted(uint64_t submission);
void NoteSwapSubmission(uint64_t submission);
bool WaitSwapSubmitted(uint32_t timeout_ms);''')
patch(X + 'facade.cpp', '''NativeContext& Native() {''', '''bool NgpuBackendAsyncSubmitCvar();   // native_gpu_present.cpp (the cvar lives with the other ngpu_backend ones)
bool AsyncSubmitEnabled() {
  static const bool on = NgpuBackendAsyncSubmitCvar();
  return on;
}
namespace {
std::atomic<uint64_t> g_executed{0}, g_swap_submission{0};
std::mutex g_exec_mu;
std::condition_variable g_exec_cv;
}  // namespace
void NoteSubmissionExecuted(uint64_t submission) {
  { std::lock_guard<std::mutex> lk(g_exec_mu); g_executed.store(submission); }
  g_exec_cv.notify_all();
}
void NoteSwapSubmission(uint64_t submission) { g_swap_submission.store(submission); }
bool WaitSwapSubmitted(uint32_t timeout_ms) {
  if (!AsyncSubmitEnabled()) return true;
  const uint64_t want = g_swap_submission.load();
  std::unique_lock<std::mutex> lk(g_exec_mu);
  return g_exec_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [want] { return g_executed.load() >= want; });
}
NativeContext& Native() {''')
patch(X + 'facade.cpp', '#include <windows.h>\n', '#include <windows.h>\n\n#include <atomic>\n#include <chrono>\n#include <condition_variable>\n#include <mutex>\n')

# --- backend swap notes its submission; present waits; cvar ----------------------------------------------------
P = 'native_gpu_present.cpp'
patch(P, '''REXCVAR_DEFINE_INT32(ngpu_backend_selfcheck_every,''', '''REXCVAR_DEFINE_BOOL(ngpu_backend_async_submit, true, "GPU", "Native-GPU BACKEND (read at startup): the backend's submissions (deferred command list replay + ExecuteCommandLists) run on their own thread instead of the plugin's GPU thread");
namespace fable2::ngpu::rtc { bool NgpuBackendAsyncSubmitCvar() { return REXCVAR_GET(ngpu_backend_async_submit); } }
REXCVAR_DEFINE_INT32(ngpu_backend_selfcheck_every,''')
patch(P, '''  g_s.queue->executeCommandLists(&cl, 1, &wait, 1, &sig, 1, g_s.fence.get());
  g_s.swap->present(g_s.frame_index, &sig, 1);''', '''  // [async submit] the backend's swap submission (the guest output this frame samples) must be on the queue first.
  if (g_backend_on) fable2::ngpu::rtc::WaitSwapSubmitted(100);
  g_s.queue->executeCommandLists(&cl, 1, &wait, 1, &sig, 1, g_s.fence.get());
  g_s.swap->present(g_s.frame_index, &sig, 1);''')
print('ok')
patch('native_gpu_backend.cpp', '''    ++stats_.swaps;
    cp_->IssueSwap(fb, w, h);
  }''', '''    ++stats_.swaps;
    cp_->IssueSwap(fb, w, h);
    ::fable2::ngpu::rtc::NoteSwapSubmission(cp_->LastQueuedSubmission());   // [async submit] the frame waits for it
  }''')
print('backend ok')
