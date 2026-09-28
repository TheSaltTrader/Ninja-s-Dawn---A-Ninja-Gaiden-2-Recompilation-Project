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
void Enter(int hook, uint32_t r3, bool lib, const uint32_t* args8 = nullptr);   // args8: r3..r10 at entry
// From the injected call before each `return;` of the same function.
void Exit(int hook);
// From the frame marker hook (the swap entry point), once per guest frame; r3 = the device.
void FrameMarker(uint32_t r3);

// P3 (full-native stage 1). NG2_P3MAP=<guest frame>:<max calls> (needs NG2_P2 on): at the exit of every call in
// that frame whose cursor advanced (up to max), the XDK device object (0x5000 bytes) and the call's own packets go
// to p3_guest.bin; every bridge draw whose packet address falls in a recorded range writes its register file
// (0x2000-0x23FF, 0x4000-0x4927) to p3_bridge.bin. tools/native_gpu/p3_flush.py maps registers to device words.
// NG2_P3FE=<N>: the guest-thread front end decodes each call's packets into its own register file and every Nth
// draw is compared with the bridge's file at that packet ([p3fe] log lines). Called from the bridge's OnDraw.
void BridgeDraw(uint32_t packet_addr, const uint32_t* regs, uint32_t reg_count);
// From the bridge's swap callback: the bridge's own frame count, stamped on p3_bridge.bin records (format v3).
void BridgeSwap();
// [p3 draw] NG2_P3DRAW: the front end draws; the bridge asks so its plugin callbacks go compare-only.
bool FrontEndDraws();
// NG2_NATIVE_FE=1: the front end without the census (called by the bridge once the plugin callbacks exist).
bool StartNativeFrontEnd();
// [gs] The game's graphics system hands each CP_RB_WPTR write here (wptr 0xFFFFFFFF = the ring was reset).
void NativeKick(uint32_t ring_ptr, uint32_t ring_bytes, uint32_t wptr);
void FrontEndCounts(uint64_t& draws, uint64_t& swaps);

}  // namespace ng2::p2

extern "C" void ng2_p2_exit(int hook);
