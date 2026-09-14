#include "ng2_perf.h"

#include <windows.h>

#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include <rex/logging.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

namespace ng2 {
namespace {

constexpr int kIntervalMs = 500;

std::mutex g_mutex;
PerfSample g_latest;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_started{false};
std::atomic<uint32_t> g_frames{0};

// --- CPU -------------------------------------------------------------------

// This process's share of all cores. GetProcessTimes gives cumulative kernel +
// user time, so the percentage is a delta over wall time, divided by the core
// count - otherwise a fully busy 16-core machine reads as 1600%.
class CpuMeter {
 public:
  float Sample() {
    FILETIME create, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user))
      return 0.0f;
    const uint64_t busy = Ticks(kernel) + Ticks(user);
    const auto now = std::chrono::steady_clock::now();
    float percent = 0.0f;
    if (have_previous_) {
      const double seconds =
          std::chrono::duration<double>(now - previous_at_).count();
      if (seconds > 0.0) {
        // FILETIME ticks are 100ns.
        const double busy_seconds = double(busy - previous_busy_) / 1e7;
        percent = float(busy_seconds / seconds / double(cores_) * 100.0);
      }
    }
    previous_busy_ = busy;
    previous_at_ = now;
    have_previous_ = true;
    return percent < 0.0f ? 0.0f : (percent > 100.0f ? 100.0f : percent);
  }

 private:
  static uint64_t Ticks(const FILETIME& t) {
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  }
  static uint32_t CoreCount() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
  }
  uint64_t previous_busy_ = 0;
  std::chrono::steady_clock::time_point previous_at_{};
  bool have_previous_ = false;
  uint32_t cores_ = CoreCount();
};

// --- GPU -------------------------------------------------------------------

// "\GPU Engine(*)\Utilization Percentage", summed over the instances belonging
// to this process. Vendor-neutral - it is what Task Manager reports - which
// matters because NVML would cover one vendor and this machine also has an
// integrated GPU that the upscaler happily runs on.
//
// The instances are named pid_<pid>_luid_..._engtype_<kind>, and a process has
// several (3D, Copy, VideoDecode). They are summed rather than maxed: work
// spread across engines is still work, and the 3D engine alone understates a
// frame that spends its time in copies - which is exactly what a texture pack
// does.
class GpuMeter {
 public:
  bool Open() {
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
      query_ = nullptr;
      return false;
    }
    wchar_t filter[64];
    swprintf_s(filter, L"pid_%lu_", GetCurrentProcessId());
    prefix_ = filter;

    // Expand the wildcard once. New engine instances can appear later, so this
    // is refreshed periodically rather than assumed complete.
    Refresh();
    return true;
  }

  void Refresh() {
    if (!query_)
      return;
    for (PDH_HCOUNTER c : counters_)
      PdhRemoveCounter(c);
    counters_.clear();

    DWORD size = 0;
    PDH_STATUS st = PdhExpandWildCardPathW(
        nullptr, L"\\GPU Engine(*)\\Utilization Percentage", nullptr, &size, 0);
    if (st != PDH_MORE_DATA || size == 0)
      return;
    std::vector<wchar_t> buffer(size);
    if (PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*)\\Utilization Percentage",
                               buffer.data(), &size, 0) != ERROR_SUCCESS) {
      return;
    }
    for (const wchar_t* p = buffer.data(); *p; p += wcslen(p) + 1) {
      if (!wcsstr(p, prefix_.c_str()))
        continue;
      PDH_HCOUNTER counter = nullptr;
      if (PdhAddCounterW(query_, p, 0, &counter) == ERROR_SUCCESS)
        counters_.push_back(counter);
    }
    // The first collection only primes the counters; a rate needs two.
    if (!counters_.empty())
      PdhCollectQueryData(query_);
  }

  bool Sample(float& out) {
    if (!query_ || counters_.empty())
      return false;
    if (PdhCollectQueryData(query_) != ERROR_SUCCESS)
      return false;
    double total = 0.0;
    bool any = false;
    for (PDH_HCOUNTER c : counters_) {
      PDH_FMT_COUNTERVALUE v{};
      if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS &&
          v.CStatus == PDH_CSTATUS_VALID_DATA) {
        total += v.doubleValue;
        any = true;
      }
    }
    if (!any)
      return false;
    out = float(total > 100.0 ? 100.0 : total);
    return true;
  }

  void Close() {
    if (query_) {
      PdhCloseQuery(query_);
      query_ = nullptr;
    }
    counters_.clear();
  }

 private:
  PDH_HQUERY query_ = nullptr;
  std::vector<PDH_HCOUNTER> counters_;
  std::wstring prefix_;
};

