# The census hang: what it was not, and how the control was wrong

A worked example of a control that did not control anything, kept because the
mistake is cheap to repeat and cost three builds and six runs here.

## The symptom

Running the PM4 census on NG2, the game stops during the Chapter 1 intro. The
watchdog reports the same signature every time:

    [watchdog] guest thread 17 has been waiting 32s in
               NtSignalAndWaitForSingleObjectEx on F800003C (Event)

sometimes with threads 9/11/12 alongside on Semaphore F8000060. The GPU worker
stops executing packets; the last frame is rendered perfectly and stays on
screen. It looks exactly like a rendering hang and is not one.

## What the evidence looked like, and why it misled

| build | runs | hangs |
|---|---:|---:|
| census on, full | 4 | 4 |
| census on, register hook removed (bisect) | 1 | 1 |
| census on, dump deferred out of the swap packet | 1 | 1 |
| "stock" SDK plugin | 1 | 0 |
| shipped v1.0.21 | 1 | 0 |

Six against zero looks conclusive, and two hypotheses were built and tested on
the strength of it:

1. *The register census*, which instruments `WriteRegister` (~350k calls/s) and
   dirties a 128 KB table. Split onto its own gate and disabled — **still hung**,
   so the hot-path hook was exonerated.
2. *Logging from inside `XE_SWAP`*, immediately after `IssueSwap` hands the
   frame to the presenter, on the thread the guest is waiting on. The dump was
   restructured to snapshot at the swap and emit at an ordinary packet boundary
   — **still hung**, and sooner.

Both wrong, because the comparison was never between "census" and "no census".

## The control that actually controlled

The census is gated on an environment variable. The correct control was
therefore free and available from the first minute: **the same DLL, `NGPU_PM4`
unset**. Run it, confirm zero `[ngpu-pm4]` lines in the log, and watch.

It hung. Same signature, same place.

    census lines emitted: 0
    [watchdog] guest thread 17 ... on F800003C (Event)

**The census was never involved.** The "stock plugin" it was being compared
against was a different build, from a different tree, built on a different day.
That comparison isolated the entire build, not the feature — and a whole-build
difference was read as a one-feature difference.

## What the difference actually is

The plugin built from this branch hangs; the plugin shipped with v1.0.21 does
not. Both select the same render target path (`rov`) and the same
`clear_memory_page_state` (true — forcing the branch build to match made no
difference, so the Fable II 0.2.12 default flip is not the cause either).

The builds are not close. The worktree was configured with
`REXGLUE_USE_VULKAN=OFF` and `REXGLUE_FIDELITYFX_SPATIAL_ONLY=OFF` where the
working tree has both ON. Matching the configuration was the next test; the
point of recording it here is that "my source change" was never the right place
to look once the gated-off control had hung.

## The answer, measured 2026-09-17

**It is `clear_memory_page_state`.** Not the census, not the draw hand-off, not
the build configuration, and not this branch at all.

Rebuilding with the shared tree's exact configuration did not fix it. What
settled it was swapping only the plugin while holding `ng2.exe` and
`rexruntime.dll` constant:

| plugin | result |
|---|---:|
| shipped v1.0.21 | alive 240 s, 50 swap reports, 60 fps |
| shared tree current | froze at 14 swap reports |
| this branch (= shared + 2 env-gated files) | froze at 13 |

The shared tree's own plugin hangs NG2, so nothing on this branch is
implicated. Then one variable on an already-built binary:

| `clear_memory_page_state` | result |
|---|---|
| `true` — the setting NG2 requires | froze at 13–14, twice |
| `false` | alive past 32 swap reports, at 60 fps |

NG2 forces the cvar true (Xenia's compat entry for 544307D5 carries
`requires_clear_memory_page_state_true`; without the page refresh Team Ninja
titles lose character models). Fable II made it default **false** in v0.2.12,
because the refresh was re-uploading every CPU page each frame. So after that
change NG2 became the only title exercising the true-path — which is why only
NG2 finds the bug, and why it looked like an NG2-branch problem.

**The cvar costs more than stability: it halves the frame rate.** Every
cvar-true run sits at 30.0 guest fps; cvar-false reaches 60.0 and holds. So the
refresh is worth fixing rather than working around.

### What it is not, with the counter that proves it

The first hypothesis was `LandImpostorReadbackBeforeUpload` taking a GPU wait
from inside a draw, because it tests `AllPagesValid` — a predicate the frame-end
refresh deliberately invalidates. Plausible, and wrong. The plugin's own
`[gpu] fence waits` line counts landing waits separately, and it reads **zero in
every configuration, including before the fix with the cvar on**:

    before fix, cvar TRUE  (froze)   landings 0 x 0.0 ms   submissions 300-530
    before fix, cvar FALSE (alive)   landings 0 x 0.0 ms   submissions 600-602
    with fix,   cvar TRUE  (froze)   landings 0 x 0.0 ms   submissions 302

A wait that is never taken cannot be the wait that hangs. And the submission
counts are not a difference either: 600 per 5 s at 60 fps and 300 at 30 fps are
both two per frame, so that column tracks frame rate, not blocking.

Still open: which call on the refresh path blocks with a submission open. The
hang is a GPU worker that stops executing packets, so it is a wait taken inside
the frame, not a wait for it.

