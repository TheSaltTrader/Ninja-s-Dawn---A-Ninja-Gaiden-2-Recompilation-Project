#!/bin/sh
# Translate NG2 shader containers with XenosRecomp and compile with dxc.
#
# Separate from translate_all.sh, which is the Fable II side's and runs only
# fix_hlsl.py. NG2 needs a SECOND pass: two thirds of its draws are auto-index,
# so most of its vertex shaders fetch their own attributes and the recompiler
# emits a main() that never declares the inputs its body reads. That pass is
# fix_hlsl_ng2.py and it is the difference between compiling and not for the
# majority population here.
#
#   translate_ng2.sh <container dir> <out dir> [glob]     (glob default: *.xvu)
#
# Naming decides the target profile, as upstream: *_p.xvu is a pixel shader.
IN="$1"; OUT="$2"; GLOB="${3:-*.xvu}"
X=/c/Users/renoi/ClaudeCode/NativeGPU/build/xenosrecomp/XenosRecomp/XenosRecomp.exe
# THE HEADER IS AN INPUT UNDER TEST, AND THE TWO ON DISK ARE NOT INTERCHANGEABLE.
#
#   fable2_shader_common.h ....... g_HalfPixelOffset : packoffset(c32.z)
#   reference/.../shader_common.h  g_HalfPixelOffset : packoffset(c16.z)
#
# DEFINE_SHARED_CONSTANTS is a macro in the header, so that offset is fixed
# text, nothing to do with the container. The stock upstream one puts the shared
# block at c16 - inside the sampler descriptor arrays XenosRecomp emits - and
# EVERY shader then fails with "packoffset overlap between 'g_HalfPixelOffset',
# 'g_Sampler3_TextureCubeDescriptorIndex'". All 607 of NG2's real translations
# were made with the fable2_ one, so that is the working header despite its name.
#
# translate_all.sh defaults to the stock header, and inheriting that default sent
# a whole batch through the wrong one - the same substitution that has already
# cost this project an afternoon via the plugin and via an env var. Whatever runs
# the tool names the input and PRINTS it.
H="${XENOS_COMMON:-/c/Users/renoi/ClaudeCode/NativeGPU/fable2_shader_common.h}"
D=/c/Users/renoi/ClaudeCode/NativeGPU/reference/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe
TOOLS="$(cd "$(dirname "$0")" && pwd)"

[ -x "$X" ] || { echo "no XenosRecomp at $X"; exit 1; }
[ -f "$H" ] || { echo "no shader_common header at $H"; exit 1; }
[ -x "$D" ] || { echo "no dxc at $D"; exit 1; }

echo "HEADER UNDER TEST: $H"
echo "  shared-constant base: $(sed -n 's/.*g_HalfPixelOffset : packoffset(\(c[0-9]*\).*/\1/p' "$H" | head -1)"
echo "RECOMPILER       : $X"

mkdir -p "$OUT/hlsl" "$OUT/dxil"
: > "$OUT/errors.txt"; : > "$OUT/failed.txt"
tr_ok=0; tr_bad=0; c_ok=0; c_bad=0
cd "$IN" || exit 1
for f in $GLOB; do
  [ -f "$f" ] || continue
  n=${f%.xvu}; n=${n%.var}
  case "$f" in *_p.xvu|*_p.var.xvu) t=ps_6_0;; *) t=vs_6_0;; esac
  # A STALE ARTEFACT IS WORSE THAN A MISSING ONE: the manifest builder keys on
  # "a .dxil with this stem exists", so a leftover from a previous run makes it
  # claim an artefact for a shader that has since failed to translate. Remove
  # this shader's outputs before trying, so absence means failure.
  rm -f "$OUT/dxil/$n.dxil" "$OUT/hlsl/$n.hlsl"
  if timeout 15 "$X" "$f" "$OUT/hlsl/$n.hlsl" "$H" >/dev/null 2>&1 && [ -s "$OUT/hlsl/$n.hlsl" ]; then
    tr_ok=$((tr_ok+1))
    python "$TOOLS/fix_hlsl.py" "$OUT/hlsl/$n.hlsl" "$OUT/hlsl/$n.hlsl.layout" >/dev/null 2>&1
    python "$TOOLS/fix_hlsl_ng2.py" "$OUT/hlsl/$n.hlsl" >/dev/null 2>&1
    if "$D" -T $t -E main -HV 2021 -all-resources-bound -Wno-ignored-attributes \
            -Fo "$OUT/dxil/$n.dxil" "$OUT/hlsl/$n.hlsl" >"$OUT/dxil/$n.err" 2>&1; then
      c_ok=$((c_ok+1))
    else
      c_bad=$((c_bad+1)); grep -m1 "error" "$OUT/dxil/$n.err" | sed "s/^/$n: /" >> "$OUT/errors.txt"
    fi
  else
    tr_bad=$((tr_bad+1)); echo "$f" >> "$OUT/failed.txt"
  fi
done
echo "translated $tr_ok, recompiler failed $tr_bad; dxc compiled $c_ok, failed $c_bad"
[ -s "$OUT/errors.txt" ] && sed 's/^[^:]*: //' "$OUT/errors.txt" | sed 's/.*error: //' | cut -c1-90 | sort | uniq -c | sort -rn | head -8
exit 0
