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

// Registers a program the consumer has already read from guest memory, keyed by
// a hash of its FULL microcode. Returns true if the manifest knows it and the
// stage agrees. Content-keyed because the offline containers and the runtime
// IM_LOAD addresses live in different address spaces; whole-program because
// fifteen of 625 containers share their first code dword.
bool RegisterShaderMicrocode(ShaderStage stage, uint32_t guest_address, const uint8_t* ucode,
                             uint32_t bytes, const uint8_t* preamble128 = nullptr);

// THE RENDER TARGET A DRAW WANTS.
//
// Measured, not assumed: NG2 uses SIXTY-ONE distinct render targets in Chapter
// 1 gameplay, keyed by exactly these fields. A native path that assumes a
// single back buffer is wrong sixty times over, so this is a cache from the
// first line rather than a back buffer with a cache bolted on later.
//
// The consumer decodes the registers (it is the side that has them) and the
// renderer owns creation, because D3D12 resources are created on the render
// thread and the draws arrive on the GPU worker thread. So this only RECORDS
// the request; the texture appears on the next frame the render thread runs.
//
// Returns true if a host target for this surface already exists.
//
// width/height are the renderer's best estimate from the surface pitch and the
// widest scissor seen, and are PROVISIONAL: a Xenos EDRAM target does not carry
// its own height, so this is a derivation rather than a reading, and it is
// reported as such rather than presented as the surface's size.
struct SurfaceDesc {
  uint32_t pitch = 0;
  uint32_t msaa = 0;
  uint32_t color_base = 0;
  uint32_t color_format = 0;
  uint32_t depth_base = 0;
  uint32_t edram_mode = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};
bool WantRenderTarget(const SurfaceDesc& desc);

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
// 4. INDEX DATA IS CPU-READABLE THROUGH THE **PHYSICAL** TRANSLATION, AND THE
//    CLAIM THAT IT IS NOT WAS AN INSTRUMENT DEFECT. Corrected 2026-09-17.
//
//    This constraint used to read "NEVER CPU-READ GUEST GEOMETRY... those pages
//    are legitimately not CPU-resident", on the strength of 1,889,699 faults
//    out of 1,889,700 indexed draws. That measurement was worthless, and its
//    shape is the lesson: the probe evaluated BOTH interpretations inside ONE
//    __try, with the VIRTUAL translation first. Virtual always faults, the
//    handler fired, and the PHYSICAL interpretation was never evaluated on any
//    draw. The instrument could not produce the answer it was asked for.
//
//    Re-measured with one guard per interpretation:
//
//      INDEX BASE of 155,050 draws: 0 virtual-only, 25,175 physical-only,
//      0 both, 129,874 neither, 0 FAULTED (virt-fault 155,049, phys-fault 0)
//
//    ZERO physical faults. This agrees with the plugin, which CPU-reads the
//    same address unguarded (primitive_processor.cpp:699,
//    memory_.TranslatePhysical(guest_index_base)) and would crash constantly if
//    those pages were not resident.
//
//    WHAT THIS DOES NOT ESTABLISH. TranslatePhysical is
//    physical_membase + (addr & 0x1FFFFFFF), which always lands inside a large
//    committed arena - so "it did not fault" is nearly free and is NOT evidence
//    that the bytes are the right bytes. Only 25,175 of 155,050 reads look like
//    a plausible 16-bit index run under the heuristic here, and the other
//    129,874 are unexplained: they may be 32-bit indices, a different endian,
//    or the heuristic's own bounds being too tight. READABILITY is settled;
//    INTERPRETATION is not, and the draw path must not assume otherwise.
//
//    The residency path remains real and is still how the GPU gets the data:
//    the ordinary path calls shared_memory_.RequestRange
//    (primitive_processor.cpp:966) and the vertex path is the same shape
//    (d3d12/command_processor.cpp:3159 requests vfetch_constant.address << 2).
//    Reading CPU-side is now an OPTION rather than a closed door.
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
// 7. AN EXCESS OF COVERAGE IS A DEFECT, NOT HEADROOM. The Xenos renders in
//    BINS, and the bin is not carried by the draw - it lives in the surrounding
//    state. So a replay that faithfully reissues every draw but loses the bin
//    partition writes every bin into one full-screen target and paints the frame
//    several times over. It FAILS UPWARD: the result looks more complete than
//    the original, not less.
//
//    Measured in the sibling project, whose replay read brighter than the very
//    control it was replaying: brighter by more than 30 in 66.6% of pixels while
//    only 2.25% was newly lit. Same pixels painted repeatedly, not more scene -
//    and their own comfortable explanation, "the bridge sees more draws than the
//    hooks", would have closed the question wrongly.
//
//    THIS IS THE ONE THE ORACLE ABOVE CANNOT SEE. Counting handed-off against
//    rendered is the right instrument for MISSING draws and is blind to this:
//    every draw issued, every draw landed, wrong region. So when draws start
//    reaching a surface, the test is a DIFFERENCE IMAGE against a control, split
//    two ways - "brighter than the control" versus "lit where the control is
//    black". Overdraw is a large first number with a small second one; genuinely
//    extra geometry is the reverse. One coverage percentage cannot tell them
//    apart, which is exactly how 99.4% there looked like success.
//
// 8. FOR AN IMMEDIATE SHADER THE ADDRESS FIELD IS STALE, NOT EMPTY - so a
//    lookup that forgets to check the flag SUCCEEDS and returns the wrong
//    program. Measured 2026-09-17 by reading the hand-off, not by running it.
//
//    IM_LOAD_IMMEDIATE carries the microcode inline in the packet, so there is
//    no address to key on. The plugin's TrackShaderLoad sets the immediate flag
//    and RETURNS WITHOUT UPDATING g_cur_vs / g_cur_ps, which keep whatever the
//    last IM_LOAD put there. The record is then filled with
//    rec.vs_address = g_cur_vs - a real, previously registered address that has
//    nothing to do with this draw.
//
//    So the failure is not a miss. An address-keyed lookup finds a genuine
//    entry, reports a hit, and binds a program the draw never asked for: a
//    plausible wrong answer, which is the hardest kind to see and exactly what
//    constraint 5 exists to prevent. WantShader is correct today - it returns
//    on `immediate` before touching the address - and the guard has to stay
//    that way in every path added later, including the one that binds.
//
//    THE SCALE MAKES THIS STRUCTURAL, NOT A CORNER. Shader load mode is
//    scene-dependent here: IMMEDIATE dominates light frames (91.8% of draws in
//    the sampled attract frames; one gameplay frame reported immediate VS
//    166,118 / PS 159,445) while by-pointer wins in heavy ones, up to 8.4:1.
//    A native draw path that can only serve by-pointer shaders therefore
//    renders a MINORITY of draws in exactly the frames that look cheapest to
//    get working first, and its coverage number will climb as scenes get
//    heavier - which reads like progress and is not.
//
//    The immediate path needs the microcode carried across the ABI, since the
//    plugin has it and the exe cannot recover it from an address. That is an
//    ABI change, and the record's struct_size is what makes it detectable
//    rather than silent.
//
// And the reason all of these are stated here rather than discovered later:
// reading and running catch DIFFERENT classes. These two are structural - visible to a
// reader, invisible to a run that never evicts or never takes the branch. The
// complement is stage 2a's lesson, where the code was built, linked and
// symbol-verified and simply never called, which no amount of reading would have
// shown and one run did.

}  // namespace ng2::ngpu::render
