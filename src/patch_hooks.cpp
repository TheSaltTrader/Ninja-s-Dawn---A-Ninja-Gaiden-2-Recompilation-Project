// Game patch hook bodies. Declared in config/hooks/patches.toml.
//
// Ninja Gaiden II renders at 1120x584 internally and the front end scales it
// to the display. Xenia has a community patch that raises it to 1280x720 by
// rewriting four immediates; this does the same thing by rewriting the
// registers those immediates land in, which is the only form a static
// recompilation can honour (the immediates are already C++ constants).
//
// Off by default: it is a hack on the game's own render setup, and 1120x584 is
// what the title shipped with. When it is off these hooks cost one comparison.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "ng2_chapter.h"

#include <cctype>
#include <cstring>
#include <string>

// 0 = leave the game's own 1120x584 alone. Set both to use a different
// internal render size; 1280x720 is the size the Xenia patch uses and the only
// one with any testing behind it.
REXCVAR_DEFINE_INT32(ng2_render_width, 0, "Ninja Gaiden II",
                     "Internal render width override (0 = stock 1120)");
REXCVAR_DEFINE_INT32(ng2_render_height, 0, "Ninja Gaiden II",
                     "Internal render height override (0 = stock 584)");

// How the pre-rendered videos are handled: 0 = the game plays them, 2 = skip
// them. 1 was a replacement mode for the broken guest decoder and is retired;
// the default was left at 1 long after that, which is a setting disagreeing
// with the one the UI writes.
REXCVAR_DEFINE_INT32(ng2_video_mode, 0, "Ninja Gaiden II",
                     "Videos: 0 the game plays them, 2 skip them");

// Stop at the setup screen instead of booting the configured game.
//
// Set by the relaunch that the game's own "Quit Game" performs, so quitting
// returns the player to this port's menu rather than to the desktop. It is a
// plain cvar rather than something private so that "--ng2_setup" also works
// from a shortcut for anyone who wants the picker every launch.
REXCVAR_DEFINE_BOOL(ng2_setup, false, "Ninja Gaiden II",
                    "Show the setup screen instead of launching the game");

// Forces the Chapter 12 early return unconditionally - the community patch's
// original behaviour. The guard below makes this unnecessary; it is kept only
// as a way out if the guard ever proves too permissive.
REXCVAR_DEFINE_BOOL(ng2_chapter12_workaround, false, "Ninja Gaiden II",
                    "Always take the Chapter 12 early return, whether or not "
                    "the pointer is valid (the community patch's behaviour)");

namespace {

// Both hook sites want the same answer, and the log line should appear once
// per site rather than once per frame - these run on the boot path, but a
// video mode change can bring them round again.
bool WantOverride(uint32_t& width, uint32_t& height) {
  const int32_t w = REXCVAR_GET(ng2_render_width);
  const int32_t h = REXCVAR_GET(ng2_render_height);
  if (w <= 0 || h <= 0)
    return false;
  width = static_cast<uint32_t>(w);
  height = static_cast<uint32_t>(h);
  return true;
}

void LogOnce(const char* site, uint32_t from_w, uint32_t from_h, uint32_t w,
             uint32_t h) {
  REXLOG_INFO("Patch: internal render size {} {}x{} -> {}x{}", site, from_w,
              from_h, w, h);
}

}  // namespace

// 0x836261E8, after `li r3,1120`. r4 still holds the height from the previous
// instruction; the call two instructions later takes (r3=width, r4=height).
void ng2PatchRenderSize1(PPCRegister& r3, PPCRegister& r4) {
  uint32_t w = 0, h = 0;
  if (!WantOverride(w, h))
    return;
  static bool logged = false;
  if (!logged) {
    LogOnce("(video mode)", r3.u32, r4.u32, w, h);
    logged = true;
  }
  r3.u32 = w;
  r4.u32 = h;
}

// 0x837C62FC, after `li r9,584`. r10 holds the width and is stored to
// [r31+0x40] three instructions later.
void ng2PatchRenderSize2(PPCRegister& r9, PPCRegister& r10) {
  uint32_t w = 0, h = 0;
  if (!WantOverride(w, h))
    return;
  static bool logged = false;
  if (!logged) {
    LogOnce("(render desc)", r10.u32, r9.u32, w, h);
    logged = true;
  }
  r10.u32 = w;
  r9.u32 = h;
}

