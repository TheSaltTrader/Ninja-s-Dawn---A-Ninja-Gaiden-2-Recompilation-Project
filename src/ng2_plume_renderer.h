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

#include <cstdint>

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

// THE SHADER LOOKUP, SYMMETRIC BY CONSTRUCTION.
//
// One function for both stages, taking the stage as an argument rather than
// having a vertex path and a pixel path. That is deliberate and it is the whole
// point: the sibling renderer grew a translation path for vertex microcode and
// never grew one for pixel, and the lookup miss fell through to a stale device
// value - so 2,197 draws in a frame shared ONE pixel shader. The frame rendered
// correct geometry and was uniformly unlit, which looks like a shading bug and
// is a lookup falling through. Two separate functions is how that becomes
// possible; one function cannot drift against itself.
//
// A MISS IS LOUD. It is counted per stage and named by address, never
// substituted with a last-known-good, a default, or whatever the device happens
// to hold. A renderer that silently substitutes produces a plausible picture
// built from the wrong programs, which is the failure this project keeps
// finding and the hardest to see.
enum class ShaderStage { kVertex, kPixel };

// Records that a draw wants this shader. Returns true if it is available to the
// native path. At stage 2b nothing is translated yet, so this returns false and
// counts the miss - which is the honest state and is reported as such.
bool WantShader(ShaderStage stage, uint32_t guest_address, uint32_t dword_count,
                bool immediate);

// STAGE 2b DESIGN CONSTRAINTS, written before the code so they are decisions
// rather than repairs. Both come from defects found in the sibling project's
// equivalent structures, in the same week, by reading rather than running.
//
// 1. NO RAW POINTERS INTO AN EVICTABLE CACHE. The shader lookups there hold raw
//    pointers into a cache whose erase sites destroy the owner, and the dangling
//    state is reachable through several early returns that skip re-registration.
//    It compiles, and it works right up until the cache evicts - then it fails
//    far from its cause. Key these maps by VALUE (the guest address) and look
//    the object up through the cache each time, or hold ownership outright.
//    Never both a cache that can evict and a pointer that outlives the check.
//
// 2. ONE MAP PER LOOKUP, AND end() FROM THE MAP THAT WAS SEARCHED. A lookup in
//    one map tested against a different map's end() has the same type, compiles
//    silently, and is wrong. With several same-typed maps side by side - by
//    address, by code hash, by microcode - this is a live hazard rather than a
//    theoretical one. It has already occurred once in the sibling codebase.
//
// 3. A DRAW NEED NOT BIND ANY VERTEX STREAM. 343 of NG2's 411 vertex
//    containers declare vertexElementCount == 0 - 83.5%, against Fable II's
//    6.6% - because two thirds of NG2's draws are auto-index POINT sprites,
//    which have nothing to fetch. Corroborated three ways: this scan, an
//    independent vfetch scan, and the PM4 census, all agreeing on 68 shaders
//    WITH fetches. A path that assumes a bound stream discards most of the
//    picture here, where in the sibling title it costs a fraction of a frame.
//
//    Two concrete bugs found in their stream-binding code, both of which this
//    path would inherit by imitation and neither of which is structural:
//      - a fill loop running s = 0..max_stream INCLUSIVE still executes once
//        when the stream mask is EMPTY, and takes the first-bound-stream
//        pointer as the filler for an unbound slot - null, and dereferenced;
//      - binding max_stream + 1 views binds one UNINITIALISED view when there
//        are no streams at all.
//    Both are the same mistake: treating "no streams" as "a stream count of
//    zero-plus-one" rather than as a case.
//
// 4. NEVER CPU-READ GUEST GEOMETRY. Index and vertex data are not obtainable by
//    translating a guest address and dereferencing it: those pages are
//    legitimately not CPU-resident. Measured - 1,889,699 faults out of
//    1,889,700 indexed draws, on an address that was correct. The plugin's own
//    CPU read (primitive_processor.cpp:699) is inside a CONVERSION branch only;
//    the ordinary path calls shared_memory_.RequestRange (line 966) and the GPU
//    reads from the shared-memory buffer. The vertex path is the same shape
//    (d3d12/command_processor.cpp:3159 requests vfetch_constant.address << 2).
//    A renderer built on CPU reads WOULD WORK in the conversion cases and fail
//    everywhere else, presenting as a data-correctness bug rather than a
//    residency one.
//
// 5. VERTEX AND PIXEL SHADER LOOKUP MUST BE SYMMETRIC, AND A MISS MUST BE LOUD.
//    The sibling renderer had a translation path for vertex microcode and none
//    for pixel; the lookup miss fell through to a stale device value, so 2,197
//    draws in a frame used ONE pixel shader. The frame rendered correct
//    geometry and was uniformly unlit - a symptom that looks like a lighting or
//    shading bug and is actually a lookup falling through. Whatever this path
//    cannot resolve must be counted and named, never silently substituted.
//
// 6. GATE EVERY DRAW-PATH FEATURE, DEFAULT OFF, FROM THE FIRST LINE. The
//    sibling session put an ungated change on the path that draws the picture,
//    and when the run crashed there was nothing to compare against - a control
//    is the same binary with the feature off, and that had been made impossible
//    on the one path where a wrong frame is hardest to notice. This costs
//    nothing to do now, while the renderer still draws nothing, and cannot be
//    retrofitted cheaply later.
//
// And the reason all of these are stated here rather than discovered later:
// reading and running catch DIFFERENT classes. These two are structural - visible to a
// reader, invisible to a run that never evicts or never takes the branch. The
// complement is stage 2a's lesson, where the code was built, linked and
// symbol-verified and simply never called, which no amount of reading would have
// shown and one run did.

}  // namespace ng2::ngpu::render
