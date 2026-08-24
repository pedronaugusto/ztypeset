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
#   ci/check-guards.sh <pattern>    # only cases whose name matches
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
trap 'rm -rf "$WORK"' EXIT

# A pristine copy, reused by every case: only the mutated file is restored
# between them, so the build cache survives and a run takes minutes not hours.
printf '%spreparing a working copy%s\n' "$DIM" "$OFF"
mkdir -p "$WORK/tree"
tar -cf - --exclude .git --exclude .zig-cache --exclude zig-out \
    --exclude tests/consumer/.zig-cache --exclude tests/consumer/zig-out . |
  tar -xf - -C "$WORK/tree"

# Warm the cache once, and refuse to go on if the unmutated tree is not green:
# every assertion below is "this test fails", which means nothing if some test
# fails already.
printf '%schecking the unmutated tree is green%s\n' "$DIM" "$OFF"
if ! (cd "$WORK/tree" && zig build test > "$WORK/baseline.log" 2>&1); then
  printf '%sthe tree fails its own suite before any mutation.%s\n' "$RED" "$OFF" >&2
  sed 's/^/  | /' "$WORK/baseline.log" | head -30 >&2
  exit 1
fi

# case <name> <file> <expect-substring> <old> <new>
#
# `expect-substring` is matched against the build output. It must name the
# specific test or the specific compile error, so a mutation that fails the
# build for an unrelated reason is not counted as caught.
case_() {
  local name="$1" file="$2" expect="$3" old="$4" new="$5"

  if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then return; fi

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
    printf '%sNOT CAUGHT%s the suite passes with this bug in it\n' "$RED" "$OFF"
    FAILED=$((FAILED + 1)); FAILED_NAMES+=("$name (not caught)")
  elif printf '%s' "$output" | grep -qF "$expect"; then
    printf '%scaught%s %s(%s)%s\n' "$GREEN" "$OFF" "$DIM" "$expect" "$OFF"
    PASSED=$((PASSED + 1))
  else
    printf '%sWRONG FAILURE%s expected to see: %s\n' "$RED" "$OFF" "$expect"
    printf '%s' "$output" | grep -E "ABI drift|^error|failed:|error:" |
      sed 's/^/      | /' | head -8
    FAILED=$((FAILED + 1)); FAILED_NAMES+=("$name (wrong failure)")
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

case_ "a declined reallocate reported as out of memory" \
  ffi/ztext_core.c \
  "error" \
  "  void* fresh = ztextAllocWith(allocator, new_size, backing);" \
  "  if (allocator->reallocate != NULL) return NULL;
  void* fresh = ztextAllocWith(allocator, new_size, backing);"

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
