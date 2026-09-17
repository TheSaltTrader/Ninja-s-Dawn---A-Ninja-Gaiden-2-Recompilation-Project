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

**These are NG2's numbers and they do not transfer.** I quoted the cvar-off
figure to the Fable II session as though it described Fable - "Fable uploads
12-16 KB a frame, so it never reaches the pool's 256 KB threshold" - on the
reasoning that Fable also runs the cvar off. Same setting, different title,
different content, different geometry: it was an extrapolation, not a
measurement, and Fable measured the truth as ~1,540 UploadRanges calls a frame
with 850-990 threshold crossings per 5 s in ordinary play. So Fable exercises
that path CONTINUOUSLY, not at level loads, and my "dormant, not immune"
characterisation was wrong in the direction that understates their exposure.
A cvar value is not a workload.

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

**That caveat was the finding.** The hash covered only a range's first 4 KB, so
"unchanged" meant *its first 4 KB* was unchanged. Re-run with head AND tail
mixed into the same FNV:

| hash | bytes unchanged | ranges unchanged | throughput |
|---|---:|---:|---:|
| head only (4 KB) | 94.4% | 97.8% | 529 MB/s |
| **head + tail (8 KB)** | **71.0%** | **94.4%** | 470 MB/s |

Boot stayed low and slightly lower either way (2.5% → 2.2% of bytes), which is
the probe's own sanity check: a stricter hash must not make anything read as
*more* unchanged.

The drop is concentrated in **bytes** (−23 points) and not in **ranges**
(−3.4). So the ranges that changed verdict are few and large — the ones
carrying most of the volume have stable heads and churning tails. That is
precisely the population a head-only skip decision would have silently
corrupted, and it is 23% of the upload volume.

**71% was an upper bound — so it was re-measured at full coverage.** Any sample
can only over-report "unchanged", because a byte not hashed is a byte that
cannot disagree. Hashing the whole range settles it:

| hash coverage | bytes unchanged | ranges unchanged | redundancy |
|---|---:|---:|---:|
| head only (4 KB) | 94.4% | 97.8% | 16.8:1 |
| head + tail (8 KB) | 71.3% | 93.4% | 2.5:1 |
| **whole range (all)** | **71.2%** | **93.9%** | **2.5:1** |

Head+tail and whole-range agree to within 0.1 points on bytes and 0.5 on ranges.
**The bound is tight, not merely lower** — 4 KB was the outlier and 8 KB had
already found the answer. The true redundancy is 2.5:1, not the 16.8:1 the first
probe implied.

Boot held and improved monotonically as coverage grew — 2.5% → 2.2% → 1.3%
unchanged — which is what shows the probe changed and the game did not.

The 93.9% of *ranges* against 71.2% of *bytes* is the case for page-granular
skipping: that gap is entirely partially-dirty large ranges, which range
granularity would forfeit.

**The hashing cost is NOT in this data, and must not be read out of it.**
Steady-state frame p50 across the three probes was 26.78, 22.89 and 22.60 ms,
with a no-probe control at 26.79 — the heaviest hashing has the lowest p50,
which is nonsense as a cost signal. These runs reach different points in the
attract sequence, so scene variation dominates. Measuring the cost needs the
same scene twice, or better, an A/B of the skip itself behind a cvar, where cost
and benefit appear together in one one-variable control.

**One polarity error to not repeat.** The suggestion that the refresh be
narrowed to "pages a resolve actually touched" is backwards:
`valid_ = valid_and_gpu_written_` *keeps* the GPU-written (resolve-touched)
pages and drops the CPU-uploaded ones. Narrowing that way is either a no-op or
the inverse of the point. The refresh is not about resolves — it is a workaround
for CPU writes the write-watch misses.

### The page-skip A/B, and a correction to the freeze claim

The Fable II session built `shared_memory_upload_skip_unchanged`: hash each 4 KB
page, skip both the memcpy and the GPU copy for pages identical to the ones last
uploaded, and copy only runs of dirty pages. Page-granular because 93.9% of
*ranges* were unchanged against 71.2% of *bytes* — the volume lives in large
ranges with a few dirty pages, which range granularity would forfeit.

