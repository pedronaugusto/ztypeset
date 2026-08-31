//===----------------------------------------------------------------------===//
// ztypeset -- the harness behind the numbers in README.md.
//
// It exists so those numbers are reproducible rather than asserted. Run it
// with `zig build bench -Doptimize=ReleaseFast -Dsanitize_c=false`; anything
// else measures the sanitiser.
//
// Written in C, against the C ABI, for one reason: `clock()` is in the C
// standard library on every platform ztypeset targets, and a portable monotonic
// clock in Zig is not. It reports CPU time, so a ratio between two rows here
// is meaningful even where the absolute numbers are not comparable across
// machines.
//
// Usage: ztypeset-bench <font.ttf>
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ztypeset.h"
#include "ztypeset_test_io.h"

static size_t live_bytes;
static size_t peak_bytes;

static void* benchAllocate(void* user, size_t size, size_t alignment) {
  (void)user;
  (void)alignment;
  void* block = malloc(size);
  if (block != NULL) {
    live_bytes += size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
  }
  return block;
}

static void benchDeallocate(void* user, void* block, size_t size,
                            size_t alignment) {
  (void)user;
  (void)alignment;
  live_bytes -= size;
  free(block);
}

static double microsPer(clock_t elapsed, long operations) {
  return (double)elapsed / (double)CLOCKS_PER_SEC * 1e6 / (double)operations;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s <font.ttf>\n", argv[0]);
    return 2;
  }
  size_t font_size = 0;
  unsigned char* font = ztypesetTestReadFile(argv[1], &font_size);
  if (font == NULL) {
    printf("could not read %s\n", argv[1]);
    return 2;
  }

  ZtypesetAllocator allocator = {benchAllocate, NULL, benchDeallocate, NULL};
  ztypesetSetAllocator(&allocator);

  ZtypesetLibrary* library = NULL;
  ZtypesetFont* the_font = NULL;
  ZtypesetFace* face = NULL;
  ZtypesetShaper* shaper = NULL;
  if (ztypesetLibraryCreate(&library) != ZTYPESET_RESULT_OK) return 1;
  if (ztypesetFontCreateFromMemory(library, font, font_size, 0,
                                &the_font) != ZTYPESET_RESULT_OK) {
    return 1;
  }
  if (ztypesetFaceCreate(the_font, 0, 32,
      &face) != ZTYPESET_RESULT_OK) return 1;
  if (ztypesetShaperCreate(&shaper) != ZTYPESET_RESULT_OK) return 1;

  printf("ztypeset bench -- %s at 32 px\n", argv[1]);
  printf("%-28s %12s  %s\n", "", "per op", "notes");

  //--------------------------------------------------------------------------
  // Shaping, warm.
  //--------------------------------------------------------------------------
  const char* sentence = "The quick brown fox jumps over the lazy dog";
  const size_t sentence_length = strlen(sentence);
  ZtypesetShapeParams params;
  memset(&params, 0, sizeof(params));
  params.direction = ZTYPESET_DIRECTION_LTR;
  params.script = ZTYPESET_TAG('L', 'a', 't', 'n');

  for (int i = 0; i < 32; i++) {
    ztypesetShaperShape(shaper, face, sentence, sentence_length,
                     ZTYPESET_ENCODING_UTF8, 0,
                         sentence_length, &params);
  }
  const long shape_runs = 20000;
  clock_t start = clock();
  for (long i = 0; i < shape_runs; i++) {
    ztypesetShaperShape(shaper, face, sentence, sentence_length,
                     ZTYPESET_ENCODING_UTF8, 0,
                         sentence_length, &params);
  }
  clock_t elapsed = clock() - start;
  printf("%-28s %9.2f us  %zu chars, %zu glyphs, reusing one shaper\n",
         "shape a run", microsPer(elapsed, shape_runs), sentence_length,
         ztypesetShaperGlyphCount(shaper));

  //--------------------------------------------------------------------------
  // The SAME run, in a longer text.
  //
  // HarfBuzz decodes the run itself plus at most five characters of context
  // either side (CONTEXT_LENGTH in libs/harfbuzz/src/hb-buffer.hh), so its
  // cost does not move with the length of the text around the run. Anything
  // that does move is ztypeset's own -- which is what this pair of numbers is
  // for. A paragraph is shaped one run at a time, so a per-call cost that
  // grows with the whole paragraph is paid once per run: quadratic in the
  // paragraph, invisible in a benchmark that shapes a single sentence.
  //--------------------------------------------------------------------------
  const size_t copies = 100u;
  char* long_text = (char*)malloc(sentence_length * copies + 1u);
  if (long_text == NULL) return 1;
  for (size_t i = 0; i < copies; i++) {
    memcpy(long_text + i * sentence_length, sentence, sentence_length);
  }
  const size_t long_length = sentence_length * copies;

  for (int i = 0; i < 32; i++) {
    ztypesetShaperShape(shaper, face, long_text, long_length,
                     ZTYPESET_ENCODING_UTF8, 0, sentence_length, &params);
  }
  start = clock();
  for (long i = 0; i < shape_runs; i++) {
    ztypesetShaperShape(shaper, face, long_text, long_length,
                     ZTYPESET_ENCODING_UTF8, 0, sentence_length, &params);
  }
  elapsed = clock() - start;
  printf("%-28s %9.2f us  the same %zu-char run, %zu chars around it\n",
         "shape a run, long text", microsPer(elapsed, shape_runs),
         sentence_length, long_length);

  // And the same run again, as a run OF A PARAGRAPH. Identical work inside
  // HarfBuzz; the difference is the walk ztypeset does not have to do, because
  // the paragraph validated this text once when it was created.
  ZtypesetParagraph* long_paragraph = NULL;
  if (ztypesetParagraphCreate(long_text, long_length, ZTYPESET_ENCODING_UTF8,
                           ZTYPESET_BASE_DIRECTION_LTR,
    ZTYPESET_SEGMENTATION_ALL,
                           &long_paragraph) != ZTYPESET_RESULT_OK) {
    return 1;
  }
  ZtypesetShapingRun bench_run;
  bench_run.offset = 0u;
  bench_run.length = (uint32_t)sentence_length;
  bench_run.script = ZTYPESET_TAG('L', 'a', 't', 'n');
  bench_run.level = 0u;
  ZtypesetShapeParams run_params;
  memset(&run_params, 0, sizeof(run_params));

  for (int i = 0; i < 32; i++) {
    ztypesetShaperShapeRun(shaper, face, long_paragraph, &bench_run,
                           &run_params);
  }
  start = clock();
  for (long i = 0; i < shape_runs; i++) {
    ztypesetShaperShapeRun(shaper, face, long_paragraph, &bench_run,
                           &run_params);
  }
  elapsed = clock() - start;
  printf("%-28s %9.2f us  the same run, from a %zu-char paragraph\n",
         "shape a paragraph run", microsPer(elapsed, shape_runs), long_length);

  ztypesetParagraphDestroy(long_paragraph);

  //--------------------------------------------------------------------------
  // Analysing a paragraph, and what the segmentation passes cost.
  //
  // Two arms over the same text, one variable: which of UAX #14 and #29 run.
  // The bidi analysis, the itemisation and the copy of the text are identical
  // in both, so the difference is libunibreak's three passes and the byte per
  // code unit each of them keeps.
  //--------------------------------------------------------------------------
  const long paragraph_rounds = 200;
  double paragraph_us[2];
  size_t paragraph_bytes[2];
  for (int arm = 0; arm < 2; arm++) {
    const uint32_t wanted =
        (arm == 0) ? (uint32_t)ZTYPESET_SEGMENTATION_ALL
                   : (uint32_t)ZTYPESET_SEGMENTATION_NONE;

    ZtypesetParagraph* measured_paragraph = NULL;
    const size_t before_paragraph = live_bytes;
    if (ztypesetParagraphCreate(long_text, long_length, ZTYPESET_ENCODING_UTF8,
                             ZTYPESET_BASE_DIRECTION_LTR, wanted,
                             &measured_paragraph) != ZTYPESET_RESULT_OK) {
      return 1;
    }
    paragraph_bytes[arm] = live_bytes - before_paragraph;
    ztypesetParagraphDestroy(measured_paragraph);

    start = clock();
    for (long i = 0; i < paragraph_rounds; i++) {
      ZtypesetParagraph* round = NULL;
      if (ztypesetParagraphCreate(long_text, long_length,
          ZTYPESET_ENCODING_UTF8,
                               ZTYPESET_BASE_DIRECTION_LTR, wanted,
                               &round) != ZTYPESET_RESULT_OK) {
        return 1;
      }
      ztypesetParagraphDestroy(round);
    }
    elapsed = clock() - start;
    paragraph_us[arm] = microsPer(elapsed, paragraph_rounds);
    printf("%-28s %9.2f us  %zu chars, %zu B live\n",
           (arm == 0) ? "paragraph, all breaks" : "paragraph, no breaks",
           paragraph_us[arm], long_length, paragraph_bytes[arm]);
  }
  if (paragraph_us[1] > 0.0) {
    printf("%-28s %9.1f%%  of a fully segmented paragraph's time, and %zu B "
           "of its memory\n",
           "segmentation cost",
           100.0 * (paragraph_us[0] - paragraph_us[1]) / paragraph_us[0],
           paragraph_bytes[0] - paragraph_bytes[1]);
  }

  free(long_text);

  //--------------------------------------------------------------------------
  // Rasterisation: coverage against distance field.
  //--------------------------------------------------------------------------
  static const uint32_t codepoints[] = {'a', 'b', 'c', 'g', 'o', 'W', 'M', '@'};
  const size_t glyph_count = sizeof(codepoints) / sizeof(codepoints[0]);
  uint32_t glyphs[8];
  for (size_t i = 0; i < glyph_count; i++) {
    glyphs[i] = ztypesetFontGlyphIndex(the_font, codepoints[i]);
  }

  double a8_us = 0.0;
  double sdf_us = 0.0;
  for (int mode = 0; mode < 2; mode++) {
    const ZtypesetRenderMode render =
        (mode == 0) ? ZTYPESET_RENDER_MODE_A8 : ZTYPESET_RENDER_MODE_SDF;
    // SDF is slow enough that a large count is simply a long wait.
    const long rounds = (mode == 0) ? 4000 : 40;
    ZtypesetGlyphBitmap bitmap;

    for (size_t i = 0; i < glyph_count; i++) {
      ztypesetFaceRenderGlyph(face, glyphs[i], render, ZTYPESET_HINTING_NONE, 0,
                              0,
                           &bitmap);
    }
    start = clock();
    for (long round = 0; round < rounds; round++) {
      for (size_t i = 0; i < glyph_count; i++) {
        ztypesetFaceRenderGlyph(face, glyphs[i], render, ZTYPESET_HINTING_NONE,
                                0, 0,
                             &bitmap);
      }
    }
    elapsed = clock() - start;
    const double per = microsPer(elapsed, rounds * (long)glyph_count);
    if (mode == 0) {
      a8_us = per;
      printf("%-28s %9.2f us  %ld glyphs, uncached\n", "rasterise A8", per,
             rounds * (long)glyph_count);
    } else {
      sdf_us = per;
      printf("%-28s %9.2f us  %ld glyphs, spread 8\n", "rasterise SDF", per,
             rounds * (long)glyph_count);
    }
  }
  if (a8_us > 0.0) {
    printf("%-28s %9.1fx  SDF relative to A8 -- bake once, never per frame\n",
           "SDF cost ratio", sdf_us / a8_us);
  }

  ztypesetFaceDestroy(face);

  //--------------------------------------------------------------------------
  // What one font at several sizes costs -- which is the number the Font/Face
  // split exists to change.
  //
  // Everything is shaped and rendered with before measuring, because a handle
  // that has never been used has not yet built the table accelerators, the
  // scaled state and the glyph buffers that dominate its footprint. Measuring
  // the bare handles would flatter the design.
  //--------------------------------------------------------------------------
  static const float sizes[] = {12.0f, 16.0f, 24.0f, 32.0f};
  ZtypesetGlyphBitmap bitmap;

  // The font on its own, paid once however many sizes follow.
  const size_t before_font = live_bytes;
  ZtypesetFont* measured = NULL;
  ztypesetFontCreateFromMemory(library, font, font_size, 0, &measured);
  ZtypesetFace* faces[4];
  ztypesetFaceCreate(measured, 0, sizes[0], &faces[0]);
  ztypesetShaperShape(shaper, faces[0], sentence, sentence_length,
                   ZTYPESET_ENCODING_UTF8, 0,
                       sentence_length, &params);
  ztypesetFaceRenderGlyph(faces[0], glyphs[0], ZTYPESET_RENDER_MODE_A8,
                       ZTYPESET_HINTING_NORMAL, 0, 0, &bitmap);
  const size_t font_and_one_face = live_bytes - before_font;

  // Three more sizes over the same font.
  for (size_t i = 1; i < 4; i++) {
    ztypesetFaceCreate(measured, 0, sizes[i], &faces[i]);
    ztypesetShaperShape(shaper, faces[i], sentence, sentence_length,
                     ZTYPESET_ENCODING_UTF8, 0,
                         sentence_length, &params);
    ztypesetFaceRenderGlyph(faces[i], glyphs[0], ZTYPESET_RENDER_MODE_A8,
                         ZTYPESET_HINTING_NORMAL, 0, 0, &bitmap);
  }
  const size_t four_sizes = live_bytes - before_font;
  printf("%-28s %9zu B   one font plus its first size\n", "font + one face",
         font_and_one_face);
  printf("%-28s %9zu B   %zu B per additional size\n", "one font, four sizes",
         four_sizes, (four_sizes - font_and_one_face) / 3u);
  for (size_t i = 0; i < 4; i++) ztypesetFaceDestroy(faces[i]);
  ztypesetFontDestroy(measured);

  //--------------------------------------------------------------------------
  // The other arm, without which the first is a number and not a comparison.
  //
  // README's memory table has two rows: what four sizes cost when they share
  // one parsed font, and what they cost when each size carries its own. The
  // second row had no arm here and was therefore uncited -- a figure with a
  // harness named beside it that the harness did not produce.
  //
  // Four independent Fonts over the same bytes is exactly the handle a
  // collapsed Font+Face design would give you: four full parses, four
  // hb_face_t, four FT_Face. Everything is shaped and rendered with, on the
  // same schedule as the shared-font arm, so the two rows differ in one thing
  // only.
  //--------------------------------------------------------------------------
  const size_t before_collapsed = live_bytes;
  ZtypesetFont* collapsed[4];
  ZtypesetFace* collapsed_faces[4];
  size_t collapsed_one = 0u;
  for (size_t i = 0; i < 4; i++) {
    collapsed[i] = NULL;
    collapsed_faces[i] = NULL;
    ztypesetFontCreateFromMemory(library, font, font_size, 0,
                              &collapsed[i]);
    ztypesetFaceCreate(collapsed[i], 0, sizes[i], &collapsed_faces[i]);
    ztypesetShaperShape(shaper, collapsed_faces[i], sentence,
                     sentence_length, ZTYPESET_ENCODING_UTF8,
                         0, sentence_length, &params);
    ztypesetFaceRenderGlyph(collapsed_faces[i], glyphs[0],
                            ZTYPESET_RENDER_MODE_A8,
                         ZTYPESET_HINTING_NORMAL, 0, 0, &bitmap);
    if (i == 0) collapsed_one = live_bytes - before_collapsed;
  }
  const size_t collapsed_four = live_bytes - before_collapsed;
  printf("%-28s %9zu B   one collapsed handle at one size\n",
         "collapsed handle, one size", collapsed_one);
  printf("%-28s %9zu B   %zu B per additional size\n",
         "collapsed handle, four sizes", collapsed_four,
         (collapsed_four - collapsed_one) / 3u);
  if (collapsed_four > four_sizes) {
    printf("%-28s %9.0f%%   of a collapsed size, saved by sharing the font\n",
           "per-size saving",
           100.0 * (double)((collapsed_four - collapsed_one) / 3u -
                            (four_sizes - font_and_one_face) / 3u) /
               (double)((collapsed_four - collapsed_one) / 3u));
  }
  for (size_t i = 0; i < 4; i++) {
    ztypesetFaceDestroy(collapsed_faces[i]);
    ztypesetFontDestroy(collapsed[i]);
  }

  ztypesetShaperDestroy(shaper);

  ztypesetFontDestroy(the_font);
  ztypesetLibraryDestroy(library);
  ztypesetSetAllocator(NULL);
  free(font);

  if (live_bytes != 0) {
    printf("bench leaked %zu bytes\n", live_bytes);
    return 1;
  }
  return 0;
}
