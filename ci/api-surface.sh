#!/usr/bin/env bash
#
# ztext -- every entry point, and everywhere it lives.
#
# A C entry point exists in up to six places: declared in ffi/ztext.h, defined
# in ffi/*.c, declared again as a Zig extern in src/c.zig, wrapped in src/*.zig,
# called from the tests, and shown in README.md. Changing a signature means
# changing all of them, and the cost of doing that from memory is a missed site
# found later by a compiler -- or not at all, in the documentation.
#
# This prints the surface with a column per home, so a change has a generated
# checklist rather than a remembered one. `--gaps` shows only the rows with a
# hole in them, and FAILS on any hole that has not been declared below -- a
# report nobody has to act on is a report, not a gate.
#
# The ABI cross-check already proves the header and the externs agree; what it
# cannot see is a wrapper that was never written or a README that still shows
# the old call.
#
# Usage:
#   ci/api-surface.sh            # the whole surface
#   ci/api-surface.sh --gaps     # only undeclared holes; exit 1 if there are any
#   ci/api-surface.sh --sweep    # fail if the null sweep has fallen behind
#   ci/api-surface.sh <name>     # every line mentioning one entry point
#
# Exit: 0 if every hole in the table is a declared one. Both --gaps and the
# full listing use the same rule, so CI can run either.

set -uo pipefail
cd "$(dirname "$0")/.."

if [ -t 1 ]; then
  BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; OFF=$'\033[0m'
else
  BOLD=; DIM=; RED=; OFF=
fi

# One entry point, every line that mentions it anywhere that matters.
if [ $# -gt 0 ] && [ "$1" != "--gaps" ] && [ "$1" != "--sweep" ]; then
  printf '%s%s%s\n' "$BOLD" "$1" "$OFF"
  grep -rn --include='*.h' --include='*.c' --include='*.zig' --include='*.md' \
    -- "$1" ffi src tests README.md 2>/dev/null |
    grep -v '/\.zig-cache/' | sed 's/^/  /'
  exit 0
fi

# --sweep: does tests/null_sweep.c still name every entry point? This is what
# keeps a sweep from silently covering less as the surface grows.
if [ "${1:-}" = "--sweep" ]; then
  missing=0
  for name in $(grep -oE '\bztext[A-Za-z0-9_]+\(' ffi/ztext.h |
                grep -oE '^ztext[A-Za-z0-9_]+' | sort -u); do
    grep -qE "ZTEXT_API[^;]*\b$name\(" ffi/ztext.h || continue
    if ! grep -qF "$name" tests/null_sweep.c; then
      printf '%s%-34s%s never called by tests/null_sweep.c\n' "$RED" "$name" "$OFF"
      missing=$((missing + 1))
    fi
  done
  if [ $missing -eq 0 ]; then
    printf '%severy entry point is exercised by the null sweep%s\n' "$BOLD" "$OFF"
    exit 0
  fi
  printf '\n%d entry point(s) added to the header but not to the sweep.\n' "$missing" >&2
  exit 1
fi

# Holes that are deliberate, each with the reason it is deliberate. A "--"
# genuinely is not always wrong -- an abi or debug entry point has no Zig
# wrapper by design -- but until this list existed the difference between "no
# wrapper because none is wanted" and "no wrapper because nobody wrote one" was
# a judgement made afresh by whoever read the output, which is to say never.
#
# Anything NOT named here is a failure. Adding a line is cheap and is a
# decision; leaving one out is caught.
declared_gap() {
  case "$1:$2" in
    # The Zig Face carries its Font as a field (src/face.zig, Face.font),
    # because a Zig handle can own the reference the C one has to hand back.
    # Wrapping the accessor as well would be a second way to ask the same
    # question, and the two could disagree after a change to either.
    ztextFaceFont:wrap) return 0 ;;
  esac
  return 1
}

GAPS_ONLY=0
[ "${1:-}" = "--gaps" ] && GAPS_ONLY=1

names=$(grep -oE '\bztext[A-Za-z0-9_]+' ffi/ztext.h | sort -u)

printf '%s%-34s %-4s %-4s %-4s %-4s %-4s%s\n' \
  "$BOLD" "entry point" "hdr" "impl" "c.zig" "wrap" "test" "$OFF"

gaps=0
declared_gaps=0
undeclared_names=
total=0
for name in $names; do
  hdr=$(grep -c "ZTEXT_API[^;]*\b$name\b" ffi/ztext.h)
  [ "$hdr" -eq 0 ] && continue          # a macro or a type, not an entry point
  total=$((total + 1))

  impl=$(grep -rl "^[A-Za-z].*\b$name\b(" ffi/*.c 2>/dev/null | wc -l | tr -d ' ')
  ext=$(grep -c "^pub extern fn $name\b" src/c.zig)
  wrap=$(grep -rl "c\.$name\b" src/*.zig 2>/dev/null |
         grep -v 'src/c\.zig' | wc -l | tr -d ' ')
  tst=$(grep -rl "\b$name\b" tests/*.c src/integration_test.zig 2>/dev/null |
        wc -l | tr -d ' ')

  mark() { [ "$1" -gt 0 ] && printf '%-4s' 'yes' || printf '%s%-4s%s' "$RED" '--' "$OFF"; }

  undeclared=0
  declared=0
  for column in impl ext wrap tst; do
    value=${!column}
    [ "$value" -gt 0 ] && continue
    if declared_gap "$name" "$column"; then
      declared=1
    else
      undeclared=1
    fi
  done
  [ $declared -eq 1 ] && declared_gaps=$((declared_gaps + 1))
  if [ $undeclared -eq 1 ]; then
    gaps=$((gaps + 1))
    undeclared_names="$undeclared_names $name"
  fi
  [ $GAPS_ONLY -eq 1 ] && [ $undeclared -eq 0 ] && continue

  printf '%-34s %-4s ' "$name" 'yes'
  mark "$impl"; printf ' '; mark "$ext"; printf ' '
  mark "$wrap"; printf ' '; mark "$tst"; printf '\n'
done

printf '\n%s%d entry points, %d declared empty columns, %d undeclared%s\n' \
  "$DIM" "$total" "$declared_gaps" "$gaps" "$OFF"

if [ $gaps -ne 0 ]; then
  printf '%san entry point has a home nobody filled and nobody decided to leave%s\n' \
    "$RED" "$OFF" >&2
  printf '  %s\n' $undeclared_names >&2
  printf 'Either write the missing home, or add the pair to declared_gap() in\n' >&2
  printf 'this script with the reason it is deliberate.\n' >&2
  exit 1
fi
exit 0
