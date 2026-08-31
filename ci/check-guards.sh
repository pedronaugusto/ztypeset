#!/usr/bin/env bash
#
# ztext -- prove the guards would actually fail.
#
# A test that passes tells you nothing about whether it CAN fail. Every guard
# in this package was validated by breaking it on purpose and watching the
# right test go red -- but done by hand, that evidence lives in a transcript
# and evaporates. This turns it into something that runs.
#
# Each case below applies one deliberate mutation to a source file, runs the
# suite, and asserts that a NAMED test failed -- not merely that something
# did. A mutation the suite survives is reported as a hole in the guard, which
# is the failure this script exists to produce.
#
# Mutations are applied to a copy of the tree so an interrupted run cannot
# leave a patched source behind.
#
# A mutation is a deliberate bug, so a case can hang rather than fail. Each is
# given a deadline derived from the baseline build, and a case that reaches it
# is reported TIMED OUT and stops the sweep -- see run_guarded.
#
# Usage:
#   ci/check-guards.sh              # every case
#   ci/check-guards.sh <regex>      # only cases whose name matches, e.g.
#                                   #   ci/check-guards.sh 'enum:|struct:'
#   ci/check-guards.sh --anchors    # every anchor still applies; run nothing
#
# Slow by nature: each case is a full rebuild of the ztext library plus the
# suite. It is a separate step from ci/run.sh for that reason.

set -uo pipefail
cd "$(dirname "$0")/.."

# --anchors asks the one question that does not need a build: does every
# mutation below still apply to the tree exactly once?
#
# A case whose anchor has moved reports NO ANCHOR, which is the right verdict
# and costs a full sweep to reach -- minutes per case, and the anchor breaks
# the moment anyone refactors the line it quotes. Asked this way it is a
# second, so a refactor that stranded a case is caught by the fast gate rather
# than by the slow one. It runs against the real tree, not a copy: there is
# nothing to mutate, so there is nothing to restore.
ANCHORS_ONLY=0
if [ "${1:-}" = "--anchors" ]; then
  ANCHORS_ONLY=1
  shift
fi

FILTER="${1:-}"

if [ -t 1 ]; then
  RED=$'\033[31m'; GREEN=$'\033[32m'; DIM=$'\033[2m'; BOLD=$'\033[1m'; OFF=$'\033[0m'
else
  RED=; GREEN=; DIM=; BOLD=; OFF=
fi

PASSED=0
FAILED=0
FAILED_NAMES=()

# What a case runs to see whether the mutation was caught. Almost every
# guard in this package is a test in the Zig suite, so that is the default;
# a guard that lives in a script instead sets this around its own cases and
# puts it back. It is not a parameter of case_ because the grouping is what
# makes the cost visible: each of these is a build of its own.
GUARD_CMD=(zig build test)

# How long one case may run before it is called hung rather than slow, and
# whether the working copy outlives the run. The deadline is measured from the
# baseline build below; it stays 0 for --anchors, which runs nothing.
GUARD_DEADLINE=0
KEEP_WORK=0

WORK=$(mktemp -d)
ANCHOR_TREE=.

