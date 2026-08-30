#!/usr/bin/env bash
#
# ztext -- every number the README claims, recomputed, and the ones that can
# be checked automatically checked.
#
# A README full of measured numbers goes stale silently: nothing fails when a
# test is added, a symbol is exported or an entry point appears. This prints
# the current value of each one, so reconciling the documentation is reading
# one output rather than auditing prose.
#
# WHAT CHANGED AND WHY. It used to claim "every number the README claims,
# recomputed" while recomputing some of them. The counts it printed had no
# connection to the sentences in README.md -- a reader still had to hold both
# in their head and compare -- and the shared-library section looked only for
# .dylib and .so, so on Windows it silently reported "no shared library" and
# on a static-only host reported nothing at all. A measurement harness that
# quietly measures less than it says is worse than one that measures nothing,
# because its output is believed.
#
# So: `--check` extracts each number from README.md and compares it with the
# recomputed one, and fails naming both. That is the part that runs in CI.
# Everything else is printed for a person to read.
#
# Usage:
#   ci/measurements.sh                 # print everything (builds, runs, benches)
#   ci/measurements.sh --check         # compare README against the sources only
#   ci/measurements.sh --check --with-build
#                                      # ... and against the suite and the C boundary
#   ci/measurements.sh --quick         # print, but skip the bench and the crash loop
#
# Exit: 0 unless --check found a number in README.md that no longer holds.
#
# BLIND SPOTS, stated because a harness that hides them is the thing this file
# exists to prevent:
#   - Numbers it cannot recompute on a host without a symbol tool (nm or
#     llvm-nm) are reported as such rather than skipped.
#   - Timings are CPU time on a machine under unknown load; only the ratios
#     between rows are worth comparing across machines.
#   - Byte counts are what ztext's allocator seam was ASKED for. They exclude
#     the host allocator's own per-block overhead, and they cannot attribute a
#     byte to FreeType rather than to HarfBuzz.
#   - --check reads README.md with regular expressions. It fails loudly when a
#     sentence it knows is gone, but it cannot see a number nobody taught it.

set -uo pipefail
cd "$(dirname "$0")/.."

CHECK=0
WITH_BUILD=0
QUICK=0
for arg in "$@"; do
  case "$arg" in
    --check) CHECK=1 ;;
    --with-build) WITH_BUILD=1 ;;
    --quick) QUICK=1 ;;
    *) printf 'measurements: unknown argument "%s"\n' "$arg" >&2; exit 2 ;;
  esac
done

if [ -t 1 ]; then
  BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GREEN=$'\033[32m'; OFF=$'\033[0m'
else
  BOLD=; DIM=; RED=; GREEN=; OFF=
fi

# shellcheck source=ci/pins.sh
. ci/pins.sh

row() { printf '  %-42s %s\n' "$1" "$2"; }
note() { printf '  %s%s%s\n' "$DIM" "$1" "$OFF"; }

MISMATCHES=0

