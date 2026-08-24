#!/usr/bin/env bash
#
# ztext -- every number the README claims, recomputed.
#
# A README full of measured numbers goes stale silently: nothing fails when a
# test is added or a symbol is exported. This prints the current value of each
# one, so reconciling the documentation is reading one output rather than
# auditing prose.
#
# It measures rather than asserts, deliberately. The numbers move for good
# reasons and a threshold here would be either useless or a nuisance; what is
# wanted is to SEE them next to what the README says.
#
# Usage: ci/measurements.sh

set -uo pipefail
cd "$(dirname "$0")/.."

if [ -t 1 ]; then
  BOLD=$'\033[1m'; DIM=$'\033[2m'; OFF=$'\033[0m'
else
  BOLD=; DIM=; OFF=
fi

row() { printf '  %-42s %s\n' "$1" "$2"; }

printf '%sSuite%s\n' "$BOLD" "$OFF"
# --summary all so the count is reported at all; the grep is deliberately for
# the count rather than for success, so a red suite prints what it managed.
tests=$(zig build test --summary all 2>&1 |
  grep -oE '[0-9]+/[0-9]+ tests passed' | head -1)
row 'zig build test' "${tests:-FAILED}"
smoke=$(zig build test-c 2>&1 | grep -E 'injection:' | sed 's/^ *//')
row 'C boundary' "${smoke:-FAILED}"

printf '\n%sShared build%s %s(-fvisibility=hidden)%s\n' "$BOLD" "$OFF" "$DIM" "$OFF"
if zig build -Dshared=true > /dev/null 2>&1; then
  lib=$(ls zig-out/lib/libztext.dylib zig-out/lib/libztext.so 2>/dev/null | head -1)
  if [ -n "$lib" ] && command -v nm > /dev/null 2>&1; then
    total=$(nm -gU "$lib" 2>/dev/null | wc -l | tr -d ' ')
    ours=$(nm -gU "$lib" 2>/dev/null | grep -c ' _\?ztext' || true)
    row 'exported symbols' "$total"
    row "  of which ztext's own" "$ours"
    row '  of which upstream (FreeType)' "$((total - ours))"
  else
    row 'exported symbols' 'no shared library or no nm on this host'
  fi
else
  row 'shared build' 'FAILED'
fi

printf '\n%sBench%s %s(ReleaseFast; timings vary 30-40%% on a loaded machine)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"
zig build bench -Doptimize=ReleaseFast 2>&1 | tail -n +3 | sed 's/^/  /'

printf '\n%sSources%s\n' "$BOLD" "$OFF"
row 'C entry points in ffi/ztext.h' \
  "$(grep -c '^ZTEXT_API' ffi/ztext.h)"
row 'externs in src/c.zig' \
  "$(grep -c '^pub extern fn' src/c.zig)"
row 'mutation cases in ci/check-guards.sh' \
  "$(grep -c '^case_ ' ci/check-guards.sh)"
