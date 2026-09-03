#!/usr/bin/env bash
#
# ztypeset -- the CI matrix, run locally.
#
# This mirrors .github/workflows/ci.yml so a failure can be reproduced and
# fixed on your own machine instead of in a pull request. Install it as a
# pre-push hook with ci/install-hooks.sh to catch problems before they are
# pushed at all.
#
# The one difference from the hosted run: CI executes the suite on Linux,
# macOS and Windows, whereas this executes it on whichever host you are on and
# cross-compiles the rest.
#
# Usage:
#   ci/run.sh                 # full matrix
#   ci/run.sh --quick         # native Debug only, for the inner loop
#   ci/run.sh --full          # + ci/check-guards.sh, which breaks each guard
#                             #   on purpose and checks a named test catches it
#
# Exits non-zero if any step fails, after running every step -- a single
# failure should not hide the others.
#
# Note on time: HarfBuzz is a large C++ template-heavy library and dominates a
# cold build. Expect a few minutes for the full matrix from an empty cache and
# well under a minute warm. --quick exists because of that, not despite it.

set -uo pipefail
cd "$(dirname "$0")/.."

QUICK=0
FULL=0
case "${1:-}" in
  --quick) QUICK=1 ;;
  # Adds the mutation harness, which is minutes rather than seconds because
  # every case is a rebuild. Worth it before a release or after touching a
  # guard; not worth it on every push.
  --full)  FULL=1 ;;
esac

if [ -t 1 ]; then
  RED=$'\033[31m'; GREEN=$'\033[32m'; DIM=$'\033[2m'; BOLD=$'\033[1m'; OFF=$'\033[0m'
else
  RED=; GREEN=; DIM=; BOLD=; OFF=
fi

PASSED=0
FAILED=0
FAILED_NAMES=()

# run <name> <command...>
run() {
  local name="$1"; shift
  printf '  %-46s' "$name"
  local start output status
  start=$(date +%s)
  output=$("$@" 2>&1)
  status=$?
  local elapsed=$(( $(date +%s) - start ))

  if [ $status -eq 0 ]; then
    printf '%sok%s %s(%ds)%s\n' "$GREEN" "$OFF" "$DIM" "$elapsed" "$OFF"
    PASSED=$((PASSED + 1))
  else
    printf '%sFAILED%s %s(%ds)%s\n' "$RED" "$OFF" "$DIM" "$elapsed" "$OFF"
    FAILED=$((FAILED + 1))
    FAILED_NAMES+=("$name")
    printf '%s' "$output" | sed 's/^/      | /' | head -40
  fi
}

section() { printf '\n%s%s%s\n' "$BOLD" "$1" "$OFF"; }

printf '%sztypeset local CI%s  %s%s%s\n' "$BOLD" "$OFF" "$DIM" "$(zig version)" "$OFF"

#-----------------------------------------------------------------------------
section 'Hygiene'
#-----------------------------------------------------------------------------

# Only our own Zig sources: libs/ is vendored verbatim and must not be
# reformatted, or the next re-vendor becomes an unreadable diff.
# Every Zig file in the repository, which is not the same as every Zig file
# under src/. tests/consumer/build.zig is a build graph a human edits by hand
# and the only place the dependency-consumer path is written down; it was
# outside the formatting gate, so the one Zig file most likely to be edited by
# someone unfamiliar with the repository was the one file nothing formatted.
run 'zig fmt (every .zig in the repo)' \
  zig fmt --check src tests/fonts.zig build.zig tests/consumer

# The C sources have no formatter, so the one rule that is actually enforced is
# enforced here -- in ci/check-columns.sh, which the hosted workflow runs too,
# because a rule with two implementations can disagree with itself.
run "ztypeset's C is ASCII, within 80 columns" ci/check-columns.sh

# CI runs the scripts in ci/ by path. One committed without its executable bit
# fails there and nowhere else, because every local runner invokes bash first.
run 'every committed script is executable' ci/check-executable.sh

# .gitignore says what does not belong in the history. Being tracked overrides
# every rule in it, so a blanket `git add -A` can put a fetched package or a
# build directory into a public clone forever with nothing to say so.
run 'nothing this repository ignores is tracked' ci/check-ignored.sh

# This file's header says it mirrors .github/workflows/ci.yml. Held rather
# than stated: the two must build the same set of -D option combinations.
run 'ci/run.sh mirrors the workflow' ci/check-mirror.sh

# The null sweep is only worth having if it still covers everything. This
# fails when the header grows an entry point the sweep never learned about.
run 'null sweep covers every entry point' ci/api-surface.sh --sweep

# And every entry point has every home it should, or a written reason for the
# one it does not. Before this had an exit code it printed "1 with an unfilled
# column" and nothing acted on it for as long as that was true.
run 'every entry point has every home' ci/api-surface.sh --gaps

# Every mutation the harness plants still quotes the tree, exactly once. The
# full harness below answers this too, but only after minutes of rebuilds and
# only behind --full, so a refactor that stranded a case would sit unnoticed
# through however many pushes came before someone ran the slow one.
run 'every mutation still applies' ci/check-guards.sh --anchors

# Every number README.md states that a script can recompute, recomputed and
# compared. A count in prose goes stale the moment a test is added, and prose
# has never once been what noticed.
run 'README numbers still hold' ci/measurements.sh --check --with-build

