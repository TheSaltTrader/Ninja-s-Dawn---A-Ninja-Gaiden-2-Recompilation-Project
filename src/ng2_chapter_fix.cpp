// The chapter that never loads after a boss ("the red mist"). The fix is a
// single word the game waits for.
//
// WHAT WAS SEEN. After a boss, the results, rating and save screens work, the
// game asks "continue to the next chapter without saving?", the player picks
// Proceed, the dialog closes and the red mist stays for ever. Every earlier
// remedy forced the loader from outside; the chapter came up playable, but
// without the loading screen, without its background, and with the mist still
// drawn over the next chapter.
//
// WHAT THE GAME DOES. The post-boss flow is a front-end state machine
// (sub_8242BCF8, object 0x85008430, a 4-slot ring of states, run every frame
// by the in-game handler sub_8364A830). Proceed moves it to the state at
// 0x8242D748. That state looks at the player's profile block at 0x8555B930:
// sixty "unlockable earned this chapter" flags at +24, matched against the
// same sixty bytes in the save data. If any earned unlockable is not yet in
// the save, the state waits until the block's field +88 reads 2 or more - the
// state the game's own achievement/profile write reaches when it completes.
// Only then does it push the next states, which raise MODE 131, set the load
// target, bump the chapter sequence word, and start the loader together with
// its loading-loop task: the aurora background, "A to proceed", and the mist
// clear all come from that task.
//
// WHY IT NEVER ARRIVES HERE. In this port the write that would complete that
// field is never issued at the chapter end (the runtime does perform the
// achievement write when the chapter is next loaded, which is why the flags
// were still pending), so the field stays 0 and the machine waits for ever.
// Xenia completes the write and the same state proceeds within one frame,
// with exactly the write set observed there: MODE 3->1, LOADSTATE, sequence
// 9->10, commit flag, cursor, chapter tasks suspended.
//
// THE FIX. At the wait, when the field is still below 2, report it as 2. This
// is what the completed write would leave behind; the game then runs its own
// transition unchanged. Verified live on 2026-09-11: with the game parked at
// the mist, writing 2 into 0x8555B988 by hand produced the native load,
// loading background and mist clear. Awarding the achievements themselves is
// left to the game, which re-issues them at the next chapter load as before.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include <cstdint>

REXCVAR_DEFINE_BOOL(ng2_chapter_award_fix, true, "Ninja Gaiden II",
                    "At the post-boss proceed, treat the profile's achievement write as "
                    "completed so the game runs its own chapter transition (fixes the red-mist "
                    "hang and restores the loading screen)");

// Runs before `cmpwi cr6,r11,2` at 0x8242D7B4, right after `lwz r11,88(r8)`.
// r8 is the profile block (0x8555B930); r11 holds field +88, the award state.
void ng2ChapterAwardFix(PPCRegister& r11, PPCRegister& r8) {
  if (!REXCVAR_GET(ng2_chapter_award_fix))
    return;
  if (r11.s32 >= 2)
    return;
  auto* mem = REX_KERNEL_MEMORY();
  if (!mem)
    return;
  uint8_t* p = mem->TranslateVirtual<uint8_t*>(r8.u32 + 88);
  if (!p)
    return;
  static int logged = 0;
  if (logged < 4) {
    ++logged;
    REXLOG_INFO("[ng2] chapter proceed: profile award state {} -> 2 (the write the game waits for)",
                r11.u32);
  }
  p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 2;  // big-endian u32 = 2
  r11.u64 = 2;
}