# The working copy goes; the failure logs under it do not, or the evidence for
# a red case dies with the run that produced it.
cleanup() {
  if [ "${FAILED:-0}" -gt 0 ] && [ -d "$WORK/failures" ]; then
    keep="${TMPDIR:-/tmp}/ztext-guard-failures.$$"
    if mkdir -p "$keep" && cp "$WORK/failures/"*.log "$keep/" 2>/dev/null; then
      printf '\nfailure logs kept in %s\n' "$keep" >&2
    fi
  fi
  # A timed-out case had its command killed, but a build that command started
  # may not have been. Deleting the tree out from under a process still
  # writing to it ends the run with a half-removed directory and an error
  # that describes neither problem. Keep it, and say where it is.
  if [ "${KEEP_WORK:-0}" -eq 1 ]; then
    printf 'working copy kept at %s\n' "$WORK" >&2
    return
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# A pristine copy, reused by every case: only the mutated file is restored
# between them, so the build cache survives and a run takes minutes not hours.
if [ "$ANCHORS_ONLY" -eq 0 ]; then
printf '%spreparing a working copy%s\n' "$DIM" "$OFF"
mkdir -p "$WORK/tree"
tar -cf - --exclude .git --exclude .zig-cache --exclude zig-out \
    --exclude tests/consumer/.zig-cache --exclude tests/consumer/zig-out . |
  tar -xf - -C "$WORK/tree"
fi

# The verdict matcher, checked before anything is judged by it.
#
# The bug this replaced was invisible for exactly one reason: a matcher that
# silently fails to match is indistinguishable from a mutation that was not
# caught, and "not caught" is a plausible answer. `grep -q` exits on its first
# match, the printf feeding it takes SIGPIPE, and under `set -o pipefail` the
# pipeline reports 141 -- so the longer the build output and the earlier the
# match, the more often a caught mutation was reported as a hole in the suite.
#
# This is the shape that broke: a match at the very front of an output far
# larger than a pipe buffer.
selftest_matcher() {
  local needle='the-line-that-must-be-found'
  local haystack
  haystack="$needle"$'\n'$(head -c 200000 /dev/zero | tr '\0' 'x')
  if ! [[ "$haystack" == *"$needle"* ]]; then
    printf '%sthe verdict matcher cannot see a match at the front of a large output;%s\n' \
      "$RED" "$OFF" >&2
    printf '  every "caught" and every "wrong failure" below would be unsound.\n' >&2
    exit 1
  fi
}
if [ "$ANCHORS_ONLY" -eq 0 ]; then
selftest_matcher

# Warm the cache once, and refuse to go on if the unmutated tree is not green:
# every assertion below is "this test fails", which means nothing if some test
# fails already.
printf '%schecking the unmutated tree is green%s\n' "$DIM" "$OFF"
baseline_started=$SECONDS
if ! (cd "$WORK/tree" && zig build test > "$WORK/baseline.log" 2>&1); then
  printf '%sthe tree fails its own suite before any mutation.%s\n' "$RED" "$OFF" >&2
  sed 's/^/  | /' "$WORK/baseline.log" | head -30 >&2
  exit 1
fi

# The bound every case gets, measured on this host rather than guessed. The
# baseline just run is the one COLD build of the sweep -- it compiles
# HarfBuzz, which dominates it, and every case after it is incremental.
# Twenty times that is far outside anything a mutation can legitimately cost
# and still be a build, and it scales with a slow machine instead of expiring
# on one. The floor is the other direction: a warm baseline can measure a
# couple of seconds, and twenty times almost nothing is almost nothing.
baseline_secs=$((SECONDS - baseline_started))
GUARD_DEADLINE=$((baseline_secs * 20))
if [ "$GUARD_DEADLINE" -lt 300 ]; then GUARD_DEADLINE=300; fi
printf '%sbaseline %ss; a case has %ss before it is called hung%s\n' \
  "$DIM" "$baseline_secs" "$GUARD_DEADLINE" "$OFF"
else
  printf '%schecking every anchor still applies%s\n' "$DIM" "$OFF"
fi

LOGDIR="$WORK/failures"
mkdir -p "$LOGDIR"

# A failed case has to leave its evidence behind. The build output of a
# 21-case run does not fit on a terminal, and the eight greppable lines this
# used to print are how a caught mutation was misdiagnosed once already.
report() {
  local name="$1" verdict="$2" detail="$3" output="$4"
  local slug
  slug=$(printf '%s' "$name" | tr -c 'A-Za-z0-9' '-')
  printf '%s' "$output" > "$LOGDIR/$slug.log"
  printf '%s%s%s %s\n' "$RED" "$verdict" "$OFF" "$detail"
  # Which command was asked. Each group below runs a different one, and a case
  # written under the wrong heading inherits a command that cannot see its
  # mutation -- indistinguishable, from the verdict alone, from a real hole in
  # the suite. Printing it turns that into one line instead of a log to read.
  printf '      %sran: %s%s\n' "$DIM" "${GUARD_CMD[*]}" "$OFF"
  printf '%s' "$output" | grep -E "ABI drift|^error|failed:|error:" |
    sed 's/^/      | /' | head -8
  printf '      %sfull output: %s%s\n' "$DIM" "$LOGDIR/$slug.log" "$OFF"
  FAILED=$((FAILED + 1)); FAILED_NAMES+=("$name")
}

# The one place a case's command is run, and the two properties that make it
# safe to wait for. This harness had neither, and the cost was a sweep that
# ran for nearly an hour on one mutated tree, produced no verdict, printed no
# further line, and could only be found in the process table.
#
#   The output goes to a FILE. It used to go down a pipe into a command
#   substitution, which couples the harness to the build: whoever is not
#   reading blocks whoever is writing, so a build the harness is not draining
#   can stop on a full pipe while the harness waits for output it is not
#   taking. A file has no back-pressure and no reader to deadlock against.
#
#   And the command has a DEADLINE. A mutation can make the suite hang rather
#   than fail -- it is a deliberate bug, and a bug is not obliged to terminate
#   -- and an unbounded wait is not a verdict. It is a run that never ends, on
#   a laptop or in a hosted CI job that will spend its whole budget before
#   anyone is told anything.
#
# `exec` in the subshell is load-bearing: without it the pid is the
# subshell's, and killing that leaves the build running with nothing pointing
# at it. With it the pid IS the command. The command's own children can still
# outlive it, which is why a timeout stops the sweep and keeps the working
# copy instead of pressing on.
run_guarded() {
  local outfile="$WORK/case.out" waited=0 pid
  : > "$outfile"
  ( cd "$WORK/tree" && exec "${GUARD_CMD[@]}" ) > "$outfile" 2>&1 &
  pid=$!
  RUN_TIMED_OUT=0
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$GUARD_DEADLINE" ]; then
      RUN_TIMED_OUT=1
      kill -9 "$pid" 2>/dev/null
      break
    fi
    sleep 5
    waited=$((waited + 5))
  done
  wait "$pid" 2>/dev/null
  RUN_STATUS=$?
  RUN_OUTPUT=$(tr -d '\000' < "$outfile")
}

# Both python blocks below name their encoding and turn newline translation
# off, and both halves of that are load-bearing on a source tree that is not
# pure ASCII with the platform's own line endings:
#
#   `open(path).read()` decodes with the LOCALE's codec. On a Windows console
#   that is cp1252, which cannot decode a UTF-8 continuation byte -- so a case
#   whose file holds any non-ASCII text died with a UnicodeDecodeError and was
#   reported NO ANCHOR, which reads as "the code moved, update the case". It
#   had not moved. Measured on src/integration_test.zig, whose fixtures carry
#   Hebrew and Arabic.
#
#   And text mode rewrites every '\n' as the platform's line ending, so
#   applying a one-line mutation on Windows rewrote all 724 lines of the file
#   as CRLF. The build did not care; a reader diffing the working copy against
#   the repo to see what a case actually changed very much did.
#
# case <name> <file> <expect-substring> <old> <new>
#
# `expect-substring` is matched against the build output. It must name the
# specific test or the specific compile error, so a mutation that fails the
# build for an unrelated reason is not counted as caught.
case_() {
  local name="$1" file="$2" expect="$3" old="$4" new="$5"

  if [ -n "$FILTER" ] && ! [[ "$name" =~ $FILTER ]]; then return; fi

  printf '  %-58s ' "$name"

  if [ "$ANCHORS_ONLY" -eq 1 ]; then
    if MUT_FILE="$file" MUT_OLD="$old" python3 - "$ANCHOR_TREE" <<'PY'
import io, os, sys
path = os.path.join(sys.argv[1], os.environ["MUT_FILE"])
n = io.open(path, encoding="utf-8", newline="").read().count(os.environ["MUT_OLD"])
sys.exit(0 if n == 1 else 1)
PY
    then
      printf '%sok%s %s(%s)%s\n' "$GREEN" "$OFF" "$DIM" "$file" "$OFF"
      PASSED=$((PASSED + 1))
    else
      printf '%sNO ANCHOR%s  it does not appear exactly once in %s\n' \
        "$RED" "$OFF" "$file"
      FAILED=$((FAILED + 1)); FAILED_NAMES+=("$name (anchor)")
    fi
    return
  fi

  if ! MUT_FILE="$file" MUT_OLD="$old" MUT_NEW="$new" python3 - "$WORK/tree" <<'PY'
import io, os, sys
tree = sys.argv[1]
path = os.path.join(tree, os.environ["MUT_FILE"])
old, new = os.environ["MUT_OLD"], os.environ["MUT_NEW"]
s = io.open(path, encoding="utf-8", newline="").read()
if s.count(old) != 1:
    sys.stderr.write(f"anchor appears {s.count(old)} times in {os.environ['MUT_FILE']}\n")
    sys.exit(1)
io.open(path, "w", encoding="utf-8", newline="").write(s.replace(old, new, 1))
PY
  then
    printf '%sNO ANCHOR%s  the mutation no longer applies; update it\n' "$RED" "$OFF"
    FAILED=$((FAILED + 1)); FAILED_NAMES+=("$name (anchor)")
    return
  fi

  local output status
  run_guarded
  output="$RUN_OUTPUT"
  status="$RUN_STATUS"

  cp "$file" "$WORK/tree/$file"

  # A hang is not a verdict. The sweep stops here rather than going on: the
  # killed command may have left a build of its own alive in the working copy,
  # and every case after this one would be judged on a tree that something
  # else is still writing to.
  if [ "$RUN_TIMED_OUT" -eq 1 ]; then
    KEEP_WORK=1
    report "$name" 'TIMED OUT' \
      "no verdict after ${GUARD_DEADLINE}s, so the command was killed" "$output"
    printf '\n%sthe sweep stops here.%s The command was killed; a build it\n' \
      "$RED" "$OFF" >&2
    printf 'started may not have been, so no later verdict would be sound.\n' >&2
    exit 1
  fi

  if [ $status -eq 0 ]; then
    report "$name" 'NOT CAUGHT' 'the suite passes with this bug in it' "$output"
  # A bash pattern match, NOT a pipeline. `grep -q` under `set -o pipefail`
  # exits on its first match, printf takes SIGPIPE, and the pipeline reports
  # 141 -- so a mutation that WAS caught reads as a wrong failure whenever the
  # match lands early enough in a long output for printf to still be writing.
  # That misreported a real catch as a hole in the suite.
  elif [[ "$output" == *"$expect"* ]]; then
    printf '%scaught%s %s(%s)%s\n' "$GREEN" "$OFF" "$DIM" "$expect" "$OFF"
    PASSED=$((PASSED + 1))
  # zig cuts its build output short when a test fails with a long trace: the
  # tail is replaced by "unable to read results of configure phase", and a
  # later failing test's diagnostics never appear. Measured: ONE failing test
  # with a long enough trace is sufficient, so this is not only the two-test
  # case it was first written for. A case judged on a truncated log is judged
  # on evidence that is not there, and "the expect string is absent" then
  # means nothing at all -- the same class of bug as the matcher that silently
  # failed to match. Name the state instead of calling it a wrong failure. The
  # fix is to expect the test that fails FIRST.
  # A mutation that does not COMPILE tested nothing. Four cases here were in
  # that state and read as wrong failures: each deleted the only use of a
  # variable or the only call to a function, and -Werror stopped the build on
  # the unused one before a test could run. The verdict said "expected to see:
  # <test name>", which is true and useless -- the fix is not the expect
  # string, it is to write a mutation that compiles and then misbehaves.
  #
  # A case whose expect string names a compile error is matched above and
  # never reaches this: the ABI cross-check is caught at comptime by design.
  elif [[ "$output" == *"compilation errors"* ]]; then
    report "$name" 'DID NOT COMPILE' \
      'the mutation broke the build, so no test ran; make it compile' "$output"
  elif [[ "$output" == *"unable to read results of configure phase"* ]]; then
    report "$name" 'TRUNCATED' \
      "the build output was cut short, so \"$expect\" could not be looked for" \
      "$output"
  else
    report "$name" 'WRONG FAILURE' "expected to see: $expect" "$output"
  fi
}

printf '\n%sABI cross-check%s %s(src/abi_check.zig, against ffi/ztext.h)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

case_ "enum: a MIDDLE enumerator renumbered" \
  ffi/ztext.h \
  "type Result.invalid_text is 3 in src/c.zig" \
  "  ZTEXT_RESULT_INVALID_TEXT = 3," \
  "  ZTEXT_RESULT_INVALID_TEXT = 99,"

case_ "enum: the tag narrowed on the Zig side" \
  src/c.zig \
  "type Hinting is" \
  "pub const Hinting = enum(c_int) {" \
  "pub const Hinting = enum(u8) {"

case_ "struct: two same-sized fields swapped" \
  src/c.zig \
  "FaceMetrics.descender is at byte 0 in src/c.zig" \
  "    ascender: f32,
    descender: f32," \
  "    descender: f32,
    ascender: f32,"

case_ "struct: a field added to the header only" \
  ffi/ztext.h \
  "type Extents is 24 bytes in src/c.zig" \
  "typedef struct ZtextExtents {" \
  "typedef struct ZtextExtents {
  float intruder;"

case_ "function: a by-value parameter widened" \
  src/c.zig \
  "ztextLibrarySetSdfSpread parameter 1" \
  "pub extern fn ztextLibrarySetSdfSpread(library: *Library, spread: u32) Result;" \
  "pub extern fn ztextLibrarySetSdfSpread(library: *Library, spread: u64) Result;"

case_ "function: a parameter dropped" \
  src/c.zig \
  "ztextFaceCreate takes" \
  "pub extern fn ztextFaceCreate(
    font: *Font,
    width: f32,
    height: f32,
    out: **Face,
) Result;" \
  "pub extern fn ztextFaceCreate(
    font: *Font,
    width: f32,
    out: **Face,
) Result;"

