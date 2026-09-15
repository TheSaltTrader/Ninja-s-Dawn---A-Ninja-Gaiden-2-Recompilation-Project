# M4 — NG2 notes (side findings while Fable II is first)

- 2026-09-15 15:28: `scan_tables.py ng2 82010000 83FFFFFF ... --funcs ng2recomp --min 16`
  on the running NG2 (read-only, during the frame-interpolation session's
  slot) found NO heap run shaped like Fable's SetRenderState dispatch table
  (101 entries with the invalid-state stub repeated in 0..9). The heap runs it
  found (97 / 64 / 32 entries at 0x41483150, 0x4152A780, 0x415DFA30, ... with
  monotonically increasing targets sub_83A2B890..sub_83A392B8) are engine
  vtables/method tables, not the device. Either NG2's XDK build reaches its
  state setters without a per-device table, or the setters live outside the
  recompiled function set the scan matches against. Raw output:
  `M4_ng2_tables.txt`. To be redone with the census tracer on NG2 (the hook
  generator is game-agnostic; NG2's entry points are in `M3_ng2.json`).