# claim <label> <extended-regex with exactly one capturing group> <recomputed>
#
# The regex is matched against README.md. A regex that matches nothing is a
# failure in its own right: it means the sentence was reworded and the check
# stopped checking without saying so, which is the silent-staleness this file
# exists to prevent.
claim() {
  local label="$1" regex="$2" actual="$3"
  local stated
  stated=$(grep -oE "$regex" README.md | head -1 | grep -oE '[0-9][0-9 ]*[0-9]|[0-9]' | head -1)
  stated=${stated// /}
  if [ -z "$stated" ]; then
    printf '  %-42s %sno sentence in README.md matches%s\n' "$label" "$RED" "$OFF"
    note "  regex: $regex"
    MISMATCHES=$((MISMATCHES + 1))
    return
  fi
  if [ "$stated" = "$actual" ]; then
    printf '  %-42s %s%s%s\n' "$label" "$GREEN" "$actual" "$OFF"
  else
    printf '  %-42s %sREADME says %s, recomputed %s%s\n' \
      "$label" "$RED" "$stated" "$actual" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  fi
}

#-----------------------------------------------------------------------------
# Source-derived. No build, no run: portable and cheap enough for every host.
#-----------------------------------------------------------------------------
entry_points=$(grep -c '^ZTEXT_API' ffi/ztext.h)
externs=$(grep -c '^pub extern fn' src/c.zig)
guard_cases=$(grep -c '^case_ ' ci/check-guards.sh)
# Declared tests, as opposed to test EXECUTIONS: `zig build test` runs the
# suite twice, once clean and once under HarfBuzz's three environment
# variables, so its own count is double this one. Both are stated in README.md
# and both are checked. Blind spot: a `test` block indented inside a struct is
# not counted, because none is written that way here.
suite_test_decls=$(grep -rh '^test ' --include='*.zig' src tests | wc -l | tr -d ' ')
swept=$(grep -oE '\bztext[A-Za-z0-9_]+\(' ffi/ztext.h |
        grep -oE '^ztext[A-Za-z0-9_]+' | sort -u |
        while read -r name; do
          grep -qE "ZTEXT_API[^;]*\b$name\(" ffi/ztext.h || continue
          grep -qF "$name" tests/null_sweep.c && printf 'x\n'
        done | wc -l | tr -d ' ')

zon_version=$(grep -oE '\.version = "[0-9]+\.[0-9]+\.[0-9]+"' build.zig.zon |
              grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
hdr_major=$(grep -oE '#define ZTEXT_VERSION_MAJOR [0-9]+' ffi/ztext.h | grep -oE '[0-9]+$')
hdr_minor=$(grep -oE '#define ZTEXT_VERSION_MINOR [0-9]+' ffi/ztext.h | grep -oE '[0-9]+$')
hdr_patch=$(grep -oE '#define ZTEXT_VERSION_PATCH [0-9]+' ffi/ztext.h | grep -oE '[0-9]+$')
hdr_version="$hdr_major.$hdr_minor.$hdr_patch"

if [ $CHECK -eq 0 ]; then
  printf '%sSources%s\n' "$BOLD" "$OFF"
  row 'C entry points in ffi/ztext.h' "$entry_points"
  row 'externs in src/c.zig' "$externs"
  row 'entry points reached by the null sweep' "$swept"
  row 'mutation cases in ci/check-guards.sh' "$guard_cases"
  row 'test declarations in src and tests' "$suite_test_decls"
  row 'build.zig.zon version' "$zon_version"
  row 'ffi/ztext.h version macros' "$hdr_version"
  note 'the pair is compared by --check, not merely printed here'
fi

#-----------------------------------------------------------------------------
# Build-derived.
#-----------------------------------------------------------------------------
suite_tests=
c_injection=
c_warm=
if [ $CHECK -eq 0 ] || [ $WITH_BUILD -eq 1 ]; then
  # --summary all so the count is reported at all; the grep is deliberately for
  # the count rather than for success, so a red suite still prints what it
  # managed rather than nothing.
  suite_out=$(zig build test --summary all 2>&1)
  suite_tests=$(printf '%s' "$suite_out" |
    grep -oE '[0-9]+/[0-9]+ tests passed' | head -1 | grep -oE '^[0-9]+')
  suite_line=$(printf '%s' "$suite_out" |
    grep -oE '[0-9]+/[0-9]+ steps succeeded; [0-9]+/[0-9]+ tests passed' | head -1)
  c_out=$(zig build test-c 2>&1)
  c_injection=$(printf '%s' "$c_out" |
    grep -oE 'injection \(plain\): [0-9]+ failure points' | grep -oE '[0-9]+' | head -1)
  c_warm=$(printf '%s' "$c_out" |
    grep -oE 'steady state: [0-9]+ shapes' | grep -oE '[0-9]+' | head -1)
fi

if [ $CHECK -eq 0 ]; then
  printf '\n%sSuite%s\n' "$BOLD" "$OFF"
  row 'zig build test' "${suite_line:-FAILED}"
  row 'C boundary, injection points per arm' "${c_injection:-FAILED}"
  row 'C boundary, warm shapes proven silent' "${c_warm:-not reported}"

  if [ $QUICK -eq 0 ]; then
    loop=$(ci/crash-loop.sh 200 2>&1 | grep -oE 'crash-loop: [0-9]+/[0-9]+ [a-zA-Z]+' | tail -1)
    row 'crash loop' "${loop:-FAILED}"
  else
    row 'crash loop' 'skipped (--quick)'
  fi
fi

#-----------------------------------------------------------------------------
# The shared build, and the symbols it does and does not export.
#
# The old version looked for .dylib and .so only, so every Windows host and
# every static-only host was told "no shared library" by a script whose whole
# job was to notice things like that.
#-----------------------------------------------------------------------------
symtool=
for candidate in llvm-nm nm; do
  command -v "$candidate" > /dev/null 2>&1 && { symtool=$candidate; break; }
done

# GNU nm wants --defined-only; BSD/macOS nm wants -U. Ask, rather than assume
# the host this was written on.
symflags='-g'
if [ -n "$symtool" ]; then
  if "$symtool" --help 2>&1 | grep -q -- '--defined-only'; then
    symflags='-g --defined-only'
  else
    symflags='-gU'
  fi
fi

count_symbols() {
  [ -n "$symtool" ] || { printf '?'; return; }
  # shellcheck disable=SC2086
  "$symtool" $symflags "$1" 2>/dev/null | wc -l | tr -d ' '
}
count_ztext_symbols() {
  [ -n "$symtool" ] || { printf '?'; return; }
  # shellcheck disable=SC2086
  "$symtool" $symflags "$1" 2>/dev/null | grep -c ' _\?ztext' || true
}

if [ $CHECK -eq 0 ]; then
  printf '\n%sShared build%s %s(-fvisibility=hidden)%s\n' "$BOLD" "$OFF" "$DIM" "$OFF"
  if zig build -Dshared=true > /dev/null 2>&1; then
    # Every extension a supported host produces, not two of them.
    # ztext's own artefact, by name: zig-out/lib also holds the upstream
    # archives, and picking the first one off a glob measured freetype once.
    shared=$(ls zig-out/lib/*ztext*.dylib zig-out/lib/*ztext*.so \
                zig-out/bin/*ztext*.dll zig-out/lib/*ztext*.dll 2>/dev/null |
             head -1)
    static=$(ls zig-out/lib/*ztext*.a zig-out/lib/*ztext*.lib 2>/dev/null |
             head -1)
    row 'shared artefact' "${shared:-none produced}"
    row 'static artefact' "${static:-none produced}"
    if [ -z "$symtool" ]; then
      row 'exported symbols' 'no nm or llvm-nm on this host'
      note 'install binutils or LLVM to recompute the symbol counts'
    else
      if [ -n "$shared" ]; then
        total=$(count_symbols "$shared")
        ours=$(count_ztext_symbols "$shared")
        row 'exported from the shared library' "$total"
        row "  of which ztext's own" "$ours"
        row '  of which upstream (FreeType)' "$((total - ours))"
      fi
      if [ -n "$static" ]; then
        row 'defined in the static archive' "$(count_symbols "$static")"
        note 'the archive keeps every upstream symbol; the shared build hides them'
      fi
    fi
  else
    row 'shared build' 'FAILED'
  fi
fi

#-----------------------------------------------------------------------------
# Bench.
#-----------------------------------------------------------------------------
if [ $CHECK -eq 0 ] && [ $QUICK -eq 0 ]; then
  printf '\n%sBench%s %s(ReleaseFast, sanitizer off; timings vary 30-40%% on a loaded machine)%s\n' \
    "$BOLD" "$OFF" "$DIM" "$OFF"
  zig build bench -Doptimize=ReleaseFast -Dsanitize_c=false 2>&1 |
    tail -n +3 | sed 's/^/  /'
  note 'byte counts are what the allocator seam was asked for; see the blind spots above'
fi

#-----------------------------------------------------------------------------
# --check: the README, against all of the above.
#-----------------------------------------------------------------------------
if [ $CHECK -eq 1 ]; then
  printf '%sREADME.md against the sources%s\n' "$BOLD" "$OFF"
  claim 'entry points'            'every one of the [0-9]+ entry points' "$entry_points"
  claim 'entry points swept'      'every one of the [0-9]+ entry points' "$swept"
  claim 'mutation cases'          'applies \*\*[0-9]+\*\* deliberate bugs' "$guard_cases"

  # Not a README claim: the two places the version is written. They are
  # separate files with separate reasons to be edited, and a package whose
  # header and manifest disagree ships a lie to whichever consumer reads the
  # other one.
  if [ "$zon_version" = "$hdr_version" ]; then
    printf '  %-42s %s%s%s\n' 'version, build.zig.zon = ffi/ztext.h' \
      "$GREEN" "$zon_version" "$OFF"
  else
    printf '  %-42s %sbuild.zig.zon %s, ffi/ztext.h %s%s\n' \
      'version' "$RED" "$zon_version" "$hdr_version" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ $WITH_BUILD -eq 1 ]; then
    printf '\n%sREADME.md against what runs%s\n' "$BOLD" "$OFF"
    claim 'tests in the suite'    '\*\*[0-9]+ tests\*\*, executed' "$suite_test_decls"
    claim 'test executions'       'reports \*\*[0-9]+/[0-9]+ passed\*\*' "$suite_tests"
    claim 'injection points'      '\*\*[0-9]+ injected allocation-failure points\*\*' "$c_injection"
    claim 'warm shapes'           '\*\*[0-9]+ warm shapes allocate nothing\*\*' "$c_warm"
  else
    printf '\n  %sthe suite and C-boundary numbers need --with-build%s\n' "$DIM" "$OFF"
  fi

  # UPSTREAM.md's table against src/pins.zig. The table is prose and the pin is
  # the fact; before this they were two independent copies, and the version row
  # said "7.0" where the library reports 7.0.0.
  printf '\n%sUPSTREAM.md against src/pins.zig%s\n' "$BOLD" "$OFF"
  upstream_cell() {
    # <row label> <1-based project column>; the table's first field is empty.
    grep -E "^[|] $1 [|]" UPSTREAM.md | head -1 |
      awk -F'|' -v n="$(($2 + 2))" '{ gsub(/^ +| +$/, "", $n); gsub(/`/, "", $n); print $n }'
  }
  column=0
  for name in $(pin_names); do
    column=$((column + 1))
    for pair in "Version:$(pin_version "$name")" "Tag:$(pin "$name" tag)" \
                "Commit:$(pin "$name" commit)"; do
      label=${pair%%:*}
      want=${pair#*:}
      got=$(upstream_cell "$label" "$column")
      if [ "$want" = "$got" ]; then
        printf '  %-42s %s%s%s\n' "$name $label" "$GREEN" "$want" "$OFF"
      else
        printf '  %-42s %sUPSTREAM.md %s, src/pins.zig %s%s\n' \
          "$name $label" "$RED" "${got:-<no such cell>}" "$want" "$OFF"
        MISMATCHES=$((MISMATCHES + 1))
      fi
    done
  done

  printf '\n'
  if [ $MISMATCHES -eq 0 ]; then
    printf '%severy number checked matches the sources%s\n' "$GREEN" "$OFF"
    printf '%s(this checks the numbers it was taught; a number nobody added a\n' "$DIM"
    printf ' claim() line for is not checked, and the bench figures and symbol\n'
    printf ' counts are platform-specific and printed rather than gated)%s\n' "$OFF"
    exit 0
  fi
  printf '%s%d recorded number(s) no longer hold%s\n' "$RED" "$MISMATCHES" "$OFF" >&2
  exit 1
fi