case_ "function: exported by the header, undeclared in c.zig" \
  src/c.zig \
  "never declares it" \
  "pub extern fn ztextFontStyleName(font: *const Font) [*:0]const u8;" \
  ""

printf '\n%sBidi%s %s(ffi/ztext_bidi.c)%s\n' "$BOLD" "$OFF" "$DIM" "$OFF"

# Named for the test that fails FIRST, which is not the test named after the
# property. zig replaces the tail of its own output once a failure carries a
# long trace, so a later test's name may never be printed at all -- an expect
# string that waits for it reads as TRUNCATED, which says only that the
# evidence is missing.
case_ "a line derived from the paragraph, skipping rule L1" \
  ffi/ztext_bidi.c \
  "the documented wrap loop covers a paragraph exactly once" \
  "  SBLineRef line = SBParagraphCreateLine(sb_paragraph, (SBUInteger)offset,
                                         (SBUInteger)length);" \
  "  SBLineRef line = SBParagraphCreateLine(sb_paragraph, 0u,
                                         SBParagraphGetLength(sb_paragraph));"

# The flag is never SET, rather than never consulted: dropping it from the
# expression left it unused and -Werror failed the build before a test ran.
case_ "script pieces emitted forwards inside an RTL run" \
  ffi/ztext_bidi.c \
  "shaping runs" \
  "    const bool rtl = (visual[v].level & 1u) != 0u;" \
  "    const bool rtl = false;"

case_ "the end of a paragraph left as no break at all" \
  ffi/ztext_bidi.c \
  "line breaks are offered between words, never inside one" \
  "  out[length - 1u] = (char)ZTEXT_BREAK_MANDATORY;" \
  "  (void)0;"

printf '\n%sSegmentation%s %s(what a paragraph was asked for)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# The passes are the paragraph's largest cost in both time and memory, and a
# paragraph that runs all three anyway still answers every question correctly
# -- so nothing but an explicit check on what was NOT built can see this.
case_ "every segmentation pass run whatever was asked for" \
  ffi/ztext_bidi.c \
  "a paragraph runs only the segmentation passes" \
  "  const uint32_t want = paragraph->segmentation;" \
  "  const uint32_t want = (uint32_t)ZTEXT_SEGMENTATION_ALL;"

# A bit a newer header defines and this build does not.
case_ "an unnamed segmentation bit accepted and ignored" \
  ffi/ztext_bidi.c \
  "a segmentation bit this build has no name for is refused" \
  "  if ((segmentation & ~(uint32_t)ZTEXT_SEGMENTATION_ALL) != 0u) {" \
  "  if ((segmentation & 0u) != 0u) {"

# The three arrays are packed in request order, so a pass that is present but
# read at the wrong offset answers with another pass's boundaries.
case_ "the word array laid over the line array" \
  ffi/ztext_bidi.c \
  "line breaks are offered between words, never inside one" \
  "    paragraph->word_breaks = next;" \
  "    paragraph->word_breaks = paragraph->breaks;"

printf '\n%sFaces and fonts%s %s(ffi/ztext_face.c, ffi/ztext_raster.c)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