A/B on one build, one cvar apart:

| check | result |
|---|---|
| eviction counter non-zero (safety) | **pass** — 11,144 hashes evicted by a GPU write |
| visual: models, textures, flash | **pass** — clean, and reached the Ch1 cinematic at 60 fps |
| upload volume | **pass** — 71–74% saved in steady state, 46.5 GB avoided |
| frame time | **no clean difference** |
| freeze still at 13 swaps? | **unanswerable — see below** |

Aligned by sample ordinal (the attract sequence is deterministic from boot), the
steady-state band saved 57, 58, 58, 74, 74, 74, 73, 71, 71 percent against the
71.2% the whole-range probe predicted. Whole-run including boot is 46.4%, lower
only because boot genuinely rewrites everything — the mechanism degrades to
current behaviour exactly when the refresh is doing necessary work.

**The correction, and then the correction to the correction.** Neither leg of
the A/B froze — skip-on ran 28 swaps and skip-off 41, both stopped by hand. That
looked like the freeze had stopped reproducing two builds *before* the skip
existed:

| plugin build | outcome |
|---|---|
| `AllPagesGpuWritten` fix | froze, 13 swaps, 1 stall |
| churn head-only | froze, 13 swaps, 1 stall |
| churn head+tail | 19 swaps, **0 stalls** |
| churn whole-range | 23 swaps, 0 stalls |
| skip on / skip off | 28 / 41 swaps, 0 stalls |

That reading was wrong, and N=3 twice settles it. Same plugin, same detector,
same 45-swap target, one cvar apart:

| `shared_memory_upload_churn` | result |
|---|---|
| **off** | **FROZE 3/3** — at 13, 12, 17 swaps |
| **on** | **alive 3/3** — all reached 45 swaps, 0 stalls |

**The freeze never stopped. The diagnostic was suppressing it.** Sorted by probe
state instead of by build, every run of the day falls into line:

    probe off:  sharedtree 14, fix_test 13, repeat 13/12/17   ALL FROZE
    probe on:   head-only 13 (froze), head+tail 19, whole 23,
                skip_on 28, skip_off 41, churnon 45/45/45     only head-only froze

**A dose-response curve, which is the real finding.** Head-only hashes 4 KB per
range and the freeze survives it. Head+tail hashes 8 KB and it starts escaping.
Whole-range hashes everything and it never froze in five runs. The freeze
disappears as the probe grows more expensive — so **it is timing-sensitive**:
something in the upload path races, and anything that slows that path down hides
it.

Consequences worth stating plainly:

- **Any fix evaluated with the probe on will look like it works.** Two did.
- The skip A/B's "neither froze" says nothing about the skip. Its upload-volume
  result (71–74%) is unaffected — that came from the skip's own counters and the
  aligned hitch lines, not from survival.
- The original `clear_memory_page_state` result had no probe running, so the
  cvar genuinely changes it. But the *mechanism* proposed for it — upload volume
  — is not established, because volume and latency move together when that cvar
  is turned off.

The discriminator is a probe that adds LATENCY without doing useful work: a
fixed delay in the upload path, cvar-gated, no hashing. If a pure delay
suppresses the freeze it is a race and the volume story is dead; if only real
hashing suppresses it, touching those pages matters and it is a different bug.

### Two distinct failures, and how to tell them apart

These logs contain two different deaths. Conflating them cost a wrong report,
so check which one a run actually hit before counting it.

**1. The freeze.** 12–17 swap reports, then the log simply stops. Watchdog
reports guest thread 17 in `NtSignalAndWaitForSingleObjectEx` on Event
`F800003C`, usually preceded by thread 8 on `40004BC4` and thread 13 on
`F80000CC`. **No GPU error of any kind.** The last frame stays on screen. This
is the one under investigation.

