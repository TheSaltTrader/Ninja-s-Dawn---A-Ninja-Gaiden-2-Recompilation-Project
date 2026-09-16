#!/bin/sh
# Translate every dumped Fable II shader container with XenosRecomp (the local
# `fable2` branch build) and compile the HLSL with dxc; print the counts and
# the most common compile errors. Crashing/hanging containers are skipped
# (15 s timeout each).
#   translate_all.sh <ngpu_shaders dir> <out dir> [glob]   (glob default: *.xvu)
IN="$1"; OUT="$2"; GLOB="${3:-*.xvu}"
X=/c/Users/renoi/ClaudeCode/NativeGPU/build/xenosrecomp/XenosRecomp/XenosRecomp.exe
H=/c/Users/renoi/ClaudeCode/NativeGPU/reference/XenosRecomp/XenosRecomp/shader_common.h
D=/c/Users/renoi/ClaudeCode/NativeGPU/reference/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe
mkdir -p "$OUT/hlsl" "$OUT/dxil"
: > "$OUT/errors.txt"; : > "$OUT/failed.txt"
tr_ok=0; tr_bad=0; c_ok=0; c_bad=0
cd "$IN" || exit 1
for f in $GLOB; do
  [ -f "$f" ] || continue
  n=${f%.xvu}; n=${n%.var}
  case "$f" in *_p.xvu|*_p.var.xvu) t=ps_6_0;; *) t=vs_6_0;; esac
  if timeout 15 "$X" "$f" "$OUT/hlsl/$n.hlsl" "$H" >/dev/null 2>&1 && [ -s "$OUT/hlsl/$n.hlsl" ]; then
    tr_ok=$((tr_ok+1))
    if "$D" -T $t -E main -HV 2021 -all-resources-bound -Wno-ignored-attributes -Fo "$OUT/dxil/$n.dxil" "$OUT/hlsl/$n.hlsl" >"$OUT/dxil/$n.err" 2>&1; then
      c_ok=$((c_ok+1))
    else
      c_bad=$((c_bad+1)); grep -m1 "error" "$OUT/dxil/$n.err" | sed "s/^/$n: /" >> "$OUT/errors.txt"
    fi
  else
    tr_bad=$((tr_bad+1)); echo "$f" >> "$OUT/failed.txt"
  fi
done
echo "translated $tr_ok, recompiler failed $tr_bad; dxc compiled $c_ok, failed $c_bad"
sed 's/^[^:]*: //' "$OUT/errors.txt" | sed 's/.*error: //' | cut -c1-90 | sort | uniq -c | sort -rn | head -8