case_ "a face loads without activating its own FT_Size" \
  ffi/ztext_raster.c \
  "share the parse but not the size" \
  "  ztextFaceActivate(face);

  const FT_Error error = FT_Load_Glyph(face->font->ft, (FT_UInt)glyph_id," \
  "  const FT_Error error = FT_Load_Glyph(face->font->ft, (FT_UInt)glyph_id,"

# isMark stops recognising marks, rather than the caller stopping asking. The
# older mutation deleted the call, which left isMark unused and failed the
# build on -Werror instead of failing a test.
case_ "a covered prefix that splits a base from its marks" \
  ffi/ztext_face.c \
  "never splits a base from its marks" \
  "    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
      return true;" \
  "    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
      return false;"

case_ "a covered prefix that breaks at a format character" \
  ffi/ztext_face.c \
  "format characters never break a run" \
  "  if (cp >= 0xFE00u && cp <= 0xFE0Fu) return true;" \
  "  (void)unicode;
  if (cp == 0u) return true;
  return false;
  if (cp >= 0xFE00u && cp <= 0xFE0Fu) return true;"

# A8 coverage and an SDF are both one byte per pixel, so a bitmap labelled
# with the wrong one produces a washed-out picture rather than an error.
#
# The label is wrong INSIDE the function that decides it, not at the call site.
# Replacing the call left bitmapFormatOf uncalled, and -Werror stopped the
# build on the unused function before a test could run: a mutation that does
# not compile proves nothing about the suite.
case_ "a bitmap that does not say which format it is" \
  ffi/ztext_raster.c \
  "says which format its bytes are in" \
  "    case ZTEXT_RENDER_MODE_SDF:
      return ZTEXT_BITMAP_FORMAT_SDF;" \
  "    case ZTEXT_RENDER_MODE_SDF:
      return ZTEXT_BITMAP_FORMAT_A8;"

case_ "a pixel size rounded to whole pixels" \
  ffi/ztext_core.c \
  "fractional pixel size is honoured" \
  "  return (int32_t)(pixels * 64.0f + 0.5f);" \
  "  return (int32_t)(pixels + 0.5f) * 64;"

# The library's half of the order-free bargain. Take the hand-back away and
# nothing crashes and no order is wrong -- the library is simply never freed,
# in every order, which the suite's allocator and the C balance test both
# report. `leaked` rather than a test name because both of them say it and
# either is enough.
case_ "a font released without telling the library that owns it" \
  ffi/ztext_face.c \
  "leaked" \
  "  library->live_fonts -= 1u;
  releaseLibrary(library);" \
  "  (void)library;"

# The other half of the allocator seam: which allocator MAKES a block. A face's
# glyph buffer is allocated lazily, long after the face, so charging it to
# whatever is installed at that moment splits one handle across two heaps --
# with no crash, no leak, and nothing but a host's own accounting to notice.
case_ "a glyph buffer charged to whatever is installed" \
  ffi/ztext_raster.c \
  "belongs to its library" \
  "ztextAllocatorOf(face)" \
  "ZTEXT_ALLOCATOR_ANY"

printf '\n%sHinting%s %s(ffi/ztext_ftoption.h, and the warm-up it needs)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# The autohinter's coverage comes from GSUB only because ztext_ftoption.h says
# so. Take the macro away and FreeType falls back to
# af_shaper_get_coverage_nohb, which can only walk the character map: every
# glyph shaping produces loses its script and is hinted against no blue zones
# at all. Nothing fails to compile, nothing errors, and the picture changes --
# which is why a golden is the only thing that can hold it.
case_ "the autohinter's coverage taken from the cmap alone" \
  ffi/ztext_ftoption.h \
  "the autohinter's coverage comes from GSUB" \
  "#ifndef FT_CONFIG_OPTION_USE_HARFBUZZ
#define FT_CONFIG_OPTION_USE_HARFBUZZ
#endif
" \
  ""

# And the allocation that coverage pass makes. hb_language_get_default is
# reached by hinting and by nothing else here, so warm-up is the only thing
# that keeps it off a host's tracking allocator. The C smoke test hints with
# the autohinter for exactly this reason, and then counts.
case_ "the language the autohinter interns, left cold" \
  ffi/ztext_shape.c \
  "blocks leaked" \
  "  (void)hb_language_get_default();" \
  ""

printf '\n%sFreeType build switches%s %s(what the options file claims)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# ffi/ztext_ftoption.h is a page of prose about macros, and every paragraph of
# it is a claim about what is in the binary. One was false for the life of the
# package: the resource-fork guessing heuristics were described as dropped and
# were compiled into every build. ffi/ztext_abi.c now turns each of the six
# switches into a #error, so the claim and the binary cannot part again --
# which makes a COMPILE failure the verdict here, not a failing test.
case_ "a FreeType switch the options file says is off" \
  ffi/ztext_ftoption.h \
  "without FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK" \
  "#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK" \
  ""

printf '\n%sSubpixel rasterisation%s %s(pixels, samples and rows)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# An LCD bitmap has three numbers that all sound like "how wide is it": the
# pixel count, the sample count, and the byte pitch. Confusing any two of them
# lays the glyph out three times too wide or too tall, with no error anywhere.

case_ "an LCD bitmap's width reported in samples" \
  ffi/ztext_raster.c \
  "an LCD bitmap is three samples per pixel" \
  "  out->width = (mode == ZTEXT_RENDER_MODE_LCD) ? slot->bitmap.width / channels
                                               : slot->bitmap.width;" \
  "  out->width = slot->bitmap.width;"

case_ "an LCD_V bitmap's height reported in sub-rows" \
  ffi/ztext_raster.c \
  "an LCD_V bitmap is three sub-rows per pixel row" \
  "  out->height = (mode == ZTEXT_RENDER_MODE_LCD_V) ? slot->bitmap.rows / channels
                                                  : slot->bitmap.rows;" \
  "  out->height = slot->bitmap.rows;"

# pitch is bytes per PIXEL row, which for LCD_V is three of FreeType's rows.
# Left untripled, pitch * height addresses a third of the buffer.
case_ "an LCD_V pitch that counts one of its three sub-rows" \
  ffi/ztext_raster.c \
  "an LCD_V bitmap is three sub-rows per pixel row" \
  "  out->pitch = (int32_t)(slot->bitmap.width *
                         ((mode == ZTEXT_RENDER_MODE_LCD_V) ? channels : 1u));" \
  "  out->pitch = (int32_t)slot->bitmap.width;"

# The mode asked for and the mode rendered are two different values, and the
# format field describes the first. Rendered greyscale and labelled LCD, the
# consumer reads one byte per pixel as three.
case_ "a subpixel mode rendered as plain greyscale" \
  ffi/ztext_raster.c \
  "an LCD bitmap is three samples per pixel" \
  "    case ZTEXT_RENDER_MODE_LCD:
      return FT_RENDER_MODE_LCD;" \
  "    case ZTEXT_RENDER_MODE_LCD:
      return FT_RENDER_MODE_NORMAL;"

# The channel count is what a consumer sizes its upload with.
case_ "three bytes per pixel counted as one" \
  ffi/ztext_raster.c \
  "an LCD bitmap is three samples per pixel" \
  "    case ZTEXT_BITMAP_FORMAT_LCD:
    case ZTEXT_BITMAP_FORMAT_LCD_V:
      return 3u;" \
  "    case ZTEXT_BITMAP_FORMAT_LCD:
    case ZTEXT_BITMAP_FORMAT_LCD_V:
      return 1u;"

printf '\n%sProcess-wide state%s %s(what two threads may touch at once)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# The compiler is the gate here, and it is the only kind a data race admits:
# no run proves the absence of one, and a run that happens to survive is the
# most misleading evidence there is. An atomic operation on a plain object is
# a constraint violation rather than a slower program, so removing the
# qualifier stops the build instead of quietly restoring the bug.
case_ "the generation counter made non-atomic" \
  ffi/ztext_core.c \
  "pointer to _Atomic" \
  "  static _Atomic uint64_t counter = 0u;" \
  "  static uint64_t counter = 0u;"

case_ "the one-time install flag made non-atomic" \
  ffi/ztext_core.c \
  "pointer to _Atomic" \
  "static _Atomic int g_sb_install_state = 0;" \
  "static int g_sb_install_state = 0;"

printf '\n%sThe internal contracts%s %s(what no public call can reach)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# The bound covers all three encodings from one place, and no public call can
# reach it: both of ztextTextDecode's callers are in bounds by construction,
# which is exactly why the read past the end survived. tests/c_internal.c is
# the caller that can.
case_ "the decoder reading the unit at the end" \
  ffi/ztext_core.c \
  "decode at index == length returns 1" \
  "  if (index >= length) {
    *out = 0xFFFDu;
    return 1u;
  }

  switch (encoding) {" \
  "  switch (encoding) {"

# Written as two negations because a NaN fails every comparison: the range
# test that reads more naturally lets one through.
case_ "the 26.6 domain written as a range test" \
  ffi/ztext_core.c \
  "a NaN is refused" \
  "  if (!(pixels > 0.0f) || !(pixels <= 16384.0f)) return 0;" \
  "  if (pixels <= 0.0f || pixels > 16384.0f) return 0;"

printf '\n%sThe ABI handshake%s %s(what proves the two sides agree)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# ZtextAbiLayout's fields are checked from their own names, so a field cannot
# be added without a check. Point one at the wrong type and the check fails --
# which is what proves it is a check and not a transcription.
case_ "a layout field measuring the wrong type" \
  ffi/ztext_abi.c \
  "ZtextAbiLayout.matrix_size" \
  "  out->matrix_size = (uint32_t)sizeof(ZtextMatrix);" \
  "  out->matrix_size = (uint32_t)sizeof(ZtextCharmap);"

# The probe's markers are what catch two same-typed fields transposed. A field
# the C side never writes is a field a transposition could hide in.
case_ "a probed field the library never writes" \
  ffi/ztext_abi.c \
  "ztextAbiProbe never writes it" \
  "  out->matrix.yx = 903.25f;" \
  ""

# Two fields of one type expecting the same marker makes them
# indistinguishable, so a real transposition between them would read back
# correct. It is a property of the TABLE rather than of a run, which is why it
# is a compile error: a check that only a well-formed table can satisfy has to
# reject a malformed one before anything is measured with it.
case_ "two probed fields of one type expecting one marker" \
  src/ztext.zig \
  "share the marker" \
  '    .{ "variation.tag", markerOf(@as(u32, 0x808)) },' \
  '    .{ "variation.tag", markerOf(@as(u32, 0x804)) },'

# And an expectation of zero, which cannot tell a field the library wrote from
# one it never touched.
case_ "a probed field expecting a marker of zero" \
  src/ztext.zig \
  "which is also what an untouched field holds" \
  '    .{ "matrix.yx", markerOf(@as(f32, 903.25)) },' \
  '    .{ "matrix.yx", markerOf(@as(f32, 0.0)) },'

printf '\n%sThe stroker%s %s(where the pen goes, and what it grows)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# Composition order. A pen traced before the synthetic styles is widened by
# them; a pen traced after the caller's matrix is a pen in device space. Both
# still draw an outlined glyph, so only a measurement can tell them apart.
case_ "the pen traced before the styles it should follow" \
  ffi/ztext_raster.c \
  "the pen traces the styled glyph" \
  "  const bool synthesised = applySynthetic(face, slot);
  bool stroked = false;
  const ZtextResult stroke_result = applyStroke(face, slot, &stroked);
  if (stroke_result != ZTEXT_RESULT_OK) return stroke_result;" \
  "  bool stroked = false;
  const ZtextResult stroke_result = applyStroke(face, slot, &stroked);
  if (stroke_result != ZTEXT_RESULT_OK) return stroke_result;
  const bool synthesised = applySynthetic(face, slot);"

case_ "the pen traced after the matrix instead of before it" \
  ffi/ztext_raster.c \
  "the matrix maps what it traced" \
  "  if (stroke_result != ZTEXT_RESULT_OK) return stroke_result;
  const bool transformed = applyTransform(face, slot);" \
  "  if (stroke_result != ZTEXT_RESULT_OK) return stroke_result;
  const bool transformed = false;
  applyTransform(face, slot);"

# A stroked glyph is a different SHAPE, so the ink metrics the extents are
# read from have to be recomputed -- the same reason the other two steps do.
case_ "a pen that reaches the pixels but not the measurements" \
  ffi/ztext_raster.c \
  "grows the glyph by 2R" \
  "  if (synthesised || stroked || transformed) refreshInkMetrics(slot);" \
  "  if (synthesised || transformed) refreshInkMetrics(slot);"

# The outline the stroker exports into is sized for the LARGEST glyph so far.
# Sized for the last one instead, a wide glyph after a narrow one overflows
# or is truncated -- and FreeType writes as many points as it counted.
case_ "an export buffer that is never grown after the first glyph" \
  ffi/ztext_raster.c \
  "grows to the widest of them" \
  "  if (points <= face->stroked_points && contours <= face->stroked_contours) {
    return ZTEXT_RESULT_OK;
  }" \
  "  if (face->stroked_points != 0u) {
    return ZTEXT_RESULT_OK;
  }"

# Which side of a contour is "outside" is the font format's business. Swapped,
# an inside rule grows the letter and an outline eats into it -- and both
# still produce a plausible picture.
case_ "the inside and outside borders swapped" \
  ffi/ztext_raster.c \
  "the glyph grown, shrunk, and the band between them" \
  "    border = face->stroke.style == ZTEXT_STROKE_STYLE_SHRUNK
                 ? FT_Outline_GetInsideBorder(&slot->outline)
                 : FT_Outline_GetOutsideBorder(&slot->outline);" \
  "    border = face->stroke.style == ZTEXT_STROKE_STYLE_SHRUNK
                 ? FT_Outline_GetOutsideBorder(&slot->outline)
                 : FT_Outline_GetInsideBorder(&slot->outline);"

# The BAND keeps both contours; either solid style keeps one. Exporting both
# for all three collapses the styles into one.
case_ "every stroke style exporting both of the stroker's contours" \
  ffi/ztext_raster.c \
  "the glyph grown, shrunk, and the band between them" \
  "  const bool whole = face->stroke.style == ZTEXT_STROKE_STYLE_BAND;" \
  "  const bool whole = true;"

# A cap, join or style this build does not name comes from a consumer built
# against a newer header. Drawing something else instead is the silent
# failure this package exists to refuse.
case_ "a pen naming a cap this build does not have, accepted" \
  ffi/ztext_raster.c \
  "does not name should be refused" \
  "    if (cap < (int)ZTEXT_LINE_CAP_BUTT || cap > (int)ZTEXT_LINE_CAP_SQUARE ||" \
  "    if (0 ||"

# A radius too large for 26.6 converts to zero, which draws no pen at all.
case_ "a radius too large for the fixed point it is converted to" \
  ffi/ztext_raster.c \
  "a pen that cannot be drawn is refused" \
  "    if (stroke->radius > 0.0f && ztextToFixed266(stroke->radius) == 0) {" \
  "    if (0) {"

printf '\n%sThe face transform%s %s(what a matrix reaches)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# The caller's 2x2 maps the glyph IMAGE, after the synthetic styles and
# reaching no advance. Each of those three is a separate way to be wrong, and
# none of them errors.

case_ "the caller's matrix applied before the font's own styles" \
  ffi/ztext_raster.c \
  "composes after the synthetic styles" \
  "  const bool synthesised = applySynthetic(face, slot);
  bool stroked = false;
  const ZtextResult stroke_result = applyStroke(face, slot, &stroked);
  if (stroke_result != ZTEXT_RESULT_OK) return stroke_result;
  const bool transformed = applyTransform(face, slot);" \
  "  const bool transformed = applyTransform(face, slot);
  const bool synthesised = applySynthetic(face, slot);
  bool stroked = false;
  const ZtextResult stroke_result = applyStroke(face, slot, &stroked);
  if (stroke_result != ZTEXT_RESULT_OK) return stroke_result;"

# xy and yx are indistinguishable under any diagonal matrix, which is why a
# shear is in the test.
case_ "the two off-diagonal terms of the matrix swapped" \
  ffi/ztext_raster.c \
  "maps the ink and leaves every advance in text space" \
  "  matrix.xy = toFixed16(face->transform.xy);
  matrix.yx = toFixed16(face->transform.yx);" \
  "  matrix.xy = toFixed16(face->transform.yx);
  matrix.yx = toFixed16(face->transform.xy);"

# FreeType's own FT_Set_Transform does exactly this. It cannot be done here:
# a shaped run's advances come from HarfBuzz, which has no matrix, so one side
# would move and the other would not.
case_ "the advance transformed too, the way FT_Set_Transform does" \
  ffi/ztext_raster.c \
  "leaves every advance in text space" \
  "  FT_Outline_Transform(&slot->outline, &matrix);
  return true;" \
  "  FT_Outline_Transform(&slot->outline, &matrix);
  FT_Vector_Transform(&slot->advance, &matrix);
  slot->metrics.horiAdvance = slot->advance.x;
  return true;"

# The one field of a face whose zero is not its default.
case_ "a face whose matrix starts at the memset's zero" \
  ffi/ztext_face.c \
  "created with the identity" \
  "  face->transform.xx = 1.0f;
  face->transform.yy = 1.0f;" \
  ""

case_ "a matrix that is not made of numbers, taken at face value" \
  ffi/ztext_raster.c \
  "is not made of numbers is refused" \
  "    if (!isFiniteStrength(matrix->xx) || !isFiniteStrength(matrix->xy) ||
        !isFiniteStrength(matrix->yx) || !isFiniteStrength(matrix->yy)) {
      return ZTEXT_RESULT_INVALID_ARGUMENT;
    }" \
  ""

