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

The builds are not close. The shipped plugin is 6.6 MB with 10,934 printable
strings; the branch build is 3.5 MB with 5,962. The worktree was configured
with `REXGLUE_USE_VULKAN=OFF` and `REXGLUE_FIDELITYFX_SPATIAL_ONLY=OFF` where
the working tree has both ON. Matching the configuration is the next test; the
point of recording it here is that "my source change" was never the right place
to look once the gated-off control had hung.

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