// --- Video memory ----------------------------------------------------------

bool SampleVram(float& used_mb, float& total_mb) {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> best;
  DXGI_ADAPTER_DESC1 best_desc = {};
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
      continue;
    if (!best || desc.DedicatedVideoMemory > best_desc.DedicatedVideoMemory) {
      best = adapter;
      best_desc = desc;
    }
  }
  if (!best)
    return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (FAILED(best.As(&adapter3)))
    return false;
  DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
  if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
    return false;
  // CurrentUsage is THIS process's, which is the number that answers "what does
  // the texture pack cost me".
  used_mb = float(double(vm.CurrentUsage) / (1024.0 * 1024.0));
  total_mb = float(double(best_desc.DedicatedVideoMemory) / (1024.0 * 1024.0));
  return true;
}

// --- System memory ---------------------------------------------------------

// This process's working set against total physical RAM, so it pairs with the
// per-process VRAM figure: "what is the game holding in main memory".
bool SampleRam(float& used_mb, float& total_mb) {
  PROCESS_MEMORY_COUNTERS pmc{};
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    return false;
  if (!GlobalMemoryStatusEx(&ms))
    return false;
  used_mb = float(double(pmc.WorkingSetSize) / (1024.0 * 1024.0));
  total_mb = float(double(ms.ullTotalPhys) / (1024.0 * 1024.0));
  return true;
}

void SampleLoop() {
  CpuMeter cpu;
  GpuMeter gpu;
  const bool gpu_ok = gpu.Open();
  auto last_refresh = std::chrono::steady_clock::now();
  auto last_frames_at = last_refresh;
  uint32_t last_frames = g_frames.load();

  while (!g_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kIntervalMs));

    PerfSample s;
    s.cpu_percent = cpu.Sample();

    const auto now = std::chrono::steady_clock::now();
    const uint32_t frames = g_frames.load();
    const double secs = std::chrono::duration<double>(now - last_frames_at).count();
    if (secs > 0.0)
      s.fps = float(double(frames - last_frames) / secs);
    last_frames = frames;
    last_frames_at = now;

    if (gpu_ok) {
      // Engine instances come and go with what the process is doing.
      if (std::chrono::duration<double>(now - last_refresh).count() > 5.0) {
        gpu.Refresh();
        last_refresh = now;
      }
      s.gpu_valid = gpu.Sample(s.gpu_percent);
    }
    if (!s.gpu_valid)
      s.gpu_percent = -1.0f;
    s.vram_valid = SampleVram(s.vram_mb, s.vram_total_mb);
    s.ram_valid = SampleRam(s.ram_mb, s.ram_total_mb);

    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_latest = s;
    }
  }
  gpu.Close();
}

}  // namespace

void StartPerfMonitor() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true))
    return;
  g_stop.store(false, std::memory_order_release);
  g_thread = std::thread(SampleLoop);
}

void StopPerfMonitor() {
  if (!g_started.load())
    return;
  g_stop.store(true, std::memory_order_release);
  if (g_thread.joinable())
    g_thread.join();
  g_started.store(false);
}

PerfSample GetPerfSample() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_latest;
}

void PerfFrameTick() { g_frames.fetch_add(1, std::memory_order_relaxed); }

}  // namespace ng2
