// Live CPU, GPU and video-memory figures for this process.
//
// The point is to make the cost of a setting visible while it is being changed.
// A texture pack that turns 2x into 4x quadruples video memory, and until now
// the only way to find that out was to play until it stuttered - the settings
// screen asked for a decision and showed nothing to decide on.
//
// Everything here is measured for THIS PROCESS, not the machine. On a box that
// also runs other GPU work - which this one does - a system-wide number says
// almost nothing about the game, and would make the pack look expensive or
// cheap depending on what else happened to be running.
//
// Sampling runs on its own thread at a fixed interval: PDH counters need two
// readings separated by real time to mean anything, and doing that on the
// render thread would be both wrong and self-defeating.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace ng2 {

struct PerfSample {
  float fps = 0.0f;          // presented frames per second
  float cpu_percent = 0.0f;  // this process, across all cores
  float gpu_percent = 0.0f;  // this process's GPU engines, -1 if unavailable
  float vram_mb = 0.0f;      // this process's local video memory
  float vram_total_mb = 0.0f;
  float ram_mb = 0.0f;       // this process's working set (system RAM)
  float ram_total_mb = 0.0f;
  bool gpu_valid = false;
  bool vram_valid = false;
  bool ram_valid = false;
};

// Starts the sampler. Safe to call twice; the second call does nothing.
void StartPerfMonitor();
void StopPerfMonitor();

// The most recent sample. Cheap - reads a snapshot, takes no lock.
PerfSample GetPerfSample();

// Called once per presented frame so the sampler can work out a frame rate.
// Deliberately not a timer: what matters is frames the player actually saw.
void PerfFrameTick();

}  // namespace ng2
