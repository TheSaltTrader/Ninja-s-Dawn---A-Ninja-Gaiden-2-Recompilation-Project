#include "ng2_hwdetect.h"

#include <windows.h>

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

#include <rex/logging.h>

namespace ng2 {
namespace {

// For scale, measured on this title: 1290 textures from one chapter came to
// 1.5 GB at 2x and 5.9 GB at 4x, and a pack covering the whole game is a
// multiple of that. Those are the sizes a cache has to hold.
constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;

std::string Utf8(const wchar_t* w) {
  if (!w)
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1)
    return {};
  std::string out(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

std::string FormatGiB(uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f GB", double(bytes) / double(kGiB));
  return buf;
}

GpuInfo DetectGpu() {
  GpuInfo info;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    return info;

  // The adapter with the most dedicated memory, which on a machine with an
  // integrated GPU beside a discrete one is the one the game will run on.
  Microsoft::WRL::ComPtr<IDXGIAdapter1> best;
  DXGI_ADAPTER_DESC1 best_desc = {};
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;
    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc)))
      continue;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
      continue;
    if (!best || desc.DedicatedVideoMemory > best_desc.DedicatedVideoMemory) {
      best = adapter;
      best_desc = desc;
    }
  }
  if (!best)
    return info;

  info.valid = true;
  info.name = Utf8(best_desc.Description);
  info.dedicated_bytes = uint64_t(best_desc.DedicatedVideoMemory);

  // Budget and current usage are read for the LOG only - they are useful when
  // diagnosing a stutter ("was the card already full?") but nothing is decided
  // from them; see RecommendSettings for why.
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (SUCCEEDED(best.As(&adapter3))) {
    DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) {
      info.budget_bytes = vm.Budget;
      info.used_bytes = vm.CurrentUsage;
    }
  }
  if (info.budget_bytes == 0)
    info.budget_bytes = info.dedicated_bytes;
  return info;
}

DetectedSettings RecommendSettings(const GpuInfo& gpu) {
  DetectedSettings out;
  if (!gpu.valid) {
    out.summary = "No GPU detected - leaving the memory settings alone.";
    return out;
  }

  // Sized against the card's OWN memory, not against what happens to be free.
  //
  // The first version used DXGI's budget, on the reasoning that it would
  // account for other applications. Measured, it does not: on a 32 GB card with
  // 24 GB held by other work, DXGI reported "budget 30.7 GB, in use 0.0 GB"
  // while nvidia-smi showed 8.2 GB free. The budget is an ALLOWANCE Windows
  // grants this process, not a measurement of free memory.
  //
  // Availability is the wrong basis regardless: it changes minute to minute, so
  // a setting derived from it is a snapshot of whatever else was running at
  // first launch and stays that way. Capacity is a fact about the machine and
  // gives the same answer every time.
  //
  // Roughly a quarter of the card for the texture cache. The rest is the game's
  // render targets, its own textures, the frames in flight, and whatever else
  // the player is running.
  const uint64_t vram = gpu.dedicated_bytes;

  if (vram >= 24 * kGiB) {
    out.texture_cache_mb = 8192;
    out.texture_scale = 4;
  } else if (vram >= 16 * kGiB) {
    out.texture_cache_mb = 4096;
    out.texture_scale = 2;
  } else if (vram >= 12 * kGiB) {
    out.texture_cache_mb = 4096;
    out.texture_scale = 2;
  } else if (vram >= 8 * kGiB) {
    out.texture_cache_mb = 2048;
    out.texture_scale = 2;
  } else if (vram >= 6 * kGiB) {
    out.texture_cache_mb = 1024;
    out.texture_scale = 2;
  } else {
    // Below this the plugin's own 384/768 is a better guess than a fraction of
    // a number this small.
    out.texture_cache_mb = 0;
    out.texture_scale = 2;
  }

  // 8x is never chosen automatically. It exists for hardware that does not
  // exist yet, and picking it for someone would be a guess dressed as a
  // measurement.
  std::string cache_text = "default";
  if (out.texture_cache_mb >= 1024)
    cache_text = std::to_string(out.texture_cache_mb / 1024) + " GB";
  else if (out.texture_cache_mb > 0)
    cache_text = std::to_string(out.texture_cache_mb) + " MB";

  char buf[320];
  std::snprintf(buf, sizeof(buf), "%s, %s of video memory. Cache %s, %dx upscale.",
                gpu.name.c_str(), FormatGiB(vram).c_str(), cache_text.c_str(),
                out.texture_scale);
  out.summary = buf;

  // 4x is a big pack. Say so rather than leaving it to be discovered as a
  // stutter, but do not refuse it - the card has the memory for it when nothing
  // else is competing for the card.
  if (out.texture_scale >= 4) {
    out.caveat =
        "4x packs are large (about 6 GB for one chapter). If the GPU is busy "
        "with other work, 2x will run smoother.";
  }
  return out;
}

}  // namespace ng2
