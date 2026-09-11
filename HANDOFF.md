# Handoff — resume point after 2026-09-05

Written so this work can be picked up cold, from nothing but this file. It is a
working document, not a published one: `local/` and this file are outside the
publication allowlist (`tools/stage_repo.py`), so absolute paths and machine
specifics are safe to write here and will never be staged.

Everything below is the state at the end of 2026-09-05.

---

## 1. One-paragraph summary

Three things were fixed and shipped: the stuck-wait watchdog (which was
instrumenting one of five kernel wait paths and no critical sections at all),
two graphics settings that had been inert since v0.5.4, and two faults in the
publication allowlist. Two open bugs — **music dying mid-session** and the
**chapter 12 → 13 hang** — were each traced to named guest functions and named
kernel objects but are *not fixed*. A third bug was discovered: the
**attract-demo freeze is not actually fixed**, and it sits on the default path.
Four earlier claims were withdrawn, one of which had been used as evidence.

---

## 2. Repository state

| Item | State |
|---|---|
| Version | `1.0.0` (`VERSION`) |
| Lodestone census | 6/6 green — `python tools/lodestone_census.py` |
| Publication staging | 83 files, sweep clean — `python tools/stage_repo.py` |
| Release decision | **Source + docs only.** No `ng2.exe`, no game assets, ever. |
| Not yet pushed | GitHub repo `TheSaltTrader/Ninja-s-Dawn---A-Ninja-Gaiden-2-Recompilation-Project` |

Modified today, all built and deployed:

```
SDK  (C:\Users\renoi\ClaudeCode\Fable 2 Recompile Xbox\rexglue-src)
  include/rex/kernel/xboxkrnl/threading.h        +26   WaitMark guard, shared
  src/kernel/xboxkrnl/xboxkrnl_threading.cpp    +174   watchdog, all wait paths
  src/kernel/xboxkrnl/xboxkrnl_rtl.cpp            +6   critical section marked
  src/codegen/builders/system.cpp                 +10  db16cyc -> REX_SPIN_YIELD
  resources/templates/codegen/pch_h.inja          +30  rex_spin_yield()

ng2recomp
  src/ng2_tuning.h            stranded-settings fix (see §6)
  src/ng2_menu.cpp            inline V-Sync warning
  tools/lodestone_census.py   new DELIVERY sweep
  tools/stage_repo.py         .txt allowed in docs; texpack denied by name
  docs/XENIA_ISSUES.md        NEW — the full technical record
  README.md, CHANGELOG.md     corrections
```

A backup of the pre-change threading file exists as
`xboxkrnl_threading.cpp.bak_<HHMMSS>` in the SDK tree.

---

## 3. Build, deploy, run

Scripts are preserved in `local/diag/`. They are the exact ones used.

```bash
# 1. SDK runtime + GPU plugin (rebuild after ANY SDK source change)
cmd /c "local\diag\build_sdk.cmd"

# 2. Deploy BOTH together. A source-built plugin against a stock runtime
#    makes the game exit at startup with no error message.
cp "C:/Users/renoi/ClaudeCode/Fable 2 Recompile Xbox/rexglue-src/out/win-amd64/Release/rexruntime.dll" \
   "C:/Users/renoi/ClaudeCode/Fable 2 Recompile Xbox/rexglue-src/out/win-amd64/Release/rexgpu-xenos.dll" \
   "C:/Users/renoi/ClaudeCode/Ninja Gaiden 2 Xbox360/ng2recomp/out/build/win-amd64-Release/"

# 3. The app itself (only needed for ng2recomp/src changes)
cmd /c "local\diag\build_app.cmd"
```

`local/diag/build_codegen.cmd` builds **`rexglue.exe`**, the codegen tool. Only
needed when an instruction builder changes; it also requires clearing the stale
PCH and regenerating all 1,109 files afterwards. Deploying the runtime DLLs does
*not* carry it — it lives in `RexBlue/win-amd64/bin/`.

Launch (PowerShell — quote the path, it has spaces):

```powershell
$root = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
Start-Process "$root\out\build\win-amd64-Release\ng2.exe" `
  -ArgumentList @('--game_data_root', ('"' + $root + '\game"')) -PassThru
```

Logs land in `out/build/win-amd64-Release/logs/ng2_NNN.log` (highest number is
newest). Add `--log_level debug` for the `[apu]` audio dispatch lines, but note
it is very verbose and perturbs timing.

---

## 4. OPEN — music stops mid-session

**Reproduced twice**, once from a player session and once locally; identical
captures. Full write-up in `docs/XENIA_ISSUES.md`.

### The chain, confirmed

```
AudioSystem::WorkerThreadMain           (rexruntime, calls the guest synchronously)
  -> sub_83752F60                       registered callback, logged at startup as
                                        "RegisterClient: callback=83752F60"
    -> sub_8374EDE8                     a single tail-call; everything stops here