printf '\n%sCharacter maps%s %s(which one is selected)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# A font may carry several character maps, and which one is selected decides
# what every lookup answers. Each mutation here leaves a plausible answer in
# place of the right one.

case_ "every map reported as the first one" \
  ffi/ztext_face.c \
  "reaches glyphs no Unicode character maps to" \
  "  const FT_CharMap map = font->ft->charmaps[index];" \
  "  const FT_CharMap map = font->ft->charmaps[0];"

case_ "whichever map is selected reported as the first" \
  ffi/ztext_face.c \
  "reaches glyphs no Unicode character maps to" \
  "  const FT_Int index = FT_Get_Charmap_Index(font->ft->charmap);
  return index < 0 ? ZTEXT_CHARMAP_INDEX_NONE : (uint32_t)index;" \
  "  return 0u;"

# The refusal is the answer to \"does this font have a symbol map\". Accept
# silently and a caller believes it selected one.
case_ "an encoding this font has no map for, accepted" \
  ffi/ztext_face.c \
  "lists its character maps" \
  "  const FT_Error error =
      FT_Select_Charmap(font->ft, (FT_Encoding)encoding);
  return ztextFromFtError(error);" \
  "  (void)FT_Select_Charmap(font->ft, (FT_Encoding)encoding);
  return ZTEXT_RESULT_OK;"

