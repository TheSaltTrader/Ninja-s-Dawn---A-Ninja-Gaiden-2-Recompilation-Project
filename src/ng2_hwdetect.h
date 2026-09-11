// Choosing the memory-hungry settings from the machine, instead of asking.
//
// The texture cache size and the upscale factor are the two settings a player
// cannot reasonably pick: both are answers to "how much video memory is there",
// and getting them wrong is not a matter of taste - too high and the cache
// evicts and re-uploads constantly, which is a stutter rather than a lower
// setting.
//
// The reading is the card's own CAPACITY, deliberately - not how much video
// memory happens to be free.
//
// Two reasons, one of them measured. DXGI's budget looked like the right signal
// because it is meant to reflect system pressure, but on a 32 GB card with
// 24 GB held by other applications it reported "budget 30.7 GB, in use 0.0 GB"
// while nvidia-smi showed 8.2 GB free: the budget is an allowance granted to
// this process, not a measurement of what is free. And availability is the
// wrong basis anyway - it changes minute to minute, so a setting derived from
// it freezes whatever else happened to be running at first launch.
//
// Capacity is a fact about the machine. It gives the same answer every time,
// which is what a setting wants.

#pragma once

#include <cstdint>
#include <string>

namespace ng2 {

struct GpuInfo {
  bool valid = false;
  std::string name;
  uint64_t dedicated_bytes = 0;  // the adapter's own memory
  uint64_t budget_bytes = 0;     // what this process may use right now
  uint64_t used_bytes = 0;       // what this process is using right now
};

// Queries the adapter. Never throws; `valid` is false if DXGI is unavailable,
// in which case callers should leave the settings alone rather than guess.
GpuInfo DetectGpu();

struct DetectedSettings {
  int texture_cache_mb = 0;  // 0 = leave the plugin's own defaults
  int texture_scale = 2;
  std::string summary;   // one line naming the card and what was chosen
  std::string caveat;    // non-empty when the GPU was busy and the answer is low
};

DetectedSettings RecommendSettings(const GpuInfo& gpu);

// Human-readable byte count for the summary lines ("7.6 GB").
std::string FormatGiB(uint64_t bytes);

}  // namespace ng2