```

`sub_8374EDE8` (in `generated/default/ng2_recomp.167.cpp`):

```
RtlEnterCriticalSection(0x83BBC810)        take the lock
KeSetEvent(0x84C29644)                     post work to worker   (event B)
KeWaitForMultipleObjects(0x84C29634, +1)   wait for completion   (event A)  <-- BLOCKS
RtlLeaveCriticalSection                    never reached
```

Counterpart worker `sub_8374E740` (`ng2_recomp.147.cpp`): waits on event B,
does the work, `KeSetEvent` event A, loops. A clean ping-pong.

### Key objects

| Address | Role | Host handle |
|---|---|---|
| `0x84C29634` | event A — completion; the callback waits here | `0xF80000E8` |
| `0x84C29644` | event B — work request; the callback sets it | `0xF80000E4` |
| `0x83BBC810` | critical section held across the wait | — |

### Why it is fatal

`WorkerThreadMain` calls the guest callback **synchronously**. A callback that
never returns stalls the entire audio loop — it cannot re-wait, pump another
client, or see its own shutdown event. One stuck callback ends all sound for the
rest of the run while the game plays on normally.

### What is established

- **Nothing is waiting on event B.** The watchdog names every wait past 30s, and
  the callback sat for 300s+; an idle worker parked on B would have printed. It
  did not. The worker is not waiting for work.
- **Not duplicate host objects.** Read live from the deadlocked process, both
  events carry a valid `REX` signature and a single stashed handle. Mapping intact.
- **Not a lost wakeup in `KeSetEvent`.** It forwards to a host Windows event;
  `SetEvent` on an auto-reset event with no waiter leaves it signalled.

### Next step

Find out why the worker never consumes B. Likeliest shapes: its thread never
started, has exited, or is blocked elsewhere. Concretely:

1. Reproduce (it happens on its own within ~20–40 min at the title screen).
2. `bash local/diag/capture_hang.sh <pid>` and look for a thread whose stack sits
   in the `sub_8374Exxx` region.
3. Check the watchdog lines for anything waiting on `84C29644` or handle
   `F80000E4`. Its absence is the whole clue.
4. `sub_8374E8B8` is the init routine (`KeInitializeSemaphore`, `KeResumeThread`,
   `ObReferenceObjectByHandle`). If the worker thread is never created or resumed,
   that is where it fails — worth instrumenting.

**Do not** try to force it by toggling the texture pack with SendKeys. It was
tried; SendKeys posts to the message queue and SDL reads raw input, so the
toggles never registered and the test proved nothing.

### The last specimen

The locally reproduced deadlock (PID 44472, log `ng2_034.log`) ended at
21:35:02 with a clean user-initiated shutdown, not a crash: `Settings: saved`,
then `Shutdown: the runtime did not finish in 3s; exiting now`. That 3-second
timeout is itself consistent with the diagnosis — the audio worker was inside
the guest callback and could not be joined. The live captures from that
specimen are `hang_212236.txt` and `events.txt` in the session scratchpad; the
findings from them are all recorded above, the raw files are not preserved.

---

## 5. OPEN — chapter 12 → 13 hang

### The handshake, confirmed

Flag at guest **`0x84C39440`**. Exactly three accesses in 24 MB of code:

| Function | Instruction | Role |
|---|---|---|
| `sub_822F0CA8` | `stw r29, -27584(r10)` | **sets** the flag, last instruction of the function |
| `sub_822F16D0` | `lwz r10, -27584(r11)` | **reads** it, at `loc_822F184C` |
| `sub_822F16D0` | `li r10,0` / `stw` | **clears** it, at `loc_822F21B0` |

Two-flag producer/consumer, both sides busy-waiting:

- **`sub_822F34B0`** (main side) calls the producer `sub_822F0CA8`, which sets
  the flag, then spins at `loc_822F356C` *while the flag is non-zero* — waiting
  for the worker to acknowledge. Then sets a second flag at `0x84C39444`.
- **`sub_822F16D0`** (worker) spins at `loc_822F184C` until non-zero, works, and
  clears at `loc_822F21B0`. Each spin escapes only on an abort word at `+6088`.

### What is established

- The worker has **zero early returns** between where it stalls and the clear —
  1,200-odd lines, no `return`. It cannot be mis-branching. It is blocked in a call.
- Only two things in that region block indefinitely: `NtWaitForSingleObjectEx`
  (the one path the old watchdog covered, which never fired) and
  **`RtlEnterCriticalSection` via eight separate chains**. The latter is the
  stronger candidate. Both are now instrumented.
- **Ruled out:** the spinner holding a lock the worker needs. `sub_822F34B0`
  takes no lock in its own body; its six pre-spin calls balance; the one split
  acquire/release pair nearby is not on that path. (Coarse — counts acquires vs
  releases, cannot follow branches. Evidence against, not proof.)

### Next step

Needs the playthrough — it only occurs at the real chapter 12 end. When it hangs,
the log should now name the object. Then `capture_hang.sh <pid>` for stacks.

---

## 6. FIXED today

### Stuck-wait watchdog — all blocking paths

Was `NtWaitForSingleObjectEx` only. Now one `WaitMark` scope guard at all five
kernel wait entries **and** at `xeKeWaitForSingleObject` itself, so anything
funnelling through the primitive (critical sections, reader/writer locks) is
covered by construction. Nesting is a no-op, so the outermost caller keeps the
slot and the most specific reason wins.

Thresholds: first report at **30s**, second and final at **300s**. A thread that
parks on the same object twice is silent the second time (released and came back
= working; a deadlocked thread never gets a second wait).

**The 300s line is not a deadlock test.** Two threads cross it on a single
unbroken wait while the game holds a clean 60 fps — some threads park until the
player acts. Read it next to a frame counter.

Known-benign long waiters at the title screen:
`thread 8` on `40004BC4`, `thread 13` on `F80000CC` (Event).

### Two settings that had never worked

`if (s.anisotropic >= 0)` in `ng2_tuning.h` had its closing brace three settings
too late, so `use_fuzzy_alpha_epsilon` and both `depth_float24_*` cvars were only
emitted when anisotropic was overridden — and it defaults to −1. Inert since
v0.5.4. New **DELIVERY** sweep in the census fails if any cvar is emitted under a
condition that does not mention the setting driving it; verified by reinstating
the exact bug and confirming a non-zero exit, with the restore byte-checked.

### Publication allowlist

`docs/VECTOR_COVERAGE.txt` was never published (docs rule allowed `.md` only).
Allowing `.txt` then exposed that `docs/texpack` — which the docstring always
claimed was excluded — was excluded only because its two files happened to be a
`.png` and a `.txt`. `texpack` is now denied **by name**.

---

## 7. NEW — the attract-demo freeze is not fixed

Leaving the title screen alone reproduced a ring parse failure three times,
twice ending in a hard freeze. **Not** the re-initialisation race fixed in
v1.0.0 (that one is genuinely fixed) — there was no `InitializeRingBuffer`
anywhere in the log, so a second cause exists.

Why it is fatal: on a bad packet `ExecutePrimaryBuffer` "recovers" by moving the
read pointer up to the write pointer, discarding every packet it stepped over —
including whatever interrupt the guest was waiting on. Guest waits forever, GPU
worker waits for commands, picture stops.

Symptom to grep: `ExecutePacketType0 overflow` followed by
`PRIMARY RINGBUFFER: Failed to execute packet`, then the log going silent.

Arguably higher impact than either bug above, because it is on the default path.

---

## 8. Claims withdrawn — do not repeat these

| Claim | Why it is wrong |
|---|---|
| `db16cyc` fixes the chapter 12 hang | The starvation it needs cannot occur: `ignore_thread_affinities` defaults **true** and is not overridden, so guest threads float across all 32 logical CPUs. Kept only as a correctness fix. |
| "0 ring failures on every build since" | Reproduced three times in one afternoon, twice ending in a freeze. |
| "Kernel waits ruled out" for chapter 12 | Rested on watchdog silence when 4 of 5 wait paths were never instrumented. |
| "Nothing healthy waits five minutes" | Disproven by the running build within minutes: two threads crossed 300s while the game held 60 fps. |

---

## 9. Gotchas that cost real time

- **Codegen writes `lis` immediates as SIGNED DECIMAL.** `0x84C40000` appears as
  `-2067529728`. Grepping a guest address as hex finds nothing and **proves
  nothing**. PowerPC also rarely materialises a field address — it forms a base
  and reaches the field with the load's displacement. Use
  `local/diag/find_flag2.py` (set `TARGET`); it tracks registers through
  `lis`/`addi`/`ori` and checks each load's displacement. This is what cracked
  both bugs.
- **Bash heredocs here eat a level of backslash escaping.** `\\b` became a
  literal backspace byte inside a regex and silently broke it. Write patch
  scripts with the Write tool.
- **Search strings with em-dashes fail to match.** README/CHANGELOG prose uses
  `—`, not `-`. Use the Edit tool on exact text.
- **Kill processes by PID only, never by window title.**
- **`$Pid` is read-only in PowerShell** — a `param([int]$Pid)` cannot bind.
- Check where log output actually *stops* before blaming what follows it.

---

## 10. Diagnostic scripts (`local/diag/`)

| Script | Use |
|---|---|
| `capture_hang.sh <pid>` | Non-invasive `cdb -pv` dump: all thread stacks, the guest flag, per-thread CPU time, locks. Read-only — cannot kill the target. |
| `audio_probe.ps1` | Per-process audio peak meter. Distinguishes "game produced nothing" from "device/routing". |
| `watch_audio.ps1 -TargetPid <pid>` | Long watch: audio death, ring failures, watchdog lines, freeze detection via log silence. |
| `find_flag2.py` | Every read/write of a guest address. Set `TARGET`. |
| `find_flag.py` | Every code site *forming* a guest address (for kernel objects passed by pointer). |
| `find_blockers.py` | Walks a function's call tree for imports that can block. |
| `lock_balance.py` | Enter/leave balance per function. Coarse; cannot follow branches. |
| `census_blocking.py` | Coverage census of blocking call sites vs instrumentation. Note: its function detection is weak and it can over-report coverage — verify hits by hand. |

To measure whether a thread is blocked or spinning without attaching a debugger,
sample `(Get-Process -Id N).Threads[].TotalProcessorTime` twice a few seconds
apart. Zero delta over 8s = genuinely blocked.

---

## 11. Suggested order of work

1. **The attract-demo freeze** — highest impact, on the default path, and
   reproducible within minutes at the title screen rather than needing a
   playthrough. Start by finding what makes the parser read a non-packet when no
   ring re-initialisation has occurred.
2. **The music bug** — reproduces on its own in 20–40 minutes; instrumented;
   the specific question is why nothing waits on event B.
3. **Chapter 12 → 13** — blocked on a real playthrough.
4. Re-cut the release package; the current one predates all of today's fixes.

Still open from before today, untouched: chapter 12 runs at ~28 fps (later
withdrawn: the test machine's GPU was saturated by other work); BC7/BC3
texture compression never attempted; ~1,300 missing guest functions need
per-candidate extent validation before bulk registration (a previous attempt
produced overlapping extents and undefined-label compile errors).

---

## 12. Session 2 findings (later 2026-09-05) — deeper on all three

### #2 chapter 12->13: the last static hypothesis is now DISPROVEN

`local/diag/chapter_cs_trace.py` resolves every critical section the stuck
worker (`sub_822F16D0`) enters and every one the spinner (`sub_822F34B0`)
enters, by tracking the `r3` argument through lis/addi/mr at each
`RtlEnterCriticalSection` call.

Result — **no shared critical section**:

```
WORKER  enters CS 0x82003A6C (x20, likely CRT heap), 0x83A7EBA4,
                  0x83BBC830, 0x859D5978, + 1 dynamic (loaded from memory)
