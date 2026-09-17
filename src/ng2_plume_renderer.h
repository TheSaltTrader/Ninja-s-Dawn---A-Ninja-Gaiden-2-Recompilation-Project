/**
 * @file        ng2_plume_renderer.h
 * @brief       The native renderer's own device and window, on the host GPU
 *
 * Stage 2a of the native path. The plugin still renders the game through Xenos
 * exactly as it always has; this opens a SECOND, independent device beside it
 * and presents into a shadow window of its own. Nothing here touches what the
 * player sees, and the game runs identically whether this is on or off.
 *
 * Why a shadow window rather than taking over the real one: the correctness
 * oracle for every draw we learn to render is the plugin's own rendering of the
 * same frame. Two windows side by side is what makes that comparison possible
 * at a glance, and it means a half-built renderer can never black out the game.
 *
 * Why its own thread: a window needs a message pump, and the only threads
 * available here are the guest's and the GPU worker's - both of which are
 * already doing something that must not be blocked by WM_PAINT.
 *
 * Off unless NG2_NATIVE_GPU_WINDOW is set in the environment.
 */

#pragma once

namespace ng2::ngpu::render {

// Brings up the Plume D3D12 device and its window on a private thread. Returns
// immediately; failure is logged and leaves the game untouched.
void Start();

// Tears the device and window down and joins the thread.
void Stop();

// Called once per guest frame, from wherever the swap is observed, so the
// shadow window can be paced by the game rather than free-running, and so the
// frame's coverage can be closed out. Cheap and safe to call when the renderer
// is off.
void EndFrame();

// THE COVERAGE ORACLE, and it exists from the first commit rather than after
// something appears on screen.
//
// A renderer that counts what it drew can only ever report a number that goes
// up, and "it draws" and "it draws the same thing every frame" are exactly the
// trap this project keeps falling into - a count cannot tell you what it never
// saw. So the two sides are counted separately: every draw the plugin handed
// over, and every draw this renderer actually issued. Their DIFFERENCE is the
// measurement, and at stage 2a it is honestly 100% - the native path renders
// none of them yet, and the instrument says so out loud instead of reporting a
// comfortable zero.
//
// Called from the draw consumer on the GPU worker thread; both are plain atomic
// increments.
void NoteDrawHandedOff();
void NoteDrawRendered();

}  // namespace ng2::ngpu::render
