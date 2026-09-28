// FULL NATIVE, P5 step 2: the game's own graphics system. See ng2_native_gs.h.
#include "ng2_native_gs.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

#include "ng2_p2_census.h"

namespace ng2::gs {
namespace {

using rex::X_STATUS;
std::atomic<bool> g_active{false};

inline uint32_t GpuSwap(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2: return _byteswap_ulong(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
  }
}

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) return X_STATUS_SUCCESS;
    if (!provider_) {
      provider_ = rex::ui::d3d12::D3D12Provider::Create();
      if (!provider_) {
        REXLOG_ERROR("[gs] no Direct3D 12 provider");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    app_context_ = app_context;
    auto loss = [](bool, bool) { rex::FatalError("Graphics device lost (native graphics system)"); };
    if (app_context_)
      app_context_->CallInUIThreadSynchronous([this, loss]() { presenter_ = provider_->CreatePresenter(loss); });
    else
      presenter_ = provider_->CreatePresenter(loss);
    if (!presenter_) {
      REXLOG_ERROR("[gs] no presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[gs] NATIVE GRAPHICS SYSTEM: Direct3D 12 provider and presenter up (no rexgpu-xenos graphics system)");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override {
    function_dispatcher_ = function_dispatcher;
    kernel_state_ = kernel_state;
    memory_ = function_dispatcher->memory();
    if (!provider_) provider_ = rex::ui::d3d12::D3D12Provider::Create();
    std::memset(regs_, 0, sizeof(regs_));
    // The GPU register window, as the plugin mapped it (0x7FC80000, 64 KB).
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
                                   reinterpret_cast<rex::runtime::MMIOReadCallback>(&ReadThunk),
                                   reinterpret_cast<rex::runtime::MMIOWriteCallback>(&WriteThunk));
    running_ = true;
    // The executor: the side effects the front end hands over, on a guest thread (interrupts run the game's handler
    // on the dispatching thread, as the hardware's interrupt context did).
    exec_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { ExecutorMain(); return 0; }));
    exec_thread_->set_name("GPU Native Executor");
    exec_thread_->Create();
    // The vblank worker, as the plugin's.
    vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { VsyncMain(); return 0; }));
    vsync_thread_->set_name("GPU VSync");
    vsync_thread_->Create();
    ng2::p2::StartNativeFrontEnd();
    REXLOG_INFO("[gs] guest GPU wired: register window 7FC80000, executor and vblank threads; the front end decodes "
                "the ring at each kick");
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    irq_cb_ = callback;
    irq_data_ = user_data;
    REXLOG_INFO("[gs] SetInterruptCallback({:08X}, {:08X})", callback, user_data);
  }
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    ring_ptr_ = ptr;
    ring_bytes_ = uint32_t(1) << (size_log2 + 3);
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    read_ptr_ = 0;
    ng2::p2::NativeKick(ring_ptr_, ring_bytes_, 0xFFFFFFFFu);   // the front end restarts its read index
    REXLOG_INFO("[gs] InitializeRingBuffer ptr {:08X} size {:08X}", ptr, ring_bytes_);
  }
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    rptr_writeback_ = ptr;
    REXLOG_INFO("[gs] EnableReadPointerWriteBack ptr {:08X} block_size_log2 {}", ptr, block_size_log2);
  }
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id, bool) override {
    std::lock_guard<std::mutex> lock(storage_mu_);
    storage_root_ = cache_root;
    storage_title_ = title_id;
    storage_set_ = true;
    REXLOG_INFO("[gs] shader storage: {:08X} under {}", title_id, cache_root.string());
  }
  void Shutdown() override {
    if (!running_.exchange(false)) {
      presenter_.reset();
      provider_.reset();
      return;
    }
    cv_.notify_all();
    if (exec_thread_) { exec_thread_->Wait(0, 0, 0, nullptr); exec_thread_.reset(); }
    if (vsync_thread_) { vsync_thread_->Wait(0, 0, 0, nullptr); vsync_thread_.reset(); }
    if (presenter_) {
      if (app_context_) app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      presenter_.reset();
    }
    provider_.reset();
  }

  void Push(const uint32_t* recs, uint32_t count4) {
    {
      std::lock_guard<std::mutex> lock(q_mu_);
      const size_t at = q_.size();
      q_.insert(q_.end(), recs, recs + size_t(count4) * 4);
      const uint32_t ep = epoch_.load(std::memory_order_acquire);
      for (size_t k = at; k < q_.size(); k += 4)
        if (q_[k] == 3) q_[k + 2] = ep;   // read pointers carry the ring epoch they were produced in
    }
    cv_.notify_one();
  }
  bool Storage(std::filesystem::path& root, uint32_t& title) {
    std::lock_guard<std::mutex> lock(storage_mu_);
    if (!storage_set_) return false;
    root = storage_root_;
    title = storage_title_;
    return true;
  }
  ID3D12Device* device() const {
    auto* p = static_cast<rex::ui::d3d12::D3D12Provider*>(provider_.get());
    return p ? p->GetDevice() : nullptr;
  }
  ID3D12CommandQueue* queue() const {
    auto* p = static_cast<rex::ui::d3d12::D3D12Provider*>(provider_.get());
    return p ? p->GetDirectQueue() : nullptr;
  }

 private:
  static uint32_t ReadThunk(void*, NativeGraphicsSystem* gs, uint32_t addr) { return gs->ReadRegister(addr); }
  static void WriteThunk(void*, NativeGraphicsSystem* gs, uint32_t addr, uint32_t value) { gs->WriteRegister(addr, value); }

  uint32_t ReadRegister(uint32_t addr) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    switch (r) {
      case 0x0F00: return 0x08100748;   // RB_EDRAM_TIMING
      case 0x0F01: return 0x0000200E;   // RB_BC_CONTROL
      case 0x194C: {                    // R500_D1MODE_V_COUNTER
        rex::system::X_VIDEO_MODE vm;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&vm);
        return std::min(uint32_t(vm.display_height), uint32_t(0x0FFF));
      }
      case 0x1951: return 1;            // interrupt status: vblank
      case 0x1961: {                    // AVIVO_D1MODE_VIEWPORT_SIZE
        rex::system::X_VIDEO_MODE vm;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&vm);
        return (std::min(uint32_t(vm.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(vm.display_height), uint32_t(0x0FFF));
      }
      default: break;
    }
    return r < kRegs ? regs_[r] : 0;
  }
  void WriteRegister(uint32_t addr, uint32_t value) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    if (r == 0x01C5) {   // CP_RB_WPTR: the kick - the front end decodes it now, on this (the kicking) thread
      ng2::p2::NativeKick(ring_ptr_, ring_bytes_, value);
      cv_.notify_one();
    }
    if (r < kRegs) regs_[r] = value;
  }

  // A register the command stream wrote, with the command processor's side effects (the plugin's
  // CommandProcessor::WriteRegister): scratch registers are mirrored to memory at SCRATCH_ADDR when their bit is set
  // in SCRATCH_UMSK - the flags the game's GPU waits poll (1F039002 / 1F039006 on NG2; without this every wait timed
  // out and the frame took 400 ms, leg gs1). COHER_STATUS_HOST: this executor completes coherency at once, so the
  // busy bit the plugin set and later cleared is never set.
  void WriteGuestRegister(uint32_t reg, uint32_t value) {
    if (reg >= kRegs) return;
    if (reg == 0x0A31) value &= ~0x80000000u;   // COHER_STATUS_HOST
    regs_[reg] = value;
    if (reg >= 0x0578 && reg <= 0x057F) {       // SCRATCH_REG0..7
      const uint32_t n = reg - 0x0578;
      if ((1u << n) & regs_[0x01DC]) {          // SCRATCH_UMSK
        if (auto* p = memory_->TranslatePhysical<uint8_t*>(regs_[0x01DD] + n * 4)) {   // SCRATCH_ADDR
          const uint32_t raw = _byteswap_ulong(value);
          std::memcpy(p, &raw, 4);
        }
      }
    }
  }

  void Dispatch(uint32_t source, uint32_t cpu) {
    if (!irq_cb_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    thread->SetActiveCpu(uint8_t(cpu == 0xFFFFFFFFu ? 2 : cpu));
    uint64_t args[] = {source, irq_data_};
    function_dispatcher_->ExecuteInterrupt(thread->thread_state(), irq_cb_, args, 2);
  }

  void VsyncMain() {
    rex::system::X_VIDEO_MODE vm;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&vm);
    const double hz = std::max(1.0, double(float(vm.refresh_rate)));
    const uint64_t f = rex::chrono::Clock::guest_tick_frequency();
    const uint64_t vs = std::max<uint64_t>(1, uint64_t(double(f) / hz));
    const uint64_t novs = std::max<uint64_t>(1, f / 1000);
    uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
    uint32_t n = 0;
    bool vsync = true;
    while (running_) {
      if ((n++ & 255) == 0) vsync = rex::cvar::GetFlagByName("vsync") != "false";
      const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
      const uint64_t iv = vsync ? vs : novs;
      while (now - last >= iv) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        Dispatch(0, 2);
        last += iv;
      }
      Sleep(1);
    }
  }

  void ExecutorMain() {
    std::vector<uint32_t> batch;
    uint64_t batches = 0, irqs = 0, fences = 0, swaps = 0, stale = 0, timeouts = 0, regs = 0, queries = 0;
    while (running_) {
      {
        std::unique_lock<std::mutex> lock(q_mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(2), [this] { return !q_.empty() || !running_; });
        batch.swap(q_);
      }
      if (batch.empty()) continue;
      ++batches;
      const size_t n4 = batch.size() / 4;
      for (size_t r = 0; r < n4; ++r) {
        const uint32_t* e = batch.data() + r * 4;
        switch (e[0]) {
          case 1:
          case 2: {
            const uint32_t v = e[0] == 2 ? counter_.load(std::memory_order_relaxed) : e[2];
            if (auto* p = memory_->TranslatePhysical<uint8_t*>(e[1] & ~3u)) {
              const uint32_t raw = GpuSwap(v, e[1] & 3);
              std::memcpy(p, &raw, 4);
            }
            ++fences;
          } break;
          case 3:
            if (e[2] != epoch_.load(std::memory_order_acquire)) { ++stale; break; }
            read_ptr_ = e[1];
            if (rptr_writeback_)
              if (auto* p = memory_->TranslatePhysical<uint8_t*>(rptr_writeback_)) {
                const uint32_t raw = _byteswap_ulong(read_ptr_);
                std::memcpy(p, &raw, 4);
              }
            break;
          case 4:
            for (int k = 0; k < 6; ++k)
              if (e[1] & (1u << k)) Dispatch(1, uint32_t(k));
            ++irqs;
            break;
          case 5:
            counter_.fetch_add(1, std::memory_order_relaxed);   // the swap (the picture is presented on the guest thread)
            if ((++swaps % 600) == 0)
              REXLOG_INFO("[gs] executor: {} batches, {} swaps, {} interrupts, {} fences, {} register writes, {} "
                          "occlusion queries; stale read pointers {}, wait timeouts {}", batches, swaps, irqs, fences,
                          regs, queries, stale, timeouts);
            break;
          case 6: {
            const uint32_t wi = e[1], poll = e[2], ref = e[3];
            const uint32_t mask = (r + 1 < n4 && batch[(r + 1) * 4] == 0) ? batch[(r + 1) * 4 + 1] : 0xFFFFFFFFu;
            const auto t0 = std::chrono::steady_clock::now();
            for (;;) {
              uint32_t v = 0;
              if (auto* p = memory_->TranslatePhysical<const uint8_t*>(poll & ~3u)) {
                uint32_t raw = 0;
                std::memcpy(&raw, p, 4);
                v = GpuSwap(raw, poll & 3);
              }
              bool m = true;
              switch (wi & 7) {
                case 1: m = (v & mask) < ref; break;
                case 2: m = (v & mask) <= ref; break;
                case 3: m = (v & mask) == ref; break;
                case 4: m = (v & mask) != ref; break;
                case 5: m = (v & mask) >= ref; break;
                case 6: m = (v & mask) > ref; break;
                default: m = true; break;
              }
              if (m || !running_) break;
              if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(100)) { ++timeouts; break; }
              SwitchToThread();
            }
          } break;
          case 7:
            WriteGuestRegister(e[1], e[2]);
            ++regs;
            break;
          case 8:   // EVENT_WRITE_ZPD: the plugin's occlusion-query workaround (query_occlusion_fake_sample_count 1000)
            if (auto* sc = memory_->TranslatePhysical<uint32_t*>(e[1])) {
              // Little endian (D3D swaps it); 0xFFFFFEED in a ZPass or ZFail slot marks D3DISSUE_END.
              constexpr uint32_t kFinished = 0xEDFEFFFFu;   // byte_swap(0xFFFFFEED)
              const bool end = sc[4] == kFinished || sc[5] == kFinished || sc[2] == kFinished || sc[3] == kFinished;
              std::memset(sc, 0, 32);
              if (end) { sc[4] = 1000; sc[0] = 1000; }   // ZPass_A, Total_A
              ++queries;
            }
            break;
          case 9:   // EVENT_WRITE_EXT: screen extents, the plugin's fixed box (0..8192 >> 3, z 0..1), 16-bit swapped
            if (auto* p = memory_->TranslatePhysical<uint8_t*>(e[1] & ~3u)) {
              const uint16_t ext[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
              for (int k = 0; k < 6; ++k) {
                const uint16_t raw = _byteswap_ushort(ext[k]);
                std::memcpy(p + k * 2, &raw, 2);
              }
            }
            break;
          default:
            break;
        }
      }
      batch.clear();
    }
  }

  static constexpr uint32_t kRegs = 0x5003;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  uint32_t regs_[kRegs];
  uint32_t ring_ptr_ = 0, ring_bytes_ = 0, rptr_writeback_ = 0, read_ptr_ = 0;
  std::atomic<uint32_t> epoch_{0};
  std::atomic<uint32_t> counter_{0};
  uint32_t irq_cb_ = 0, irq_data_ = 0;
  std::atomic<bool> running_{false};
  std::mutex q_mu_;
  std::condition_variable cv_;
  std::vector<uint32_t> q_;
  rex::system::object_ref<rex::system::XHostThread> exec_thread_, vsync_thread_;
  std::mutex storage_mu_;
  std::filesystem::path storage_root_;
  uint32_t storage_title_ = 0;
  bool storage_set_ = false;
};

