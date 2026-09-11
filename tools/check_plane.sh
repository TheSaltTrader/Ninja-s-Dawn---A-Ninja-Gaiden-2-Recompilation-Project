#!/usr/bin/env bash
# Run the game with the given flags, dump the guest Y plane during the intro,
# and report how much of it the decoder actually wrote.
#
# This is a far better signal than looking at the screen: the plane is the
# decoder's output in guest memory, before the GPU plugin touches it. A correct
# plane is ~100% written; the defect shows up as a regular 6-rows-on/2-rows-off
# pattern.
set -u
TAG="$1"; shift
ROOT="/c/Users/renoi/ClaudeCode/Ninja Gaiden 2 Xbox360/ng2recomp"
cd "$ROOT" || exit 1
powershell -NoProfile -Command "Get-Process ng2 -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1
rm -f "out/plane_${TAG}.bin"
ARGS=""
for a in "$@"; do ARGS="$ARGS,'$a'"; done
powershell -NoProfile -Command "
\$root='C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp'
\$env:REX_NG2_PLANE_ADDR='0BD16000'
\$env:REX_NG2_PLANE_BYTES='311296'
\$env:REX_NG2_PLANE_AT_MS='12000'
\$env:REX_NG2_PLANE_OUT='out/plane_${TAG}.bin'
\$p=Start-Process -FilePath (Join-Path \$root 'out\build\win-amd64-Release\ng2.exe') -WorkingDirectory \$root -PassThru -ArgumentList @('--game_data_root','game'${ARGS})
Start-Sleep -Seconds 17; Stop-Process -Id \$p.Id -Force -ErrorAction SilentlyContinue" >/dev/null 2>&1
python tools/plane_stats.py "out/plane_${TAG}.bin" "$TAG $*"
