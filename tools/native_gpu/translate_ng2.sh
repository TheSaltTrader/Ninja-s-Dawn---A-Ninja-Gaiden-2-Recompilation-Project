#!/bin/sh
# Translate NG2 shader containers with XenosRecomp and compile with dxc,
# PRODUCING ONLY ARTEFACTS THAT REPRODUCE.
#
#   translate_ng2.sh <container dir> <out dir> [glob]     (glob default: *.xvu)
#
# Separate from translate_all.sh, which is the Fable II side's and runs only
# fix_hlsl.py. NG2 needs a SECOND pass: two thirds of its draws are auto-index,
# so most of its vertex shaders fetch their own attributes and the recompiler
# emits a main() that never declares the inputs its body reads. That pass is
# fix_hlsl_ng2.py.
#
# WHY EVERY SHADER IS TRANSLATED SEVERAL TIMES. Measured over 80 containers,
# three runs each, on an idle machine:
#
#     STABLE 54 | UNSTABLE 13 | always failed 1 | SOMETIMES failed 12
#
# Thirty-one percent of containers do not translate to the same thing twice -
# some produce different HLSL between successful runs, some segfault on one run
# and succeed byte-identically on the next. A single pass therefore produces a
# manifest that is one roll of the dice, and for the unstable ones WHICH
# translation reached the artefact is a coin flip. So a result is only kept when
# STABLE_RUNS attempts agree byte for byte; everything else is recorded and
# refused. A smaller manifest that rebuilds identically beats a larger one that
# cannot be reproduced.
#
# The comparison is of the RAW recompiler output, before the fix passes, because
# that is where the non-determinism lives.
#
# A KNOWN LIMIT, STATED SO THE NUMBERS ARE NOT OVER-TRUSTED. On a shared machine
# whose free memory oscillates faster than a container takes to translate five
# times - measured here swinging between 2 GB and 46 GB within minutes - the
# FAILED column is not reliable. A refused allocation and the translator's own
# end-iterator crash both exit 126; the floor catches the case where memory is
# low when a container STARTS, and the post-failure re-check catches the case
# where it is still low when the container FINISHES, but neither catches a
# collapse that happens and recovers in between. Spot-checked: a container
# recorded "crashed" translated cleanly by hand minutes later, twice, on
# different runs.
#
# So FAILED is PROVISIONAL until re-measured on a quiet machine. STABLE and
# UNSTABLE are not affected: pressure can stop a translation happening, but it
# cannot make two completed translations of the same input agree when they
# would otherwise differ, nor differ when they would otherwise agree.
IN="$1"; OUT="$2"; GLOB="${3:-*.xvu}"
# FIVE, NOT THREE, AND "STABLE IN N RUNS" IS NOT "DETERMINISTIC". Two censuses
# over the same 80 containers named DIFFERENT containers as unstable, so N runs
# agreeing is evidence rather than proof. The first column of any report built
# on this must never be read as a coverage number.
STABLE_RUNS="${STABLE_RUNS:-5}"
TIME_CAP_S="${TIME_CAP_S:-120}"
MEM_CAP_MB="${MEM_CAP_MB:-4096}"
# A FLOOR ON FREE MEMORY, because a machine condition must never be recorded as
# a shader property. Under pressure XenosRecomp fails an allocation and dies
# with the SAME exit code as the end-iterator crash - 11 containers were logged
# "crashed" while another process on this machine held 66 GB, and the first one
# I re-ran by hand translated cleanly at exit 0. Both causes produce 126 and
# nothing in the code can tell them apart, so the only honest move is to refuse
# to measure at all below a floor and say which containers were skipped.
#
# Deferred, not failed: with RESUME=1 a later pass picks them up, so a busy
# machine costs coverage temporarily instead of corrupting the ledger.
MEM_FLOOR_MB="${MEM_FLOOR_MB:-8192}"

X=/c/Users/renoi/ClaudeCode/NativeGPU/build/xenosrecomp/XenosRecomp/XenosRecomp.exe
# THE HEADER IS AN INPUT UNDER TEST, AND THE TWO ON DISK ARE NOT INTERCHANGEABLE.
#
#   fable2_shader_common.h ....... g_HalfPixelOffset : packoffset(c32.z)
#   reference/.../shader_common.h  g_HalfPixelOffset : packoffset(c16.z)
#
# DEFINE_SHARED_CONSTANTS is a macro in the header, so that offset is fixed
# text, nothing to do with the container. The stock upstream one puts the shared
# block at c16 - inside the sampler descriptor arrays XenosRecomp emits - and
# EVERY shader then fails with "packoffset overlap". All 625 of NG2's real
# translations were made with the fable2_ one, so that is the working header
# despite its name.
H="${XENOS_COMMON:-/c/Users/renoi/ClaudeCode/NativeGPU/fable2_shader_common.h}"
D=/c/Users/renoi/ClaudeCode/NativeGPU/reference/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe
TOOLS="$(cd "$(dirname "$0")" && pwd)"
# A TIME CAP BOUNDS HOW LONG A PROCESS RUNS, NOT HOW MUCH IT TAKES WITH IT.
# XenosRecomp reached a 55 GB working set on one container and took the machine
# from 93.6 GB free to 0.5 GB, killing unrelated background work. Raising the
# deadline made that WORSE by giving it longer to allocate. run_capped.py puts
# both caps on, and tells crash (126) apart from time (124) and memory (125).
CAP="python $TOOLS/run_capped.py $TIME_CAP_S $MEM_CAP_MB --"

