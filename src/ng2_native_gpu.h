/**
 * @file        ng2_native_gpu.h
 * @brief       NG2's native renderer: the exe side of the plugin's draw hand-off
 *
 * The plugin executes NG2's PM4 stream and hands every draw over as it does
 * (rex/system/gpu_plugin.h, rex_gpu_set_draw_callback). This is what receives
 * them.
 *
 * Why here and not in the plugin: rexgpu-xenos.dll is the SHARED component both
 * titles load, and a game-specific renderer does not belong in it. Why driven
 * from the plugin and not from NG2's own Direct3D wrappers: 100% of NG2's draws
 * arrive inside chained INDIRECT_BUFFERs, so wrapper hooks are a structurally
 * partial view of a frame. See docs/native_gpu/M5_ng2_architecture.md.
 *
 * The callback arrives ON THE GPU WORKER THREAD, inside packet execution.
 */

#pragma once

namespace ng2::ngpu {

// Resolves the plugin's hand-off export and installs the consumer. Safe to call
// when the plugin is absent or older than the export - it logs and does nothing,
// because a missing native path must never stop the game running.
void Start();

// Removes the consumer. The plugin holds a raw pointer into this module, so
// this has to happen before teardown.
void Stop();

}  // namespace ng2::ngpu