SPINNER enters CS 0x83A783DC, 0x83BE3850, 0x84C3BD80, 0x84F3F154,
                  + 1 dynamic
```

So the hang is NOT a hold-and-wait on a named lock. The worker is blocked on
`NtWaitForSingleObjectEx` (an event/semaphore) or on its one *dynamic* CS whose
address is not statically resolvable. **Static analysis is exhausted for #2** —
it genuinely needs the playthrough, at which point the watchdog names the object.
(Aside: worker CS `0x83BBC830` sits `0x20` from the music bug's lock
`0x83BBC810` — adjacent slots in one lock array, not the same lock.)

### #1 music: the worker thread is created CONDITIONALLY

The worker entry `0x8374E740` is formed exactly once, in `sub_8374E8B8`
(`ng2_recomp.270.cpp`), and passed to `ExCreateThread` as the start address (r7):

```
lbzx r11,...              ; mode selector byte
clrlwi r10,r11,30         ; r10 = r11 & 3
beq  loc_8374EBC4         ; (byte & 3)==0 -> NO thread created
cmplwi r11,1
bne  loc_8374EB60         ; byte != 1    -> a DIFFERENT worker entry
addi r7,r11,-6336         ; r7 = 0x8374E740   (our worker) only when byte==1
ExCreateThread ; blt -> skip resume if create returns < 0
KeResumeThread
```

BUT audio plays fine before it dies mid-session, so non-creation is NOT the
cause — the worker is created and runs, then stops answering.

**What the death-moment capture must distinguish** (worker idle-waits on event B
= `0x84C29644` via `KeWaitForSingleObject`):
  - a watchdog wait on `0x84C29644` / handle around it  => worker EXISTS and is idle
    (means B was never set to it — signalling path broke)
  - a host stack inside `sub_8374E7xx` / its work callees => worker is STUCK mid-work
  - neither                                              => worker EXITED or died
This is decidable from `capture_hang.sh <pid>` at the moment audio peak hits 0.
No rebuild needed — the current watchdog already covers the idle wait.

### #3 attract freeze: the ring error is common; the FREEZE is the rare outcome

The handoff conflated two things. The ring parse error at the attract transition
is frequent and usually **recovers** (drops to 30fps, plays the demo). Example:
`ng2_030` recovered; `ng2_029` froze. The freeze is the subset where the packets
skipped by `ExecutePrimaryBuffer`'s recovery happened to contain an interrupt the
guest was waiting on.

Two distinct attract paths, confirmed live:
  - WITH `InitializeRingBuffer` (twice in ~1ms): the re-init race — **fixed**,
    reproduced live this session with 0 failures.
  - WITHOUT `InitializeRingBuffer` (029/030): the open second cause. The garbage
    Type-0 packet count (e.g. `0x7B40`) is suspiciously near the 32KB ring size
    (`0x8000` dwords), i.e. **a write-pointer value being read as a packet
    header** — a stale/desynced read pointer, not random corruption.

The `DumpRingState` + `DumpRingHistory` output (read_idx, write_idx, wptr
history) is already deployed and will fire on the next no-reinit failure. Logs
029/030 predate its deployment, which is why they show the overflow but not the
dump. The fresh dump is THE artifact needed to fix #3.

Onset clue: the freeze in `ng2_029` was immediately preceded by
`XMPSetPlaybackController(0,0)` (the XMP background-music API) +
`BroadcastNotification(id=0xa000003)`. Hold loosely — may be coincidence with
the attract transition, may connect #1 and #3.

---

## 13. Session 2, part 2 — music reproduced live + settings UI work

### #1 music: REPRODUCED again, mechanism reconfirmed (worker ID still blocked)

Reproduced on the instrumented build (pid 53144, capture
`local/diag/captures/music_full_221011.txt`). Solid, defensible facts:
  - Audio peak = 0 at the TITLE SCREEN, which normally has music -> audio broken.
  - The audio callback (guest thread 2, `sub_8374EDE8`) is stuck in
    `KeWaitForMultipleObjects` on event A (`0x84C29634`) for 300s+.
  - NOTHING waits on event B (`0x84C29644`). Watchdog reports every wait >30s;
    an idle worker on B would show. It does not.
  - Event objects verified intact again: A->handle `F80000E8`, B->`F80000E4`,
    both valid `REX` signatures. So the SDK event mapping and waits are fine.

CONCLUSION: the fault is a GUEST-SIDE failure to signal completion, not an SDK
event/threading bug. Same CLASS as the chapter 12->13 worker.

**What could NOT be nailed, and why** (recorded so it is not over-claimed):
  - Which guest thread is the audio worker (`sub_8374E740`) is UNRESOLVED.
    Several guest threads spin at 100% (tids 2f44/XThread-E0, e48/XThread-D4,
    da00, and Main), but their CPU totals are 12-20 MINUTES while the deadlock
    is only ~5 min old -- so they have been busy far longer than audio has been
    dead, and are probably the game's normal busy threads, NOT the stuck worker.
  - No `ng2.pdb` exists, so guest frames show only as `ng2+0xRVA` and cannot be
    mapped to `sub_XXXX`. This is the single biggest blocker to finishing #1.

**The concrete next step for #1**: get symbols. Either (a) a build that emits a
guest-VA -> RVA map (codegen knows both), or (b) targeted logging INSIDE the
audio path at the SDK boundary -- e.g. a watchdog in
`AudioSystem::WorkerThreadMain` that fires when a dispatched callback has not
returned in N seconds and dumps the guest thread that holds event B. Without one
of these, live forensics stalls at "worker never signals A".

### Settings UI changes (user request, DONE + built)

`src/ng2_menu.cpp`, built into `ng2.exe` at 22:12, census still 6/6, staging 83:

1. **Browse button for the texture folder.** The "Folder" row now has a
   `Browse...` button beside the text field, using the same `PickFolder` picker
   every other folder row already uses. Typing a path still works.
2. **All settings editable everywhere.** The restart-bound display settings
   (Monitor, Resolution, Frame rate, Internal render size, Supersampling,
   Accurate depth, Fuzzy alpha, Sharpening, Texture cache, Anisotropic) were
   disabled in the in-game overlay. The five `BeginDisabled(!live)` /
   `BeginDisabled(live)` gates are now `BeginDisabled(false)` -- always editable.
3. **Red "restart required" tag** replaces the amber "(restart)". Shown in-game
   on restart-bound rows (still conditional on `!live`, since a setup-screen
   change applies at launch and needs no restart).

SAFETY (verified before editing): `ApplyLiveSettings` (ng2_menu.cpp:831) pushes
only a curated live-safe subset (post-effects, vsync, sharpness, texture pack,
video_mode). NONE of the newly-editable settings are in it, so changing them
in-game only SAVES them for next launch -- exactly what "restart required" means,
and provably cannot mis-apply live. Validated with
`local/diag/check_menu.py` (brace/paren balance, BeginDisabled==EndDisabled 12/12).

---

## 14. #3 attract freeze — the dump was never deployed; overflow is DETERMINISTIC

Two discoveries that unblock #3:

1. **The `DumpRingState` code was never in the deployed GPU plugin.** Across four
   sessions the overflow printed but the dump never did - because the deployed
   `rexgpu-xenos.dll` did NOT contain the dump strings (`grep -c "ring: base"` on
   the deployed DLL = 0), even though the source has the call. The obj was stale
   (ninja did not recompile `command_processor.cpp` on the intervening SDK
   builds, which only relinked). **Lesson: after editing an SDK .cpp, confirm the
   string actually landed in the DEPLOYED dll (`grep -c <marker> <dll>`), not just
   that the build succeeded.** The dump is now `RINGDUMP:` and verified present in
   the deployed DLL as of 22:16.

2. **The overflow is deterministic.** At the `demo1.wmv` attract transition it
   fires with the EXACT same values every time: `read count 00000A64,
   packet count 00002638` (ng2_030 and ng2_036 identical). Same garbage read at
   the same offset - not random corruption. It usually RECOVERS (drops to 30fps,
   plays the demo); the FREEZE is the rarer subset where a skipped packet held a
   needed interrupt. Because it is deterministic and fires within ~1-2 min of
   idling at the title screen, the RINGDUMP (read_idx/write_idx/epoch + wptr
   history) is now easy to capture - that is the artifact that roots-causes #3.

The dump line now also states `read AHEAD of write` vs `read behind write` and
the ring epoch, so a stale-read desync with no intervening re-init (epoch
unchanged) is readable at a glance.

---

## 15. CRITICAL: the GPU plugin was NEVER deployed - build reverted it every time

The single most important discovery of the night. **The running game has been
using a Sept-4 GPU plugin for all of today's work.** None of the GPU-side fixes
(ring re-init fix, RINGDUMP, per-stage texture warming) were ever in the running
game, despite every build "succeeding".

### Mechanism

`build_app.cmd` -> `rexglue_setup_target(ng2 GPU_PLUGINS xenos)` copies the GPU
plugin (and runtime) from the SDK INSTALL tree
`Ninja Gaiden 2 Xbox360/RexBlue/win-amd64/bin/` into the run dir. That install
tree held Sept-4 DLLs. So the deploy sequence that FAILED was:

  1. build SDK -> fresh DLLs in rexglue-src/out/win-amd64/Release/ (6.4MB gpu)
  2. cp fresh DLLs -> run dir  (correct, but...)
  3. build_app  -> copies STALE 2.7MB gpu from RexBlue/bin OVER the fresh one
  4. launch -> runs the Sept-4 plugin

Sizes are the tell: fresh rexgpu-xenos.dll = 6,479,872 bytes (RINGDUMP=1,
warm=1); stale = 2,725,376 bytes (Sept-4, RINGDUMP=0, warm=0). Fresh
rexruntime.dll = 11,024,384; stale = 9,930,240.

### The fix (applied ~22:45)

Deploy fresh DLLs to BOTH the run dir AND `RexBlue/win-amd64/bin/` (the tree the
app build copies from). Verified: after a subsequent `build_app`, the deployed
gpu plugin STAYS 6.4MB with RINGDUMP. Stale originals backed up as
`RexBlue/win-amd64/bin/*.dll.sept4bak`.

### CORRECT deploy procedure from now on

```bash
SRC=".../rexglue-src/out/win-amd64/Release"
for d in rexgpu-xenos.dll rexruntime.dll; do
  cp "$SRC/$d" ".../RexBlue/win-amd64/bin/$d"                    # the install tree
  cp "$SRC/$d" ".../ng2recomp/out/build/win-amd64-Release/$d"    # the run dir
done
```
ALWAYS verify a marker landed in the RUN-DIR dll AND survives an app build:
`grep -c RINGDUMP <rundir>/rexgpu-xenos.dll` must be 1.

### What this means for the three bugs

- **#3 attract freeze**: the ring fix was NEVER in the running game. Now it is.
  Re-test whether the attract overflow still occurs at all. If it does, the
  RINGDUMP now fires and roots-causes it.
- **Texture warming (user feature)**: never ran (warm=0 plugin). Now deployable.
  It also needs `texture_pack=1` (currently 0; dump=1 instead - mutually
  exclusive) and builds per-stage lists during play (bar shows on stage REVISIT,
  since first visit catalogues the stage). Pack has 4125 .tex but no stages/ dir
  yet because nothing ever recorded them.
- **#1 music**: this is a GUEST livelock, unaffected by the plugin deployment
  (audio is in the runtime, which was independently checked). Still open.

### Lesson

"Build succeeded" is not "fix deployed". For anything GPU-side, the app build
re-stages the plugin from RexBlue/bin, so a fresh build in rexglue-src/out is
NOT enough. Verify the marker in the RUN-DIR dll after the app build.

---

## 16. 2026-09-06 morning - the NG2 window crashed at 07:07; v1.0.1 cut at 08:10

### What the crash left behind, and what it turned out to be

- The session died seconds after launching the game for a chapter run. The
  listener it had started (`watch_ch13.py`) died with it; the game did not.
- The listener's "stall" trigger was WRONG: flag A (`0x84C39440`) stays set
  for as long as a mode runs (main thread spins in `sub_822F34B0`, worker
  `sub_822F16D0` runs the level as a real XThread at 100% CPU) and is cleared
  only when the mode ends. "Held 15s" fired at a healthy title screen and the
  cdb attach cost a 2.7s frame. It now captures only on a watchdog line, log
  silence, or by hand (`python local/diag/watch_ch13.py --capture`).
- **"Access is denied" for py.exe -> python.exe was the LAUNCHER.** A game
  started from the Claude Code tool shell could not have py.exe start
  python.exe; the same game started via WMI
  (`Invoke-CimMethod Win32_Process Create`) ran the whole upscale. Launch the
  game and long-lived listeners through WMI, always. It also keeps them alive
  when the session dies.
- **The 3840x1600 stretch was a SETTINGS change, not a code regression.**
  Every log through 039 (22:46) says window 1280x720; the settings file was
  saved twice from inside the game at 22:49 and 22:52 (the first run of the
  build that made Monitor/Resolution editable in-game), and 040/041 came up at
  3840x1600. The binaries were identical across the good and bad runs. The
  release copy in `Releases/v1.0.0` "worked" because it has its own settings
  file at 1280x720.

### v1.0.1 - what shipped (all three verified or measured)

1. **Aspect ratio.** `ApplyDisplaySettings` (ng2_app.h) now tells the guest the
   largest 16:9 box that fits the window (3840x1600 -> 2844x1600) instead of
   the window size. Plus an SDK cvar **`video_mode_explicit`** (window.cpp,
   ui/flags.h, xboxkrnl_video.cpp): the kernel judged "not configured" by
   comparing with the default, so an explicit 1280x720 fell back to the window
   size; the port sets it by name. **Measured** with
   `local/diag/verify_pillarbox.py <pid>` (Windows Graphics Capture + numpy):
   lit columns 498..3341 of 3840 = picture 2844 wide, bars 0.0/0.3 brightness,
   60 fps. Screenshot `local/diag/captures/pillarbox_v101_title.png`.
2. **Cancel.** `RunHidden` (ng2_textool.cpp) puts the child in a job object
   with KILL_ON_JOB_CLOSE, starts it suspended, polls `PeekNamedPipe` so the
   cancel flag is seen within 50 ms, and `TerminateJobObject`s the whole
   py.exe -> python.exe -> realesrgan tree. Closing the job handle (game exit)
   kills it too. NOT yet exercised by a human - the user should try Cancel.
3. **Progress bar.** `upscale_textures.py` prints `PHASE 1/2 ...` / `PHASE 2/2
   ...`; `ExtractProgress` carries phase/phases/label; the menu restarts its
   clock per step and shows "Step 2 of 2 - 37% - about 12 min left". The
   misleading "Real-ESRGAN not installed" NOTE is gone from the AI path (the
   Lanczos object is only built when --ai is absent).

Deployed: SDK DLLs built 08:07 (`rexruntime.dll` 11,026,432 B with the
`video_mode_explicit` string; `rexgpu-xenos.dll` 6,479,872 B, RINGDUMP=1) in
BOTH `RexBlue/win-amd64/bin` and the run dir; `ng2.exe` rebuilt after. Release
cut with `tools/make_release.py` into `Releases/v1.0.1` + zip (45.7 MB).

### State of the three bugs

- **#3 attract freeze: looks fixed.** With the ring fix finally deployed
  (22:45), the 67-minute run 039 did 26 attract cycles with 0 ring faults;
  040/041/042 added more clean cycles. Not closed until a long unattended run
  says so, but it has not recurred since the real deploy.
- **#1 music, #2 chapter 12->13: unchanged**, both need the live capture. The
  listener now follows the logs folder of whichever ng2.exe is running (a
  release copy logs under its own folder), and reports the flag-A hold time
  in its status line instead of acting on it.

### Verified after the cut (08:12-08:20), and one more bug fixed

- **AI upscaler: was producing GARBAGE, now fixed.** `ai_verify.py` (4 real
  dump textures through `ai_upscale.upscale_many`) showed the "AI" output
  shifted, tiled and mostly black. Root cause: `realesrgan-ncnn-vulkan.exe -n
  realesrgan-x4plus -s 2` - the x4plus network is 4x only; at `-s 2` the tool
  assembles 4x tiles into a 2x canvas (starburst texture: max value 26/255).
  Fix in `tools/ai_upscale.py`: `NATIVE_SCALE`, run at 4x, Lanczos down to the
  requested scale. Re-verified: all 4 exactly 2x, non-blank, sharpness up on
  3/4. Sheets: `local/diag/captures/ai_scale_compare.png` (input | -s 4 | -s 2)
  and `ai_verify.png`. The 240 .tex files the 07:45 run wrote are garbled;
  `local/diag/repair_ai_textures.py` regenerates them from the dump PNGs.
  `realesr-animevideov3 -s 2` hit `vkQueueSubmit failed -4` (device lost) with
  the FLUX training holding the GPU - not used by the pipeline, noted only.
- **Per-stage warm + bar: WORKS end to end.** With a stage list present the
  boot ("chapter 1 is loading") warms it: log `[texpack] warmed stage 1: 4125
  files, 6100 MB` 12.5 s after the chapter line, and `shots_by_pid.py` caught
  the overlay: "LOADING TEXTURE CACHE 2099 / 4125 (51%)" (blue) then "TEXTURE
  CACHE READY" (green). Frames `local/diag/captures/warm_*.png`. The list was
  a SYNTHETIC `stages/ch01.txt` of every pack id, deleted after the test - the
  real lists are recorded during play, and no run so far has recorded one
  because the title screen replaces nothing (0 `replacements` lines in every
  log with the pack on: title textures are UI, excluded from the pack). Expect
  `[texpack] stage N now lists M textures` on the first real chapter with the
  pack on, and the bar on the next visit to that chapter.

### Also noticed, not fixed

- "Process textures" was relaunched 13x in 5s in log 040 - that was the
  launcher failure making each attempt fail in ~80 ms; with the spawn working
  it ran once. If it recurs with a working spawn, the button is re-arming.

---

## 17. 2026-09-06 09:35-10:00 - THE MUSIC BUG, ROOT-CAUSED AND FIXED (v1.0.2)

### The specimen (log 044, capture `ch13_manual_093524`; log 045, capture `ch13_audio_094801`)

Music died at the title screen at 09:28:47 (8 s after the attract demo handed
back to the title) and again at 09:47:52 in the idle repro run (same
transition). Both times, with NOTHING attached:

- the runtime's audio thread ("Audio Worker (F8000014)") sat in
  `KeWaitForMultipleObjects` on event A inside `sub_8374EDE8` (the callback);
- BOTH guest audio workers - handles F80000DC (`sub_8374E740`) and F80000E0
  (`sub_8374E808`) - were at 100% CPU inside `sub_8374F078`, called from
  `sub_8374EF40` at two different sites (+0x1F8 and +0x2B2);
- `sub_8374F078` decoded from the generated C++: a rendezvous barrier. Arrive =
  `stbx 1` into byte [hardware-thread number from r13+0x10C] of an 8-byte word
  at r4; mask = one byte per non-zero slot of six at engine+308..328; spin
  (`db16cyc`) until word == mask, then the observer stores 0; others exit on 0.
  Two words at engine+356 / +364 alternate per round (`addi r4,r31,356` and
  `r11 = r31 + 8*(r29 toggling 0/1)`). Engine pointer = global 0x84C3BA94.
- Live values (read-only poll in `watch_ch13.py`): slots = [0,0,0,0,F80000DC,
  F80000E0]; word[0] = 00 00 00 00 01 00 00 00; expected = 00 00 00 00 01 01
  00 00; word[1] = 0. Each worker's r13+0x10C: 04 and 05 (correct).

So worker 5 DID arrive and its byte was CLEARED afterwards, and the word now
holds only worker 4's fresh arrival. Worker 4 (at site 1 = a new job's first
round) is one job ahead of worker 5 (at site 2 = the previous job's last
round). A job with an even number of rounds ends on word 0, the next job starts
on word 0: the double-buffering that protects a slow reader fails on that
boundary. Worker 5's re-read was delayed (yield / preemption) across worker
4's clear -> KeSetEvent(A) -> callback -> KeSetEvent(B) -> next job -> arrive
window, so it never saw the zero. On the console the re-read is ~30 ns and
always wins. **This is the game's own race, exposed by host scheduling.**

### The fix

`config/hooks/patches.toml` + `src/ng2_audio_fix.cpp`: midasm hook
`ng2AudioBarrierFix` at 0x8374F164 (after `ld r11,0(r4)`, before `cmpdi`):
if the thread's own byte is no longer in the word, set r11 = 0 so the loop
takes its own `blr`. Cvar `ng2_audio_barrier_fix` (default true) for A/B.
Each rescue is logged: `Patch: audio barrier - worker N took the completion
its loop missed`. That line appearing = the race happened AND was survived.

### The instrument (keep using it)

`watch_ch13.py` now polls the engine slots and both barrier words every 2 s and
captures when a word is non-zero and UNCHANGED for 6 s (`AUDIO BARRIER STUCK`
line names the stray/missing bytes). Reproduced at 09:47:52, 3 minutes into
an idle title-screen run. `audio_probe.ps1` confirms the meter at 0.

**Do not run long cdb sessions against this game.** The 09:38 session that
disassembled with symbol loading (all threads suspended 5+ s) was followed by
the process vanishing with no crash record and no shutdown line (log 044 ends
09:37:27). Short `-pv` captures (~3 s) have been survived every time.

### Open thread this points at

`build_db16cyc` says the game has TWELVE `db16cyc` spin loops. Decoded: eight
are one countdown delay in `sub_83737AE0` (not a rendezvous); `sub_83748588`
has a "wait for my bit" loop (loc_837485B0, bitmask at r31+48, abort at
r31+52) and a "wait for flag to clear" loop (loc_83748818, r31+88);
`sub_83751C40` is a timed wait on the timebase. None is the barrier shape, but
the two in `sub_83748588` are the next candidates if the chapter 12->13 hang
shows a worker spinning rather than blocked.

### v1.0.2 cut 12:36 (fix + texture-menu upgrade), verification run started

`tools/build.cmd` is fine; it was MY invocation that broke: `cmd /c "<path
with spaces>\build.cmd Release"` makes cmd strip the outer quotes (its rule:
quotes are kept only when the whole quoted text is one existing file), so it
ran `C:\Users\renoi\ClaudeCode\Ninja`. Without an argument the quotes survive,
which is why `build_app.cmd` always worked. Invoke it as
`cmd /c ""<path>\build.cmd" Release"` or from PowerShell. Equivalent sequence:
`"../RexBlue/win-amd64/bin/rexglue.exe" codegen ng2_manifest.toml` (only the
changed generated file is rewritten: "1 written, 1108 unchanged", so no PCH
churn) then `local/diag/build_app.cmd`. Codegen prints two pre-existing
"Unresolved b target 0x83A4A9E0 / 0x83A4A9B4" lines (CRT region) - unchanged
from before, not chased.

Release `Releases/v1.0.2` (hook string, `--only-missing`, `NATIVE_SCALE` all
verified in the package; census 6/6, staging clean). Idle verification run
launched 12:36 (pid 25060, log 046) with the listener polling the barrier.
What proves the fix: `Patch: audio barrier - worker N took the completion its
loop missed` in the log WITH the audio meter still non-zero afterwards. What
disproves it: `AUDIO BARRIER STUCK` from the listener.

The v1.0.2 menu was photographed live (`local/diag/shoot_menu.py <pid>`:
F10, shot, scroll, shot, F10) - `local/diag/captures/menu_2_scrolled.png`
shows the counts, "Enhanced textures: ON - 0 enhanced and 27821 original
loaded right now" (title screen: nothing from the pack is on screen), "The
pack was made at 2x, Real-ESRGAN, detail 1.00 - matches the settings above",
the Redo box, "Process 0 waiting textures" greyed with its reason.

Benign waiters added to the listener: the attract demo creates short-lived
guest threads (86, 90, 94 in log 044) that park ~30 s on 400FDC34 / 400FDCA4
and then exit; they tripped a needless capture each cycle.

---

## 18. 12:50 - THE CHAPTER 12 -> 13 HANG WAS OUR OWN GUARD (v1.0.3)

Live specimen, log 046, pid 25060: the user loaded the chapter-12 save,
killed the boss, and the game sat "in the mist" - 60 fps, answering the pad
(the A/B handshake flags cycled on button presses at 12:56), the chapter
worker in its normal per-frame loop (`sub_83737AE0` is a bounded frame wait,
not a rendezvous; it was on the stack at every healthy capture too). Not a
deadlock: a skipped state.

The log named it:

    12:50:19.675 Patch: chapter-12 site, [0x84C23C48] = 0xF0B72AE0
    12:50:19.675 Patch: chapter-12 guard - [0x84C23C48] = 0xF0B72AE0,
                 taking the game's early return (chapter 12)

`ng2PatchChapter12` had `force = cvar || CurrentChapter() == 12` - inside
chapter 12 it took the community workaround's early return UNCONDITIONALLY,
bypassing its own readability test. The pointer is a real object: readable in
the live process (all aliases A0/C0/E0/F0 committed), +0x30 = 0x40000000. The
site had carried 0xF0AAD450 for 500+ calls all game; the boss's death is the
first NEW value, i.e. the transition object, and the forced return threw its
work away. Gliniak's "causes problems" was exactly this.

Fix (`local/diag/patch_ch12_guard.py`, byte-level because patch_hooks.cpp
holds a literal NUL): `force = cvar` only; the readability check decides in
chapter 12 like everywhere else. Compiles. NOT YET VERIFIED - needs the boss
killed again on the rebuilt exe. If the game then crashes at 0x82834CD8 /
0x82834CE4 / 0x82834CEC (the dereferences), that is the real Xenia crash and
the guard needs to check the lwzx target too; if it proceeds, chapter 13 loads.

Also learned: `local/diag/sample_thread.py <pid> <tid>` (SuspendThread +
GetThreadContext sampler, microsecond pauses, names guest functions via the
live table) is ready for the next "spinning, not blocked" case.

Earlier today I called the chapter-12 CARD screen a hang (no file opens for 4
minutes after "chapter 12 is loading"): it was waiting for the A button. The
"chapter N is loading" line fires at the load-game menu's file enumeration,
not at the level start; the level's own opens come after Proceed.
- The hardware-detect / monitor-scale log line ("windows are scaled") did not
  appear in 040-042; `MonitorWorkArea` may be failing silently at 3840x1600.