case_ "a charmap index past the end quietly clamped" \
  ffi/ztext_face.c \
  "lists its character maps" \
  "  if (font == NULL || index >= (uint32_t)font->ft->num_charmaps) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }
  const FT_CharMap map = font->ft->charmaps[index];" \
  "  if (font == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (index >= (uint32_t)font->ft->num_charmaps) {
    index = (uint32_t)font->ft->num_charmaps - 1u;
  }
  const FT_CharMap map = font->ft->charmaps[index];"

printf '\n%sSynthetic styles%s %s(two upstreams, one weight)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# A synthetic style has to reach BOTH upstreams or the picture disagrees with
# itself: FreeType widens the ink, HarfBuzz widens the advance, and a run laid
# out with one and not the other overlaps its own glyphs. Every case here is a
# way for exactly half of that to happen -- no crash, no error, just text that
# is a little too tight.

# The setter widens the ink and forgets to tell HarfBuzz. This is the defect
# the API used to document as a limitation.
case_ "a style applied to the ink and not to the shaping" \
  ffi/ztext_raster.c \
  "widens a shaped run's advances" \
  "  face->generation = ztextNextGeneration();
  ztextFaceApplySynthetic(face);
  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFaceSetSyntheticOblique(ZtextFace* face, float slant) {" \
  "  face->generation = ztextNextGeneration();
  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFaceSetSyntheticOblique(ZtextFace* face, float slant) {"

# HarfBuzz's in-place mode is font GRADING: the ink thickens and the advance
# does not move. It is a real effect with a real name, and it is not this one.
case_ "emboldening asked for in place, so the advance never moves" \
  ffi/ztext_face.c \
  "widens a shaped run's advances" \
  "  hb_font_set_synthetic_bold(face->hb_font, face->synthetic_bold,
                             face->synthetic_bold, false);" \
  "  hb_font_set_synthetic_bold(face->hb_font, face->synthetic_bold,
                             face->synthetic_bold, true);"

# The FreeType-backed HarfBuzz font is built lazily, on the first shape that
# asks for FreeType metrics -- which can be long after the style was set. Drop
# the hand-over and one of the two metric sources silently keeps the unstyled
# widths while the other does not.
case_ "the lazily built font never told what style it was born into" \
  ffi/ztext_shape.c \
  "before the FreeType metrics font exists" \
  "  face->hb_ft_font = font;
  // The style may have been set long before this font existed.
  ztextFaceApplySynthetic(face);" \
  "  face->hb_ft_font = font;"

# Shaped advances move with the strength now, so a run shaped before the
# change is as stale as one shaped before a resize. Without the bump,
# ztextShaperExtents mixes ink from one weight with advances from another.
case_ "a restyled face that still passes for the one a run was shaped against" \
  ffi/ztext_raster.c \
  "ages a run shaped before it" \
  "  face->synthetic_bold = strength;
  // Shaped advances move with this now, so a run measured against this face
  // before the change is as stale as one measured before a resize.
  face->generation = ztextNextGeneration();" \
  "  face->synthetic_bold = strength;"

# The strength is a number the caller chooses, not a flag with one value.
# Quantise it back to the reference weight and every call still succeeds --
# a display face asked for three times the weight simply does not get it.
case_ "a strength quantised back to the one weight upstream ships" \
  ffi/ztext_raster.c \
  "is a strength the caller chooses" \
  "  const double scaled = (double)ppem * 64.0 * (double)strength;" \
  "  const double reference = strength == 0.0f ? 0.0 : (strength > 0.0f ? 0.041656494 : -0.041656494);
  const double scaled = (double)ppem * 64.0 * reference;"

# A NaN reaches FreeType's fixed-point conversion as an undefined cast and
# HarfBuzz's roundf as a NaN advance; neither reports anything.
case_ "a strength that is not a number, taken at face value" \
  ffi/ztext_raster.c \
  "is not a number is refused" \
  "  if (face == NULL || !isFiniteStrength(strength)) {" \
  "  if (face == NULL) {"

printf '\n%sEncodings%s %s(three upstreams, three seams)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# SheenBidi takes the encoding as a field of the sequence it is handed. Pin it
# to UTF-8 and a UTF-16 paragraph is analysed as if its high bytes were
# characters: no crash, no error, just the wrong embedding levels -- which is
# the whole reason the gate is a differential test rather than a golden.
case_ "SheenBidi told the text is UTF-8 whatever it is" \
  ffi/ztext_bidi.c \
  "one text, three encodings" \
  "  sequence.stringEncoding = (SBStringEncoding)encoding;" \
  "  sequence.stringEncoding = SBStringEncodingUTF8;"

# libunibreak has three entry points per algorithm and ztext pairs them up in
# one switch. Cross one pair and the line breaks of a UTF-16 paragraph are
# computed over its bytes.
case_ "libunibreak given UTF-16 through its UTF-8 entry point" \
  ffi/ztext_bidi.c \
  "one text, three encodings" \
  "      if (lines != NULL) set_linebreaks_utf16(text, length, NULL, lines);" \
  "      if (lines != NULL) set_linebreaks_utf8((const utf8_t*)text, length, NULL, lines);"

# The same seam again, in HarfBuzz. Shaping UTF-16 through hb_buffer_add_utf8
# produces glyphs -- for a text nobody wrote.
case_ "HarfBuzz handed UTF-16 as UTF-8" \
  ffi/ztext_shape.c \
  "one text, three encodings" \
  "      hb_buffer_add_utf16(buffer, (const uint16_t*)text, (int)length,
                          (unsigned int)run_offset, (int)run_length);" \
  "      hb_buffer_add_utf8(buffer, (const char*)text, (int)length,
                         (unsigned int)run_offset, (int)run_length);"

# And the half of an encoding that is not an upstream's business: which
# indices are inside a character. Say none are, and a line may start on the
# second half of a surrogate pair.
case_ "a UTF-16 surrogate pair treated as two characters" \
  ffi/ztext_core.c \
  "a range that would split a character is refused" \
  "      return unit >= 0xDC00u && unit <= 0xDFFFu;" \
  "      return false;"

printf '\n%sShaping%s %s(ffi/ztext_shape.c)%s\n' "$BOLD" "$OFF" "$DIM" "$OFF"

case_ "a run shaped without the text around it" \
  ffi/ztext_shape.c \
  "shaping a run with context matches" \
  "      hb_buffer_add_utf8(buffer, (const char*)text, (int)length,
                         (unsigned int)run_offset, (int)run_length);" \
  "      hb_buffer_add_utf8(buffer, (const char*)text + run_offset,
                         (int)run_length, 0u, (int)run_length);"

# HarfBuzz produces unsafe-to-break whether or not it is asked, and withholds
# the other two unless told. Stop asking, and the flags a line-breaker acts on
# quietly become "not computed" -- which is indistinguishable from "not set"
# at the call site, and reads as "safe" to anyone who does not know.
case_ "the optional glyph flags never asked for" \
  ffi/ztext_shape.c \
  "reports where a line may be broken" \
  "  hb_buffer_set_flags(buffer,
                      (hb_buffer_flags_t)(
                          HB_BUFFER_FLAG_PRODUCE_UNSAFE_TO_CONCAT |
                          HB_BUFFER_FLAG_PRODUCE_SAFE_TO_INSERT_TATWEEL));" \
  "  (void)0;"

case_ "extents taken from a face the run was not shaped against" \
  ffi/ztext_shape.c \
  "extents refuse a face" \
  "  if (face->generation != shaper->face_generation) {" \
  "  if (false) {"

case_ "a rejected shape leaves the previous run queryable" \
  ffi/ztext_shape.c \
  "does not leave the previous run queryable" \
  "  shaper->shaped = false;
  shaper->glyphs.count = 0u;" \
  "  // shaper->shaped = false;
  // shaper->glyphs.count = 0u;"

