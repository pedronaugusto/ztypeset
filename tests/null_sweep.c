//===----------------------------------------------------------------------===//
// ztext -- every entry point, called with nothing.
//
// ztext's contract is that no entry point crashes on a NULL handle: a
// destructor accepts one, an accessor answers zero or NULL, and anything
// returning a ZtextResult rejects it. That contract was previously asserted
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
// STAYING COMPLETE: ci/api-surface.sh --sweep fails if ffi/ztext.h declares an
// entry point this file never names. Adding a function to the header without
// adding it here is a build-visible omission rather than a silent hole.
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ztext.h"

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

/// Every function returning ZtextResult must refuse a NULL handle rather than
/// dereference it, and must not report success.
#define REFUSES(call) \
  CHECK((call) != ZTEXT_RESULT_OK, "%s accepted NULL", #call)

/// A function whose out-parameter is NULL has nowhere to put an answer.
#define REFUSES_OUT(call)                              \
  CHECK((call) == ZTEXT_RESULT_INVALID_ARGUMENT,       \
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
  ztextLibraryDestroy(NULL);
  ztextFontDestroy(NULL);
  ztextFaceDestroy(NULL);
  ztextShaperDestroy(NULL);
  ztextParagraphDestroy(NULL);
  ztextLineDestroy(NULL);

  //--------------------------------------------------------------------------
  // Free functions, which have no handle to be NULL but do have out-params.
  //--------------------------------------------------------------------------
  ztextWarmup();
  (void)ztextVersion();
  (void)ztextFreetypeVersion();
  (void)ztextHarfbuzzVersion();
  (void)ztextSheenbidiVersion();
  (void)ztextUnibreakVersion();
  CHECK(ztextResultName(ZTEXT_RESULT_OK) != NULL, "result name was NULL");
  CHECK(ztextLastErrorDetail() != NULL, "error detail was NULL");
  // NULL is not an error here: it is the documented way to put the default
  // allocator back. Asserted rather than skipped, because "resets" and
  // "refuses" are both plausible readings of a NULL and only one is true.
  CHECK(ztextSetAllocator(NULL) == ZTEXT_RESULT_OK,
        "ztextSetAllocator(NULL) should reset to the default");
  ZtextAllocator broken;
  memset(&broken, 0, sizeof(broken));
  REFUSES(ztextSetAllocator(&broken));

  ZtextAbiLayout layout;
  memset(&layout, 0, sizeof(layout));
  ztextAbiLayout(&layout);
  CHECK(layout.layout_size == sizeof(ZtextAbiLayout), "layout size disagrees");
  ztextAbiLayout(NULL);
  ZtextAbiProbe probe;
  memset(&probe, 0, sizeof(probe));
  ztextAbiProbe(&probe);
  ztextAbiProbe(NULL);

  //--------------------------------------------------------------------------
  // Library.
  //--------------------------------------------------------------------------
  REFUSES_OUT(ztextLibraryCreate(NULL));
  REFUSES(ztextLibrarySetSdfSpread(NULL, 8));

  uint32_t count = 12345u;
  REFUSES(ztextLibraryCountFaces(NULL, "x", 1, &count));
  CHECK(count == 0u, "a refused count still wrote its out-parameter");
  REFUSES_OUT(ztextLibraryCountFaces(NULL, "x", 1, NULL));

  //--------------------------------------------------------------------------
  // Fonts.
  //--------------------------------------------------------------------------
  ZtextFont* font = NULL;
  REFUSES(ztextFontCreateFromMemory(NULL, "x", 1, 0, &font));
  CHECK(font == NULL, "a refused font create wrote a handle");
  REFUSES_OUT(ztextFontCreateFromMemory(NULL, "x", 1, 0, NULL));

  CHECK(strcmp(ztextFontFamilyName(NULL), "") == 0, "family name of NULL");
  CHECK(strcmp(ztextFontStyleName(NULL), "") == 0, "style name of NULL");
  CHECK(ztextFontGlyphIndex(NULL, 'a') == 0u, "glyph index of NULL");
  CHECK(ztextFontVariantGlyphIndex(NULL, 'a', 0xFE00u) == 0u,
        "variant glyph index of NULL");
  CHECK(ztextFontGlyphCount(NULL) == 0u, "glyph count of NULL");
  CHECK(ztextFontUnitsPerEm(NULL) == 0u, "units per em of NULL");
  CHECK(ztextFontAxisCount(NULL) == 0u, "axis count of NULL");

  size_t covered = 999u;
  REFUSES(ztextFontCoveredPrefix(NULL, "abc", 3, &covered));
  CHECK(covered == 0u, "a refused covered prefix wrote its out-parameter");
  REFUSES_OUT(ztextFontCoveredPrefix(NULL, "abc", 3, NULL));

  ZtextVariationAxis axis;
  memset(&axis, 0, sizeof(axis));
  REFUSES(ztextFontAxis(NULL, 0, &axis));
  REFUSES_OUT(ztextFontAxis(NULL, 0, NULL));
  float value = 0.0f;
  REFUSES(ztextFontVariation(NULL, 0, &value));
  REFUSES_OUT(ztextFontVariation(NULL, 0, NULL));
  REFUSES(ztextFontSetVariations(NULL, NULL, 0));

  CHECK(ztextFontNamedInstanceCount(NULL) == 0u, "named instance count of NULL");
  float instance_coords[4];
  memset(instance_coords, 0, sizeof(instance_coords));
  size_t instance_count = 4u;
  REFUSES(ztextFontNamedInstanceCoords(NULL, 0, instance_coords,
                                       &instance_count));
  CHECK(instance_count == 0u,
        "a refused instance-coords call wrote its out-parameter");
  REFUSES_OUT(ztextFontNamedInstanceCoords(NULL, 0, instance_coords, NULL));
  char instance_name[32];
  memset(instance_name, 0, sizeof(instance_name));
  size_t instance_size = sizeof(instance_name);
  REFUSES(ztextFontNamedInstanceName(NULL, 0, instance_name, &instance_size));
  CHECK(instance_size == 0u,
        "a refused instance-name call wrote its out-parameter");
  REFUSES_OUT(ztextFontNamedInstanceName(NULL, 0, instance_name, NULL));
  REFUSES(ztextFontSetNamedInstance(NULL, 0));

  //--------------------------------------------------------------------------
  // Faces.
  //--------------------------------------------------------------------------
  ZtextFace* face = NULL;
  REFUSES(ztextFaceCreate(NULL, 0, 16, &face));
  CHECK(face == NULL, "a refused face create wrote a handle");
  REFUSES_OUT(ztextFaceCreate(NULL, 0, 16, NULL));
  CHECK(ztextFaceFont(NULL) == NULL, "face font of NULL");
  REFUSES(ztextFaceSetPixelSize(NULL, 0, 16));

  ZtextFaceMetrics metrics;
  memset(&metrics, 0, sizeof(metrics));
  REFUSES(ztextFaceMetrics(NULL, &metrics));
  REFUSES_OUT(ztextFaceMetrics(NULL, NULL));

  float metric = 1.0f;
  REFUSES(ztextFaceMetric(NULL, ZTEXT_METRIC_X_HEIGHT, &metric));
  CHECK(metric == 0.0f, "a refused metric read wrote its out-parameter");
  REFUSES_OUT(ztextFaceMetric(NULL, ZTEXT_METRIC_X_HEIGHT, NULL));
  metric = 1.0f;
  REFUSES(ztextFaceMetricWithFallback(NULL, ZTEXT_METRIC_X_HEIGHT, &metric));
  CHECK(metric == 0.0f, "a refused fallback metric wrote its out-parameter");
  REFUSES_OUT(ztextFaceMetricWithFallback(NULL, ZTEXT_METRIC_X_HEIGHT, NULL));

  ZtextGlyphBitmap bitmap;
  memset(&bitmap, 0, sizeof(bitmap));
  REFUSES(ztextFaceRenderGlyph(NULL, 1, ZTEXT_RENDER_MODE_A8,
                               ZTEXT_HINTING_NORMAL, 0, 0, &bitmap));
  CHECK(bitmap.pixels == NULL, "a refused render wrote pixels");
  REFUSES_OUT(ztextFaceRenderGlyph(NULL, 1, ZTEXT_RENDER_MODE_A8,
                                   ZTEXT_HINTING_NORMAL, 0, 0, NULL));
  REFUSES(ztextFaceSetSyntheticBold(NULL, 1));
  REFUSES(ztextFaceSetSyntheticOblique(NULL, 1));

  ZtextExtents extents;
  memset(&extents, 0, sizeof(extents));
  REFUSES(ztextFaceGlyphExtents(NULL, 1, ZTEXT_HINTING_NORMAL, &extents));
  REFUSES_OUT(ztextFaceGlyphExtents(NULL, 1, ZTEXT_HINTING_NORMAL, NULL));

  ZtextOutlineFuncs outline_funcs;
  memset(&outline_funcs, 0, sizeof(outline_funcs));
  REFUSES(ztextFaceDecomposeOutline(NULL, 1, ZTEXT_HINTING_NORMAL,
                                    &outline_funcs));
  REFUSES(ztextFaceDecomposeOutline(NULL, 1, ZTEXT_HINTING_NORMAL, NULL));

  //--------------------------------------------------------------------------
  // Shaper.
  //--------------------------------------------------------------------------
  ZtextShaper* shaper = NULL;
  REFUSES_OUT(ztextShaperCreate(NULL));

  ZtextShapeParams params;
  memset(&params, 0, sizeof(params));
  REFUSES(ztextShaperShapeUtf8(NULL, NULL, "x", 1, 0, 1, &params));
  CHECK(ztextShaperGlyphCount(NULL) == 0u, "glyph count of NULL shaper");
  CHECK(ztextShaperGlyphs(NULL) == NULL, "glyphs of NULL shaper");
  CHECK(ztextShaperDirection(NULL) == ZTEXT_DIRECTION_AUTO,
        "direction of NULL shaper");
  REFUSES(ztextShaperExtents(NULL, NULL, &extents));
  REFUSES_OUT(ztextShaperExtents(NULL, NULL, NULL));

  //--------------------------------------------------------------------------
  // Paragraphs and lines.
  //--------------------------------------------------------------------------
  ZtextParagraph* paragraph = NULL;
  REFUSES_OUT(
      ztextParagraphCreateUtf8("x", 1, ZTEXT_BASE_DIRECTION_AUTO, NULL));
  CHECK(ztextParagraphLength(NULL) == 0u, "length of NULL paragraph");
  CHECK(ztextParagraphBaseLevel(NULL) == 0u, "base level of NULL paragraph");
  CHECK(ztextParagraphLevels(NULL) == NULL, "levels of NULL paragraph");
  CHECK(ztextParagraphVisualRunCount(NULL) == 0u, "visual run count of NULL");
  CHECK(ztextParagraphVisualRuns(NULL) == NULL, "visual runs of NULL");
  CHECK(ztextParagraphScriptRunCount(NULL) == 0u, "script run count of NULL");
  CHECK(ztextParagraphScriptRuns(NULL) == NULL, "script runs of NULL");
  CHECK(ztextParagraphShapingRunCount(NULL) == 0u, "shaping run count of NULL");
  CHECK(ztextParagraphShapingRuns(NULL) == NULL, "shaping runs of NULL");
  CHECK(ztextParagraphLineBreaks(NULL) == NULL, "line breaks of NULL");
  CHECK(ztextParagraphGraphemeBreaks(NULL) == NULL, "grapheme breaks of NULL");
  CHECK(ztextParagraphWordBreaks(NULL) == NULL, "word breaks of NULL");
  CHECK(ztextParagraphNextGrapheme(NULL, 0) == 0u, "next grapheme of NULL");
  CHECK(ztextParagraphPreviousGrapheme(NULL, 0) == 0u, "prev grapheme of NULL");

  ZtextLine* line = NULL;
  REFUSES(ztextLineCreate(NULL, 0, 0, &line));
  CHECK(line == NULL, "a refused line create wrote a handle");
  REFUSES_OUT(ztextLineCreate(NULL, 0, 0, NULL));
  CHECK(ztextLineOffset(NULL) == 0u, "offset of NULL line");
  CHECK(ztextLineLength(NULL) == 0u, "length of NULL line");
  CHECK(ztextLineVisualRunCount(NULL) == 0u, "visual run count of NULL line");
  CHECK(ztextLineVisualRuns(NULL) == NULL, "visual runs of NULL line");
  CHECK(ztextLineShapingRunCount(NULL) == 0u, "shaping run count of NULL line");
  CHECK(ztextLineShapingRuns(NULL) == NULL, "shaping runs of NULL line");

  //--------------------------------------------------------------------------
  // The other half: real handles, NULL out-parameters. A host whose allocation
  // failed two lines up produces exactly this, and the handle being valid is
  // what makes it a different code path from everything above.
  //--------------------------------------------------------------------------
  ZtextLibrary* library = NULL;
  if (ztextLibraryCreate(&library) != ZTEXT_RESULT_OK) {
    printf("  FAIL could not create a library\n");
    return 1;
  }

  FILE* file = fopen(argv[1], "rb");
  if (file == NULL) {
    printf("  FAIL could not open %s\n", argv[1]);
    return 2;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  rewind(file);
  unsigned char* bytes = (unsigned char*)malloc((size_t)size);
  if (bytes == NULL || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
    printf("  FAIL could not read %s\n", argv[1]);
    return 2;
  }
  fclose(file);

  REFUSES_OUT(ztextFontCreateFromMemory(library, bytes, (size_t)size, 0, NULL));
  REFUSES(ztextFontCreateFromMemory(library, NULL, (size_t)size, 0, &font));
  REFUSES(ztextFontCreateFromMemory(library, bytes, 0, 0, &font));
  if (ztextFontCreateFromMemory(library, bytes, (size_t)size, 0, &font) !=
      ZTEXT_RESULT_OK) {
    printf("  FAIL could not create a font\n");
    return 1;
  }

  REFUSES_OUT(ztextFaceCreate(font, 0, 16, NULL));
  REFUSES_OUT(ztextFontCoveredPrefix(font, "abc", 3, NULL));
  REFUSES(ztextFontCoveredPrefix(font, NULL, 3, &covered));
  REFUSES_OUT(ztextFontAxis(font, 0, NULL));
  REFUSES_OUT(ztextFontVariation(font, 0, NULL));
  REFUSES(ztextFontSetVariations(font, NULL, 3));
  REFUSES_OUT(ztextFontNamedInstanceCoords(font, 0, instance_coords, NULL));
  REFUSES_OUT(ztextFontNamedInstanceName(font, 0, instance_name, NULL));

  if (ztextFaceCreate(font, 0, 16, &face) != ZTEXT_RESULT_OK) {
    printf("  FAIL could not create a face\n");
    return 1;
  }
  REFUSES_OUT(ztextFaceMetrics(face, NULL));
  REFUSES_OUT(ztextFaceMetric(face, ZTEXT_METRIC_X_HEIGHT, NULL));
  REFUSES_OUT(ztextFaceMetricWithFallback(face, ZTEXT_METRIC_X_HEIGHT, NULL));
  REFUSES_OUT(ztextFaceRenderGlyph(face, 1, ZTEXT_RENDER_MODE_A8,
                                   ZTEXT_HINTING_NORMAL, 0, 0, NULL));
  REFUSES_OUT(ztextFaceGlyphExtents(face, 1, ZTEXT_HINTING_NORMAL, NULL));
  REFUSES(ztextFaceDecomposeOutline(face, 1, ZTEXT_HINTING_NORMAL, NULL));

  if (ztextShaperCreate(&shaper) != ZTEXT_RESULT_OK) {
    printf("  FAIL could not create a shaper\n");
    return 1;
  }
  REFUSES(ztextShaperShapeUtf8(shaper, face, "x", 1, 0, 1, NULL));
  REFUSES(ztextShaperShapeUtf8(shaper, NULL, "x", 1, 0, 1, &params));
  REFUSES(ztextShaperShapeUtf8(shaper, face, NULL, 1, 0, 1, &params));
  REFUSES_OUT(ztextShaperExtents(shaper, face, NULL));

  if (ztextParagraphCreateUtf8("abc", 3, ZTEXT_BASE_DIRECTION_AUTO,
                               &paragraph) != ZTEXT_RESULT_OK) {
    printf("  FAIL could not create a paragraph\n");
    return 1;
  }
  REFUSES_OUT(ztextLineCreate(paragraph, 0, 3, NULL));

  ztextParagraphDestroy(paragraph);
  ztextShaperDestroy(shaper);
  ztextFaceDestroy(face);
  ztextFontDestroy(font);
  ztextLibraryDestroy(library);
  free(bytes);

  if (failures != 0) {
    printf("null sweep: %d FAILED\n", failures);
    return 1;
  }
  printf("null sweep: ok\n");
  return 0;
}
