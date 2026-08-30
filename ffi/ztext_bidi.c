//===----------------------------------------------------------------------===//
// ztext -- the Unicode Bidirectional Algorithm and script itemisation.
//
// Both come from SheenBidi, and both stop at runs: what ztext produces is the
// input a shaper needs, not a laid-out line.
//
// "Not a laid-out line" is not the same as "not per line". Rules L1 and L2 of
// UAX #9 -- the whitespace reset and the run reversal -- are defined over a
// LINE, so a paragraph that wraps has ordering its own run list cannot
// describe. That is what ZtextLine is for; ZtextParagraph's runs are the
// paragraph laid out as a single line, which is the common case and not the
// only one.
//===----------------------------------------------------------------------===//

#include <SheenBidi/SBScriptLocator.h>

#include "ztext_internal.h"

static SBLevel toSbBaseLevel(ZtextBaseDirection base) {
  switch (base) {
    case ZTEXT_BASE_DIRECTION_LTR:
      return 0;
    case ZTEXT_BASE_DIRECTION_RTL:
      return 1;
    case ZTEXT_BASE_DIRECTION_AUTO:
    default:
      // Rules P2/P3: take the direction of the first strong character, and
      // fall back to left-to-right when there is none.
      return SBLevelDefaultLTR;
  }
}

/// Copies one SBLine's runs out, in visual order.
static ZtextResult runsFromLine(ZtextAllocatorId owner, SBLineRef line,
                                ZtextArray* out) {
  const SBUInteger run_count = SBLineGetRunCount(line);
  if (run_count == 0u) return ZTEXT_RESULT_OK;

  const SBRun* runs = SBLineGetRunsPtr(line);
  if (!ztextArrayReserve(owner, out, (size_t)run_count,
                         sizeof(ZtextVisualRun))) {
    return ZTEXT_RESULT_OUT_OF_MEMORY;
  }

  ZtextVisualRun* dst = (ZtextVisualRun*)out->data;
  for (SBUInteger i = 0u; i < run_count; i++) {
    dst[i].offset = (uint32_t)runs[i].offset;
    dst[i].length = (uint32_t)runs[i].length;
    dst[i].level = (uint8_t)runs[i].level;
  }
  out->count = (size_t)run_count;
  return ZTEXT_RESULT_OK;
}

