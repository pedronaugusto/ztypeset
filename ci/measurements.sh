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
#   - The contents check compares the section names in README's own table of
#     contents with its "## " headings. It does not check that a link
#     RESOLVES: an anchor GitHub would generate differently from the heading
#     -- one holding punctuation, say -- passes this and still 404s.
#   - The 26.6 check greps for the divisor. It cannot see a conversion
#     spelled some other way -- `* 0.015625f`, or a shift on the fixed value
#     before the cast -- and it deliberately does not look outside ffi/*.c,
#     since the Zig side never sees fixed point.
#   - The restated-number check searches for the VALUES the claims above
#     recompute: anywhere in SECURITY.md or CONTRIBUTING.md, and on a second
#     line of README.md. It cannot see a number written as a word, it cannot
#     see one in a document nobody added to its list, and a value that happens
#     to be right for two unrelated things reads as a restatement -- a false
#     positive an author resolves by rewording, not a hole.
#   - It cannot see a copy that has ALREADY gone stale, because a stale copy
#     holds the old value and the check searches for the current one. Nothing
#     can: what prevents that defect is there being one place to write the
#     number, which is the rule this check enforces rather than the symptom it
#     detects.
#   - The `paths` check compares TOP-LEVEL names only. It proves nothing
#     about what is inside a listed directory, and it cannot tell a file that
#     should ship from one that should not -- only that every entry the
#     repository has is either shipped or on the short exclusion list beside
#     the check, where adding one is a visible edit.
#   - The fopen check proves the C tests do not open files themselves. It
#     does not prove they USE the helper -- a test that never reads a font
#     passes it trivially -- and it says nothing about ffi/, which opens no
#     file at all and is not searched.
#   - The *Destroy check looks for the words "exactly once" in the doc
#     comment attached to each declaration. It cannot tell a correct
#     explanation from an incorrect one -- only that the rule is stated where
#     a reader of that function will see it.
#   - The consumer-artifact check proves each name is PASSED to
#     ztext.artifact(); only running tests/consumer proves it resolves, which
#     CI does on all three hosts and both Windows ABIs.
#   - The guard-table check compares whole rows: the section name, then the
#     harness's case names in its order. It cannot tell whether a case name
#     describes what that case actually mutates -- only that the table and the
#     harness say the same thing, which is the promise a paraphrase could not
#     make.
#   - The call-site check counts a TOKEN. It holds the guard command to one
#     call site inside run_guarded; it does not read what run_guarded does, so
#     a deadline deleted from inside that function, or a case that shells out
#     to the build by some other spelling, is invisible to it. What catches
#     the first is the guard case that mutates the call site; the deadline
#     itself is held by nothing but that function being the one place to
#     look.
#   - The installed-allocator check reads src/integration_test.zig only, and
#     asks only that the words appear in the same test block. It cannot see a
#     defer that resets the wrong thing, a helper called by a test that
#     installs one, or the same defect in another file. What makes those
#     unlikely rather than unchecked is that the suite has one place where an
#     allocator is installed for a whole test and one Fixture that does it for
#     the rest.
#   - The build-option check asks that the flag's NAME appear in README.md,
#     not that what README says about it is true. A row that names -Dshared
#     and then describes the wrong effect passes. It also cannot see an option
#     declared anywhere but build.zig, and there is nowhere else.
#   - The ffi/ztext.h banner check proves every pinned upstream is NAMED
#     there. It cannot prove the sentence around those names is true, and it
#     says nothing about the counts written in prose elsewhere -- a count with
#     no list beside it is not checkable, which is the argument for not
#     writing one.
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
# README.md's own table of contents, against its own headings. A table of
# contents is a copy of a list that is already in the file, which is the shape
# every other check here exists for.
readme_headings=$(grep '^## ' README.md | sed 's/^## //' | grep -v '^Contents$')
readme_toc=$(sed -n '/^## Contents$/,/^## Usage$/p' README.md |
             grep -oE '^\[[^]]+\]|·[[:space:]]*\[[^]]+\]|^\[[^]]+\]' |
             sed 's/.*\[//; s/\]//')
toc_diff=$(diff <(printf '%s\n' "$readme_headings") \
                <(printf '%s\n' "$readme_toc") 2>&1 || true)

