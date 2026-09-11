#!/usr/bin/env bash
# Encode the three intro panels at a given geometry, install them, capture the
# intro and score it - one full experiment, no human in the loop.
#   tools/try_variant.sh <width> <height> <tag> [fps]
set -u
W="$1"; H="$2"; TAG="$3"; FPS="${4:-60}"
ROOT="/c/Users/renoi/ClaudeCode/Ninja Gaiden 2 Xbox360/ng2recomp"
R='C:/Users/renoi/ClaudeCode/Ninja Gaiden 2 Xbox360/ng2recomp'
PS=/c/Windows/SysWOW64/WindowsPowerShell/v1.0/powershell.exe
cd "$ROOT" || exit 1

# A game instance left running from a previous experiment holds files and
# starves the 32-bit encoder. Always start from a clean slate - by process
# name of OUR OWN executable, never by window title.
powershell -NoProfile -Command "Get-Process ng2 -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1
sleep 1

for spec in "NinjaVI:6" "NinjaVI_Left:2" "NinjaVI_Right:2"; do
  f="${spec%%:*}"; ch="${spec##*:}"
  out="out/var_${TAG}/${f}"
  mkdir -p "$out"
  # EE4 intermittently fails if the output dir was just recreated; retry once.
  # EE4 opens its source through DirectShow; back-to-back invocations can hit
  # a graph the previous run has not finished releasing and throw
  # "File not found" on a file that plainly exists. Retry with backoff.
  for attempt in 1 2 3 4; do
    "$PS" -NoProfile -ExecutionPolicy Bypass -File "$R/tools/encode_vc1.ps1" \
      -Source "$R/game_video_backup/${f}.wmv" -OutDir "$R/out/var_${TAG}/${f}" \
      -Width "$W" -Height "$H" -Fps "$FPS" -Kbps 4000 -Channels "$ch" -AllIntra \
      >"out/var_${TAG}/${f}.enc.log" 2>&1
    [ -f "$out/$f.wmv" ] && break
    sleep $(( attempt * 4 ))
  done
  if [ ! -f "$out/$f.wmv" ]; then
    echo "  ENCODE FAILED: $f"; tail -6 "out/var_${TAG}/${f}.enc.log" | sed "s/^/      /"; exit 1
  fi
  cp "$out/$f.wmv" "game/$f.wmv"
done

rm -rf "out/shots_${TAG}"
python tools/capture_intro.py --seconds 34 --interval 1.5 \
  --outdir "out/shots_${TAG}" --tag "$TAG" >/dev/null 2>&1
printf "  %-14s %sx%s @%s  -> " "$TAG" "$W" "$H" "$FPS"
python tools/score_shots.py "out/shots_${TAG}/${TAG}_*.png"
