//===----------------------------------------------------------------------===//
// ztypeset -- every entry point, called with nothing.
//
// ztypeset's contract is that no entry point crashes on a NULL handle: a
// destructor accepts one, an accessor answers zero or NULL, and anything
// returning a ZtypesetResult rejects it. That contract was previously asserted
// only where some other test happened to touch it, which for a sixty-function
// surface means most of it was asserted nowhere at all.
//
// This calls all of them, twice: once with NULL handles and zeroed
// out-parameters, and once with a real handle but a NULL out-parameter. Both
// are the shapes a C host produces by accident -- an allocation that failed
// two lines up, a struct field never initialised.
//
// It is a separate translation unit from c_smoke.c because it is a sweep and
// not a scenario: c_smoke drives the library the way a consumer would, this
// one drives it the way nobody should.
//
// STAYING COMPLETE: ci/api-surface.sh --sweep fails if ffi/ztypeset.h declares
// an entry point this file never names. Adding a function to the header without
// adding it here is a build-visible omission rather than a silent hole.
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ztypeset.h"
#include "ztypeset_test_io.h"

static int failures = 0;

#define CHECK(cond, ...)                             \
  do {                                               \
    if (!(cond)) {                                   \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);  \
      printf(__VA_ARGS__);                           \
      printf("\n");                                  \
      failures++;                                    \
    }                                                \
  } while (0)

