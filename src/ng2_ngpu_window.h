/**
 * @file        ng2_ngpu_window.h
 * @brief       The native window: raw D3D12 (no Plume), its own thread, one
 *              swap chain, one blit. It owns the D3D12 device and direct queue
 *              the transplanted backend renders on, and presents the backend's
 *              gamma-applied guest output at the plugin's SWAP (never from the
 *              guest's present hook, which runs ahead of the GPU thread and
 *              showed an old frame in Fable II).
 *
 * THE ULTRAWIDE PRESENTER HALF LIVES HERE NOW. rexruntime's presenter reads
 * the ng2_uw_mode cvar the plugin writes at every swap (0 = the player's
 * Keep-aspect setting, 1 = gameplay: fill the screen with the FOV-widened
 * frame, 2 = menu/video: pillarbox 16:9). Under offload that presenter no
 * longer presents anything, so this window applies the same rule to its
 * viewport - the carry-forward ledger's item 1.2, re-homed.
 */

#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace ng2::ngpu::render {

// What the game's own window was asked to be; the native window mirrors it.
struct WindowSpec {
  int width = 1280;
  int height = 720;
  bool fullscreen = false;
  int monitor = 0;        // 0-based, the settings' index
  bool letterbox = true;  // the player's Keep-aspect setting (ng2_uw_mode 0)
};

// Creates the window and the device on a private thread and waits until the
// device exists (or creation failed). Returns whether the device is up.
bool Start(const WindowSpec& spec);
void Stop();

bool Ready();
ID3D12Device* Device();
ID3D12CommandQueue* Queue();

// From the plugin's swap callback: present the backend's latest guest output
// once its swap submission has been executed. Non-blocking.
void RequestPresent();

// Statistics for the periodic log line.
struct Stats {
  uint64_t presented = 0, requests = 0, skipped_no_output = 0, waits_timed_out = 0;
  uint32_t last_mode = 0;   // the ng2_uw_mode the last frame was presented with
};
Stats GetStats();

}  // namespace ng2::ngpu::render
