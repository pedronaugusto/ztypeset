//===----------------------------------------------------------------------===//
// ztext -- the harness behind the numbers in README.md.
//
// It exists so those numbers are reproducible rather than asserted. Run it
// with `zig build bench -Doptimize=ReleaseFast -Dsanitize_c=false`; anything
// else measures the sanitiser.
//
// Written in C, against the C ABI, for one reason: `clock()` is in the C
// standard library on every platform ztext targets, and a portable monotonic
// clock in Zig is not. It reports CPU time, so a ratio between two rows here
// is meaningful even where the absolute numbers are not comparable across
// machines.
//
// Usage: ztext-bench <font.ttf>
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ztext.h"

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
  FILE* file = fopen(argv[1], "rb");
  if (file == NULL) return 2;
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  unsigned char* font = (unsigned char*)malloc((size_t)file_size);
  if (font == NULL || fread(font, 1, (size_t)file_size, file) != (size_t)file_size) {
    return 2;
  }
  fclose(file);

  ztextWarmup();
  ZtextAllocator allocator = {benchAllocate, NULL, benchDeallocate, NULL};
  ztextSetAllocator(&allocator);

  ZtextLibrary* library = NULL;
  ZtextFont* the_font = NULL;
  ZtextFace* face = NULL;
  ZtextShaper* shaper = NULL;
  if (ztextLibraryCreate(&library) != ZTEXT_RESULT_OK) return 1;
  if (ztextFontCreateFromMemory(library, font, (size_t)file_size, 0,
                                &the_font) != ZTEXT_RESULT_OK) {
    return 1;
  }
  if (ztextFaceCreate(the_font, 0, 32, &face) != ZTEXT_RESULT_OK) return 1;
  if (ztextShaperCreate(&shaper) != ZTEXT_RESULT_OK) return 1;

  printf("ztext bench — %s at 32 px\n", argv[1]);
  printf("%-28s %12s  %s\n", "", "per op", "notes");

  //--------------------------------------------------------------------------
  // Shaping, warm.
  //--------------------------------------------------------------------------
  const char* sentence = "The quick brown fox jumps over the lazy dog";
  const size_t sentence_length = strlen(sentence);
  ZtextShapeParams params;
  memset(&params, 0, sizeof(params));
  params.direction = ZTEXT_DIRECTION_LTR;
  params.script = ZTEXT_TAG('L', 'a', 't', 'n');

  for (int i = 0; i < 32; i++) {
    ztextShaperShapeUtf8(shaper, face, sentence, sentence_length, 0,
                         sentence_length, &params);
  }
  const long shape_runs = 20000;
  clock_t start = clock();
  for (long i = 0; i < shape_runs; i++) {
    ztextShaperShapeUtf8(shaper, face, sentence, sentence_length, 0,
                         sentence_length, &params);
  }
  clock_t elapsed = clock() - start;
  printf("%-28s %9.2f us  %zu chars, %zu glyphs, reusing one shaper\n",
         "shape a run", microsPer(elapsed, shape_runs), sentence_length,
         ztextShaperGlyphCount(shaper));

  //--------------------------------------------------------------------------
  // Rasterisation: coverage against distance field.
  //--------------------------------------------------------------------------
  static const uint32_t codepoints[] = {'a', 'b', 'c', 'g', 'o', 'W', 'M', '@'};
  const size_t glyph_count = sizeof(codepoints) / sizeof(codepoints[0]);
  uint32_t glyphs[8];
  for (size_t i = 0; i < glyph_count; i++) {
    glyphs[i] = ztextFontGlyphIndex(the_font, codepoints[i]);
  }

  double a8_us = 0.0;
  double sdf_us = 0.0;
  for (int mode = 0; mode < 2; mode++) {
    const ZtextRenderMode render =
        (mode == 0) ? ZTEXT_RENDER_MODE_A8 : ZTEXT_RENDER_MODE_SDF;
    // SDF is slow enough that a large count is simply a long wait.
    const long rounds = (mode == 0) ? 4000 : 40;
    ZtextGlyphBitmap bitmap;

    for (size_t i = 0; i < glyph_count; i++) {
      ztextFaceRenderGlyph(face, glyphs[i], render, ZTEXT_HINTING_NONE, &bitmap);
    }
    start = clock();
    for (long round = 0; round < rounds; round++) {
      for (size_t i = 0; i < glyph_count; i++) {
        ztextFaceRenderGlyph(face, glyphs[i], render, ZTEXT_HINTING_NONE,
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

  ztextFaceDestroy(face);

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
  ZtextGlyphBitmap bitmap;

  // The font on its own, paid once however many sizes follow.
  const size_t before_font = live_bytes;
  ZtextFont* measured = NULL;
  ztextFontCreateFromMemory(library, font, (size_t)file_size, 0, &measured);
  ZtextFace* faces[4];
  ztextFaceCreate(measured, 0, sizes[0], &faces[0]);
  ztextShaperShapeUtf8(shaper, faces[0], sentence, sentence_length, 0,
                       sentence_length, &params);
  ztextFaceRenderGlyph(faces[0], glyphs[0], ZTEXT_RENDER_MODE_A8,
                       ZTEXT_HINTING_NORMAL, &bitmap);
  const size_t font_and_one_face = live_bytes - before_font;

  // Three more sizes over the same font.
  for (size_t i = 1; i < 4; i++) {
    ztextFaceCreate(measured, 0, sizes[i], &faces[i]);
    ztextShaperShapeUtf8(shaper, faces[i], sentence, sentence_length, 0,
                         sentence_length, &params);
    ztextFaceRenderGlyph(faces[i], glyphs[0], ZTEXT_RENDER_MODE_A8,
                         ZTEXT_HINTING_NORMAL, &bitmap);
  }
  const size_t four_sizes = live_bytes - before_font;
  printf("%-28s %9zu B   one font plus its first size\n", "font + one face",
         font_and_one_face);
  printf("%-28s %9zu B   %zu B per additional size\n", "one font, four sizes",
         four_sizes, (four_sizes - font_and_one_face) / 3u);
  for (size_t i = 0; i < 4; i++) ztextFaceDestroy(faces[i]);
  ztextFontDestroy(measured);
  ztextShaperDestroy(shaper);

  ztextFontDestroy(the_font);
  ztextLibraryDestroy(library);
  ztextSetAllocator(NULL);
  free(font);

  if (live_bytes != 0) {
    printf("bench leaked %zu bytes\n", live_bytes);
    return 1;
  }
  return 0;
}