# The version has a fourth home nobody was checking: README.md's status line,
# which said v0.1 while the header, the manifest and the changelog all said
# 0.2.0. Three homes were gated and the fourth was prose beside them.
readme_version=$(sed -n 's/^Status: \*\*v\([0-9]*\.[0-9]*\)\*\*.*/\1/p' README.md)

# The 26.6 conversion, which has exactly one home. `(float)x / 64.0f` was
# written out at twenty-six sites in three translation units -- four of them
# per glyph in the shaping loop -- for a conversion this library performs
# everywhere. ztextFrom266 is that home, and a grep is the gate: an
# open-coded one is visible in the source, so nothing subtler is needed.
open_coded_266=$(grep -n '/ 64\.0f' ffi/*.c || true)

# The C tests open a file in exactly one place. Three copies of "read this
# font into memory" is what the helper replaced, and a fourth would be written
# the moment someone needs one, so the gate is that no test .c calls fopen at
# all -- the helper header is the only file allowed to.
test_fopen=$(grep -n 'fopen' tests/*.c || true)
helper_fopen=$(grep -c 'fopen(' tests/ztext_test_io.h || true)

# Every top-level entry of the repository against build.zig.zon's `paths`.
# `paths` is what a consumer receives: anything missing from it is absent from
# the fetched package while still present in the checkout, so the in-repo
# suite cannot see the difference and tests/consumer -- which resolves the
# dependency by local path -- cannot either. build.zig compiles
# examples/quickstart.zig, and `examples` was not in `paths`, so a consumer
# fetching ztext got a build graph naming a file its package did not contain.
# CONTRIBUTING.md and SECURITY.md were missing the same way.
zon_paths=$(sed -n '/\.paths = \.{/,/}/p' build.zig.zon |
            sed -n 's/.*"\([^"]*\)".*/\1/p' | sort)
# The exclusions, each for a reason. .git, .zig-cache and zig-out are not
# source. .gitignore describes a working copy rather than a package, and Zig
# does not read it.
top_level=$(ls -A . |
            grep -vxE '\.git|\.gitignore|\.zig-cache|zig-out' | sort)
unshipped=$(comm -23 <(printf '%s\n' "$top_level") <(printf '%s\n' "$zon_paths"))

# Every *Destroy in ffi/ztext.h must state the exactly-once rule in its own
# documentation. Two of the six used to be documented as tolerating a repeat,
# on the strength of a flag test that reads like a repeat guard and is not one
# -- by the time a caller could repeat the call the handle is freed, so the
# flag test is the use-after-free. That claim was written from reading the
# source and a test crashed on it. Prose is where this rule lives, because no
# runtime check for it can exist, so the gate is on the prose.
undocumented_destroy=$(awk '
  /^\/\/\// { block = block $0 "\n"; next }
  /^ZTEXT_API void ztext[A-Za-z]*Destroy\(/ {
    name = $0
    sub(/.*void /, "", name)
    sub(/\(.*/, "", name)
    if (block !~ /exactly once/) print name
    block = ""
    next
  }
  { block = "" }
' ffi/ztext.h)
destroy_count=$(grep -c '^ZTEXT_API void ztext[A-Za-z]*Destroy(' ffi/ztext.h)

# Every library ztext installs, against the consumer that is supposed to link
# each of them. tests/consumer exists because `dependency.artifact(name)`
# panics on a name the dependency does not register and nothing in the in-repo
# suite goes through that path -- and it linked four of the five, having missed
# libunibreak for as long as it has been vendored. A list checked against a
# list, since that is what it is.
installed_artifacts=$(grep -A2 'b.addLibrary(.{' build.zig |
                      sed -n 's/.*\.name = "\([^"]*\)".*/\1/p' | sort -u)
unlinked_artifacts=
for artifact in $installed_artifacts; do
  grep -qF "ztext.artifact(\"$artifact\")" tests/consumer/build.zig ||
    unlinked_artifacts="$unlinked_artifacts $artifact"
done

# ci/check-guards.sh's sections AND case names, against the table in
# README.md that lists them. The table was a hand-kept mirror, which is the
# defect this file exists to catch everywhere else: four sections were added
# to the harness and none of them reached the README, and later two rows went
# on describing five cases in a section that had grown to seven.
#
# So the table is no longer a paraphrase of the cases. It IS the case names,
# in the harness's own order, and this compares the whole row rather than the
# section name alone. Order included: a table listing the same sections in a
# different order from the file no longer reads as a map of it.
guard_rows=$(awk '
  /^printf .\\n%s/ {
    s = $0
    sub(/^[^%]*%s/, "", s)
    sub(/%s.*$/, "", s)
    sec = s
    order[++n] = sec
    next
  }
  /^case_ "/ {
    s = $0
    sub(/^case_ "/, "", s)
    sub(/".*$/, "", s)
    names[sec] = (names[sec] == "" ? s : names[sec] "; " s)
  }
  END { for (i = 1; i <= n; i++)
          print "| " order[i] " | " names[order[i]] " |" }
' ci/check-guards.sh)
readme_rows=$(awk '/^\| section \| what is broken/ { f = 1; next }
                   /^\|---\|---\|$/ { next }
                   f && !/^\| / { exit }
                   f { print }' README.md)
guard_section_diff=$(diff <(printf '%s\n' "$guard_rows") \
                          <(printf '%s\n' "$readme_rows") 2>&1 || true)

# And that the harness runs those cases in exactly one place, the bounded one.
#
# ci/check-guards.sh used to run each case's command straight into a command
# substitution, with no deadline: a case that hung took the whole sweep with
# it, silently, for as long as anyone let it. The fix is run_guarded, which
# writes to a file and gives the command a deadline -- and a fix like that is
# only worth as much as the guarantee that nothing runs the command anywhere
# else, because a second call site would be unbounded again and would read
# exactly like the first.
# Every build option `build.zig` declares is named in README.md.
#
# An option nobody can find is an option that does not exist: `-Dshared` was
# declared, tested by ci/run.sh, and mentioned in README only as the phrase "a
# shared build" -- with no way to learn what to type. The name is the part
# that has to be written down, so the name is what this matches on.
#
# The first string literal after `b.option(` is the option's name; the line
# between them is its type.
build_options=$(awk '
  index($0, "b.option(") { pending = 1; next }
  pending && match($0, /"[^"]+"/) {
    print substr($0, RSTART + 1, RLENGTH - 2)
    pending = 0
  }
' build.zig)
options_total=$(printf '%s
' "$build_options" | grep -c .)
options_undocumented=""
for opt in $build_options; do
  grep -qF -- "-D$opt" README.md || options_undocumented="$options_undocumented $opt"
done

# A test that installs a process-wide allocator has to take it out again on
# EVERY path, and only `defer` is every path.
#
# The allocator ztext installs is process-wide and these tests back it with a
# DebugAllocator living in the test's own frame. A bare reset at the end of the
# body is not reached when an assertion above it fails, so a FAILING test used
# to leave ztext allocating through a frame that had ended -- for every test
# after it, in the same process. It is the defect the C smoke test was already
# fixed for, in the language the wrapper is written in, and it does not
# announce itself: what it did here was hang a mutation sweep with no verdict,
# no output and nothing to read but the process table.
#
# So: any `test` block that installs one must also defer the reset. The
# Fixture's own deinit is not a test block and is not asked to.
alloc_tests_total=$(awk '
  /^test "/ { inside = 1; has_set = 0; next }
  inside && /^\}/ { if (has_set) n++; inside = 0; next }
  inside && index($0, "ztext.setAllocator(") { has_set = 1 }
  END { print n + 0 }
' src/integration_test.zig)
alloc_tests_loose=$(awk '
  /^test "/ {
    name = $0
    sub(/^test "/, "", name)
    sub(/".*$/, "", name)
    inside = 1; has_set = 0; has_defer = 0
    next
  }
  inside && /^\}/ {
    if (has_set && !has_defer) print name
    inside = 0
    next
  }
  inside && index($0, "ztext.setAllocator(") { has_set = 1 }
  inside && index($0, "defer ztext.resetAllocator()") { has_defer = 1 }
' src/integration_test.zig)
guard_run_sites=$(grep -cF '"${GUARD_CMD[@]}"' ci/check-guards.sh)
guard_run_loose=$(awk '
  index($0, "run_guarded() {") == 1 { inside = 1; next }
  inside && $0 == "}" { inside = 0; next }
  !inside && index($0, "\"${GUARD_CMD[@]}\"") { print FNR ": " $0 }
' ci/check-guards.sh)

# The upstreams ffi/ztext.h's banner names, against src/pins.zig. Six places
# said "three" when the package had vendored four for months, and the one that
# matters is this one: it is the first line a consumer reads and it is a LIST,
# so it can be checked rather than proof-read. A count in prose elsewhere is
# still prose; this is the enumeration.
banner=$(sed -n '2p' ffi/ztext.h)
missing_upstreams=
for name in $(pin_names); do
  printf '%s' "$banner" | grep -qi -- "$name" || missing_upstreams="$missing_upstreams $name"
done

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

# The numbers README.md states are recomputed above, one at a time, against
# the source each comes from. The other prose documents must not restate any
# of them. A number written twice goes stale in one place while the other
# still reads as current -- the defect this whole file exists to prevent --
# and it had already happened twice by the time this check was written:
# SECURITY.md carried the entry-point count, and CONTRIBUTING.md, in the very
# paragraph telling a contributor to add the line that recomputes a number,
# carried an approximate count of the mutation cases that was no longer true.
#
# CHANGELOG.md is not searched, and must not be: a released entry states what
# was true at that release, and correcting it later would make the history a
# second, lying copy of the present.
gated_numbers="$entry_points $swept $guard_cases $suite_test_decls"
# The build-derived four are empty without --with-build, and an empty token
# would be searched for as the empty string -- matching every line.
if [ -n "$suite_tests" ]; then
  gated_numbers="$gated_numbers $suite_tests $c_injection $c_warm"
fi
#
# README.md is searched too, for a SECOND line carrying the same value. One
# line is the home; a second is a copy, and it was one of these that went
# stale first -- the quick-reference list said "do all 90 still apply" while
# the sentence the check reads said 92. Lines rather than matches, because
# "294/294 passed" is one number written once.
restated=
for gated in $gated_numbers; do
  hit=$(grep -nE "(^|[^0-9.])${gated}([^0-9.]|\$)" \
          SECURITY.md CONTRIBUTING.md 2> /dev/null || true)
  [ -n "$hit" ] && restated="${restated}${hit}
"
  twice=$(grep -nE "(^|[^0-9.])${gated}([^0-9.]|\$)" README.md || true)
  if [ "$(printf '%s' "$twice" | grep -c .)" -gt 1 ]; then
    restated="${restated}${twice}
"
  fi
done

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

  # The FOUR places ztext's version is written. Four files with four reasons
  # to be edited, and a package whose header, manifest, changelog and README
  # disagree gives a different answer to each consumer depending on which one
  # they read. CHANGELOG.md states the policy those numbers follow; this is
  # what stops the policy from being prose.
  #
  # README.md was the fourth and was not checked, so it sat at "Status: v0.1"
  # through the whole of 0.2's development -- three homes gated and a fourth
  # in prose beside them, which is worse than not gating any, because the
  # green row reads as though it covered them all. It carries only major.minor
  # (a patch release is not a status change), so it is compared as a prefix.
  #
  # An empty value fails here too: if any of the four greps stops matching --
  # a heading reworded, a macro renamed -- the comparison must go red rather
  # than compare two blanks and pass.
  if [ -n "$zon_version" ] && [ -n "$readme_version" ] &&
     [ "$zon_version" = "$hdr_version" ] &&
     [ "$hdr_version" = "$changelog_version" ] &&
     [ "${hdr_version%.*}" = "$readme_version" ]; then
    printf '  %-42s %s%s%s\n' 'version, zon = header = CHANGELOG = README' \
      "$GREEN" "$zon_version" "$OFF"
  else
    printf '  %-42s %sthe four version homes disagree: build.zig.zon %s, ffi/ztext.h %s, CHANGELOG.md %s, README.md v%s%s\n' \
      'version' "$RED" "${zon_version:-<none>}" "${hdr_version:-<none>}" \
      "${changelog_version:-<none>}" "${readme_version:-<none>}" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$toc_diff" ]; then
    printf '  %-42s %s%s sections%s\n' 'README contents = README headings' \
      "$GREEN" "$(printf '%s\n' "$readme_headings" | wc -l | tr -d ' ')" "$OFF"
  else
    printf '  %-42s %sthe two lists differ%s\n' \
      'README contents = README headings' "$RED" "$OFF"
    printf '%s\n' "$toc_diff" | sed 's/^/    /'
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$open_coded_266" ]; then
    printf '  %-42s %sztextFrom266%s\n' '26.6 to pixels has one home' \
      "$GREEN" "$OFF"
  else
    printf '  %-42s %sopen-coded%s\n' '26.6 to pixels has one home' \
      "$RED" "$OFF"
    printf '%s\n' "$open_coded_266" | sed 's/^/    /'
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$restated" ]; then
    printf '  %-42s %s%s numbers, one home each%s\n' \
      'each gated number is written in one place' \
      "$GREEN" "$(printf '%s' "$gated_numbers" | wc -w | tr -d ' ')" "$OFF"
  else
    printf '  %-42s %sstated a second time%s\n' \
      'each gated number is written in one place' "$RED" "$OFF"
    printf '%s' "$restated" | sort -u | sed 's/^/    /'
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$unshipped" ]; then
    printf '  %-42s %s%s entries%s\n' 'build.zig.zon ships every top-level entry' \
      "$GREEN" "$(printf '%s\n' "$top_level" | wc -l | tr -d ' ')" "$OFF"
  else
    printf '  %-42s %snot in paths:%s%s\n' \
      'build.zig.zon ships every top-level entry' "$RED" \
      "$(printf ' %s' $unshipped)" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$test_fopen" ] && [ "$helper_fopen" = 1 ]; then
    printf '  %-42s %sztextTestReadFile%s\n' 'the C tests open a file in one place' \
      "$GREEN" "$OFF"
  else
    printf '  %-42s %sopen-coded%s\n' 'the C tests open a file in one place' \
      "$RED" "$OFF"
    printf '%s\n' "$test_fopen" | sed 's/^/    /'
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$undocumented_destroy" ]; then
    printf '  %-42s %s%s state it%s\n' 'every *Destroy documents exactly-once' \
      "$GREEN" "$destroy_count" "$OFF"
  else
    printf '  %-42s %sno rule on%s%s\n' 'every *Destroy documents exactly-once' \
      "$RED" "$(printf ' %s' $undocumented_destroy)" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$unlinked_artifacts" ]; then
    printf '  %-42s %s%s%s\n' 'the consumer links every artifact' \
      "$GREEN" "$(printf '%s ' $installed_artifacts)" "$OFF"
  else
    printf '  %-42s %stests/consumer/build.zig links no%s%s\n' \
      'the consumer links every artifact' "$RED" "$unlinked_artifacts" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$guard_section_diff" ]; then
    printf '  %-42s %s%s rows%s\n' 'check-guards.sh = README guard table' \
      "$GREEN" "$(printf '%s\n' "$guard_rows" | wc -l | tr -d ' ')" "$OFF"
  else
    printf '  %-42s %sthe two lists differ%s\n' \
      'check-guards.sh = README guard table' "$RED" "$OFF"
    printf '%s\n' "$guard_section_diff" | sed 's/^/    /'
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$alloc_tests_loose" ]; then
    printf '  %-42s %s%s defer the reset%s\n' \
      'tests that install an allocator' "$GREEN" "$alloc_tests_total" "$OFF"
  else
    printf '  %-42s %sinstalls an allocator without deferring the reset%s\n' \
      'tests that install an allocator' "$RED" "$OFF"
    printf '%s\n' "$alloc_tests_loose" | sed 's/^/    /'
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ "$guard_run_sites" -eq 1 ] && [ -z "$guard_run_loose" ]; then
    printf '  %-42s %sone bounded call site%s\n' \
      'check-guards.sh runs a case in one place' "$GREEN" "$OFF"
  else
    printf '  %-42s %sthe guard command is run outside the bounded runner%s\n' \
      'check-guards.sh runs a case in one place' "$RED" "$OFF"
    printf '%s\n' "$guard_run_loose" | sed 's/^/    /'
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$options_undocumented" ]; then
    printf '  %-42s %s%s, each named in README.md%s
'       'build options' "$GREEN" "$options_total" "$OFF"
  else
    printf '  %-42s %sbuild.zig declares%s, and README.md names no such flag%s
'       'build options' "$RED" "$options_undocumented" "$OFF"
    MISMATCHES=$((MISMATCHES + 1))
  fi

  if [ -z "$missing_upstreams" ]; then
    printf '  %-42s %s%s%s\n' 'ffi/ztext.h names every pinned upstream' \
      "$GREEN" "$(pin_names | tr '\n' ' ')" "$OFF"
  else
    printf '  %-42s %sffi/ztext.h line 2 names no%s%s\n' \
      'ffi/ztext.h upstreams' "$RED" "$missing_upstreams" "$OFF"
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
