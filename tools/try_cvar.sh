#!/usr/bin/env bash
# Capture and score the intro with extra runtime flags. No re-encoding - the
# installed videos stay put, only the runtime configuration changes.
#   tools/try_cvar.sh <tag> [--flag=value ...]
set -u
TAG="$1"; shift
ROOT="/c/Users/renoi/ClaudeCode/Ninja Gaiden 2 Xbox360/ng2recomp"
cd "$ROOT" || exit 1
powershell -NoProfile -Command "Get-Process ng2 -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1
sleep 1
rm -rf "out/shots_${TAG}"
python tools/capture_intro.py --seconds 30 --interval 1.5 \
  --outdir "out/shots_${TAG}" --tag "$TAG" --game-args "$@" >/dev/null 2>&1
printf "  %-42s -> " "$TAG $*"
python tools/score_shots.py "out/shots_${TAG}/${TAG}_*.png" 2>/dev/null || echo "no frames"
