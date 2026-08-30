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
# Usage:
#   ci/check-guards.sh              # every case
#   ci/check-guards.sh <regex>      # only cases whose name matches, e.g.
#                                   #   ci/check-guards.sh 'enum:|struct:'
#
# Slow by nature: each case is a full rebuild of the ztext library plus the
# suite. It is a separate step from ci/run.sh for that reason.

set -uo pipefail
cd "$(dirname "$0")/.."

FILTER="${1:-}"

if [ -t 1 ]; then
  RED=$'\033[31m'; GREEN=$'\033[32m'; DIM=$'\033[2m'; BOLD=$'\033[1m'; OFF=$'\033[0m'
else
  RED=; GREEN=; DIM=; BOLD=; OFF=
fi

PASSED=0
FAILED=0
FAILED_NAMES=()

WORK=$(mktemp -d)
# The working copy goes; the failure logs under it do not, or the evidence for
# a red case dies with the run that produced it.
cleanup() {
  if [ "${FAILED:-0}" -gt 0 ] && [ -d "$WORK/failures" ]; then
    keep="${TMPDIR:-/tmp}/ztext-guard-failures.$$"
    if mkdir -p "$keep" && cp "$WORK/failures/"*.log "$keep/" 2>/dev/null; then
      printf '\nfailure logs kept in %s\n' "$keep" >&2
    fi
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# A pristine copy, reused by every case: only the mutated file is restored
# between them, so the build cache survives and a run takes minutes not hours.
printf '%spreparing a working copy%s\n' "$DIM" "$OFF"
mkdir -p "$WORK/tree"
tar -cf - --exclude .git --exclude .zig-cache --exclude zig-out \
    --exclude tests/consumer/.zig-cache --exclude tests/consumer/zig-out . |
  tar -xf - -C "$WORK/tree"

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
selftest_matcher

# Warm the cache once, and refuse to go on if the unmutated tree is not green:
# every assertion below is "this test fails", which means nothing if some test
# fails already.
printf '%schecking the unmutated tree is green%s\n' "$DIM" "$OFF"
if ! (cd "$WORK/tree" && zig build test > "$WORK/baseline.log" 2>&1); then
  printf '%sthe tree fails its own suite before any mutation.%s\n' "$RED" "$OFF" >&2
  sed 's/^/  | /' "$WORK/baseline.log" | head -30 >&2
  exit 1
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
  printf '%s' "$output" | grep -E "ABI drift|^error|failed:|error:" |
    sed 's/^/      | /' | head -8
  printf '      %sfull output: %s%s\n' "$DIM" "$LOGDIR/$slug.log" "$OFF"
  FAILED=$((FAILED + 1)); FAILED_NAMES+=("$name")
}

# case <name> <file> <expect-substring> <old> <new>
#
# `expect-substring` is matched against the build output. It must name the
# specific test or the specific compile error, so a mutation that fails the
# build for an unrelated reason is not counted as caught.
case_() {
  local name="$1" file="$2" expect="$3" old="$4" new="$5"

  if [ -n "$FILTER" ] && ! [[ "$name" =~ $FILTER ]]; then return; fi

  printf '  %-52s' "$name"

  if ! MUT_FILE="$file" MUT_OLD="$old" MUT_NEW="$new" python3 - "$WORK/tree" <<'PY'
import os, sys
tree = sys.argv[1]
path = os.path.join(tree, os.environ["MUT_FILE"])
old, new = os.environ["MUT_OLD"], os.environ["MUT_NEW"]
s = open(path).read()
if s.count(old) != 1:
    sys.stderr.write(f"anchor appears {s.count(old)} times in {os.environ['MUT_FILE']}\n")
    sys.exit(1)
open(path, "w").write(s.replace(old, new, 1))
PY
  then
    printf '%sNO ANCHOR%s  the mutation no longer applies; update it\n' "$RED" "$OFF"
    FAILED=$((FAILED + 1)); FAILED_NAMES+=("$name (anchor)")
    return
  fi

  local output status
  output=$(cd "$WORK/tree" && zig build test 2>&1)
  status=$?

  cp "$file" "$WORK/tree/$file"

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
  else
    report "$name" 'WRONG FAILURE' "expected to see: $expect" "$output"
  fi
}

printf '\n%sABI cross-check%s %s(src/abi_check.zig, against ffi/ztext.h)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

case_ "enum: a MIDDLE enumerator renumbered" \
  ffi/ztext.h \
  "type Result.invalid_utf8 is 3 in src/c.zig" \
  "  ZTEXT_RESULT_INVALID_UTF8 = 3," \
  "  ZTEXT_RESULT_INVALID_UTF8 = 99,"

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