### What the cvar actually costs, measured

The mechanism is upload volume, not waiting. From the `[hitch]` line's own
`uploads N KB`, per frame:

| `clear_memory_page_state` | samples | mean | min | max |
|---|---:|---:|---:|---:|
| `true` (froze) | 83 | **17,451 KB/frame** | 36 KB | 43,176 KB |
| `false` (alive) | 88 | **820 KB/frame** | 8 KB | 43,172 KB |

A 21x difference in the mean. At the 30 fps those runs sit at, 17.5 MB/frame is
about 520 MB/s — inside the 460–700 MB/s Fable II measured for this refresh in
v0.2.12, so it is the same phenomenon seen from NG2.

The identical maxima matter: both configurations spike to ~43 MB on a streaming
load, so this is not a difference in peak capacity. It is that the true-path
pays a large cost on *every* frame where the false-path pays it only on real
loads. Steady-state frames are the tell — 13,548 and 22,232 KB with the cvar on
against 12 and 16 KB with it off, on frames drawing comparable geometry.

The upload pool never fails, though. `Shared memory: Failed to get an upload
buffer` appears **zero** times in all four frozen runs, and there is no
`E_OUTOFMEMORY`. (The one "device removed" hit in every log is the
`DRED (Device Removed Extended Data) enabled` banner, not a removal — worth
checking before reporting it as one.) So the pool is not exhausting, it is
churning ~17 MB of upload buffers per frame indefinitely, which fits a GPU
worker that stalls with no fence wait ever firing.

Which suggests where a fix lives: the refresh invalidates every CPU-uploaded
page at frame close, so the next frame re-uploads all of them. Whether it needs
to invalidate all of them, rather than only pages a resolve actually touched, is
a question about what the refresh *marks* — not about who waits.

### Is any of it necessary? 94% is not

The question that decides whether a fix can exist, put by the Fable II session
and worth restating because it reframes the cost: an invalidated page costs
nothing until something asks for that range. So the refresh is not *adding*
work — it is removing the ability to skip it. Which means the 17 MB a frame is
only waste if the bytes being re-read are the same bytes the GPU already had.

  * **different** — NG2 really is rewriting its working set every frame, the
    refresh is necessary, and the cvar's cost is inherent. No fix exists in
    shared memory.
  * **unchanged** — the write-watch was adequate for those pages and the whole
    volume is redundant. A fix is real and worth designing.

Their `shared_memory_upload_churn=true` probe hashes each re-uploaded range and
reports every 5 s. Thirteen samples on NG2 at cvar-true:

| phase | bytes unchanged | ranges unchanged | throughput |
|---|---:|---:|---:|
| boot / menu (4 samples) | **2.5%** | 27.3% | 15 MB/s |
| steady-state (9 samples) | **94.4%** | **97.8%** | 529 MB/s |

Steady-state totals: 22,469.6 MB unchanged across 690,726 ranges, against
1,334.1 MB changed across 15,873 — about 19 redundant bytes for every byte that
genuinely changed.

The progression is clean rather than noisy: 1%, 16%, 13%, 15% while loading,
then 87, 92, 93, 94, 96, 97, 97, 97, 93 once rendering. The transition is the
streaming load finishing. **So the refresh does necessary work during boot and
almost none in steady state** — which is a constraint on any fix, not just a
licence to weaken it.

The 529 MB/s also independently confirms the ~520 MB/s derived from the
`[hitch]` uploads figure: two different instruments, same number.

Caveat worth keeping: the hash covers the first 4 KB of a range, so "unchanged"
strictly means *its first 4 KB* is unchanged. 97.8% of 690,726 ranges is hard to
explain by sampling position, but hashing head and tail would close it.

**One polarity error to not repeat.** The suggestion that the refresh be
narrowed to "pages a resolve actually touched" is backwards:
`valid_ = valid_and_gpu_written_` *keeps* the GPU-written (resolve-touched)
pages and drops the CPU-uploaded ones. Narrowing that way is either a no-op or
the inverse of the point. The refresh is not about resolves — it is a workaround
for CPU writes the write-watch misses.

### The dev workaround

`--clear_memory_page_state=false` on the command line. Character models degrade,
which does not matter for counting draws — the stage-1 hand-off measurement
(5,593,900 draws, serials unbroken) was taken through it.

## What this does and does not affect

- **The census measurements stand.** They were read from a stream that executed
  thousands of correct frames before any stall, and the numbers are stable
  across frames, across scenes and across two different census builds.
- **The shipped product is fine.** v1.0.21 was run through the same Chapter 1
  intro to gameplay with no stall. The hang only exists in a build of this
  branch, which has never shipped.
- **It blocks long NG2 runs on this branch** until the build configuration is
  reconciled, which is why it is worth chasing rather than noting.

## The rule, for next time

When the thing under test is behind a flag, cvar or environment variable, the
control is **the same binary with the flag off**, and it is run FIRST, before
any bisect. Anything else is a comparison of two builds wearing the costume of
a comparison of one feature.

The inverse error was made earlier on this project — reading a control that also
failed as exonerating a feature, when it only shows the feature is not
*necessary* for the failure. Both are the same mistake about what a control run
can tell you.
