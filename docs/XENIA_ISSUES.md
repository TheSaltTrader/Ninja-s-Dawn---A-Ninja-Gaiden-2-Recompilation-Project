# Xenia's known issues for Ninja Gaiden II, and how this port answers each

Xenia tracks per-title compatibility as labels on a GitHub issue. Ninja Gaiden II
(title `544307D5`) is [issue #296](https://github.com/xenia-project/game-compatibility/issues/296),
open since December 2015, and its report reads in full:

> loads, plays some sounds for a moment and crashes

**The subjects of this census are the labels on that issue, not a list written
here.** That matters: a document naming the problems it checks silently stops
covering the next one added. The labels below were read off the issue, all eight
of them, and each gets a verdict whether or not the verdict is flattering.

One of the eight, `state-gameplay`, is a status label rather than a defect. The
other seven are defects.

## The census

| Label | Verdict | What it is, and what this port does |
|---|---|---|
| `requires_protect_zero_false` | **handled** | The game reads guest address 0 during init. A runtime that protects the zero page faults there. `protect_zero=false` is sent before the guest starts, from `ng2_tuning.h`. |
| `requires_clear_memory_page_state_true` | **handled** | Pages the GPU writes need their state refreshed or the CPU reads stale contents. `clear_memory_page_state=true`, same route. |
| `vsync-off-speedup` | **handled** | Not cosmetic here. With vsync off the runtime raises the guest's vblank from 60 Hz to 1000 Hz (`graphics_system.cpp`, `no_vsync_interval_ticks = guest_tick_frequency / 1000`), and this title advances its logic on vblank — so the game runs *faster*, not smoother. Vsync defaults on, and the setting carries the consequence in both its tooltip and an inline warning when switched off. |
| `gpu-drawing-corrupt` | **fixed** | Root-caused to a codegen defect, not a GPU one: VMX128 registers v64–v127 were translated as reads of zero. Seven functions were affected and all seven were the video codec, which is why it presented as corruption. The investigation is written up in [VECTOR_COVERAGE.txt](VECTOR_COVERAGE.txt), including the two dead ends — re-encoding a video to a byte-matching profile, and confirming the game imports no media APIs at all — that ruled out the file and the SDK decoder before the codegen was suspected. |
| `gpu-drawing-missing` | **partly fixed** | Two causes found. The same v64–v127 defect above, and a GPU ring-buffer desync — `InitializeRingBuffer` reset the read pointer but not the write pointer, so after a re-init the consumer read a stale write index and drew nothing. That one is fixed and was verified over 107 ring re-inits across 52 attract-demo cycles with 0 failures. **A second cause remains**, see below. |
| `gpu-driver-issues-nvidia` | **available, off by default** | The runtime's answer is `use_fuzzy_alpha_epsilon`, an approximate alpha-test compare that stops the flicker this label describes. It is exposed as "Fuzzy alpha test" and defaults off, because it changes what the alpha test accepts. **It was not actually deliverable until 2026-09-05:** a guard reading `if (s.anisotropic >= 0)` had grown to enclose it, so ticking the box reached the plugin only if the player had also overridden anisotropic filtering — which defaults to off. Fixed, and the DELIVERY sweep in `tools/lodestone_census.py` now fails if any setting is stranded behind an unrelated one. No alpha flicker has been observed in this title on the hardware tested (RTX 5090). |
| `kernel-save-file-errors` | **handled** | Saves are written and read through the SDK's content manager against the signed-in profile, and an importer accepts real console STFS packages (`ng2_saveimport.*`): the package is extracted with the SDK, then moved from content type `00000002`/XUID 0 — where DLC lands — to `00000001` under the profile's XUID, which is the only place the game looks. Content opens were traced through a full session and every one was balanced. |
| `state-gameplay` | *status, not a defect* | Xenia reaches gameplay. So does this. |

**Seven defects: five fixed or handled outright, one handled by default with the
consequence surfaced, one available but off by default and unobserved here.**

## Where the census stops

The honest limit of the table above is that it covers Xenia's list, and Xenia's
list is a 2015 report that never got past "plays some sounds for a moment."
Everything this port hits *after* that point is, by definition, not in it.

Issues found here that Xenia never recorded:

| Problem | Status |
|---|---|
| Audio deadlock — the SDL callback consumed a semaphore credit and did not return it on the silence path, so the producer starved and sound stopped for the rest of the session | fixed |
| `db16cyc` compiled to nothing. On Xenon it delays ~16 cycles and hands them to the sibling SMT thread; the correct x86 translation is `PAUSE`, which hints the spin, cuts its power and pipeline cost, and avoids the memory-order-violation penalty on loop exit. It now emits one, in twelve guest spin loops. **This is a correctness fix and nothing more** — see the note below | fixed |
| Five guest functions absent from the manifest, faulting as "unregistered function" | fixed |
| Chapter 12 crash | fixed, chapter-scoped |
| **Chapter 12 → 13 transition** (the red mist after any boss) | **fixed** in v1.0.6: the post-boss state machine waits for the profile's achievement-write state, which the port never set; `ng2ChapterAwardFix` supplies it and the game runs its own transition. The handshake trace below is kept as the record of the investigation; see `ISSUES_AND_FIXES.md` C2. |
| **A second ring-buffer desync**, unrelated to re-initialisation: reproduced three times at the attract demo on 2026-09-05, twice ending in a hard freeze, with no `InitializeRingBuffer` anywhere in the log | open, not seen since the ring fix was fully deployed (`ISSUES_AND_FIXES.md` O2) |
| **Music stops mid-session** and does not come back. Captured live: the guest's audio callback blocked in `KeWaitForMultipleObjects` and never returned, which kills the whole audio worker loop — it cannot re-wait, pump another client, or even see the shutdown event | **fixed** in v1.0.2: the game's own audio rendezvous barrier missed a round; `ng2AudioBarrierFix` takes the loop's own exit (`ISSUES_AND_FIXES.md` A2). The trace below is the record. |

The ring-buffer row is the one open question left in this table. The two
investigations below reached their answers after this page was first written;
they are kept as written, as the record of how the answers were found.

### The music bug: reproduced, and traced to a specific handshake

Sound stops part-way through a session and never returns while the game carries
on normally. Reproduced on 2026-09-05 with the new instrumentation running, and
the capture matches the one taken from a player session earlier the same day
frame for frame — the same guest callback address, blocked in the same call.

The chain, all of it confirmed rather than inferred:

`AudioSystem::WorkerThreadMain` dispatches the guest's registered audio
callback, logged at startup as `RegisterClient: callback=83752F60`. That
function is a single tail-call to **`sub_8374EDE8`**, and it is there that
everything stops:

```
RtlEnterCriticalSection(0x83BBC810)      take the lock
KeSetEvent(0x84C29644)                   post work to the worker   (event B)
KeWaitForMultipleObjects(0x84C29634, +1) wait for completion       (event A)
RtlLeaveCriticalSection                  never reached
```

The counterpart is **`sub_8374E740`**, a worker loop with the mirror shape: wait
on event B, do the work, `KeSetEvent` event A, repeat. A clean ping-pong.

In the deadlock the callback has been waiting on A for over five minutes. What
makes that interesting is what is *not* there: **no thread is waiting on event
B.** The watchdog now reports every wait past 30 s and names its object, so an
idle worker parked on B would have been printed. It was not. The worker is not
waiting for work, and event A is never signalled, so the callback never returns
— and because `WorkerThreadMain` calls the callback synchronously, one stuck
callback ends *all* audio for the rest of the run. It cannot re-wait, pump
another client, or even see its own shutdown event.

Two candidate explanations were checked and rejected:

- **Duplicate host objects for one guest event.** Guest-declared events live in
  the title's static memory and the runtime maps them by stashing a handle in
  the guest dispatch header. Read live from the deadlocked process, both events
  carry a valid `REX` signature and a single stashed handle — A is
  `0xF80000E8`, B is `0xF80000E4`. The mapping is intact; a signal on one is not
  landing on some other object.
- **A lost wakeup in `KeSetEvent`.** `XEvent::Set` forwards to a host Windows
  event, and `SetEvent` on an auto-reset event with no waiter leaves it
  signalled, so a worker arriving late would still be released.

What remains open is why the worker never consumes B. The likeliest remaining
shapes are that its thread never started or has exited, or that it is blocked
somewhere else entirely. That is the next thing to establish, and the watchdog
will name it if it is blocked in anything the kernel owns.

### The chapter 12 → 13 handshake, and where it stalls

The flag the hand-off waits on is at guest `0x84C39440`. An earlier pass reported
that no code in the translated image touched it and concluded it must be a
runtime pointer. That was wrong, and wrong for a dull reason: codegen writes
`lis` immediates as **signed decimal**, so `0x84C40000` appears in the generated
C++ as `-2067529728` and a grep for the hex could never have matched. PowerPC
also rarely materialises a field's address — it forms a base and reaches the
field with the load's own displacement — so the address never appears whole
anywhere.

Tracking register values through `lis`/`addi` and checking each load's
displacement finds exactly **three** accesses in 24 MB of translated code:

| Where | What |
|---|---|
| `sub_822F0CA8` | `stw r29, -27584(r10)` — **sets** the flag, at the function's last instruction |
| `sub_822F16D0` | `lwz r10, -27584(r11)` — **reads** it, at `loc_822F184C` |
| `sub_822F16D0` | `li r10,0; stw r10, -27584(r11)` — **clears** it, at `loc_822F21B0` |

That is a two-flag producer/consumer handshake, and both sides busy-wait:

- **`sub_822F34B0`** (main side) calls the producer, which sets `0x84C39440`,
  then spins at `loc_822F356C` *while the flag is non-zero* — waiting for the
  worker to acknowledge. It then sets a second flag at `0x84C39444`.
- **`sub_822F16D0`** (worker) spins at `loc_822F184C` until the flag is
  non-zero, does the work, and clears it at `loc_822F21B0`. Each spin has one
  escape: a shared abort word at `+6088`.

In the hang, the main side is spinning because the worker never clears the flag.
And the worker **has no early return** anywhere between the point it stalls at
and the clear — zero `return` statements in those 1,200-odd lines. It therefore
cannot be taking a wrong branch. It is blocked inside a call.

Walking the worker's call tree two levels deep finds only two things in that
region that can block indefinitely: `NtWaitForSingleObjectEx`, and
`RtlEnterCriticalSection` by eight separate chains. The first was the one path
the old watchdog covered, and it never fired — which points at the critical
section, whose contended path waits with a null timeout, burns no CPU, and until
now printed nothing at all.

Both are instrumented now. The next time this happens, the log names the object.

One tempting explanation has been **ruled out**: that the spinning side holds a
critical section the worker needs. That would deadlock exactly as observed — one
thread at 100%, one at 0%, no kernel wait on the spinner — but it does not hold
here. `sub_822F34B0` takes no lock in its own body, the six calls it makes
before the spin either take none or acquire and release within themselves, and
the one split acquire/release pair in that neighbourhood
(`sub_822F3440` acquires, its caller `sub_822F4140` releases — legitimate, if
fragile) is not on the pre-spin path at all.

That check is coarse: it counts acquires against releases and cannot follow
branches, so it can miss a path that returns early while holding a lock. It is
evidence against the hypothesis, not proof, and it is recorded here so the next
person does not spend the afternoon on it again.

### The watchdog covered one wait path in five, and its silence was used as evidence

Worth recording as a mistake rather than a fix. The stuck-wait watchdog was
written for this very bug, and it instrumented `NtWaitForSingleObjectEx` and
nothing else. `KeWaitForSingleObject`, `KeWaitForMultipleObjects`,
`NtWaitForMultipleObjectsEx` and `NtSignalAndWaitForSingleObjectEx` were all
invisible, as was `RtlEnterCriticalSection`, which does not go through any of
them. So "the watchdog reports no stuck waits" — offered here and in the README
as a ruled-out cause — meant only "none on the one path that was watched".

A live capture settled it: the audio callback sat in `KeWaitForMultipleObjects`
for over thirty minutes, killing all sound, and the watchdog said nothing.

All five entry points now claim a slot through one scope guard, and the guard is
also applied to `xeKeWaitForSingleObject` itself so that everything funnelling
through the primitive — the critical section, the reader/writer locks — is
covered by construction rather than by remembering. Nesting is a no-op, so the
outermost caller keeps the slot and the most specific reason wins. Thresholds
are 30 s for the first report and 300 s for a second, final one: at the old 10 s
a healthy title screen produced thirteen warnings a minute, all of them worker
threads parked on a job semaphore, and a warning that is usually nothing is a
warning people learn to skip. A thread that parks on the same object again is
also silent the second time — it was released and came back, so it is working,
however long each wait runs; a deadlocked thread never gets to start a second
wait.

The 300 s line is **not** a deadlock test, and an earlier draft of this document
claimed it was. Measured instead of assumed: with the game running at a clean
60 fps, two threads crossed 300 s on a single unbroken wait, because some
threads here simply park until the player does something. The line reports a
fact — one long unbroken wait — and is read next to whether the game is still
running. It narrows the field; it does not decide.

### Why `db16cyc` is not the answer, though it looked like it

`db16cyc` was briefly the leading candidate for the Chapter 12 → 13 hang, and
the reasoning is worth recording because it was wrong for an instructive reason.

The argument was: on Xenon, `db16cyc` hands its cycles to the sibling SMT
thread, so a guest spin loop built around it is *cooperative*. Translate it to
nothing and the spin becomes hostile — a core pinned at 100% that never lets
the thread it is waiting for make progress. The game uses it in twelve spin
loops. A livelock where one side spins and the other never runs fits that shape
exactly.

What kills the argument is that the starvation it depends on cannot happen
here. The runtime *can* pin guest threads to host logical processors by their
Xbox 360 hardware-thread number — `XThread::SetAffinity` does
`set_affinity_mask(1 << cpu_index)` — but only when `ignore_thread_affinities`
is false, and it defaults to **true**. This port does not override it. So guest
threads are not pinned, they float across all 32 logical processors on the test
machine, and a thread spinning without yielding does not deny anything else a
processor: there are 31 others. The mechanism requires contention that does not
exist.

Two further points close it off. The address the main fibre spins on is a
runtime pointer, not a link-time constant, so it appears nowhere in the
generated code and could not be tied to any of the twelve sites. And the
consumer that should clear the flag, `sub_822F16D0`, contains no `db16cyc` at
all — the twelve sites sit in what look like the game's own lock primitives,
none of them in the function that stalls.

The fix stays in, because translating a spin hint to nothing is wrong on its own
terms and `PAUSE` is what it should have been. It is simply not this bug, and
saying otherwise would have retired an open question by assertion.

## Method note

This census follows the project's Lodestone rule: enumerate subjects from the
artefact so that what is *not* covered is counted rather than averaged away. The
label list came from the issue; the verdicts came from the code and from logs.
Where a verdict is weaker than "fixed" it says so, and where a claim was
over-stated once already it says that too.