case_ "a paragraph run shaped left to right whatever its level" \
  ffi/ztext_shape.c \
  "a paragraph run is shaped from the paragraph" \
  "                   (run->level % 2u == 0u) ? ZTEXT_DIRECTION_LTR
                                           : ZTEXT_DIRECTION_RTL," \
  "                   ZTEXT_DIRECTION_LTR,"

case_ "a run's direction and the caller's, both accepted" \
  ffi/ztext_shape.c \
  "a paragraph run refuses a direction or script" \
  "  if (params->direction != ZTEXT_DIRECTION_AUTO || params->script != 0u) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }" \
  "  if (params->direction != ZTEXT_DIRECTION_AUTO && params->script != 0u) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }"

case_ "a hand-built run trusted about its own bounds" \
  ffi/ztext_shape.c \
  "a run built by hand cannot reach outside its paragraph" \
  "  if (!rangeIsUsable(paragraph->text, paragraph->length, paragraph->encoding,
                     run->offset, run->length)) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }" \
  "  (void)0;"

printf '\n%sAllocator%s %s(the seam, both sides of it)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# Named, like every other case here. It asserted the substring "error" once,
# which any build failure satisfies — including one caused by a typo in the
# mutation itself, which would have been counted as the guard working.
case_ "a declined reallocate reported as out of memory" \
  ffi/ztext_core.c \
  "a rendered bitmap survives anything but the next render on its own face" \
  "  void* fresh = ztextAllocFrom(owner, new_size, backing);" \
  "  if (allocator->reallocate != NULL) return NULL;
  void* fresh = ztextAllocFrom(owner, new_size, backing);"

# The block header records WHICH allocator issued a block, and every free is
# routed back to that entry. Route it to whatever is installed instead -- the
# behaviour before the registry existed -- and a handle created under one
# allocator and destroyed under another goes to the wrong heap.
case_ "a block freed through whatever is installed now" \
  ffi/ztext_core.c \
  "never returned to the creating allocator" \
  "  const ZtextAllocator* allocator = g_registry[header->allocator];
  const size_t total = header->total_size;" \
  "  const ZtextAllocator* allocator = g_registry[g_installed];
  const size_t total = header->total_size;"

# The other half: the check that says a library-owned block really is the
# library's. Free a font through the default allocator by name and the
# mismatch has to stop the process rather than corrupt a heap.
case_ "a library-owned block released by the wrong allocator" \
  ffi/ztext_face.c \
  "released through the wrong allocator" \
  "  if (font->ft != NULL) FT_Done_Face(font->ft);
  ztextFreeFrom(library->allocator, font);" \
  "  if (font->ft != NULL) FT_Done_Face(font->ft);
  ztextFreeFrom(ZTEXT_ALLOCATOR_DEFAULT, font);"

# HarfBuzz's process-lifetime singletons are never freed in this build --
# hb_atexit expands to nothing without HAVE_ATEXIT -- so a host that audits its
# heap only balances if they were populated before its allocator went in.
# ztextSetAllocator is the one place that does that, and no test warms up by
# hand any more, so deleting it there deletes it everywhere: the C boundary's
# own accounting says so.
case_ "the process-lifetime caches left unwarmed" \
  ffi/ztext_core.c \
  "blocks leaked" \
  "  ztextWarmup();" \
  ""

# A slot outlives every block its allocator issued, so slots are never
# freed -- which makes one slot per install a leak with a slow fuse and no
# allocator left to report it. A host that installs per frame, or a suite
# that installs per test, is exactly the shape that finds it late.
case_ "an allocator slot per install rather than per allocator" \
  src/memory.zig \
  "installing the same allocator twice reuses its slot" \
  "    for (slots.items) |slot| {
        if (slot.ptr == gpa.ptr and slot.vtable == gpa.vtable) return slot;
    }" \
  ""

# SheenBidi 3.0.0 reads a field it has not written on its own
# allocation-failure path, and ztext's seam zeroes every block it hands over
# so that read finds NULL. Remove the memset and the poisoned injection arm
# dies -- every run, at the same injection point, rather than one run in fifty.
case_ "SheenBidi handed memory ztext did not write" \
  ffi/ztext_core.c \
  "during phase: injection-poisoned" \
  "  if (block != NULL) memset(block, 0, (size_t)size);" \
  "  (void)0;"

printf '\n%sDocumentation%s %s(the examples are one text)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# README.md and src/ztext.zig quote examples/quickstart.zig, which the build
# compiles and runs. Put the old bug back into the README copy -- shaping a
# SLICE of the text instead of the run in place -- and a named test has to say
# the document no longer quotes the program. It went the other way once: the
# README was corrected and the module doc kept the bug for as long as nobody
# read both.
case_ "a documented example edited away from the program" \
  README.md \
  "README.md quotes the usage example verbatim" \
  '    // text a paragraph already validated is not validated again per run.
    const glyphs = try shaper.shapeRun(face, paragraph, run, .{});' \
  '    // text a paragraph already validated is not validated again per run.
    const glyphs = try shaper.shape(face, text[run.offset..][0..run.length], .{});'

printf '\n%sReproducibility%s %s(the environment must not reach the picture)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# HarfBuzz reads HB_SHAPER_LIST, HB_FONT_FUNCS and HB_FACE_LOADER. build.zig
# compiles it with -DHB_NO_GETENV so all three read empty, and runs the suite
# a second time with them set to hostile values. Remove the define and the
# second pass shapes something else.
case_ "the environment allowed to reach HarfBuzz" \
  build.zig \
  "golden: Latin applies standard ligatures" \
  '    "-DHB_NO_GETENV",
' \
  ''

printf '\n%sOpenType metrics%s %s(ffi/ztext_face.c)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# ZtextMetric's enumerators are OpenType tags, not an ordinal range, so there is
# no bounds check that a caller casting an integer in would trip. Drop the
# lookup against ZTEXT_METRIC_LIST and an unnamed tag reaches HarfBuzz, which
# answers "this font does not have it" -- a plausible-looking UNSUPPORTED for a
# metric that does not exist.
case_ "a metric tag nobody vetted, forwarded to HarfBuzz" \
  ffi/ztext_face.c \
  "should be refused, not forwarded" \
  "#undef ZTEXT_METRIC_CASE
    return true;
  default:
    return false;
  }" \
  "#undef ZTEXT_METRIC_CASE
    return true;
  default:
    return true;
  }"

printf '\n%sVariable fonts%s %s(ffi/ztext_face.c)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# A named instance is a point in the axis space that nothing else can derive.
# Hand back zeros instead and a picker offers "Condensed Light" and applies the
# default instance, with every axis reading as its minimum.
case_ "named instance coordinates that are not the font's" \
  ffi/ztext_face.c \
  "named instances are the points" \
  "    values[i] = fixedToDesign(style->coords[i]);" \
  "    values[i] = 0.0f;"

# HarfBuzz returns the name's length excluding its NUL and ztext passes that
# on. Count the NUL and every caller that slices by the returned length carries
# a trailing zero byte into whatever it draws.
case_ "an instance name reported one byte longer than it is" \
  ffi/ztext_face.c \
  "named instances are the points" \
  "  *size = (size_t)room;" \
  "  *size = (size_t)room + 1u;"

printf '\n%sVariation sequences%s %s(ffi/ztext_face.c, and the fixture that reaches it)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# The whole point of the entry point is the selector. Answer with the base
# character alone and a coverage check reports coverage for a sequence the font
# has never heard of -- which is exactly the bug this exists to prevent.
case_ "a variation selector ignored, the base answered" \
  ffi/ztext_face.c \
  "a variation sequence names its own glyph" \
  "  return (uint32_t)FT_Face_GetCharVariantIndex(
      font->ft, (FT_ULong)codepoint, (FT_ULong)variation_selector);" \
  "  return (uint32_t)FT_Get_Char_Index(font->ft, (FT_ULong)codepoint);"

