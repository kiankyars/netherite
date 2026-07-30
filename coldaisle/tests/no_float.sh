#!/usr/bin/env bash
# The determinism contract: core/ is integer-only, so CPU and CUDA agree
# exactly rather than within a tolerance. Grep the real translation unit, not
# the source, so a float arriving through a macro or an include is still caught.
set -uo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf '#include "core/dc_tick.h"\n' > "$tmp/probe.c"
${CC:-gcc} -std=c99 -I. -E "$tmp/probe.c" -o "$tmp/probe.i" || exit 1
hits=$(grep -v '^#' "$tmp/probe.i" | grep -nE '\b(float|double)\b|[0-9]\.[0-9]|[0-9]+[eE][-+][0-9]' || true)
if [ -n "$hits" ]; then
    echo "FAIL: floating point reached core/ (see SPEC.md determinism contract)"
    echo "$hits" | head -20
    exit 1
fi
echo "ok: core/ is integer-only"