#-----------------------------------------------------------------------------
section 'Tests -- native'
#-----------------------------------------------------------------------------

# The C sanitizer is opt-in -- a library must not force its runtime into a
# consumer's link -- so ztypeset's own Debug run asks for it explicitly. This is
# the run that would catch undefined behaviour in ztypeset's own C.
run 'test Debug (UBSan on)' zig build test -Doptimize=Debug -Dsanitize_c=true

if [ $QUICK -eq 0 ]; then
  for mode in ReleaseSafe ReleaseFast ReleaseSmall; do
    run "test $mode" zig build test -Doptimize="$mode"
  done

  # The C boundary on its own, with no Zig in the picture.
  run 'test-c (C ABI standalone)' zig build test-c

  # And the same boundary two hundred times, because one run cannot see an
  # intermittent fault. This package shipped a 1.8%-per-run segfault that
  # every single-run gate was green on. See ci/crash-loop.sh.
  run 'crash loop (200 x test-c)' ci/crash-loop.sh 200

  # Consuming ztypeset as a dependency is a different code path from building it,
  # and the difference has bitten before -- see tests/consumer/build.zig.
  run 'consumer (module + artifacts)' \
    env -C tests/consumer zig build run

  # What a consumer is HANDED, checked against what was built: every
  # installed header compiles, is reachable from a documented root, and has
  # every entry point it declares defined by a library installed beside it.
  # Four HarfBuzz headers and three FreeType entry points were on the wrong
  # side of that when this was written.
  run 'installed headers compile and link' ci/header-link.sh
fi

#-----------------------------------------------------------------------------
if [ $QUICK -eq 0 ]; then
section 'Cross-compilation'
#-----------------------------------------------------------------------------

# Compile-only. These prove the sources and build graph are portable; the
# tests above are what prove behaviour, on this host. CI executes the suite on
# Linux, macOS and Windows as well.
for target in \
  x86_64-linux-gnu \
  aarch64-linux-gnu \
  x86_64-linux-musl \
  aarch64-linux-musl \
  x86_64-windows-gnu \
  aarch64-windows-gnu \
  x86_64-macos \
  aarch64-macos
do
  run "build $target" zig build -Dtarget="$target"
done

# x86_64-windows-msvc is absent here because it needs the Microsoft standard
# library, which a non-Windows host does not have. CI covers it natively on a
# Windows runner -- and that matters, because Zig defaults the Windows ABI to
# gnu, so every MSVC branch in build.zig is otherwise never built at all.

#-----------------------------------------------------------------------------
section 'Build configurations'
#-----------------------------------------------------------------------------

run 'shared library' zig build -Dshared=true
run 'sanitizer on in ReleaseSafe' \
  zig build -Doptimize=ReleaseSafe -Dsanitize_c=true

# The other Windows ABI, executed rather than only compiled. It sits here
# rather than in the cross-compilation block because these LINK and RUN, which
# needs a host that can execute the result -- the same reason the msvc target
# is absent above.
#
# Zig defaults the Windows ABI to gnu, so without this block every MSVC branch
# in build.zig is unbuilt and every difference between the two ABIs -- struct
# packing, the C runtime, how a DLL declares its exports -- is untested on the
# only machine that could test it. CI's Windows runner does all of this; a
# Windows developer's local run used to do one quarter of it, and the header
# gate was the quarter it did.
case "$(uname -s 2> /dev/null)" in
  MINGW* | MSYS* | CYGWIN* | Windows*)
    run 'test (msvc ABI)' zig build test -Dtarget=native-native-msvc
    run 'crash loop 200 x test-c (msvc ABI)' \
      ci/crash-loop.sh 200 -Dtarget=native-native-msvc
    run 'consumer (msvc ABI)' \
      env -C tests/consumer zig build run -Dtarget=native-native-msvc
    run 'installed headers link (msvc ABI)' \
      ci/header-link.sh --target=native-native-msvc
    ;;
esac
fi

#-----------------------------------------------------------------------------
if [ $FULL -eq 1 ]; then
section 'Guards -- do they actually fail?'
#-----------------------------------------------------------------------------

# A passing test says nothing about whether it CAN fail. This applies one
# deliberate bug at a time and asserts a NAMED test catches each. Minutes, not
# seconds, which is why it is behind --full.
run 'mutation harness (ci/check-guards.sh)' ci/check-guards.sh
fi

#-----------------------------------------------------------------------------
printf '\n'
if [ $FAILED -eq 0 ]; then
  printf '%s%d passed, 0 failed%s\n' "$GREEN" "$PASSED" "$OFF"
  printf '%s(ci/verify-vendor.sh is separate -- it needs network)%s\n' "$DIM" "$OFF"
  [ $FULL -eq 1 ] ||
    printf '%s(ci/run.sh --full adds the mutation harness)%s\n' "$DIM" "$OFF"
  exit 0
fi

printf '%s%d passed, %d FAILED%s\n' "$RED" "$PASSED" "$FAILED" "$OFF"
for name in "${FAILED_NAMES[@]}"; do
  printf '  %s- %s%s\n' "$RED" "$name" "$OFF"
done
exit 1