[ -x "$X" ] || { echo "no XenosRecomp at $X"; exit 1; }
[ -f "$H" ] || { echo "no shader_common header at $H"; exit 1; }
[ -x "$D" ] || { echo "no dxc at $D"; exit 1; }

echo "HEADER UNDER TEST: $H"
echo "  shared-constant base: $(sed -n 's/.*g_HalfPixelOffset : packoffset(\(c[0-9]*\).*/\1/p' "$H" | head -1)"
echo "RECOMPILER       : $X"
echo "STABILITY        : $STABLE_RUNS runs must agree byte-for-byte"
echo "CAPS             : ${TIME_CAP_S}s, ${MEM_CAP_MB}MB"

mkdir -p "$OUT/hlsl" "$OUT/dxil" "$OUT/tmp"

# ONE WRITER PER OUTPUT DIRECTORY. Stopping a background run does not
# necessarily stop its children promptly, so a RESUME started straight
# afterwards can race the run it is resuming - two processes appending to the
# same ledgers. That is what made the consistency check fire: the resumed run
# reported "keeping 23 stable" and added none, while stable.txt kept growing
# underneath it. The ledger was genuinely corrupt, so the check was right; the
# cause was a second writer, which is worth preventing rather than detecting.
LOCK="$OUT/.writer.lock"
if [ -e "$LOCK" ]; then
  holder=$(cat "$LOCK" 2>/dev/null)
  if kill -0 "$holder" 2>/dev/null; then
    echo "REFUSING: another run (pid $holder) is writing $OUT - stop it first"
    exit 1
  fi
  echo "note: stale lock from pid $holder, taking it"
