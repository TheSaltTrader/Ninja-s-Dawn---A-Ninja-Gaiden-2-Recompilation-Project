#!/usr/bin/env bash
# Run every ReXGlue codegen validator pass against a generated tree.
# Usage: ./run_all.sh [generated/default]   (default: generated/default)
# Exit non-zero if any pass reports a mismatch.
set -u
GEN="${1:-generated/default}"
HERE="$(cd "$(dirname "$0")" && pwd)"
fail=0
for p in codegen_validate rlwinm_validate validate2 validate3 validate4; do
  echo "================ $p ================"
  out=$(python "$HERE/$p.py" "$GEN")
  echo "$out"
  if echo "$out" | grep -qE 'mismatches=[1-9]'; then fail=1; fi
done
echo "================ coverage ================"
python "$HERE/coverage.py" "$GEN"
echo
[ $fail -eq 0 ] && echo "RESULT: ALL PASSES CLEAN" || echo "RESULT: MISMATCHES FOUND (investigate; may be a validator-model gap, verify against ISA before treating as codegen bug)"
exit $fail
