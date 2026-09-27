/**
 * @file        ng2_p2_census.h
 * @brief       P2 ATTRIBUTION CENSUS, guest side (full-native plan, 2026-09-27): which Direct3D library entry point
 *              (or direct ring site) produced each PM4 packet. At the ENTRY and every EXIT of the 108 hooked XDK
 *              functions (config/hooks/native_gpu_trace.toml; exits injected into the generated code by
 *              tools/native_gpu/p2_inject_exits.py) the XDK device's ring write cursor is read (device+0x30, the
 *              push pointer every draw entry point advances, and device+0x3484, the copy some inline paths commit
 *              through - both, per the Fable II notes), so each call owns a byte range of whatever command buffer
 *              the library was writing. The plugin records every packet's address (fork command_processor.cpp,
 *              NG2_P2) and tools/native_gpu/p2_census.py joins the two by frame and address.
 *
 *              Off unless NG2_P2=<first frame>:<frames> is set (frames counted by the swap entry point, the same
 *              count the plugin keeps by swap packet). NG2_P2_CURSOR / NG2_P2_CURSOR2 override the two offsets;
 *              NG2_P2_DISCOVER=1 logs the device object's first 32 words at the first calls, for finding the
 *              cursor on another build. Output: p2_guest.bin in the working directory.
 */

#pragma once

#include <cstdint>

namespace ng2::p2 {

// From the trace hook at the first instruction of hooked function `hook` (index into native_gpu_trace.cpp's
// kEntries); r3 = the first argument (the device for library entry points, `lib` true), which is how the device
// is learned at the FIRST library call - the library writes its persistent packet templates (NG2: the 24-draw
// block sub_8373B060 emits once) at device creation, long before the first swap.
void Enter(int hook, uint32_t r3, bool lib);
// From the injected call before each `return;` of the same function.
void Exit(int hook);
// From the frame marker hook (the swap entry point), once per guest frame; r3 = the device.
void FrameMarker(uint32_t r3);

}  // namespace ng2::p2

extern "C" void ng2_p2_exit(int hook);