case_ "a line derived from the paragraph, skipping rule L1" \
  ffi/ztext_bidi.c \
  "a line applies rule L1" \
  "  SBLineRef line = SBParagraphCreateLine(sb_paragraph, (SBUInteger)offset,
                                         (SBUInteger)length);" \
  "  SBLineRef line = SBParagraphCreateLine(sb_paragraph, 0u,
                                         SBParagraphGetLength(sb_paragraph));"

case_ "script pieces emitted forwards inside an RTL run" \
  ffi/ztext_bidi.c \
  "shaping runs" \
  "      const size_t s = rtl ? (last - 1u - n) : (first + n);" \
  "      const size_t s = first + n;"

case_ "the end of a paragraph left as no break at all" \
  ffi/ztext_bidi.c \
  "wrap loop covers a paragraph exactly once" \
  "  out[length - 1u] = (char)ZTEXT_BREAK_MANDATORY;" \
  "  (void)0;"

printf '\n%sFaces and fonts%s %s(ffi/ztext_face.c, ffi/ztext_raster.c)%s\n' \
  "$BOLD" "$OFF" "$DIM" "$OFF"

case_ "a face loads without activating its own FT_Size" \
  ffi/ztext_raster.c \
  "share the parse but not the size" \
  "  ztextFaceActivate(face);

  const FT_Error error = FT_Load_Glyph(face->font->ft, (FT_UInt)glyph_id," \
  "  const FT_Error error = FT_Load_Glyph(face->font->ft, (FT_UInt)glyph_id,"

case_ "a covered prefix that splits a base from its marks" \
  ffi/ztext_face.c \
  "never splits a base from its marks" \
  "    if (!isMark(unicode, next)) boundary = i;" \
  "    (void)next;
    boundary = i;"

case_ "a covered prefix that breaks at a format character" \
  ffi/ztext_face.c \
  "format characters never break a run" \
  "  if (cp >= 0xFE00u && cp <= 0xFE0Fu) return true;" \
  "  (void)unicode;
  if (cp == 0u) return true;
  return false;
  if (cp >= 0xFE00u && cp <= 0xFE0Fu) return true;"

case_ "a pixel size rounded to whole pixels" \
  ffi/ztext_face.c \
  "fractional pixel size is honoured" \
  "  return (int32_t)(pixels * 64.0f + 0.5f);" \
  "  return (int32_t)(pixels + 0.5f) * 64;"

printf '\n%sShaping%s %s(ffi/ztext_shape.c)%s\n' "$BOLD" "$OFF" "$DIM" "$OFF"

case_ "a run shaped without the text around it" \
  ffi/ztext_shape.c \
  "shaping a run with context matches" \
  "  hb_buffer_add_utf8(buffer, text, (int)length, (unsigned int)run_offset,
                     (int)run_length);" \
  "  hb_buffer_add_utf8(buffer, text + run_offset, (int)run_length, 0u,
                     (int)run_length);"

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

printf '\n%sAllocator%s %s(ffi/ztext_core.c)%s\n' "$BOLD" "$OFF" "$DIM" "$OFF"

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
  "  FT_Done_Face(font->ft);
  ztextFreeFrom(library->allocator, font);" \
  "  FT_Done_Face(font->ft);
  ztextFreeFrom(ZTEXT_ALLOCATOR_DEFAULT, font);"

# SheenBidi 3.0.0 reads a field it has not written on its own
# allocation-failure path, and ztext's seam zeroes every block it hands over
# so that read finds NULL. Remove the memset and the poisoned injection arm
# dies -- every run, at the same injection point, rather than one run in fifty.
case_ "SheenBidi handed memory ztext did not write" \
  ffi/ztext_core.c \
  "during phase: injection-poisoned" \
  "  if (block != NULL) memset(block, 0, (size_t)size);" \
  "  (void)0;"

printf '\n'
if [ $FAILED -eq 0 ]; then
  printf '%s%d mutations, every one caught by a named test%s\n' "$GREEN" "$PASSED" "$OFF"
  exit 0
fi

printf '%s%d of %d mutations were not caught as expected:%s\n' \
  "$RED" "$FAILED" "$((PASSED + FAILED))" "$OFF" >&2
for name in "${FAILED_NAMES[@]}"; do printf '  %s\n' "$name" >&2; done
printf '\nA mutation that is NOT CAUGHT is a hole in the suite, not a bug in\n' >&2
printf 'this script. A mutation with NO ANCHOR means the code moved and the\n' >&2
printf 'case needs updating -- which is the point of it being checked in.\n' >&2
exit 1
