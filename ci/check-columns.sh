#!/usr/bin/env bash
#
# ztypeset -- the C sources stay ASCII, and within eighty columns.
#
# The C has no formatter, so this is the one layout rule that is enforced
# rather than hoped for. Eighty columns is not taste here: these files are read
# in side-by-side diffs against the upstream headers they mirror, and a wrapped
# line in that view costs more than the characters saved.
#
# It lives in its own file because ci/run.sh and the hosted workflow both need
# it, and a rule with two implementations can disagree with itself.
#
# Two directories, not one. ffi/ is the library; tests/ is the rest of ztypeset's
# own C, written to the same standard and built with the same warnings, and it
# had been outside this check for no stated reason -- which showed: twelve
# lines had drifted past the limit there while ffi/ could not hold one.
# libs/ is never touched, here or anywhere: it is upstream, unmodified.
#
# The ASCII check is not decoration. Counting BYTES per line is only the same
# as counting columns while every byte is one column, and the comment this
# file used to carry claimed the width check "is what keeps ffi/ ASCII" --
# which it never did. A short line of UTF-8 passes a byte count. So the
# property the width check depends on is now checked on its own, which is what
# makes the width number mean what it says.
#
# Usage: ci/check-columns.sh
# Exit:  0 if every line of ffi/ and tests/ C is ASCII and 80 columns or fewer.

set -uo pipefail
cd "$(dirname "$0")/.."

sources=(ffi/*.h ffi/*.c tests/*.h tests/*.c)

over=$(awk 'length($0) > 80 { printf "%s:%d: %d columns\n", FILENAME, FNR, length($0) }' \
  "${sources[@]}" 2>/dev/null)

# LC_ALL=C so the bracket expression is a BYTE range: tab, plus space through
# tilde. Anything else -- a stray UTF-8 sequence, a carriage return, a control
# character -- is named with its line.
non_ascii=$(LC_ALL=C grep -nH '[^ -~\t]' "${sources[@]}" 2>/dev/null || true)

if [ -n "$over" ] || [ -n "$non_ascii" ]; then
  [ -n "$over" ] && printf '%s\n' "$over" >&2
  [ -n "$non_ascii" ] && printf '%s\n' "$non_ascii" >&2
  exit 1
fi

printf 'every line of ffi/ and tests/ C is ASCII and within 80 columns\n'
