#!/bin/sh
# Count how many containers the PATCHED XenosRecomp refuses.
#
# A refusal means `vertexElements.find(address)` missed - which in the UNPATCHED
# binary dereferences end() and emits a shader built from whatever byte sat
# there. So for the original corpus:
#
#     A REFUSAL IS A SHADER MY MANIFEST CURRENTLY HOLDS A WRONG TRANSLATION FOR.
#
# That is the number, and it cannot be obtained any other way: the unpatched
# binary produces a plausible shader either way and nothing downstream can tell
# the two apart.
#
# Run over BOTH corpora, which is the pairing that makes each a check on the
# other:
#   originals        -> the size of the corruption in the current manifest
#   synth_fetchless  -> containers my rebuild FAILED to give a usable table;
#                       these should be zero if the workaround is complete
#
# One run per container is enough. A refusal is a deterministic lookup miss, not
# the non-deterministic crash - so unlike everything else measured today this
# does not need a stability gate.
FIX="C:/Users/renoi/AppData/Local/Temp/claude/C--users-renoi-claudecode/7d093996-252d-49c9-a593-f1ba6e716609/scratchpad/XenosRecomp.fetchfix.exe"
H=/c/Users/renoi/ClaudeCode/NativeGPU/fable2_shader_common.h
CAP="python D:/ng2_frameinterp/ng2recomp-worktree/tools/native_gpu/run_capped.py 120 4096 --"
IN="$1"; LABEL="$2"
TMP="D:/ng2_frameinterp/work/refusal_tmp"
mkdir -p "$TMP"; rm -f "$TMP"/*.hlsl "$TMP"/*.layout

[ -x "$FIX" ] || { echo "patched binary missing: $FIX"; exit 1; }

# THE PATCHED BINARY NEEDS ITS DLLs. It lives in a scratchpad; dxcompiler.dll,
# dxil.dll and fmt.dll sit beside the SHARED binary. Run from anywhere else it
# exits 0 and writes nothing - and the first version of this audit counted that
# as 625 crashes and reported "REFUSED 0", which reads as "the manifest is
# clean". Same shape as every other instrument failure today: the broken state
# and the nothing-to-report state printed the same number.
DLLDIR=/c/Users/renoi/ClaudeCode/NativeGPU/build/xenosrecomp/XenosRecomp
cd "$DLLDIR" || { echo "cannot reach the dll directory"; exit 1; }

# A CANARY BEFORE THE RUN: one container that must translate. If it does not,
# the environment is wrong and no count from this pass means anything.
$CAP "$FIX" "D:/ng2_frameinterp/shaders/xvu/ng2_8200AC88.xvu" "$TMP/canary.hlsl" "$H" >/dev/null 2>&1
if [ ! -s "$TMP/canary.hlsl" ]; then
  echo "REFUSING: canary container produced no output - the patched binary cannot run here"
  exit 1
fi
rm -f "$TMP/canary.hlsl" "$TMP/canary.hlsl.layout"

total=0; refused=0; ok=0; crashed=0
for f in "$IN"/*.xvu; do
  [ -f "$f" ] || continue
  total=$((total + 1))
  n=$(basename "$f" .xvu)
  out="$TMP/$n.hlsl"
  rm -f "$out"
  $CAP "$FIX" "$f" "$out" "$H" >/dev/null 2>&1
  rc=$?
  if [ -s "$out" ] && grep -q "no vertex element for fetch address" "$out" 2>/dev/null; then
    refused=$((refused + 1))
    echo "$n" >> "$TMP/${LABEL}_refused.txt"
  elif [ -s "$out" ]; then
    ok=$((ok + 1))
  else
    # Still the separate, unexplained crash defect - NOT a refusal, and folding
    # the two together would let one hide inside the other.
    crashed=$((crashed + 1))
  fi
  rm -f "$out" "$out.layout"
done
echo "$LABEL: $total containers | translated $ok | REFUSED $refused | crashed $crashed"
