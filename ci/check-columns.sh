#!/usr/bin/env bash
#
# ztext -- the C sources stay within eighty columns.
#
# The C has no formatter, so this is the one layout rule that is enforced
# rather than hoped for. Eighty columns is not taste here: these files are read
# in side-by-side diffs against the upstream headers they mirror, and a wrapped
# line in that view costs more than the characters saved.
#
# It lives in its own file because ci/run.sh and the hosted workflow both need
# it, and a rule with two implementations can disagree with itself.
#
# Usage: ci/check-columns.sh
# Exit:  0 if every line of ffi/*.h and ffi/*.c is 80 columns or fewer.
#
# Blind spot: it counts BYTES per line, not display columns, so a non-ASCII
# character reads as wider than it prints. ffi/ is ASCII by convention and this
# is what keeps it that way.

set -uo pipefail
cd "$(dirname "$0")/.."

over=$(awk 'length($0) > 80 { printf "%s:%d: %d columns\n", FILENAME, FNR, length($0) }' \
  ffi/*.h ffi/*.c 2>/dev/null)

if [ -n "$over" ]; then
  printf '%s\n' "$over" >&2
  exit 1
fi

printf 'every line of ffi/*.h and ffi/*.c is within 80 columns\n'