NativeGraphicsSystem* g_gs = nullptr;

}  // namespace

bool Requested(bool row_on) {
  static const bool on = [row_on] {
    if (const char* e = std::getenv("NG2_NATIVE_GS"); e && *e) return *e != '0';
    if (const char* g = std::getenv("NG2_NATIVE_GPU"); g && *g) return *g != '0';
    return row_on;
  }();
  return on;
}

std::unique_ptr<rex::system::IGraphicsSystem> Create() {
  if (!rex::ui::d3d12::D3D12Provider::IsD3D12APIAvailable()) return nullptr;
  auto gs = std::make_unique<NativeGraphicsSystem>();
  g_gs = gs.get();
  g_active = true;
  return gs;
}

bool Active() { return g_active.load(); }
ID3D12Device* Device() { return g_gs ? g_gs->device() : nullptr; }
ID3D12CommandQueue* Queue() { return g_gs ? g_gs->queue() : nullptr; }
rex::ui::Presenter* Presenter() { return g_gs ? g_gs->presenter() : nullptr; }
void PushSideEffects(const uint32_t* recs, uint32_t count4) {
  if (g_gs && recs && count4) g_gs->Push(recs, count4);
}
bool ShaderStorage(std::filesystem::path& cache_root, uint32_t& title_id) {
  return g_gs && g_gs->Storage(cache_root, title_id);
}

}  // namespace ng2::gs
