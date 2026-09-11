// The music that dies mid-session. Declared in config/hooks/patches.toml.
//
// WHAT WAS SEEN. With the music dead, both guest audio workers (the threads
// behind guest handles F80000DC and F80000E0, started by sub_8374E740 and
// sub_8374E808) were spinning at 100% CPU inside sub_8374F078, and the audio
// callback (sub_8374EDE8, on the runtime's audio thread) was waiting on the
// completion event they never signal. Read live from the running game, with
// nothing attached: the barrier word held byte 4 only, the mask expected bytes
// 4 and 5, and each worker's own hardware-thread number was correct - 4 and 5.
//
// WHAT sub_8374F078 IS. A rendezvous barrier. Each arriving worker writes 1
// into ITS byte of an 8-byte word - the byte index is its hardware-thread
// number, read from r13+0x10C - then spins until the word equals a mask built
// from six per-hardware-thread slots in the audio engine object. Whoever sees
// equality stores 0 over the word; everyone else leaves when it reads 0. There
// are two words, used alternately per round, precisely so that a slow reader
// of round N is not confused by the arrivals of round N+1.
//
// WHAT GOES WRONG. A job's first round uses word 0, and its loop then
// alternates 1, 0, 1, 0... A job with an even number of rounds ends on word 0,
// and the next job starts on word 0 again. Worker 4 saw the last round
// complete, cleared the word, finished the job, signalled the callback, was
// handed the next job, and arrived at its first round - setting byte 4 in the
// SAME word - before worker 5 had re-read the word after its own arrival.
// Worker 5 then sees {4}: not the mask, not zero. It spins forever, worker 4
// waits forever for byte 5, and the callback waits forever for both.
//
// WHY ONLY HERE. On the console the spinner re-reads the word within tens of
// nanoseconds of the clear, and the path from the clear to the next arrival
// runs through two kernel calls and the callback; the spinner always wins.
// Under host scheduling a spinning thread can be off the CPU for milliseconds
// - a yield, a time slice - and the whole window passes while it is away.
// The game's assumption is about hardware timing, and the port does not
// provide it.
//
// THE FIX. A thread that has arrived and finds its own byte gone from the
// word knows the round completed: only completion clears bytes. So it takes
// the exit the loop missed. This is exactly what the thread would have done
// had it read the zero; nothing else changes.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include <atomic>
#include <cstdint>

REXCVAR_DEFINE_BOOL(ng2_audio_barrier_fix, true, "Ninja Gaiden II",
                    "Let an audio worker leave the rendezvous barrier when its own arrival "
                    "byte has already been cleared (fixes the music dying mid-session)");

// Runs after `ld r11,0(r4)` at 0x8374F160 and before `cmpdi cr6,r11,0`.
// r11 holds the barrier word as the guest sees it: byte 0 of memory in bits
// 63..56. r13 is the thread's PCR; +0x10C is its hardware-thread number.
void ng2AudioBarrierFix(PPCRegister& r11, PPCRegister& r13) {
  if (!REXCVAR_GET(ng2_audio_barrier_fix))
    return;
  const uint64_t word = r11.u64;
  if (word == 0)
    return;  // the loop's own exit
  auto* memory = REX_KERNEL_MEMORY();
  if (memory == nullptr)
    return;
  const uint32_t pcr = r13.u32;
  if (pcr < 0x1000)
    return;
  const uint8_t* number = memory->TranslateVirtual<const uint8_t*>(pcr + 0x10C);
  if (number == nullptr)
    return;
  const unsigned index = *number;
  if (index >= 8)
    return;
  const uint64_t mine = 0xFFull << (56 - 8 * index);
  if (word & mine)
    return;  // still waiting for the others: the normal spin

  // My byte is gone: the round completed and was cleared, and another worker
  // has already arrived at the next round on this word. Take the exit.
  static std::atomic<uint32_t> rescued{0};
  const uint32_t n = ++rescued;
  if (n <= 3 || (n % 100) == 0) {
    REXLOG_INFO("Patch: audio barrier - worker {} took the completion its loop missed "
                "(word {:016X}, {} so far)",
                index, word, n);
  }
  r11.u64 = 0;
}