fi
echo $$ > "$LOCK"
# Only release a lock we still own, or a superseded run frees the live one.
trap '[ "$(cat "$LOCK" 2>/dev/null)" = "$$" ] && rm -f "$LOCK"' EXIT INT TERM
tr_ok=0; tr_bad=0; unstable=0; c_ok=0; c_bad=0; deferred=0
# RESUMABLE, because this machine is shared and a long run is not safe from it.
# Two multi-hour regenerations were killed by the OS low-memory killer - not for
# anything this script did, but because another process on the machine was
# allocating tens of gigabytes uncapped and the OS picks a victim rather than a
# culprit. A run that loses everything to someone else's bug is a run that
# cannot be completed on a shared machine at all.
#
# RESUME=1 keeps the ledgers and skips any container already recorded in one of
# them. Default is a fresh run, because silently resuming into a ledger written
# by a DIFFERENT configuration - other caps, other STABLE_RUNS - would mix two
# populations under one heading, which is the error this whole file exists to
# prevent.
if [ "${RESUME:-0}" = "1" ] && [ -f "$OUT/stable.txt" ]; then
  echo "RESUME: keeping $(wc -l < "$OUT/stable.txt") stable, $(wc -l < "$OUT/unstable.txt" 2>/dev/null) unstable,"        "$(wc -l < "$OUT/failed.txt" 2>/dev/null) failed - already-recorded containers will be skipped"
  tr_ok=$(wc -l < "$OUT/stable.txt"); tr_ok=$((tr_ok))
  c_ok=$(ls "$OUT/dxil"/*.dxil 2>/dev/null | wc -l)
  unstable=$(wc -l < "$OUT/unstable.txt" 2>/dev/null); unstable=$((unstable))
  tr_bad=$(wc -l < "$OUT/failed.txt" 2>/dev/null); tr_bad=$((tr_bad))
else
  : > "$OUT/errors.txt"; : > "$OUT/failed.txt"; : > "$OUT/unstable.txt"; : > "$OUT/stable.txt"; : > "$OUT/deferred.txt"
fi
cd "$IN" || exit 1
for f in $GLOB; do
  [ -f "$f" ] || continue
  n=${f%.xvu}; n=${n%.var}
  # RE-VALIDATE THE LOCK EVERY CONTAINER, not only at startup. Stopping a
  # background task kills the process it tracks and NOT its children, so a run
  # that was "stopped" keeps looping - and a startup-only check waves it
  # straight through. That is how two writers ended up in one directory twice:
  # the second run truncated stable.txt and deferred.txt underneath the first,
  # two minutes after the first had started.
  #
  # A lock nobody re-reads is a lock that only protects against a race it can
  # see at one instant. If someone else now holds it, this run is the stale one
  # and stands down rather than corrupting a ledger it no longer owns.
  if [ "$(cat "$LOCK" 2>/dev/null)" != "$$" ]; then
    echo "STANDING DOWN: $OUT is now owned by pid $(cat "$LOCK" 2>/dev/null) - this run is superseded"
    exit 1
  fi

  free_mb=$(awk '/^MemFree:/ {print int($2/1024)}' /proc/meminfo 2>/dev/null)
  if [ -n "$free_mb" ] && [ "$free_mb" -lt "$MEM_FLOOR_MB" ]; then
    deferred=$((deferred+1))
    echo "$f  deferred, only ${free_mb}MB free" >> "$OUT/deferred.txt"
    continue
  fi
  if [ "${RESUME:-0}" = "1" ]; then
    if grep -qxF -- "$f  agreed $STABLE_RUNS/$STABLE_RUNS" "$OUT/stable.txt" 2>/dev/null ||
       grep -q -- "^$f  " "$OUT/unstable.txt" 2>/dev/null ||
       grep -q -- "^$f  " "$OUT/failed.txt" 2>/dev/null; then
      continue
    fi
  fi
  case "$f" in *_p.xvu|*_p.var.xvu|*_p.cpu.xvu) t=ps_6_0;; *) t=vs_6_0;; esac
  # A STALE ARTEFACT IS WORSE THAN A MISSING ONE: the manifest keys on "a .dxil
  # with this stem exists", so a leftover from a previous run makes it claim an
  # artefact for a shader that has since failed. Absence must mean failure.
  rm -f "$OUT/dxil/$n.dxil" "$OUT/hlsl/$n.hlsl"

  # REQUIRE N SUCCESSES THAT AGREE, NOT N CONSECUTIVE SUCCESSES.
  #
  # Crashes and output-instability are DIFFERENT defects - a dense element table
  # that makes the lookup impossible to miss still crashes 5/8 - so a gate that
  # demands an unbroken run of successes multiplies the crash rate into the
  # coverage figure. Measured per run on a quiet machine, paired and interleaved:
  # originals succeed 17/30 and rebuilds 12/30, so about 43% of attempts crash.
  # Demanding five in a row yields 0.57^5, about 6% - which is exactly the 4
  # stable out of 54 this gate was producing. The artefacts were not scarce
  # because the shaders were bad; they were scarce because the gate was
  # compounding an unrelated failure.
  #
  # So a crash is RETRIED within a budget and only the SUCCESSES have to agree.
  # That keeps the property worth having - no artefact whose translation is
  # non-deterministic - without inheriting a defect it was never about.
  attempts_left=$((STABLE_RUNS * 4))
  first=""; agreed=1; reason=""; crashes=0
  i=1
  while [ "$i" -le "$STABLE_RUNS" ] && [ "$attempts_left" -gt 0 ]; do
    attempts_left=$((attempts_left - 1))
    out="$OUT/tmp/$n.$i.hlsl"
    rm -f "$out"
    $CAP "$X" "$f" "$out" "$H" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -ne 0 ] || [ ! -s "$out" ]; then
      # A crash does not end the container - it costs an attempt and is retried.
      # A TIME or MEMORY cap does end it, because those say the machine could not
      # carry this work and retrying would measure the machine again.
      case "$rc" in
        124) agreed=0; reason="time cap"; break;;
        125) agreed=0; reason="MEMORY CAP"; break;;
        *)   crashes=$((crashes + 1)); continue;;
      esac
    fi
    h=$(md5sum "$out" | cut -d' ' -f1)
    if [ -z "$first" ]; then first="$h"
    elif [ "$h" != "$first" ]; then agreed=0; reason="UNSTABLE output"; break; fi
    i=$((i+1))
  done

  # Out of attempts without N agreeing successes: that IS a property of the
  # shader, and it is recorded as its own outcome rather than as a crash.
  if [ "$agreed" -eq 1 ] && [ "$i" -le "$STABLE_RUNS" ]; then
    agreed=0
    reason="only $((i - 1))/$STABLE_RUNS successes in $((STABLE_RUNS * 4)) attempts ($crashes crashed)"
  fi

  if [ "$agreed" -eq 0 ]; then
    # RE-CHECK MEMORY AFTER A FAILURE, not only before the attempts. The floor
    # above is tested once per container, but a container takes STABLE_RUNS
    # translations and the machine can collapse during them - which is exactly
    # what happened: 37 "failures" were recorded in a window when another
    # process was taking the machine from 26 GB free to under 2, and a crash
    # from a refused allocation is indistinguishable from the translator's own.
    # So a failure is only attributed to the SHADER if the machine was still
    # healthy when it happened; otherwise it is deferred and re-measured later.
    now_mb=$(awk '/^MemFree:/ {print int($2/1024)}' /proc/meminfo 2>/dev/null)
    if [ -n "$now_mb" ] && [ "$now_mb" -lt "$MEM_FLOOR_MB" ] && [ "$reason" != "UNSTABLE output" ]; then
      deferred=$((deferred+1))
      echo "$f  deferred, ${reason} with only ${now_mb}MB free - not attributable" >> "$OUT/deferred.txt"
      rm -f "$OUT/tmp/$n".*.hlsl "$OUT/tmp/$n".*.hlsl.layout
      continue
    fi
    case "$reason" in
      "UNSTABLE output") unstable=$((unstable+1)); echo "$f  $reason" >> "$OUT/unstable.txt";;
      *) tr_bad=$((tr_bad+1)); echo "$f  $reason" >> "$OUT/failed.txt";;
    esac
    rm -f "$OUT/tmp/$n".*.hlsl "$OUT/tmp/$n".*.hlsl.layout
    continue
  fi

  # RECORD THE AGREEMENT COUNT, so an artefact carries its own provenance.
  # Agreement is unanimous by construction today, so this is always N/N - but if
  # it ever loosens to a majority rule, "agreed 5/5" and "agreed 3/5, kept the
  # majority" must not both read as "translated".
  echo "$f  agreed $STABLE_RUNS/$STABLE_RUNS, $crashes crashed attempts" >> "$OUT/stable.txt"
  cp "$OUT/tmp/$n.1.hlsl" "$OUT/hlsl/$n.hlsl"
  rm -f "$OUT/tmp/$n".*.hlsl "$OUT/tmp/$n".*.hlsl.layout
  tr_ok=$((tr_ok+1))
  python "$TOOLS/fix_hlsl.py" "$OUT/hlsl/$n.hlsl" "$OUT/hlsl/$n.hlsl.layout" >/dev/null 2>&1
  python "$TOOLS/fix_hlsl_ng2.py" "$OUT/hlsl/$n.hlsl" >/dev/null 2>&1
  if "$D" -T $t -E main -HV 2021 -all-resources-bound -Wno-ignored-attributes \
          -Fo "$OUT/dxil/$n.dxil" "$OUT/hlsl/$n.hlsl" >"$OUT/dxil/$n.err" 2>&1; then
    c_ok=$((c_ok+1))
  else
    c_bad=$((c_bad+1)); grep -m1 "error" "$OUT/dxil/$n.err" | sed "s/^/$n: /" >> "$OUT/errors.txt"
  fi
done
rmdir "$OUT/tmp" 2>/dev/null

# THE SET MUST MATCH THE LEDGER. A `rm -rf` of this directory silently
# half-failed once, because the previous run's children still held files, and
# the output then held artefacts from TWO runs at once - 221 .dxil against 144
# stable entries. The per-shader removal above means a COMPLETED run always ends
# consistent, so a mismatch here means the run did not complete or something
# else wrote into this directory. Either way the set is not what the ledger
# says, and saying so is cheaper than trusting a count taken mid-run.
have=$(ls "$OUT/dxil"/*.dxil 2>/dev/null | wc -l)
want=$(wc -l < "$OUT/stable.txt" 2>/dev/null)
if [ "$have" -ne "$c_ok" ] || [ "$want" -ne "$tr_ok" ]; then
  echo "INCONSISTENT: $have .dxil on disk, $c_ok compiled this run;" \
       "$want stable entries, $tr_ok reproducible this run - DO NOT build a manifest from this"
fi
if [ "$deferred" -gt 0 ]; then
  echo "DEFERRED $deferred container(s): free memory was below ${MEM_FLOOR_MB}MB, so they were NOT"
  echo "  measured rather than measured badly. Re-run with RESUME=1 on a quiet machine."
fi
echo "reproducible $tr_ok, UNSTABLE $unstable, recompiler failed $tr_bad; dxc compiled $c_ok, failed $c_bad"
[ -s "$OUT/errors.txt" ] && sed 's/^[^:]*: //' "$OUT/errors.txt" | sed 's/.*error: //' | cut -c1-90 | sort | uniq -c | sort -rn | head -8
exit 0