// 0x82834C78, after `lwz r30, 0x30(r11)` has loaded a pointer from
// [0x84C23C48].
//
// The next instructions test it against zero and return early when it is null.
// Further down, when it is not null, the function dereferences it:
//
//   0x82834CD8  lbz  r10, 8(r30)
//   0x82834CE4  lwz  r10, 0x20(r30)
//   0x82834CEC  lwzx r11, r11, r10
//
// The community patch (Gliniak, via Xenia's patch file for this title) writes
// `li r30, 0` here unconditionally, which is why its author warns it causes
// problems outside Chapter 12: it makes the function give up even when the
// pointer was perfectly good.
//
// Since the crash is a bad-pointer dereference, the pointer can simply be
// checked. If the two fields the function reads are in mapped, readable guest
// memory, nothing happens and the game behaves exactly as it always has. If
// they are not, the read two instructions from here would fault, so r30 is
// zeroed and the game takes its OWN early return - the same path it takes for
// a null pointer.
//
// This costs one heap lookup on a path that is not hot, and it is safe in
// every chapter because it only acts where the alternative is a crash.
void ng2PatchChapter12(PPCRegister& r30) {
  // Logged once per DISTINCT value, which is both cheap and the thing worth
  // knowing. This site is busy - over 500 calls before Chapter 1 is even
  // playable - and it carried the same pointer, 0xF0AAD450, every time. That
  // measurement is what makes "the guard never fires in normal play" mean
  // something: a hook that is never reached would also never fire.
  //
  // If Chapter 12 puts a different value here, this line names it, which is
  // the evidence for whether the crash is what this guard assumes it is.
  static uint32_t last_seen = 1;  // 1 is not a plausible pointer, so the
                                  // first real value always logs
  const uint32_t va = r30.u32;
  if (va != last_seen) {
    last_seen = va;
    REXLOG_INFO("Patch: chapter-12 site, [0x84C23C48] = 0x{:08X}", va);
  }
  if (va == 0)
    return;  // the game already returns early here

  auto* memory = REX_KERNEL_MEMORY();
  if (memory == nullptr)
    return;

  // The pointer is TESTED, never assumed bad - in chapter 12 as everywhere.
  //
  // This used to force the community patch's early return throughout chapter
  // 12 ("scoped to where it belongs"). That was the chapter 12 -> 13 hang.
  // 2026-09-06 12:50:19: the boss died, the site was called with a NEW
  // pointer, 0xF0B72AE0 - a real object in mapped guest memory (+0x30 held a
  // sane heap pointer) - and the forced return threw its work away. The game
  // then sat in the mist forever, still answering input, because the state
  // that starts chapter 13 had been skipped. Its author's warning that the
  // patch "causes problems" was this. The crash the patch exists for is a
  // bad-pointer dereference, and the readability check below is the right
  // guard for that: a pointer that can be read is a pointer the game may use.
  //
  // The cvar remains as an unconditional override for anyone who wants the
  // original always-on behaviour.
  const bool force = REXCVAR_GET(ng2_chapter12_workaround);
  if (!force) {
    auto readable = [memory](uint32_t address) {
      auto* heap = memory->LookupHeap(address);
      if (heap == nullptr)
        return false;
      uint32_t protect = 0;
      if (!heap->QueryProtect(address, &protect))
        return false;
      return (protect & rex::memory::kMemoryProtectRead) != 0;
    };
    // The two offsets the function actually reads.
    if (readable(va + 8) && readable(va + 0x20))
      return;
  }

  // Log every distinct bad pointer once: if this ever fires, that line is the
  // evidence that the guard did its job, and which value caused it.
  static uint32_t last_reported = 0;
  if (va != last_reported) {
    last_reported = va;
    REXLOG_WARN("Patch: chapter-12 guard - [0x84C23C48] = 0x{:08X}, taking the "
                "game's early return{}",
                va, force ? " (forced by ng2_chapter12_workaround)" : " - not readable");
  }
  r30.u32 = 0;
}

// Guest address of a path that does not resolve. The app allocates and fills
// it once the runtime exists; zero means "not available", and the hook then
// does nothing rather than pointing the game at an address that is not ours.
uint32_t g_ng2_skip_path_va = 0;

// 0x8380EB9C, right after `mr r28, r3` copied the path the file wrapper was
// called with.
//
// This exists for one setting: "Skip intro videos". It points the game at a
// path that does not resolve, and a failed open makes it move straight on.
//
// The one class that must NEVER be skipped is aurora* - the chapter-loading
// videos. The game reads a failed open of one of those as a bad disc and
// stops, with the whole chain visible in the log inside 34 milliseconds:
//
//   [NtCreateFile] FAILED: path='game:__video_skipped__.wmv' -> 0xc000000f
//   XamShowDirtyDiscErrorUI called! user_index=0
//
// There was also a mode that took a video away from the game and drew replacement
// frames over it. That only ever existed to work around the guest's decoder
// reading its vperm control vectors out of registers the recompiler had
// replaced with zeroes, which garbled every video. With the v64-v127 codegen
// fix the decoder is correct - decoded planes match the source at r = 1.000
// with 0.0% of pixels clipped, against r = 0.18-0.25 and 3-10% before - so the
// replacement path has been removed rather than left switched off.
void ng2PatchSkipVideos(PPCRegister& r28) {
  const int32_t mode = REXCVAR_GET(ng2_video_mode);
  if (mode != 2)  // 0 = the game plays them; nothing else is a skip
    return;
  if (g_ng2_skip_path_va == 0)
    return;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory)
    return;
  const uint32_t va = r28.u32;
  if (va < 0x1000)
    return;
  const char* path = memory->TranslateVirtual<const char*>(va);
  if (path == nullptr)
    return;
  // Bounded: this is a guest pointer on a hot path, and a missing NUL must not
  // walk the whole address space.
  size_t len = 0;
  while (len < 260 && path[len] != ' ')
    ++len;
  if (len < 4)
    return;
  if (_stricmp(path + len - 4, ".wmv") != 0)
    return;

  const std::string name(path, len);

  // Skip the openings and the attract demos, never a chapter-loading video.
  const auto slash = name.find_last_of("\/:");
  const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
  if (_strnicmp(base.c_str(), "aurora", 6) == 0)
    return;

  static std::string last;
  if (name != last) {
    REXLOG_INFO("Patch: skipping video {}", name);
    last = name;
  }
  r28.u32 = g_ng2_skip_path_va;
}