# Not a guard on the library but on the FIXTURE, and it earns its place: the
# comment beside this sort claims that leaving the encoding records unsorted
# makes FreeType adopt the format-14 subtable as the face's default charmap,
# after which every ordinary lookup returns .notdef. That claim is worth a
# gate, because a fixture that is subtly wrong makes the test above prove
# nothing.
case_ "the fixture's cmap records left unsorted" \
  tests/fonts.zig \
  "a variation sequence names its own glyph" \
  "    std.mem.sort(Record, records.items, {}, Record.before);" \
  "    if (records.items.len == 0) return error.NoCmapTable;"

printf '\n%sVersioning and licences%s %s(ci/measurements.sh, not the suite)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# ztext's version is written in three files, and CHANGELOG.md states what a
# bump of each position promises. The suite cannot see any of that: a build
# whose manifest says 0.2.0 and whose header says 0.3.0 compiles, links, and
# passes every test.
GUARD_CMD=(ci/measurements.sh --check)

case_ "a version bump that reached three of its four homes" \
  build.zig.zon \
  "version homes disagree" \
  '.version = "0.2.0",' \
  '.version = "0.3.0",'

# The other half, and the reason the comparison rejects an empty value rather
# than comparing two blanks: a heading reworded stops the grep matching, and a
# check that silently stops checking is worse than no check.
case_ "the changelog heading the gate reads by shape" \
  CHANGELOG.md \
  "version homes disagree" \
  '## 0.2.0' \
  '## v0.2.0'

# A number README states and a script recomputes, written a second time in
# another document. Both copies read as current until one of them is not, and
# the reader has no way to tell which. This is the same defect the version
# homes above have, in the documents rather than in the manifest.
case_ "a gated number restated in another document" \
  SECURITY.md \
  "stated a second time" \
  '  every one of them, checked against the header by' \
  '  all 88 of them, checked against the header by'

# What a consumer receives, as opposed to what the checkout contains. Nothing
# in the repository can notice a missing `paths` entry: every file is present
# locally, and tests/consumer resolves ztext by path rather than by fetch.
case_ "a directory build.zig compiles, dropped from the package" \
  build.zig.zon \
  "not in paths" \
  '        "examples",' \
  '        "exampels",'

# LICENSES.md's legal statements were written against specific bytes under
# libs/. A re-vendor that changes a licence leaves ci/verify-vendor.sh green --
# the tree still matches its new pinned upstream -- and the document goes on
# describing terms that are no longer there. Same GUARD_CMD: the suite has no
# view of any of this either.
case_ "a licence text changed under the page that summarises it" \
  LICENSES.md \
  "LICENSES.md says" \
  '`ba8f810f2455c2f08e2d56bb49b72f37fcf68f1f4fade38977cfd7372050ad64`' \
  '`0a8f810f2455c2f08e2d56bb49b72f37fcf68f1f4fade38977cfd7372050ad64`'

# And the other way out: delete the row instead of fixing it. A document that
# stops claiming anything must not become a document that passes.
case_ "a licence row deleted rather than rechecked" \
  LICENSES.md \
  "the table has been emptied" \
  '| `libs/harfbuzz/src/ms-use/COPYING` | `c2cfccb812fe482101a8f04597dfc5a9991a6b2748266c47ac91b6a5aae15383` |
' \
  ''

# LICENSES.md's "Reaches your binary?" answer for FreeType's autofit-HarfBuzz
# files is decided by one macro in ffi/ztext_ftoption.h. Flip the cell and the
# page tells a consumer they ship one licence fewer than they do; nothing in
# the suite, and nothing in the digest check above, can see it.
case_ "a licence row that no longer matches the build" \
  LICENSES.md \
  "LICENSES.md ft-hb row" \
  '| "Old MIT", taken from HarfBuzz | **Yes.**' \
  '| "Old MIT", taken from HarfBuzz | No.'

# A build option renamed on the command line and not in the documentation.
# The suite cannot see this at all: -Dsanitize_c is passed by ci/run.sh, so a
# renamed option makes the flag inert rather than an error, and every test
# still passes -- with the sanitizer silently off.
case_ "a build option README cannot name"   build.zig   "README.md names no such flag"   '            "sanitize_c",'   '            "sanitize_the_c",'

# A process-wide allocator installed for a test and taken out only on the
# happy path. Nothing the suite runs can see this either -- the failure it
# creates is in the FAILING run of some other test, which is not a state the
# suite reaches when it passes -- so a source gate is the only thing that can.
case_ "a test allocator taken out only on the happy path" \
  src/integration_test.zig \
  "without deferring the reset" \
  "        try ztext.setAllocator(first);
        defer ztext.resetAllocator();

        const library = try ztext.Library.init();
        const font = try library.createFont(fonts.hebrew, 0);" \
  "        try ztext.setAllocator(first);

        const library = try ztext.Library.init();
        const font = try library.createFont(fonts.hebrew, 0);"

# The call site put back the way it was: straight into a command
# substitution, unbounded, and joined to the harness by a pipe. Nothing the
# suite runs can see this -- the harness is read, not built -- so the gate
# that reads it is the only thing that can.
#
# Both strings are assembled rather than written out, and that is not style.
# This is the one case that mutates the file it lives in: an anchor spelled
# literally would appear twice, once in the code and once here, and a case
# whose anchor is not unique cannot run at all. The replacement is split for
# the same reason pointed the other way -- ci/measurements.sh counts the
# call-site token, and a literal copy of it here would be a second site the
# gate would have to be taught to forgive.
bounded_call='  run_'"guarded"
unbounded_call='  RUN_OUTPUT=$(cd "$WORK/tree" && "${GUARD_'"CMD[@]"'}" 2>&1)
  RUN_STATUS=$?
  RUN_TIMED_OUT=0'

case_ "a guard case run unbounded again" \
  ci/check-guards.sh \
  "outside the bounded runner" \
  "$bounded_call" \
  "$unbounded_call"

printf '\n%sInstalled headers%s %s(ci/header-link.sh, not the suite)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

# These two are held by a script rather than by `zig build test`, because
# neither defect is visible from inside the tree: both reach a consumer and
# nothing else. `zig build test` passes with either mutation applied, which is
# the whole reason the gate exists.
GUARD_CMD=(ci/header-link.sh)

# A header put back into the install list with nothing behind it. hb-gpu.h is
# the public face of upstream's libharfbuzz-gpu, which this package does not
# build; it was installed for real until the gate said so.
case_ "an installed header no compiled code stands behind" \
  build.zig \
  "no root reaches it" \
  '    "hb-ft.h",' \
  '    "hb-ft.h",
    "hb-gpu.h",'

# And the other direction: the header stays, its implementation goes. A
# consumer that calls FT_Get_FSType_Flags then compiles and fails at link,
# which is exactly what happened before ftfstype.c was compiled.
case_ "a declared entry point with no implementation" \
  build.zig \
  "declares an entry point nothing defines" \
  '    "libs/freetype/src/base/ftfstype.c",
' \
  ''

GUARD_CMD=(zig build test)

printf '\n'
if [ "$ANCHORS_ONLY" -eq 1 ]; then
  what='anchors, every one applying to exactly one place'
  wrong='anchors no longer apply to exactly one place:'
else
  what='mutations, every one caught by a named test'
  wrong='mutations were not caught as expected:'
fi

if [ $FAILED -eq 0 ]; then
  printf '%s%d %s%s\n' "$GREEN" "$PASSED" "$what" "$OFF"
  exit 0
fi

printf '%s%d of %d %s%s\n' \
  "$RED" "$FAILED" "$((PASSED + FAILED))" "$wrong" "$OFF" >&2
for name in "${FAILED_NAMES[@]}"; do printf '  %s\n' "$name" >&2; done
printf '\nA mutation that is NOT CAUGHT is a hole in the suite, not a bug in\n' >&2
printf 'this script. A mutation with NO ANCHOR means the code moved and the\n' >&2
printf 'case needs updating -- which is the point of it being checked in.\n' >&2
exit 1