/// Every function returning ZtypesetResult must refuse a NULL handle rather
/// than dereference it, and must not report success.
#define REFUSES(call) \
  CHECK((call) != ZTYPESET_RESULT_OK, "%s accepted NULL", #call)

/// A function whose out-parameter is NULL has nowhere to put an answer.
#define REFUSES_OUT(call)                              \
  CHECK((call) == ZTYPESET_RESULT_INVALID_ARGUMENT,       \
        "%s accepted a NULL out", #call)

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s <font.ttf>\n", argv[0]);
    return 2;
  }

  //--------------------------------------------------------------------------
  // Destructors, first and last. A NULL is documented to be accepted, and
  // calling them here also means the rest of this file runs after a
  // double-free would have shown up.
  //--------------------------------------------------------------------------
  ztypesetLibraryDestroy(NULL);
  ztypesetFontDestroy(NULL);
  ztypesetFaceDestroy(NULL);
  ztypesetShaperDestroy(NULL);
  ztypesetParagraphDestroy(NULL);
  ztypesetLineDestroy(NULL);

  //--------------------------------------------------------------------------
  // Free functions, which have no handle to be NULL but do have out-params.
  //--------------------------------------------------------------------------
  ztypesetWarmup();
  (void)ztypesetVersion();
  (void)ztypesetFreetypeVersion();
  (void)ztypesetHarfbuzzVersion();
  (void)ztypesetSheenbidiVersion();
  (void)ztypesetUnibreakVersion();
  CHECK(ztypesetResultName(ZTYPESET_RESULT_OK) != NULL, "result name was NULL");
  CHECK(ztypesetLastErrorDetail() != NULL, "error detail was NULL");
  // NULL is not an error here: it is the documented way to put the default
  // allocator back. Asserted rather than skipped, because "resets" and
  // "refuses" are both plausible readings of a NULL and only one is true.
  CHECK(ztypesetSetAllocator(NULL) == ZTYPESET_RESULT_OK,
        "ztypesetSetAllocator(NULL) should reset to the default");
  ZtypesetAllocator broken;
  memset(&broken, 0, sizeof(broken));
  REFUSES(ztypesetSetAllocator(&broken));

  ZtypesetAbiLayout layout;
  memset(&layout, 0, sizeof(layout));
  ztypesetAbiLayout(&layout);
  CHECK(layout.layout_size == sizeof(ZtypesetAbiLayout),
        "layout size disagrees");
  ztypesetAbiLayout(NULL);
  ZtypesetAbiProbe probe;
  memset(&probe, 0, sizeof(probe));
  ztypesetAbiProbe(&probe);
  ztypesetAbiProbe(NULL);

  //--------------------------------------------------------------------------
  // Library.
  //--------------------------------------------------------------------------
  REFUSES_OUT(ztypesetLibraryCreate(NULL));
  REFUSES(ztypesetLibrarySetSdfSpread(NULL, 8));

  uint32_t count = 12345u;
  REFUSES(ztypesetLibraryCountFaces(NULL, "x", 1, &count));
  CHECK(count == 0u, "a refused count still wrote its out-parameter");
  REFUSES_OUT(ztypesetLibraryCountFaces(NULL, "x", 1, NULL));

  //--------------------------------------------------------------------------
  // Fonts.
  //--------------------------------------------------------------------------
  ZtypesetFont* font = NULL;
  REFUSES(ztypesetFontCreateFromMemory(NULL, "x", 1, 0, &font));
  CHECK(font == NULL, "a refused font create wrote a handle");
  REFUSES_OUT(ztypesetFontCreateFromMemory(NULL, "x", 1, 0, NULL));

  CHECK(strcmp(ztypesetFontFamilyName(NULL), "") == 0, "family name of NULL");
  CHECK(strcmp(ztypesetFontStyleName(NULL), "") == 0, "style name of NULL");
  CHECK(ztypesetFontGlyphIndex(NULL, 'a') == 0u, "glyph index of NULL");
  CHECK(ztypesetFontVariantGlyphIndex(NULL, 'a', 0xFE00u) == 0u,
        "variant glyph index of NULL");
  CHECK(ztypesetFontGlyphCount(NULL) == 0u, "glyph count of NULL");
  CHECK(ztypesetFontUnitsPerEm(NULL) == 0u, "units per em of NULL");
  CHECK(ztypesetFontAxisCount(NULL) == 0u, "axis count of NULL");
  CHECK(ztypesetFontCharmapCount(NULL) == 0u, "charmap count of NULL");
  CHECK(ztypesetFontActiveCharmap(NULL) == ZTYPESET_CHARMAP_INDEX_NONE,
        "active charmap of NULL");

  ZtypesetCharmap charmap;
  memset(&charmap, 0xAB, sizeof(charmap));
  REFUSES(ztypesetFontCharmap(NULL, 0u, &charmap));
  CHECK(charmap.encoding == 0u, "a refused charmap query wrote a value");
  REFUSES_OUT(ztypesetFontCharmap(NULL, 0u, NULL));
  REFUSES(ztypesetFontSelectCharmap(NULL, 0u));
  REFUSES(ztypesetFontSelectCharmapEncoding(NULL, ZTYPESET_CHARMAP_UNICODE));

  size_t covered = 999u;
  REFUSES(ztypesetFontCoveredPrefix(NULL, "abc", 3, ZTYPESET_ENCODING_UTF8,
                                 &covered));
  CHECK(covered == 0u, "a refused covered prefix wrote its out-parameter");
  REFUSES_OUT(ztypesetFontCoveredPrefix(NULL, "abc", 3, ZTYPESET_ENCODING_UTF8,
                                     NULL));

  ZtypesetVariationAxis axis;
  memset(&axis, 0, sizeof(axis));
  REFUSES(ztypesetFontAxis(NULL, 0, &axis));
  REFUSES_OUT(ztypesetFontAxis(NULL, 0, NULL));
  float value = 0.0f;
  REFUSES(ztypesetFontVariation(NULL, 0, &value));
  REFUSES_OUT(ztypesetFontVariation(NULL, 0, NULL));
  REFUSES(ztypesetFontSetVariations(NULL, NULL, 0));

  CHECK(ztypesetFontNamedInstanceCount(NULL) == 0u,
        "named instance count of NULL");
  float instance_coords[4];
  memset(instance_coords, 0, sizeof(instance_coords));
  size_t instance_count = 4u;
  REFUSES(ztypesetFontNamedInstanceCoords(NULL, 0, instance_coords,
                                       &instance_count));
  CHECK(instance_count == 0u,
        "a refused instance-coords call wrote its out-parameter");
  REFUSES_OUT(ztypesetFontNamedInstanceCoords(NULL, 0, instance_coords, NULL));
  char instance_name[32];
  memset(instance_name, 0, sizeof(instance_name));
  size_t instance_size = sizeof(instance_name);
  REFUSES(ztypesetFontNamedInstanceName(NULL, 0, instance_name,
          &instance_size));
  CHECK(instance_size == 0u,
        "a refused instance-name call wrote its out-parameter");
  REFUSES_OUT(ztypesetFontNamedInstanceName(NULL, 0, instance_name, NULL));
  REFUSES(ztypesetFontSetNamedInstance(NULL, 0));

  //--------------------------------------------------------------------------
  // Faces.
  //--------------------------------------------------------------------------
  ZtypesetFace* face = NULL;
  REFUSES(ztypesetFaceCreate(NULL, 0, 16, &face));
  CHECK(face == NULL, "a refused face create wrote a handle");
  REFUSES_OUT(ztypesetFaceCreate(NULL, 0, 16, NULL));
  CHECK(ztypesetFaceFont(NULL) == NULL, "face font of NULL");
  REFUSES(ztypesetFaceSetPixelSize(NULL, 0, 16));

  ZtypesetFaceMetrics metrics;
  memset(&metrics, 0, sizeof(metrics));
  REFUSES(ztypesetFaceMetrics(NULL, &metrics));
  REFUSES_OUT(ztypesetFaceMetrics(NULL, NULL));

  float metric = 1.0f;
  REFUSES(ztypesetFaceMetric(NULL, ZTYPESET_METRIC_X_HEIGHT, &metric));
  CHECK(metric == 0.0f, "a refused metric read wrote its out-parameter");
  REFUSES_OUT(ztypesetFaceMetric(NULL, ZTYPESET_METRIC_X_HEIGHT, NULL));
  metric = 1.0f;
  REFUSES(ztypesetFaceMetricWithFallback(NULL, ZTYPESET_METRIC_X_HEIGHT,
          &metric));
  CHECK(metric == 0.0f, "a refused fallback metric wrote its out-parameter");
  REFUSES_OUT(ztypesetFaceMetricWithFallback(NULL, ZTYPESET_METRIC_X_HEIGHT,
              NULL));

  ZtypesetGlyphBitmap bitmap;
  memset(&bitmap, 0, sizeof(bitmap));
  REFUSES(ztypesetFaceRenderGlyph(NULL, 1, ZTYPESET_RENDER_MODE_A8,
                               ZTYPESET_HINTING_NORMAL, 0, 0, &bitmap));
  CHECK(bitmap.pixels == NULL, "a refused render wrote pixels");
  REFUSES_OUT(ztypesetFaceRenderGlyph(NULL, 1, ZTYPESET_RENDER_MODE_A8,
                                   ZTYPESET_HINTING_NORMAL, 0, 0, NULL));
  // Not a handle call at all, so there is no NULL to pass -- what it must
  // never do is answer for a value this build does not name.
  CHECK(ztypesetBitmapFormatChannels((ZtypesetBitmapFormat)999) == 0u,
        "channels of an unnamed bitmap format");

  ZtypesetStroke stroke;
  memset(&stroke, 0xAB, sizeof(stroke));
  REFUSES(ztypesetFaceSetStroke(NULL, NULL));
  REFUSES(ztypesetFaceStroke(NULL, &stroke));
  CHECK(stroke.radius == 0.0f, "a refused stroke query wrote a value");
  REFUSES_OUT(ztypesetFaceStroke(NULL, NULL));

  ZtypesetMatrix matrix;
  memset(&matrix, 0xAB, sizeof(matrix));
  REFUSES(ztypesetFaceSetTransform(NULL, NULL));
  REFUSES(ztypesetFaceTransform(NULL, &matrix));
  CHECK(matrix.xx == 0.0f, "a refused transform query wrote a value");
  REFUSES_OUT(ztypesetFaceTransform(NULL, NULL));
  REFUSES(ztypesetFaceSetSyntheticBold(NULL, ZTYPESET_SYNTHETIC_BOLD_DEFAULT));
  REFUSES(ztypesetFaceSetSyntheticOblique(NULL,
          ZTYPESET_SYNTHETIC_OBLIQUE_DEFAULT));

  ZtypesetExtents extents;
  memset(&extents, 0, sizeof(extents));
  REFUSES(ztypesetFaceGlyphExtents(NULL, 1, ZTYPESET_HINTING_NORMAL, &extents));
  REFUSES_OUT(ztypesetFaceGlyphExtents(NULL, 1, ZTYPESET_HINTING_NORMAL, NULL));

  ZtypesetOutlineFuncs outline_funcs;
  memset(&outline_funcs, 0, sizeof(outline_funcs));
  REFUSES(ztypesetFaceDecomposeOutline(NULL, 1, ZTYPESET_HINTING_NORMAL,
                                    &outline_funcs));
  REFUSES(ztypesetFaceDecomposeOutline(NULL, 1, ZTYPESET_HINTING_NORMAL, NULL));

  //--------------------------------------------------------------------------
  // Shaper.
  //--------------------------------------------------------------------------
  ZtypesetShaper* shaper = NULL;
  REFUSES_OUT(ztypesetShaperCreate(NULL));

  ZtypesetShapeParams params;
  memset(&params, 0, sizeof(params));
  REFUSES(ztypesetShaperShape(NULL, NULL, "x", 1, ZTYPESET_ENCODING_UTF8, 0, 1,
                           &params));
  ZtypesetShapingRun null_run;
  memset(&null_run, 0, sizeof(null_run));
  REFUSES(ztypesetShaperShapeRun(NULL, NULL, NULL, &null_run, &params));
  CHECK(ztypesetShaperGlyphCount(NULL) == 0u, "glyph count of NULL shaper");
  CHECK(ztypesetShaperGlyphs(NULL) == NULL, "glyphs of NULL shaper");
  CHECK(ztypesetShaperDirection(NULL) == ZTYPESET_DIRECTION_AUTO,
        "direction of NULL shaper");
  REFUSES(ztypesetShaperExtents(NULL, NULL, &extents));
  REFUSES_OUT(ztypesetShaperExtents(NULL, NULL, NULL));

  //--------------------------------------------------------------------------
  // Paragraphs and lines.
  //--------------------------------------------------------------------------
  ZtypesetParagraph* paragraph = NULL;
  REFUSES_OUT(ztypesetParagraphCreate("x", 1, ZTYPESET_ENCODING_UTF8,
                                   ZTYPESET_BASE_DIRECTION_AUTO,
                                   ZTYPESET_SEGMENTATION_ALL, NULL));
  CHECK(ztypesetParagraphLength(NULL) == 0u, "length of NULL paragraph");
  CHECK(ztypesetParagraphSegmentation(NULL) == ZTYPESET_SEGMENTATION_NONE,
        "segmentation of NULL paragraph");
  CHECK(ztypesetParagraphEncoding(NULL) == ZTYPESET_ENCODING_UTF8,
        "encoding of NULL paragraph");
  CHECK(ztypesetParagraphBaseLevel(NULL) == 0u, "base level of NULL paragraph");
  CHECK(ztypesetParagraphLevels(NULL) == NULL, "levels of NULL paragraph");
  CHECK(ztypesetParagraphVisualRunCount(NULL) == 0u,
        "visual run count of NULL");
  CHECK(ztypesetParagraphVisualRuns(NULL) == NULL, "visual runs of NULL");
  CHECK(ztypesetParagraphScriptRunCount(NULL) == 0u,
        "script run count of NULL");
  CHECK(ztypesetParagraphScriptRuns(NULL) == NULL, "script runs of NULL");
  CHECK(ztypesetParagraphShapingRunCount(NULL) == 0u,
        "shaping run count of NULL");
  CHECK(ztypesetParagraphShapingRuns(NULL) == NULL, "shaping runs of NULL");
  CHECK(ztypesetParagraphLineBreaks(NULL) == NULL, "line breaks of NULL");
  CHECK(ztypesetParagraphGraphemeBreaks(NULL) == NULL,
        "grapheme breaks of NULL");
  CHECK(ztypesetParagraphWordBreaks(NULL) == NULL, "word breaks of NULL");
  CHECK(ztypesetParagraphNextGrapheme(NULL, 0) == 0u, "next grapheme of NULL");
  CHECK(ztypesetParagraphPreviousGrapheme(NULL, 0) == 0u,
        "prev grapheme of NULL");

  ZtypesetLine* line = NULL;
  REFUSES(ztypesetLineCreate(NULL, 0, 0, &line));
  CHECK(line == NULL, "a refused line create wrote a handle");
  REFUSES_OUT(ztypesetLineCreate(NULL, 0, 0, NULL));
  CHECK(ztypesetLineOffset(NULL) == 0u, "offset of NULL line");
  CHECK(ztypesetLineLength(NULL) == 0u, "length of NULL line");
  CHECK(ztypesetLineVisualRunCount(NULL) == 0u,
        "visual run count of NULL line");
  CHECK(ztypesetLineVisualRuns(NULL) == NULL, "visual runs of NULL line");
  CHECK(ztypesetLineShapingRunCount(NULL) == 0u,
        "shaping run count of NULL line");
  CHECK(ztypesetLineShapingRuns(NULL) == NULL, "shaping runs of NULL line");

  //--------------------------------------------------------------------------
  // The other half: real handles, NULL out-parameters. A host whose allocation
  // failed two lines up produces exactly this, and the handle being valid is
  // what makes it a different code path from everything above.
  //--------------------------------------------------------------------------
  ZtypesetLibrary* library = NULL;
  if (ztypesetLibraryCreate(&library) != ZTYPESET_RESULT_OK) {
    printf("  FAIL could not create a library\n");
    return 1;
  }

  size_t font_bytes = 0;
  unsigned char* bytes = ztypesetTestReadFile(argv[1], &font_bytes);
  if (bytes == NULL) {
    printf("  FAIL could not read %s\n", argv[1]);
    return 2;
  }

  REFUSES_OUT(ztypesetFontCreateFromMemory(library, bytes, font_bytes, 0,
              NULL));
  REFUSES(ztypesetFontCreateFromMemory(library, NULL, font_bytes, 0, &font));
  REFUSES(ztypesetFontCreateFromMemory(library, bytes, 0, 0, &font));
  if (ztypesetFontCreateFromMemory(library, bytes, font_bytes, 0, &font) !=
      ZTYPESET_RESULT_OK) {
    printf("  FAIL could not create a font\n");
    return 1;
  }

  REFUSES_OUT(ztypesetFaceCreate(font, 0, 16, NULL));
  REFUSES_OUT(ztypesetFontCoveredPrefix(font, "abc", 3, ZTYPESET_ENCODING_UTF8,
                                     NULL));
  REFUSES(ztypesetFontCoveredPrefix(font, NULL, 3, ZTYPESET_ENCODING_UTF8,
                                 &covered));
  REFUSES_OUT(ztypesetFontAxis(font, 0, NULL));
  REFUSES_OUT(ztypesetFontVariation(font, 0, NULL));
  REFUSES(ztypesetFontSetVariations(font, NULL, 3));
  REFUSES_OUT(ztypesetFontNamedInstanceCoords(font, 0, instance_coords, NULL));
  REFUSES_OUT(ztypesetFontNamedInstanceName(font, 0, instance_name, NULL));

  if (ztypesetFaceCreate(font, 0, 16, &face) != ZTYPESET_RESULT_OK) {
    printf("  FAIL could not create a face\n");
    return 1;
  }
  REFUSES_OUT(ztypesetFaceMetrics(face, NULL));
  REFUSES_OUT(ztypesetFaceMetric(face, ZTYPESET_METRIC_X_HEIGHT, NULL));
  REFUSES_OUT(ztypesetFaceMetricWithFallback(face, ZTYPESET_METRIC_X_HEIGHT,
              NULL));
  REFUSES_OUT(ztypesetFaceRenderGlyph(face, 1, ZTYPESET_RENDER_MODE_A8,
                                   ZTYPESET_HINTING_NORMAL, 0, 0, NULL));
  REFUSES_OUT(ztypesetFaceGlyphExtents(face, 1, ZTYPESET_HINTING_NORMAL, NULL));
  REFUSES(ztypesetFaceDecomposeOutline(face, 1, ZTYPESET_HINTING_NORMAL, NULL));

  if (ztypesetShaperCreate(&shaper) != ZTYPESET_RESULT_OK) {
    printf("  FAIL could not create a shaper\n");
    return 1;
  }
  REFUSES(ztypesetShaperShape(shaper, face, "x", 1, ZTYPESET_ENCODING_UTF8, 0,
          1,
                           NULL));
  REFUSES(ztypesetShaperShape(shaper, NULL, "x", 1, ZTYPESET_ENCODING_UTF8, 0,
          1,
                           &params));
  REFUSES(ztypesetShaperShape(shaper, face, NULL, 1, ZTYPESET_ENCODING_UTF8, 0,
          1,
                           &params));
  REFUSES(ztypesetShaperShapeRun(shaper, face, NULL, &null_run, &params));
  REFUSES(ztypesetShaperShapeRun(shaper, NULL, NULL, &null_run, &params));
  REFUSES(ztypesetShaperShapeRun(shaper, face, NULL, NULL, &params));
  REFUSES(ztypesetShaperShapeRun(shaper, face, NULL, &null_run, NULL));
  REFUSES_OUT(ztypesetShaperExtents(shaper, face, NULL));

  if (ztypesetParagraphCreate("abc", 3, ZTYPESET_ENCODING_UTF8,
                           ZTYPESET_BASE_DIRECTION_AUTO,
    ZTYPESET_SEGMENTATION_ALL,
                           &paragraph) != ZTYPESET_RESULT_OK) {
    printf("  FAIL could not create a paragraph\n");
    return 1;
  }
  REFUSES_OUT(ztypesetLineCreate(paragraph, 0, 3, NULL));

  ztypesetParagraphDestroy(paragraph);
  ztypesetShaperDestroy(shaper);
  ztypesetFaceDestroy(face);
  ztypesetFontDestroy(font);
  ztypesetLibraryDestroy(library);
  free(bytes);

  if (failures != 0) {
    printf("null sweep: %d FAILED\n", failures);
    return 1;
  }
  printf("null sweep: ok\n");
  return 0;
}