**2. The ring-buffer desync.** Zero swap reports — it dies during boot, before
the first `[swap]` line. Loud, not silent:

    ExecutePacketType0 overflow (read count 00000024, packet count 0000FE00)
    **** INDIRECT RINGBUFFER: Failed to execute packet.
    ExecutePacketType0 overflow (read count 00000018, packet count 00007B90)

Seen twice, in `touch_3` and `spin5k_1`, and **the values are byte-identical
across both runs** — `24/FE00` then `18/7B90`, different probes, minutes apart.
Identical garbage twice is not a race scattering random bytes; the reader is
deterministically reading the same wrong memory. It appears in **none** of the
other seventeen runs, only in the two using the newest `shared_memory_upload_touch`
and `shared_memory_upload_spin_ns` probes.

In `spin5k_1` the line immediately before the first overflow is the shared
tree's own v0.2.12 diagnostic:

    [diag] gpu-written pages invalidated by a CPU write: 920 page(s) at 1F3DD000 len 3768320

Adjacency is not causation, but the ring buffer lives in guest memory, and
anything in the page-state path that can invalidate or re-upload pages holding
the ring itself would produce exactly this — structurally valid packets with
impossible counts.

**Detector note.** The freeze detector originally required `swaps > 0` before it
would declare a freeze, which made failure (2) invisible: the counter sits at
zero, the guard rejects it, and a real death reads as a timeout. It now watches
the log file growing instead, since a live game always writes something.

### The three-way discriminator

If the churn probe suppresses the freeze, *what about it* does? Three knobs,
each N=3, same plugin and same criterion:

| probe | what it costs | freeze suppressed? |
|---|---|---|
| `upload_churn` (hashes every byte) | time **and** memory traffic | **yes** — 3/3 alive to 45 swaps |
| `upload_touch` (one byte per page) | almost no time, same pages touched | **no** — froze at 17 and 13 |
| `upload_spin_ns=5000` (busy-spin, touches nothing) | time only | **no** — froze at 19 and 20 |

Pure delay at 5 µs/range — a figure chosen to match churn's own estimated cost —
does not suppress the freeze. That much stands, and it kills the clean "it is a
timing race" reading.

**But "neither half suppresses" was wrong, and the error is worth keeping.**
`touch` is not the memory half at full strength: it reads one byte per 4 KB
page, one cache line in sixty-four, about **1.5% of churn's traffic**. So the
three probes are not {time, memory, both} but:

    spin    0% of the traffic,    full time
    touch   ~1.5% of the traffic, ~no time
    churn   100% of the traffic,  full time

Read correctly, the result is not "neither half suppresses" but **"suppression
appears somewhere between 1.5% and 100% of the traffic"** — an enormous untested
gap, and the obvious place for the mechanism to live. The coverage sweep
(4 KB / 8 KB / whole-range) was a dose-response in **bytes read**, not in delay,
which makes it the most informative leg rather than a footnote.

**The invalidation adjacency is also dead.** The `[diag] gpu-written pages
invalidated by a CPU write` lines that precede one failure are at **fixed
addresses and fixed sizes in every run** — `1F045000` and `1F3DD000`, 3,768,320
bytes each (920 pages), exactly three lines — in runs that froze at 13 swaps and
in runs that reached 45 alike. Something that happens identically in every run
cannot be what distinguishes them, and its appearing just before an overflow is
coincidence: it appears just before everything.

**The spin probe has its own failure**, unrelated to the freeze: the ring-buffer
desync appeared in 1 of 3 runs at 5000 ns and 2 of 3 at 200 ns.

**That is not evidence of an inverse relationship**, and reading it as one here
was an error. One event's difference at N=3 is what three samples look like: a
single underlying rate near 0.5 produces that split about a third of the time.
It does not rule out the lag explanation and must not be quoted as doing so.

What does need explaining, and is a real signal: **the desync appears only in
probe runs** — zero in seventeen runs without a probe, several in the handful
with. The honest position is that its mechanism is unknown, not that one has
been ruled out.

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
