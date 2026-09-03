#!/usr/bin/env bash
#
# ztypeset — every checked-in script is executable, except the ones that are not
# entry points.
#
# CI runs these by path (`- run: ci/check-columns.sh`), so a script committed
# without the executable bit fails on the runner with "permission denied" and
# passes on the machine that wrote it, where bash was invoked explicitly. The
# bit lives in the index, and on Windows nothing sets it by default:
# `git update-index --chmod=+x <path>` is the fix.

set -euo pipefail
cd "$(dirname "$0")/.."

# Sourced by other scripts (`. ci/pins.sh`), never run. An executable bit here
# would advertise an entry point that does not exist; the reason has to be
# written down, which is what this list is for.
SOURCED='^ci/pins\.sh$'

not_executable=$(git ls-files -s -- '*.sh' |
  awk '$1 != "100755" { print $4 }' |
  grep -Ev "$SOURCED" || true)

# And the reverse, so an exception cannot outlive its reason: a file listed
# above that HAS the bit is either a real entry point now or a stale line.
wrongly_executable=$(git ls-files -s -- '*.sh' |
  awk '$1 == "100755" { print $4 }' |
  grep -E "$SOURCED" || true)

fails=0
if [ -n "$not_executable" ]; then
  printf 'these scripts are committed without the executable bit:\n' >&2
  printf '%s\n' "$not_executable" | sed 's/^/  /' >&2
  printf 'fix with: git update-index --chmod=+x <path>\n' >&2
  fails=1
fi
if [ -n "$wrongly_executable" ]; then
  printf 'these are listed as sourced-only but carry the executable bit:\n' >&2
  printf '%s\n' "$wrongly_executable" | sed 's/^/  /' >&2
  printf 'drop the SOURCED entry in this script, or drop the bit\n' >&2
  fails=1
fi
[ "$fails" -ne 0 ] && exit 1

printf 'OK  every committed script is executable, or listed as sourced-only\n'
