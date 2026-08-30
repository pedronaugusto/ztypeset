#!/usr/bin/env bash
#
# ztext -- the same C smoke test, two hundred times.
#
# WHY THIS IS A SEPARATE GATE. `zig build test-c` runs the C boundary once, and
# one run of a fault that happens one run in fifty is indistinguishable from no
# fault at all. That is not a hypothetical: this repository shipped a segfault
# for months that `zig build test` was green on every time, because the failure
# rate was 1.8% and nobody ran it twice. A gate that can only observe one
# sample cannot hold a claim about an intermittent fault, and "it passed" is
# then a statement about luck.
#
# So the loop IS the harness, and 200/200 is its exit condition. Anything less
# is a failure, including 199.
#
# It also captures WHERE. tests/c_smoke.c writes a phase marker to stderr
# before each stage, runs stdout unbuffered, and reports a fault from a signal
# handler naming the phase -- so a run that dies says where instead of exiting
# 139 (or, from the Zig build runner on Windows, 5) with an empty transcript.
#
# Usage:
#   ci/crash-loop.sh                 # 200 runs of the default (gnu) build
#   ci/crash-loop.sh 500             # a different count
#   ci/crash-loop.sh 200 -Dtarget=native-native-msvc
#                                    # any extra arguments go to `zig build`
#
# Exit: 0 only if every run exited 0.

set -uo pipefail
cd "$(dirname "$0")/.."

RUNS="${1:-200}"
shift || true

if ! [ "$RUNS" -gt 0 ] 2>/dev/null; then
  printf 'crash-loop: run count must be a positive integer, got "%s"\n' "$RUNS" >&2
  exit 2
fi

if [ -t 1 ]; then
  RED=$'\033[31m'; GREEN=$'\033[32m'; DIM=$'\033[2m'; OFF=$'\033[0m'
else
  RED=; GREEN=; DIM=; OFF=
fi

# Built and installed rather than located in the cache: `zig build test-c`
# leaves the executable somewhere content-addressed, and picking it out by
# modification time is how a stale binary gets measured and believed. The
# install-c-tests step exists for exactly this.
printf 'crash-loop: building%s\n' "${*:+ ($*)}"
if ! zig build install-c-tests "$@"; then
  printf 'crash-loop: the build failed; nothing was measured\n' >&2
  exit 1
fi

EXE=zig-out/bin/ztext-c-smoke
[ -x "$EXE" ] || EXE="$EXE.exe"
if [ ! -x "$EXE" ]; then
  printf 'crash-loop: no ztext-c-smoke in zig-out/bin after install-c-tests\n' >&2
  exit 1
fi

FONT=tests/fonts/NotoSansHebrew-Regular.ttf
if [ ! -f "$FONT" ]; then
  printf 'crash-loop: missing %s\n' "$FONT" >&2
  exit 1
fi

STDERR=$(mktemp)
# shellcheck disable=SC2064
trap "rm -f '$STDERR'" EXIT

printf 'crash-loop: %s x %s\n' "$EXE" "$RUNS"

ok=0
bad=0
first_failure=

i=0
while [ "$i" -lt "$RUNS" ]; do
  i=$((i + 1))
  if "$EXE" "$FONT" >/dev/null 2>"$STDERR"; then
    ok=$((ok + 1))
  else
    status=$?
    bad=$((bad + 1))
    # The smoke test's own fault report if it got one out, and otherwise the
    # last phase marker. Without either, the only evidence a crash leaves is
    # its exit status.
    where=$(grep 'c smoke: FAULT' "$STDERR" | tail -1)
    if [ -z "$where" ]; then
      where=$(grep '^phase: ' "$STDERR" | tail -1)
    fi
    printf '%s  run %d/%d exited %d  after %s%s\n' \
      "$RED" "$i" "$RUNS" "$status" "${where:-no phase marker}" "$OFF"
    if [ -z "$first_failure" ]; then
      first_failure="run $i exited $status after ${where:-no phase marker}"
    fi
  fi
done

printf '\n'
if [ "$bad" -eq 0 ]; then
  printf '%scrash-loop: %d/%d clean%s\n' "$GREEN" "$ok" "$RUNS" "$OFF"
  printf '%s(this gate can only see faults the smoke test reaches; the Zig\n' "$DIM"
  printf ' suite and ci/check-guards.sh cover what it does not)%s\n' "$OFF"
  exit 0
fi

printf '%scrash-loop: %d/%d clean, %d FAILED%s\n' \
  "$RED" "$ok" "$RUNS" "$bad" "$OFF"
printf '  first: %s\n' "$first_failure"
exit 1
