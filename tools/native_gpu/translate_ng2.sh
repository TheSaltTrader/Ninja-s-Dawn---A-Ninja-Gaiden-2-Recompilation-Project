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
IN="$1"; OUT="$2"; GLOB="${3:-*.xvu}"
# FIVE, NOT THREE, AND "STABLE IN N RUNS" IS NOT "DETERMINISTIC". Two censuses
# over the same 80 containers named DIFFERENT containers as unstable, so N runs
# agreeing is evidence rather than proof. The first column of any report built
# on this must never be read as a coverage number.
STABLE_RUNS="${STABLE_RUNS:-5}"
TIME_CAP_S="${TIME_CAP_S:-120}"
MEM_CAP_MB="${MEM_CAP_MB:-4096}"

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
tr_ok=0; tr_bad=0; unstable=0; c_ok=0; c_bad=0
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
  : > "$OUT/errors.txt"; : > "$OUT/failed.txt"; : > "$OUT/unstable.txt"; : > "$OUT/stable.txt"
fi
cd "$IN" || exit 1
for f in $GLOB; do
  [ -f "$f" ] || continue
  n=${f%.xvu}; n=${n%.var}
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

  first=""; agreed=1; reason=""
  i=1
  while [ "$i" -le "$STABLE_RUNS" ]; do
    out="$OUT/tmp/$n.$i.hlsl"
    rm -f "$out"
    $CAP "$X" "$f" "$out" "$H" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -ne 0 ] || [ ! -s "$out" ]; then
      agreed=0
      case "$rc" in
        124) reason="time cap";;
        125) reason="MEMORY CAP";;
        126) reason="crashed";;
        *)   reason="no output (rc $rc)";;
      esac
      break
    fi
    h=$(md5sum "$out" | cut -d' ' -f1)
    if [ -z "$first" ]; then first="$h"
    elif [ "$h" != "$first" ]; then agreed=0; reason="UNSTABLE output"; break; fi
    i=$((i+1))
  done

  if [ "$agreed" -eq 0 ]; then
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
  echo "$f  agreed $STABLE_RUNS/$STABLE_RUNS" >> "$OUT/stable.txt"
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
echo "reproducible $tr_ok, UNSTABLE $unstable, recompiler failed $tr_bad; dxc compiled $c_ok, failed $c_bad"
[ -s "$OUT/errors.txt" ] && sed 's/^[^:]*: //' "$OUT/errors.txt" | sed 's/.*error: //' | cut -c1-90 | sort | uniq -c | sort -rn | head -8
exit 0
