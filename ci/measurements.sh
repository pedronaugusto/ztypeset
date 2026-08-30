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
#   - The LICENSES.md check covers the ONE row whose answer a build macro
#     decides. The other rows are settled by whether a file is in build.zig's
#     source list, which nothing here reads; they are prose with a citation,
#     not a gated number.
#   - sizeof(ZtextAbiLayout) is arithmetic over the header's field list, not a
#     compiler's answer. It is right because every field is a uint32_t and the
#     check refuses to compute anything when that stops holding, but it would
#     not notice a pragma pack or an attribute that changed the layout without
#     changing a field type. The null sweep compares the two for real:
#     ztextAbiLayout's own layout_size against the consumer's sizeof.

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
# shellcheck source=ci/sha256.sh
. ci/sha256.sh

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
# LICENSES.md's "Reaches your binary?" cell for the FreeType autofit files
# that call HarfBuzz, and the macro that decides it. Those five files compile
# to a dummy typedef without FT_CONFIG_OPTION_USE_HARFBUZZ and to real code
# with it, so one edit to ffi/ztext_ftoption.h silently makes a licence page
# wrong about what a consumer ships. This is the pair that drifted.
ft_hb_macro=no
grep -qE '^#define FT_CONFIG_OPTION_USE_HARFBUZZ$' ffi/ztext_ftoption.h &&
  ft_hb_macro=yes
ft_hb_cell=$(grep -m1 -F 'src/autofit/ft-hb.c' LICENSES.md |
             awk -F'|' '{ print $4 }')
ft_hb_claim=unreadable
case "$ft_hb_cell" in
  ' **Yes.**'*) ft_hb_claim=yes ;;
  ' No.'*)      ft_hb_claim=no ;;
esac

# The newest release heading in CHANGELOG.md. `grep -m1` rather than a sort,
# because newest-first is the file's own order and a sort would quietly accept
# an entry filed in the wrong place.
changelog_version=$(grep -m1 -oE '^## [0-9]+\.[0-9]+\.[0-9]+' CHANGELOG.md |
                    grep -oE '[0-9]+\.[0-9]+\.[0-9]+')

# sizeof(ZtextAbiLayout), recomputed from the header rather than believed from
# CHANGELOG.md's ABI table. That table tells a consumer how much the runtime
# handshake grew, which is exactly the number nobody re-derives when a field is
# appended -- so it is a documented number with a source, and this is the
# source.
#
# Every field of that struct is a uint32_t by construction: it is a handshake,
# not a payload, so the size is the field count times four and there is no
# padding to reason about. The "by construction" is the part that needs
# checking rather than assuming. A line of any other type makes the arithmetic
# invalid, so it produces an empty size, which --check reports as a failure
# instead of a plausible wrong number. An empty struct body does the same, so
# a grep that silently stops matching goes red too.
abi_layout_body=$(awk '/^typedef struct ZtextAbiLayout \{/,/^\} ZtextAbiLayout;/' \
                    ffi/ztext.h | grep -E '^  [A-Za-z_].*;$')
abi_layout_size=
if [ -n "$abi_layout_body" ] &&
   ! printf '%s\n' "$abi_layout_body" | grep -qv '^  uint32_t '; then
  abi_layout_size=$(($(printf '%s\n' "$abi_layout_body" | wc -l | tr -d ' ') * 4))
fi
# The size the newest ABI table says ZtextAbiLayout grew TO -- the third
# column of its row, the second being what it grew from.
changelog_layout_size=$(grep -m1 -E '^\| `ZtextAbiLayout` \|' CHANGELOG.md |
                        awk -F'|' '{ gsub(/[^0-9]/, "", $4); print $4 }')