/// Collects maximal single-script spans, in logical order.
static ZtextResult collectScriptRuns(ZtextParagraph* paragraph,
                                     const SBCodepointSequence* sequence) {
  SBScriptLocatorRef locator = SBScriptLocatorCreate();
  if (locator == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;

  SBScriptLocatorLoadCodepoints(locator, sequence);

  ZtextResult result = ZTEXT_RESULT_OK;
  const SBScriptAgent* agent = SBScriptLocatorGetAgent(locator);
  while (SBScriptLocatorMoveNext(locator) != SBFalse) {
    if (!ztextArrayReserve(ztextAllocatorOf(paragraph),
                           &paragraph->script_runs,
                           paragraph->script_runs.count + 1u,
                           sizeof(ZtextScriptRun))) {
      result = ZTEXT_RESULT_OUT_OF_MEMORY;
      break;
    }
    ZtextScriptRun* out = (ZtextScriptRun*)paragraph->script_runs.data;
    ZtextScriptRun* slot = &out[paragraph->script_runs.count];
    slot->offset = (uint32_t)agent->offset;
    slot->length = (uint32_t)agent->length;
    // The Unicode tag is the ISO 15924 code -- 'Arab', 'Hebr', 'Latn' -- which
    // is exactly what ZtextShapeParams::script takes. SheenBidi also offers an
    // OpenType tag, which is a different thing and would be wrong here.
    slot->script = (uint32_t)SBScriptGetUnicodeTag(agent->script);
    paragraph->script_runs.count += 1u;
  }

  SBScriptLocatorRelease(locator);
  return result;
}

/// Intersects visual runs with script runs.
///
/// Visual runs are in visual order and each carries one embedding level;
/// script runs are in logical order and each carries one script. A shaper
/// needs spans that are uniform in BOTH, in the order they will be drawn.
///
/// The subtlety is the emission order inside a right-to-left visual run. Its
/// script pieces are stored ascending in the text, but they are drawn
/// right-to-left, so they must be emitted from the last one backwards. A
/// left-to-right run emits them forwards. Getting this wrong reorders scripts
/// within a word and produces output that looks nearly right.
///
/// Script runs are paragraph-wide and are clipped to each visual run here,
/// which is what lets a line reuse the paragraph's list unchanged: where the
/// text wraps changes the visual runs, never the scripts.
static ZtextResult intersectRuns(ZtextAllocatorId owner,
                                 const ZtextVisualRun* visual,
                                 size_t visual_count,
                                 const ZtextScriptRun* scripts,
                                 size_t script_count, ZtextArray* out) {
  for (size_t v = 0u; v < visual_count; v++) {
    const uint32_t run_start = visual[v].offset;
    const uint32_t run_end = run_start + visual[v].length;
    const bool rtl = (visual[v].level & 1u) != 0u;

    // The script runs overlapping this visual run, as a half-open range.
    size_t first = script_count;
    size_t last = script_count;
    for (size_t s = 0u; s < script_count; s++) {
      const uint32_t script_end = scripts[s].offset + scripts[s].length;
      if (script_end <= run_start || scripts[s].offset >= run_end) continue;
      if (first == script_count) first = s;
      last = s + 1u;
    }

    // A paragraph with no script runs at all still has to produce something
    // shapeable, so the visual run passes through with an undetermined script.
    if (first == script_count) {
      if (!ztextArrayReserve(owner, out, out->count + 1u,
                             sizeof(ZtextShapingRun))) {
        return ZTEXT_RESULT_OUT_OF_MEMORY;
      }
      ZtextShapingRun* dst = (ZtextShapingRun*)out->data;
      dst[out->count].offset = run_start;
      dst[out->count].length = visual[v].length;
      dst[out->count].script = 0u;
      dst[out->count].level = visual[v].level;
      out->count += 1u;
      continue;
    }

    for (size_t n = 0u; n < last - first; n++) {
      const size_t s = rtl ? (last - 1u - n) : (first + n);
      uint32_t start = scripts[s].offset;
      uint32_t end = start + scripts[s].length;
      if (start < run_start) start = run_start;
      if (end > run_end) end = run_end;
      if (end <= start) continue;

      if (!ztextArrayReserve(owner, out, out->count + 1u,
                             sizeof(ZtextShapingRun))) {
        return ZTEXT_RESULT_OUT_OF_MEMORY;
      }
      ZtextShapingRun* dst = (ZtextShapingRun*)out->data;
      dst[out->count].offset = start;
      dst[out->count].length = end - start;
      dst[out->count].script = scripts[s].script;
      dst[out->count].level = visual[v].level;
      out->count += 1u;
    }
  }

  return ZTEXT_RESULT_OK;
}

/// Reorders one byte range of a paragraph and intersects the result with the
/// paragraph's scripts. This is the whole of both ztextParagraphCreate's
/// run collection and ztextLineCreate's -- the difference between them is
/// only which range is asked for.
static ZtextResult runsForRange(ZtextAllocatorId owner,
                                SBParagraphRef sb_paragraph, size_t offset,
                                size_t length, const ZtextScriptRun* scripts,
                                size_t script_count, ZtextArray* visual_out,
                                ZtextArray* shaping_out) {
  // SBParagraphCreateLine is what applies rules L1 and L2, over exactly this
  // range. Asking for the whole paragraph is the single-line case, not a
  // different code path.
  SBLineRef line = SBParagraphCreateLine(sb_paragraph, (SBUInteger)offset,
                                         (SBUInteger)length);
  // The range was validated by the caller, so NULL here is an allocation
  // failure rather than a rejected range.
  if (line == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;

  ZtextResult result = runsFromLine(owner, line, visual_out);
  SBLineRelease(line);
  if (result != ZTEXT_RESULT_OK) return result;

  return intersectRuns(owner, (const ZtextVisualRun*)visual_out->data,
                       visual_out->count, scripts, script_count, shaping_out);
}

//===----------------------------------------------------------------------===//
// Segmentation
//
// UAX #14 and #29, from libunibreak. It allocates nothing and keeps no global
// state, so all ztext supplies is the buffer and the translation of upstream's
// per-algorithm constants into one ZtextBreak.
//===----------------------------------------------------------------------===//

/// libunibreak's line-break codes, which have their own numbering per
/// algorithm, translated into the one enum ztext exposes.
static uint8_t fromLineBreak(char code) {
  switch (code) {
    case LINEBREAK_MUSTBREAK:
      return ZTEXT_BREAK_MANDATORY;
    case LINEBREAK_ALLOWBREAK:
      return ZTEXT_BREAK_ALLOWED;
    // NOBREAK, INSIDEACHAR and INDETERMINATE all mean "not here". Collapsing
    // them is deliberate: the distinction is about why, and a caller looking
    // for somewhere to break only needs to know it cannot.
    default:
      return ZTEXT_BREAK_NONE;
  }
}

/// GRAPHEMEBREAK_* and WORDBREAK_* happen to agree: 0 is a boundary, anything
/// else is not. Written once rather than twice so a future divergence is a
/// change in one place.
static uint8_t fromClusterBreak(char code) {
  return code == 0 ? ZTEXT_BREAK_ALLOWED : ZTEXT_BREAK_NONE;
}

/// libunibreak's three algorithms over one paragraph, in its own encoding.
///
/// Written as one switch rather than three, so an encoding cannot be handled
/// by two of the three algorithms: upstream numbers its entry points by
/// encoding, and the arms are what pairs them up.
static void segment(const ZtextParagraph* paragraph) {
  const size_t length = paragraph->length;
  char* lines = (char*)paragraph->line_breaks;
  char* graphemes = (char*)paragraph->grapheme_breaks;
  char* words = (char*)paragraph->word_breaks;
  switch (paragraph->encoding) {
    case ZTEXT_ENCODING_UTF16: {
      const utf16_t* text = (const utf16_t*)paragraph->text;
      if (lines != NULL) set_linebreaks_utf16(text, length, NULL, lines);
      if (graphemes != NULL) {
        set_graphemebreaks_utf16(text, length, NULL, graphemes);
      }
      if (words != NULL) set_wordbreaks_utf16(text, length, NULL, words);
      break;
    }
    case ZTEXT_ENCODING_UTF32: {
      const utf32_t* text = (const utf32_t*)paragraph->text;
      if (lines != NULL) set_linebreaks_utf32(text, length, NULL, lines);
      if (graphemes != NULL) {
        set_graphemebreaks_utf32(text, length, NULL, graphemes);
      }
      if (words != NULL) set_wordbreaks_utf32(text, length, NULL, words);
      break;
    }
    case ZTEXT_ENCODING_UTF8:
    default: {
      const utf8_t* text = (const utf8_t*)paragraph->text;
      if (lines != NULL) set_linebreaks_utf8(text, length, NULL, lines);
      if (graphemes != NULL) {
        set_graphemebreaks_utf8(text, length, NULL, graphemes);
      }
      if (words != NULL) set_wordbreaks_utf8(text, length, NULL, words);
      break;
    }
  }
}

/// Fills the break arrays the paragraph was asked for, and only those.
///
/// One allocation for however many passes were requested, because they are
/// always the same length and always live and die together.
static ZtextResult collectBreaks(ZtextParagraph* paragraph) {
  const size_t length = paragraph->length;
  const uint32_t want = paragraph->segmentation;
  if (length == 0u || want == (uint32_t)ZTEXT_SEGMENTATION_NONE) {
    return ZTEXT_RESULT_OK;
  }

  size_t arrays = 0u;
  if ((want & (uint32_t)ZTEXT_SEGMENTATION_LINES) != 0u) arrays++;
  if ((want & (uint32_t)ZTEXT_SEGMENTATION_GRAPHEMES) != 0u) arrays++;
  if ((want & (uint32_t)ZTEXT_SEGMENTATION_WORDS) != 0u) arrays++;

  // `length` is already bounded by 0xFFFFFFFF, but that is not a bound on the
  // product on a 32-bit target, so it is checked as a division.
  if (length > SIZE_MAX / arrays) return ZTEXT_RESULT_OUT_OF_MEMORY;
  paragraph->breaks =
      (uint8_t*)ztextAlloc(length * arrays, ZTEXT_DEFAULT_ALIGN);
  if (paragraph->breaks == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;

  uint8_t* next = paragraph->breaks;
  if ((want & (uint32_t)ZTEXT_SEGMENTATION_LINES) != 0u) {
    paragraph->line_breaks = next;
    next += length;
  }
  if ((want & (uint32_t)ZTEXT_SEGMENTATION_GRAPHEMES) != 0u) {
    paragraph->grapheme_breaks = next;
    next += length;
  }
  if ((want & (uint32_t)ZTEXT_SEGMENTATION_WORDS) != 0u) {
    paragraph->word_breaks = next;
  }

  // libunibreak writes one char per input code unit and reads nothing else,
  // so its output buffer can be ztext's own storage rather than a staging
  // copy.
  segment(paragraph);

  if (paragraph->line_breaks != NULL) {
    char* out = (char*)paragraph->line_breaks;
    for (size_t i = 0u; i < length; i++) out[i] = (char)fromLineBreak(out[i]);

    // The end of a paragraph is always a break, and libunibreak does not say
    // so: text ending on a character that is not a line terminator gets
    // LINEBREAK_INDETERMINATE, meaning "unknown, the input stopped". Unknown
    // is the honest answer to a stream; for a paragraph it is not, because
    // there is nothing after it. Left as NONE, a host walking the allowed
    // positions would never reach the end of its own text -- which is exactly
    // what the wrap test caught.
    out[length - 1u] = (char)ZTEXT_BREAK_MANDATORY;
  }
  for (size_t pass = 0u; pass < 2u; pass++) {
    char* out = (char*)(pass == 0u ? paragraph->grapheme_breaks
                                   : paragraph->word_breaks);
    if (out == NULL) continue;
    for (size_t i = 0u; i < length; i++) {
      out[i] = (char)fromClusterBreak(out[i]);
    }
  }

  return ZTEXT_RESULT_OK;
}

ZtextResult ztextParagraphCreate(const void* text, size_t length,
                                 ZtextEncoding encoding,
                                 ZtextBaseDirection base,
                                 uint32_t segmentation,
                                 ZtextParagraph** out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  *out = NULL;
  // A bit this build has no name for is refused rather than ignored: it is
  // how a consumer compiled against a newer header finds out.
  if ((segmentation & ~(uint32_t)ZTEXT_SEGMENTATION_ALL) != 0u) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }
  if (text == NULL && length != 0u) return ZTEXT_RESULT_INVALID_ARGUMENT;
  // Zero for an encoding this build does not name, which is how a consumer
  // compiled against a newer header is refused rather than misread.
  const size_t unit = ztextEncodingUnitSize(encoding);
  if (unit == 0u) return ZTEXT_RESULT_INVALID_ARGUMENT;
  // Run offsets and lengths are reported as uint32_t. Past that, they would
  // wrap while ztextParagraphLength kept returning the true size_t, and a
  // caller slicing its buffer by those offsets would read the wrong bytes --
  // or past the end. ztext_abi.c asserts this bound is enforced somewhere;
  // here is where.
  if (length > 0xFFFFFFFFu) return ZTEXT_RESULT_INVALID_ARGUMENT;
  // The copy below is `length * unit` bytes. On a 32-bit target that product
  // can exceed what size_t holds even under the uint32_t bound above, so it
  // is checked as a division rather than computed and hoped for.
  if (length > SIZE_MAX / unit) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (!ztextTextIsWellFormed(text, length, encoding)) {
    return ZTEXT_RESULT_INVALID_TEXT;
  }

  const ZtextResult installed = ztextInstallSheenbidiAllocator();
  if (installed != ZTEXT_RESULT_OK) return installed;

  ZtextParagraph* paragraph = ZTEXT_NEW(ZtextParagraph);
  if (paragraph == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;

  // An empty paragraph is legitimate -- an empty label, a blank line -- and is
  // answered directly rather than pushed through SheenBidi, which has nothing
  // to say about zero code units.
  paragraph->encoding = encoding;
  paragraph->segmentation = segmentation;
  if (length == 0u) {
    paragraph->base_level = (base == ZTEXT_BASE_DIRECTION_RTL) ? 1u : 0u;
    *out = paragraph;
    return ZTEXT_RESULT_OK;
  }

  // Copied before anything else, so every SheenBidi object below is built over
  // memory this paragraph owns. See ZtextParagraph::text for why that matters.
  // ZTEXT_DEFAULT_ALIGN is max_align_t, so the copy is aligned for uint16_t
  // and uint32_t as well as for bytes -- which is what lets the text helpers
  // and libunibreak read it as an array of its own unit type.
  paragraph->text = (char*)ztextAlloc(length * unit, ZTEXT_DEFAULT_ALIGN);
  if (paragraph->text == NULL) {
    ztextFree(paragraph);
    return ZTEXT_RESULT_OUT_OF_MEMORY;
  }
  memcpy(paragraph->text, text, length * unit);

  SBCodepointSequence sequence;
  // ztext_abi.c asserts the three values are SheenBidi's own, which is what
  // makes this a cast rather than a translation table.
  sequence.stringEncoding = (SBStringEncoding)encoding;
  sequence.stringBuffer = paragraph->text;
  sequence.stringLength = (SBUInteger)length;

  SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
  if (algorithm == NULL) {
    ztextParagraphDestroy(paragraph);
    return ZTEXT_RESULT_OUT_OF_MEMORY;
  }

  // The algorithm is defined per paragraph, so a buffer containing a paragraph
  // separator describes more than one. ztext analyses the first and reports
  // how much of the input that covered, rather than silently treating the rest
  // as part of it. The copy above still holds all `length` bytes; the tail
  // past the separator is simply never looked at again.
  SBUInteger actual_length = 0u;
  SBUInteger separator_length = 0u;
  SBAlgorithmGetParagraphBoundary(algorithm, 0u, (SBUInteger)length,
                                  &actual_length, &separator_length);
  if (actual_length == 0u) actual_length = (SBUInteger)length;

  paragraph->sb_paragraph = SBAlgorithmCreateParagraph(
      algorithm, 0u, actual_length, toSbBaseLevel(base));
  // SBParagraph retains the algorithm, so ztext's own reference is done with.
  SBAlgorithmRelease(algorithm);
  if (paragraph->sb_paragraph == NULL) {
    ztextParagraphDestroy(paragraph);
    return ZTEXT_RESULT_BIDI_FAILED;
  }

  paragraph->length = (size_t)actual_length;
  paragraph->base_level =
      (uint8_t)SBParagraphGetBaseLevel(paragraph->sb_paragraph);

  const ZtextResult segmented = collectBreaks(paragraph);
  if (segmented != ZTEXT_RESULT_OK) {
    ztextParagraphDestroy(paragraph);
    return segmented;
  }

  // Itemised over the analysed paragraph only, so script runs and visual runs
  // index the same span.
  SBCodepointSequence paragraph_sequence = sequence;
  paragraph_sequence.stringLength = actual_length;
  ZtextResult result = collectScriptRuns(paragraph, &paragraph_sequence);

  if (result == ZTEXT_RESULT_OK) {
    result = runsForRange(ztextAllocatorOf(paragraph), paragraph->sb_paragraph,
                          0u, (size_t)actual_length,
                          (const ZtextScriptRun*)paragraph->script_runs.data,
                          paragraph->script_runs.count, &paragraph->visual_runs,
                          &paragraph->shaping_runs);
  }

  if (result != ZTEXT_RESULT_OK) {
    ztextParagraphDestroy(paragraph);
    return result;
  }

  *out = paragraph;
  return ZTEXT_RESULT_OK;
}

void ztextParagraphDestroy(ZtextParagraph* paragraph) {
  if (paragraph == NULL) return;
  if (paragraph->sb_paragraph != NULL) {
    SBParagraphRelease(paragraph->sb_paragraph);
  }
  ztextFree(paragraph->breaks);
  ztextFree(paragraph->text);
  const ZtextAllocatorId owner = ztextAllocatorOf(paragraph);
  ztextArrayFree(owner, &paragraph->visual_runs, sizeof(ZtextVisualRun));
  ztextArrayFree(owner, &paragraph->script_runs, sizeof(ZtextScriptRun));
  ztextArrayFree(owner, &paragraph->shaping_runs, sizeof(ZtextShapingRun));
  ztextFree(paragraph);
}

size_t ztextParagraphLength(const ZtextParagraph* paragraph) {
  return paragraph == NULL ? 0u : paragraph->length;
}

ZtextEncoding ztextParagraphEncoding(const ZtextParagraph* paragraph) {
  // UTF-8 for a NULL paragraph, like every other accessor's zero: it is the
  // enum's own zero, and there is no text to misread.
  return paragraph == NULL ? ZTEXT_ENCODING_UTF8 : paragraph->encoding;
}

uint8_t ztextParagraphBaseLevel(const ZtextParagraph* paragraph) {
  return paragraph == NULL ? 0u : paragraph->base_level;
}

const uint8_t* ztextParagraphLevels(const ZtextParagraph* paragraph) {
  if (paragraph == NULL || paragraph->sb_paragraph == NULL) return NULL;
  // SBLevel is a byte -- asserted in ztext_abi.c, because this cast is the
  // reason it has to stay one.
  return (const uint8_t*)SBParagraphGetLevelsPtr(paragraph->sb_paragraph);
}

uint32_t ztextParagraphSegmentation(const ZtextParagraph* paragraph) {
  return paragraph == NULL ? (uint32_t)ZTEXT_SEGMENTATION_NONE
                           : paragraph->segmentation;
}

const uint8_t* ztextParagraphLineBreaks(const ZtextParagraph* paragraph) {
  return paragraph == NULL ? NULL : paragraph->line_breaks;
}

const uint8_t* ztextParagraphGraphemeBreaks(const ZtextParagraph* paragraph) {
  return paragraph == NULL ? NULL : paragraph->grapheme_breaks;
}

const uint8_t* ztextParagraphWordBreaks(const ZtextParagraph* paragraph) {
  return paragraph == NULL ? NULL : paragraph->word_breaks;
}

size_t ztextParagraphNextGrapheme(const ZtextParagraph* paragraph,
                                  size_t offset) {
  // Without the grapheme pass there are no boundaries to move to, so the
  // caret stays where it is rather than jumping to the end of the text.
  const uint8_t* breaks = ztextParagraphGraphemeBreaks(paragraph);
  if (paragraph == NULL) return 0u;
  if (breaks == NULL) return offset;
  if (offset >= paragraph->length) return paragraph->length;
  // The entry at i describes the boundary AFTER byte i, so the next boundary
  // at or past `offset` is the first non-NONE entry from `offset` onward,
  // reported as one past it.
  for (size_t i = offset; i < paragraph->length; i++) {
    if (breaks[i] != ZTEXT_BREAK_NONE) return i + 1u;
  }
  return paragraph->length;
}

size_t ztextParagraphPreviousGrapheme(const ZtextParagraph* paragraph,
                                      size_t offset) {
  const uint8_t* breaks = ztextParagraphGraphemeBreaks(paragraph);
  if (paragraph == NULL || offset == 0u) return 0u;
  // As above: no pass, no boundary, no movement.
  if (breaks == NULL) return offset;
  if (offset > paragraph->length) offset = paragraph->length;

  // Walk back past the boundary the caret is sitting on, then find the one
  // before it. Without the first step a caret would never move.
  size_t i = offset - 1u;
  while (i > 0u) {
    i--;
    if (breaks[i] != ZTEXT_BREAK_NONE) return i + 1u;
  }
  return 0u;
}

size_t ztextParagraphVisualRunCount(const ZtextParagraph* paragraph) {
  return paragraph == NULL ? 0u : paragraph->visual_runs.count;
}

const ZtextVisualRun* ztextParagraphVisualRuns(
    const ZtextParagraph* paragraph) {
  if (paragraph == NULL) return NULL;
  return (const ZtextVisualRun*)paragraph->visual_runs.data;
}

size_t ztextParagraphScriptRunCount(const ZtextParagraph* paragraph) {
  return paragraph == NULL ? 0u : paragraph->script_runs.count;
}

const ZtextScriptRun* ztextParagraphScriptRuns(
    const ZtextParagraph* paragraph) {
  if (paragraph == NULL) return NULL;
  return (const ZtextScriptRun*)paragraph->script_runs.data;
}

size_t ztextParagraphShapingRunCount(const ZtextParagraph* paragraph) {
  return paragraph == NULL ? 0u : paragraph->shaping_runs.count;
}

const ZtextShapingRun* ztextParagraphShapingRuns(
    const ZtextParagraph* paragraph) {
  if (paragraph == NULL) return NULL;
  return (const ZtextShapingRun*)paragraph->shaping_runs.data;
}

//===----------------------------------------------------------------------===//
// Lines
//===----------------------------------------------------------------------===//

ZtextResult ztextLineCreate(const ZtextParagraph* paragraph, size_t offset,
                            size_t length, ZtextLine** out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  *out = NULL;
  if (paragraph == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;

  // Written as a subtraction so it cannot overflow, whatever the caller passed.
  if (offset > paragraph->length) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (length > paragraph->length - offset) return ZTEXT_RESULT_INVALID_ARGUMENT;

  // A range that splits a character would produce runs slicing one glyph's
  // bytes across two lines. Refused rather than reordered: it is always a
  // caller bug, and the paragraph holds the bytes needed to see it.
  if (ztextTextSplitsCharacter(paragraph->text, paragraph->length,
                               paragraph->encoding, offset) ||
      ztextTextSplitsCharacter(paragraph->text, paragraph->length,
                               paragraph->encoding, offset + length)) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  ZtextLine* line = ZTEXT_NEW(ZtextLine);
  if (line == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;
  line->offset = offset;
  line->length = length;

  // An empty line is legitimate and has no runs. It also never reaches
  // SheenBidi, which requires a non-empty range.
  if (length == 0u) {
    *out = line;
    return ZTEXT_RESULT_OK;
  }

  const ZtextResult installed = ztextInstallSheenbidiAllocator();
  if (installed != ZTEXT_RESULT_OK) {
    ztextLineDestroy(line);
    return installed;
  }

  const ZtextResult result = runsForRange(
      ztextAllocatorOf(line), paragraph->sb_paragraph, offset, length,
      (const ZtextScriptRun*)paragraph->script_runs.data,
      paragraph->script_runs.count, &line->visual_runs, &line->shaping_runs);
  if (result != ZTEXT_RESULT_OK) {
    ztextLineDestroy(line);
    return result;
  }

  *out = line;
  return ZTEXT_RESULT_OK;
}

void ztextLineDestroy(ZtextLine* line) {
  if (line == NULL) return;
  const ZtextAllocatorId owner = ztextAllocatorOf(line);
  ztextArrayFree(owner, &line->visual_runs, sizeof(ZtextVisualRun));
  ztextArrayFree(owner, &line->shaping_runs, sizeof(ZtextShapingRun));
  ztextFree(line);
}

size_t ztextLineOffset(const ZtextLine* line) {
  return line == NULL ? 0u : line->offset;
}

size_t ztextLineLength(const ZtextLine* line) {
  return line == NULL ? 0u : line->length;
}

size_t ztextLineVisualRunCount(const ZtextLine* line) {
  return line == NULL ? 0u : line->visual_runs.count;
}

const ZtextVisualRun* ztextLineVisualRuns(const ZtextLine* line) {
  if (line == NULL) return NULL;
  return (const ZtextVisualRun*)line->visual_runs.data;
}

size_t ztextLineShapingRunCount(const ZtextLine* line) {
  return line == NULL ? 0u : line->shaping_runs.count;
}

const ZtextShapingRun* ztextLineShapingRuns(const ZtextLine* line) {
  if (line == NULL) return NULL;
  return (const ZtextShapingRun*)line->shaping_runs.data;
}