if [ $CHECK -eq 0 ]; then
  printf '%sSources%s\n' "$BOLD" "$OFF"
  row 'C entry points in ffi/ztext.h' "$entry_points"
  row 'externs in src/c.zig' "$externs"
  row 'entry points reached by the null sweep' "$swept"
  row 'mutation cases in ci/check-guards.sh' "$guard_cases"
  row 'test declarations in src and tests' "$suite_test_decls"
  row 'build.zig.zon version' "$zon_version"
  row 'ffi/ztext.h version macros' "$hdr_version"
  row 'CHANGELOG.md newest entry' "$changelog_version"
  note 'the three are compared by --check, not merely printed here'
  row 'sizeof(ZtextAbiLayout) from ffi/ztext.h' "${abi_layout_size:-<not all uint32_t: unrecomputable>}"
  row "CHANGELOG.md's ABI table says" "${changelog_layout_size:-<none>}"
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

  # Not a README claim: the THREE places ztext's version is written. Three
  # files with three reasons to be edited, and a package whose header,
  # manifest and changelog disagree gives a different answer to each consumer
  # depending on which one it read. CHANGELOG.md states the policy those
  # numbers follow; this is what stops the policy from being prose.
  #
  # An empty value fails here too: if any of the three greps stops matching --
  # a heading reworded, a macro renamed -- the comparison must go red rather
  # than compare two blanks and pass.
  if [ -n "$zon_version" ] && [ "$zon_version" = "$hdr_version" ] &&
     [ "$hdr_version" = "$changelog_version" ]; then
    printf '  %-42s %s%s%s\n' 'version, zon = header = CHANGELOG' \
      "$GREEN" "$zon_version" "$OFF"
  else
    printf '  %-42s %sthe three version homes disagree: build.zig.zon %s, ffi/ztext.h %s, CHANGELOG.md %s%s\n' \
      'version' "$RED" "${zon_version:-<none>}" "${hdr_version:-<none>}" \
      "${changelog_version:-<none>}" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ "$ft_hb_macro" = "$ft_hb_claim" ]; then
    printf '  %-42s %s%s%s\n' 'LICENSES.md ft-hb row = ftoption.h' \
      "$GREEN" "$ft_hb_claim" "$OFF"
  else
    printf '  %-42s %sffi/ztext_ftoption.h defines the macro: %s; LICENSES.md claims: %s%s\n' \
      'LICENSES.md ft-hb row' "$RED" "$ft_hb_macro" "$ft_hb_claim" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  fi

  # The handshake struct's size, as the newest CHANGELOG entry states it. A
  # consumer that compiled against the old header is told to compare
  # layout_size at run time; the table is what tells a reader which value that
  # is, and an appended field that nobody added four bytes for here makes the
  # entry describe a struct that no longer exists.
  if [ -n "$abi_layout_size" ] && [ "$abi_layout_size" = "$changelog_layout_size" ]; then
    printf '  %-42s %s%s B%s\n' 'sizeof(ZtextAbiLayout) = CHANGELOG' \
      "$GREEN" "$abi_layout_size" "$OFF"
  else
    printf '  %-42s %sffi/ztext.h %s, CHANGELOG.md %s%s\n' \
      'sizeof(ZtextAbiLayout)' "$RED" \
      "${abi_layout_size:-<not all uint32_t: unrecomputable>}" \
      "${changelog_layout_size:-<none>}" "$OFF"
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

  # LICENSES.md against the texts it summarises.
  #
  # Every legal statement in that file was written against specific bytes
  # under libs/. Nothing else notices when a re-vendor changes one: the tree
  # still matches its pinned upstream, so ci/verify-vendor.sh is green, and
  # the document goes on describing a licence that is no longer there.
  #
  # This lives here rather than in verify-vendor.sh because it needs no
  # network -- it compares a document against the working tree, which is what
  # this script is for -- and because ci/run.sh runs this and not that.
  #
  # The rows drive the check; there is no second list. A row whose file is
  # gone, or whose digest has moved, is a failure, and so is a table with
  # nothing in it: a document that stops claiming anything must not become a
  # document that passes.
  printf '\n%sLICENSES.md against the texts it summarises%s\n' "$BOLD" "$OFF"
  if [ -z "$(sha256_tool)" ]; then
    printf '  %-42s %sno sha256sum or shasum available%s\n' \
      'licence texts' "$RED" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  else
    licence_rows=0
    while read -r path want; do
      licence_rows=$((licence_rows + 1))
      if [ ! -f "$path" ]; then
        printf '  %-42s %sno such file%s\n' "$path" "$RED" "$OFF"
        MISMATCHES=$((MISMATCHES + 1))
        continue
      fi
      got=$(sha256_of "$path")
      if [ "$got" = "$want" ]; then
        printf '  %-42s %sunchanged%s\n' "$path" "$GREEN" "$OFF"
      else
        printf '  %-42s %sLICENSES.md says %s, the file is %s%s\n' \
          "$path" "$RED" "${want:0:12}..." "${got:0:12}..." "$OFF"
        MISMATCHES=$((MISMATCHES + 1))
      fi
    done <<EOF
$(grep -oE '`libs/[^`]+` [|] `[0-9a-f]{64}`' LICENSES.md | tr -d '`' |
  sed 's/ | / /')
EOF
    if [ "$licence_rows" -lt 7 ]; then
      printf '  %-42s %sonly %d rows; the table has been emptied%s\n' \
        'licence texts' "$RED" "$licence_rows" "$OFF"
      MISMATCHES=$((MISMATCHES + 1))
    fi
  fi

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
