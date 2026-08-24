//===----------------------------------------------------------------------===//
// ztext -- the C boundary on its own.
//
// Nothing here is Zig. That is the point: ztext.h has to be a real C contract
// that a plain C host can use, and the allocator seam has to be genuinely in
// use rather than a parameter nobody passes. This asserts both, and asserts
// that every byte handed out comes back.
//
// Usage: ztext-c-smoke <path-to-font.ttf>
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ztext.h"

static int failures = 0;

#define CHECK(condition, ...)                                     \
  do {                                                            \
    if (!(condition)) {                                           \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);               \
      printf(__VA_ARGS__);                                        \
      printf("\n");                                               \
      failures++;                                                 \
    }                                                             \
  } while (0)

#define CHECK_OK(expression)                                      \
  do {                                                            \
    ZtextResult result_ = (expression);                           \
    CHECK(result_ == ZTEXT_RESULT_OK, "%s -> %s (%s)", #expression,      \
          ztextResultName(result_), ztextLastErrorDetail());       \
  } while (0)

//===----------------------------------------------------------------------===//
// A counting allocator, so the seam is measurably in use.
//
// Deliberately does NOT provide `reallocate`, which exercises ztext's
// allocate-copy-deallocate fallback path. The Zig side covers the other case.
//===----------------------------------------------------------------------===//

typedef struct Counters {
  size_t live_bytes;
  size_t live_blocks;
  size_t total_allocations;
} Counters;

static void* countingAllocate(void* user, size_t size, size_t alignment) {
  Counters* counters = (Counters*)user;
  // Every alignment ztext asks for is at most max_align_t's, which is what
  // malloc already guarantees; assert rather than silently mis-serve it.
  if (alignment > _Alignof(max_align_t)) return NULL;
  void* block = malloc(size);
  if (block == NULL) return NULL;
  counters->live_bytes += size;
  counters->live_blocks += 1u;
  counters->total_allocations += 1u;
  return block;
}

static void countingDeallocate(void* user, void* block, size_t size,
                               size_t alignment) {
  Counters* counters = (Counters*)user;
  (void)alignment;
  // The size comes back from ztext, which is the whole point of the seam
  // carrying one -- a plain C host with a pool or an arena can use it.
  counters->live_bytes -= size;
  counters->live_blocks -= 1u;
  free(block);
}

//===----------------------------------------------------------------------===//
// Proof that FreeType's allocation really is per-library.
//
// ztext.h claims a ZtextLibrary captures the allocator installed when it was
// created, and that FreeType memory follows that copy rather than the
// process-wide one. That claim was false once -- the callbacks ignored their
// FT_Memory and dispatched through the global -- and nothing noticed, because
// every test installed one allocator and kept it.
//
// ztextLibraryCountFaces is the lever: it opens and closes an FT_Face and
// touches nothing else, so it is pure FreeType traffic. Run it after swapping
// the process-wide allocator and watch which allocator sees the work.
//===----------------------------------------------------------------------===//

static int checkPerLibraryAllocator(const unsigned char* font,
                                    size_t font_size) {
  Counters first;
  Counters second;
  memset(&first, 0, sizeof(first));
  memset(&second, 0, sizeof(second));

  ZtextAllocator a = {countingAllocate, NULL, countingDeallocate, &first};
  ZtextAllocator b = {countingAllocate, NULL, countingDeallocate, &second};

  ztextSetAllocator(&a);
  ZtextLibrary* library = NULL;
  if (ztextLibraryCreate(&library) != ZTEXT_RESULT_OK) {
    printf("  FAIL per-library: could not create a library\n");
    return 1;
  }

  // Everything from here on is nominally the second allocator's.
  ztextSetAllocator(&b);
  const size_t second_before = second.total_allocations;
  const size_t first_before = first.total_allocations;

  uint32_t faces = 0;
  if (ztextLibraryCountFaces(library, font, font_size, &faces) !=
      ZTEXT_RESULT_OK) {
    printf("  FAIL per-library: counting faces failed\n");
    return 1;
  }

  const size_t first_used = first.total_allocations - first_before;
  const size_t second_used = second.total_allocations - second_before;

  if (first_used == 0) {
    printf("  FAIL per-library: FreeType made no allocations to observe\n");
    return 1;
  }
  if (second_used != 0) {
    printf("  FAIL per-library: %zu FreeType allocations leaked to the "
           "process-wide allocator; the library did not capture its own\n",
           second_used);
    return 1;
  }

  // Destroying the library while the OTHER allocator is installed must still
  // return every byte to the one that issued it.
  ztextLibraryDestroy(library);
  ztextSetAllocator(NULL);

  if (first.live_blocks != 0 || first.live_bytes != 0) {
    printf("  FAIL per-library: %zu blocks / %zu bytes never returned to the "
           "creating allocator\n", first.live_blocks, first.live_bytes);
    return 1;
  }
  if (second.live_blocks != 0) {
    printf("  FAIL per-library: %zu blocks left with the wrong allocator\n",
           second.live_blocks);
    return 1;
  }

  printf("  per-library allocator: %zu FreeType allocations followed the "
         "library across a global swap, 0 went to the new global\n",
         first_used);
  return 0;
}

//===----------------------------------------------------------------------===//
// Proof that a warm shaper allocates nothing.
//
// README calls this "steady state, zero allocations". It was an inference from
// the code -- the buffer and both arrays are reused and never shrink -- until
// something counted.
//===----------------------------------------------------------------------===//

static int checkSteadyStateAllocations(const unsigned char* font,
                                       size_t font_size) {
  Counters counters;
  memset(&counters, 0, sizeof(counters));
  ZtextAllocator allocator = {countingAllocate, NULL, countingDeallocate,
                              &counters};
  ztextSetAllocator(&allocator);

  ZtextLibrary* library = NULL;
  ZtextFont* ffont = NULL;
  ZtextFace* face = NULL;
  ZtextShaper* shaper = NULL;
  int failed = 0;

  if (ztextLibraryCreate(&library) != ZTEXT_RESULT_OK ||
      ztextFontCreateFromMemory(library, font, font_size, 0, &ffont) !=
          ZTEXT_RESULT_OK ||
      ztextFaceCreate(ffont, 0, 18, &face) != ZTEXT_RESULT_OK ||
      ztextShaperCreate(&shaper) != ZTEXT_RESULT_OK) {
    printf("  FAIL steady-state: set-up failed\n");
    failed = 1;
  }

  if (!failed) {
    ZtextShapeParams params;
    memset(&params, 0, sizeof(params));
    params.direction = ZTEXT_DIRECTION_RTL;
    params.script = ZTEXT_TAG('H', 'e', 'b', 'r');
    const char* text =
        "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
    const size_t length = strlen(text);

    // Warm: let every buffer reach its high-water mark.
    for (int i = 0; i < 8; i++) {
      if (ztextShaperShapeUtf8(shaper, face, text, length, 0, length,
                               &params) !=
          ZTEXT_RESULT_OK) {
        printf("  FAIL steady-state: warm-up shape failed\n");
        failed = 1;
        break;
      }
    }

    if (!failed) {
      const size_t before = counters.total_allocations;
      for (int i = 0; i < 500; i++) {
        if (ztextShaperShapeUtf8(shaper, face, text, length, 0, length,
                                 &params) !=
            ZTEXT_RESULT_OK) {
          printf("  FAIL steady-state: shape failed\n");
          failed = 1;
          break;
        }
      }
      const size_t during = counters.total_allocations - before;
      if (!failed && during != 0) {
        printf("  FAIL steady-state: %zu allocations across 500 warm shapes; "
               "the zero-allocation claim is wrong\n", during);
        failed = 1;
      } else if (!failed) {
        printf("  steady state: 500 shapes, 0 allocations\n");
      }
    }
  }

  ztextShaperDestroy(shaper);
  ztextFaceDestroy(face);
  ztextFontDestroy(ffont);
  ztextLibraryDestroy(library);
  ztextSetAllocator(NULL);

  if (!failed && (counters.live_blocks != 0 || counters.live_bytes != 0)) {
    printf("  FAIL steady-state: %zu blocks leaked\n", counters.live_blocks);
    failed = 1;
  }
  return failed;
}

//===----------------------------------------------------------------------===//
// An allocator that fails the Nth allocation, for injection testing.
//
// Every step of the pipeline is run with the failure point walked forward one
// allocation at a time. At each point ztext must do three things: report a
// typed error rather than crashing, leave the caller's out-parameter NULL
// rather than handing back a half-built handle, and free everything it had
// already allocated.
//
// This is the check that a `goto cleanup` chain is actually correct, and it
// cannot be written in Zig -- the failure has to be injected below the C
// boundary, into the allocations FreeType, HarfBuzz and SheenBidi make.
//===----------------------------------------------------------------------===//

typedef struct Injector {
  long budget;   /* allocations to serve before failing; negative = fail */
  size_t live;   /* blocks currently handed out */
} Injector;

static void* injectAllocate(void* user, size_t size, size_t alignment) {
  Injector* injector = (Injector*)user;
  (void)alignment;
  if (injector->budget-- <= 0) return NULL;
  void* block = malloc(size);
  if (block != NULL) injector->live += 1u;
  return block;
}

static void injectDeallocate(void* user, void* block, size_t size,
                             size_t alignment) {
  Injector* injector = (Injector*)user;
  (void)size;
  (void)alignment;
  injector->live -= 1u;
  free(block);
}

/// Returns non-zero on a violation.
static int runInjectionSweep(const unsigned char* font, size_t font_size) {
  const long points = 220;
  int completed = 0, out_of_memory = 0, other_error = 0;

  for (long limit = 0; limit < points; limit++) {
    Injector injector;
    injector.budget = limit;
    injector.live = 0u;

    ZtextAllocator allocator;
    allocator.allocate = injectAllocate;
    allocator.reallocate = NULL;
    allocator.deallocate = injectDeallocate;
    allocator.user = &injector;
    ztextSetAllocator(&allocator);

    ZtextLibrary* library = NULL;
    ZtextFont* fnt = NULL;
    ZtextFace* face = NULL;
    ZtextShaper* shaper = NULL;
    ZtextParagraph* paragraph = NULL;
    ZtextLine* line = NULL;
    ZtextResult result = ZTEXT_RESULT_OK;
    int stopped = 0;

#define STEP(call, handle)                                                  \
    if (!stopped) {                                                         \
      result = (call);                                                      \
      if (result != ZTEXT_RESULT_OK) {                                      \
        stopped = 1;                                                        \
        if ((handle) != NULL) {                                             \
          printf("  FAIL injection %ld: %s failed but wrote its handle\n",  \
                 limit, #call);                                             \
          return 1;                                                         \
        }                                                                   \
      }                                                                     \
    }

    STEP(ztextLibraryCreate(&library), library)
    STEP(ztextFontCreateFromMemory(library, font, font_size, 0, &fnt), fnt)
    STEP(ztextFaceCreate(fnt, 0, 24.5f, &face), face)
    STEP(ztextShaperCreate(&shaper), shaper)
    STEP(ztextParagraphCreateUtf8("a \xd7\xa9\xd7\x9c b", 8,
                                  ZTEXT_BASE_DIRECTION_AUTO, &paragraph),
         paragraph)
    STEP(ztextLineCreate(paragraph, 0, 6, &line), line)
#undef STEP

    if (!stopped) {
      ZtextShapeParams params;
      memset(&params, 0, sizeof(params));
      params.direction = ZTEXT_DIRECTION_RTL;
      params.script = ZTEXT_TAG('H', 'e', 'b', 'r');
      result = ztextShaperShapeUtf8(
          shaper, face, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", 8, 0, 8,
          &params);
      if (result != ZTEXT_RESULT_OK) stopped = 1;
    }

    if (!stopped) completed++;
    else if (result == ZTEXT_RESULT_OUT_OF_MEMORY) out_of_memory++;
    else other_error++;

    ztextLineDestroy(line);
    ztextParagraphDestroy(paragraph);
    ztextShaperDestroy(shaper);
    // Deliberately the "wrong" way round: the font is released while its face
    // is still alive, so every one of the 220 injection points also walks the
    // order-free teardown.
    ztextFontDestroy(fnt);
    ztextFaceDestroy(face);
    ztextLibraryDestroy(library);

    if (injector.live != 0u) {
      printf("  FAIL injection %ld: %zu blocks leaked after teardown (%s)\n",
             limit, injector.live, ztextResultName(result));
      return 1;
    }
  }

  printf("  injection: %ld failure points — %d completed, %d out-of-memory, "
         "%d other typed error, 0 leaks, 0 crashes\n",
         points, completed, out_of_memory, other_error);

  // Both ends must occur, or the sweep is not testing what it claims.
  if (completed == 0) {
    printf("  FAIL injection: no failure point ever let the pipeline finish\n");
    return 1;
  }
  if (out_of_memory == 0) {
    printf("  FAIL injection: no failure point ever reported out-of-memory\n");
    return 1;
  }
  return 0;
}

//===----------------------------------------------------------------------===//

static unsigned char* readFile(const char* path, size_t* size_out) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size <= 0) {
    fclose(file);
    return NULL;
  }
  rewind(file);
  unsigned char* buffer = (unsigned char*)malloc((size_t)size);
  if (buffer == NULL) {
    fclose(file);
    return NULL;
  }
  const size_t read = fread(buffer, 1, (size_t)size, file);
  fclose(file);
  if (read != (size_t)size) {
    free(buffer);
    return NULL;
  }
  *size_out = read;
  return buffer;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s <font.ttf>\n", argv[0]);
    return 2;
  }

  size_t font_size = 0;
  unsigned char* font = readFile(argv[1], &font_size);
  if (font == NULL) {
    printf("could not read %s\n", argv[1]);
    return 2;
  }

  printf("ztext %u.%u.%u  freetype %u.%u.%u  harfbuzz %u.%u.%u  "
         "sheenbidi %u.%u.%u\n",
         ztextVersion() >> 16, (ztextVersion() >> 8) & 0xFF,
         ztextVersion() & 0xFF,
         ztextFreetypeVersion() >> 16, (ztextFreetypeVersion() >> 8) & 0xFF,
         ztextFreetypeVersion() & 0xFF,
         ztextHarfbuzzVersion() >> 16, (ztextHarfbuzzVersion() >> 8) & 0xFF,
         ztextHarfbuzzVersion() & 0xFF,
         ztextSheenbidiVersion() >> 16, (ztextSheenbidiVersion() >> 8) & 0xFF,
         ztextSheenbidiVersion() & 0xFF);

  CHECK(ztextVersion() == (uint32_t)((ZTEXT_VERSION_MAJOR << 16) |
                                     (ZTEXT_VERSION_MINOR << 8) |
                                     ZTEXT_VERSION_PATCH),
        "the header and the library disagree about ztext's version");

  // The upstreams' process-lifetime caches are populated before the counting
  // allocator goes in, so what it counts is ztext's own working set.
  ztextWarmup();

  Counters counters;
  memset(&counters, 0, sizeof(counters));
  ZtextAllocator allocator;
  allocator.allocate = countingAllocate;
  allocator.reallocate = NULL;
  allocator.deallocate = countingDeallocate;
  allocator.user = &counters;
  CHECK_OK(ztextSetAllocator(&allocator));

  // A partial allocator must be refused without disturbing the working one.
  ZtextAllocator broken;
  memset(&broken, 0, sizeof(broken));
  CHECK(ztextSetAllocator(&broken) == ZTEXT_RESULT_INVALID_ARGUMENT,
        "an allocator with no allocate should be refused");

  ZtextLibrary* library = NULL;
  CHECK_OK(ztextLibraryCreate(&library));

  ZtextFont* the_font = NULL;
  CHECK_OK(ztextFontCreateFromMemory(library, font, font_size, 0, &the_font));
  CHECK(ztextFontGlyphCount(the_font) > 0, "the font should have glyphs");
  CHECK(ztextFontUnitsPerEm(the_font) > 0, "units_per_em should be positive");

  // Variable axes, from C. This smoke test is handed ONE font path (see
  // build.zig) and it is a static one, so what is provable here is the half
  // that does not need an fvar table: that the four symbols link, that the
  // structs are a real C type a caller can declare on the stack, and that a
  // font with no axes says 0 and refuses the rest rather than answering with
  // whatever was already in the caller's struct. The Zig suite drives the
  // other half against a variable font.
  ZtextVariationAxis axis;
  memset(&axis, 0xFF, sizeof(axis));
  ZtextVariation wanted;
  wanted.tag = ZTEXT_TAG('w', 'g', 'h', 't');
  wanted.value = 700.0f;
  float current = -1.0f;

  CHECK(ztextFontAxisCount(the_font) == 0,
        "a static font should report 0 axes, got %u",
        ztextFontAxisCount(the_font));
  CHECK(ztextFontAxis(the_font, 0, &axis) == ZTEXT_RESULT_INVALID_ARGUMENT,
        "asking a static font for axis 0 should be refused");
  CHECK(axis.tag == 0u, "a refused axis query should clear its output");
  CHECK(ztextFontVariation(the_font, 0, &current) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "reading an axis of a static font should be refused");
  CHECK(ztextFontSetVariations(the_font, &wanted, 1) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "setting an axis on a static font should be refused, not ignored");
  CHECK(ztextFontSetVariations(the_font, NULL, 1) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "a NULL array with a non-zero count should be refused");
  CHECK(ztextFontAxisCount(NULL) == 0, "a NULL font should report 0 axes");

  // A fractional size through the C boundary, where the 26.6 conversion is.
  ZtextFace* fractional_face = NULL;
  CHECK_OK(ztextFaceCreate(the_font, 0, 24.5f, &fractional_face));
  ZtextFaceMetrics fractional;
  CHECK_OK(ztextFaceMetrics(fractional_face, &fractional));
  CHECK(fractional.pixel_size == 24.5f, "expected 24.5 px, got %f",
        (double)fractional.pixel_size);
  ztextFaceDestroy(fractional_face);

  ZtextFace* nothing = NULL;
  CHECK(ztextFaceCreate(the_font, 0, 0, &nothing) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "a zero size should be refused");
  CHECK(ztextFaceCreate(the_font, 0, -1.0f, &nothing) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "a negative size should be refused");
  CHECK(nothing == NULL, "a refused face should not write a handle");

  ZtextFace* face = NULL;
  CHECK_OK(ztextFaceCreate(the_font, 0, 24, &face));
  CHECK(ztextFaceFont(face) == the_font, "a face should name its own font");

  ZtextFaceMetrics metrics;
  CHECK_OK(ztextFaceMetrics(face, &metrics));
  CHECK(metrics.units_per_em > 0, "units_per_em should be positive");
  CHECK(metrics.num_glyphs > 0, "the face should have glyphs");
  CHECK(metrics.ascender > 0.0f, "ascender should be above the baseline");
  CHECK(metrics.descender < 0.0f, "descender should be below the baseline");
  printf("  face: %s %s, %u glyphs, %u upem\n", ztextFontFamilyName(the_font),
         ztextFontStyleName(the_font), metrics.num_glyphs,
         metrics.units_per_em);

  // Shape a right-to-left run: the font passed in is Hebrew.
  ZtextShaper* shaper = NULL;
  CHECK_OK(ztextShaperCreate(&shaper));

  // shin lamed vav mem
  const char* shalom = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
  ZtextShapeParams params;
  memset(&params, 0, sizeof(params));
  params.direction = ZTEXT_DIRECTION_RTL;
  params.script = ZTEXT_TAG('H', 'e', 'b', 'r');
  CHECK_OK(ztextShaperShapeUtf8(shaper, face, shalom, strlen(shalom), 0,
                                strlen(shalom), &params));

  const size_t glyph_count = ztextShaperGlyphCount(shaper);
  const ZtextGlyph* glyphs = ztextShaperGlyphs(shaper);
  CHECK(glyph_count == 4, "expected 4 glyphs, got %zu", glyph_count);
  CHECK(glyphs != NULL, "glyph array should not be NULL after a shape");
  CHECK(ztextShaperDirection(shaper) == ZTEXT_DIRECTION_RTL,
        "the shaper should report the direction it used");
  if (glyphs != NULL && glyph_count == 4) {
    // Right-to-left output runs in visual order, so clusters descend.
    CHECK(glyphs[0].cluster == 6 && glyphs[3].cluster == 0,
          "RTL clusters should descend: got %u..%u", glyphs[0].cluster,
          glyphs[3].cluster);
    for (size_t i = 0; i < glyph_count; i++) {
      CHECK(glyphs[i].glyph_id != 0, "glyph %zu is .notdef", i);
      CHECK(glyphs[i].x_advance > 0.0f, "glyph %zu has no advance", i);
    }
  }

  ZtextExtents extents;
  CHECK_OK(ztextShaperExtents(shaper, face, &extents));
  CHECK(extents.x_advance > 0.0f, "a shaped run should advance the pen");
  CHECK(extents.x_max > extents.x_min, "extents should enclose some ink");

  // Rasterise.
  if (glyphs != NULL && glyph_count > 0) {
    ZtextGlyphBitmap bitmap;
    CHECK_OK(ztextFaceRenderGlyph(face, glyphs[0].glyph_id,
                                  ZTEXT_RENDER_MODE_A8, ZTEXT_HINTING_NORMAL,
                                  &bitmap));
    CHECK(bitmap.width > 0 && bitmap.height > 0,
          "a letter should rasterise to a non-empty bitmap");
    CHECK(bitmap.pixels != NULL, "a non-empty bitmap needs pixels");
  }

  // Bidi, with no face involved at all.
  const char* mixed = "a \xd7\xa9\xd7\x9c b";
  ZtextParagraph* paragraph = NULL;
  CHECK_OK(ztextParagraphCreateUtf8(mixed, strlen(mixed),
                                    ZTEXT_BASE_DIRECTION_AUTO, &paragraph));
  CHECK(ztextParagraphBaseLevel(paragraph) == 0,
        "a paragraph starting with Latin should resolve to an LTR base");
  CHECK(ztextParagraphVisualRunCount(paragraph) == 3,
        "expected 3 visual runs, got %zu",
        ztextParagraphVisualRunCount(paragraph));
  CHECK(ztextParagraphScriptRunCount(paragraph) >= 2,
        "expected at least 2 script runs");

  // And one line of it, which is where rules L1 and L2 are actually applied.
  ZtextLine* line = NULL;
  const size_t whole = ztextParagraphLength(paragraph);
  CHECK_OK(ztextLineCreate(paragraph, 0, whole, &line));
  CHECK(ztextLineOffset(line) == 0, "line offset should be as given");
  CHECK(ztextLineVisualRunCount(line) ==
            ztextParagraphVisualRunCount(paragraph),
        "a line spanning the paragraph should agree with it");
  CHECK(ztextLineShapingRuns(line) != NULL, "a non-empty line needs runs");
  ztextLineDestroy(line);

  // Into a fresh handle: a refused create writes NULL to its out-parameter,
  // so reusing `line` here would drop the one above on the floor -- which is
  // what the leak count at the end of this function is for.
  ZtextLine* rejected = NULL;
  CHECK(ztextLineCreate(paragraph, 0, whole + 1, &rejected) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "a line past the end of the paragraph should be refused");
  CHECK(rejected == NULL, "a refused line should not write a handle");

  ztextParagraphDestroy(paragraph);

  // Malformed input must be refused rather than substituted.
  const char bad_utf8[] = {(char)0xC3, (char)0x28, 0};
  CHECK(ztextShaperShapeUtf8(shaper, face, bad_utf8, 2, 0, 2, &params) ==
            ZTEXT_RESULT_INVALID_UTF8,
        "malformed UTF-8 should be refused by the shaper");
  CHECK(ztextParagraphCreateUtf8(bad_utf8, 2, ZTEXT_BASE_DIRECTION_AUTO,
                                 &paragraph) == ZTEXT_RESULT_INVALID_UTF8,
        "malformed UTF-8 should be refused by the bidi analyser");

  // Face counting: a plain TTF has exactly one face.
  uint32_t face_count = 0;
  CHECK_OK(ztextLibraryCountFaces(library, font, font_size, &face_count));
  CHECK(face_count == 1, "expected 1 face in a plain TTF, got %u", face_count);
  CHECK(ztextLibraryCountFaces(library, font, 0, &face_count) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "counting faces in an empty buffer should be refused");

  // The SDF spread is validated rather than clamped.
  CHECK_OK(ztextLibrarySetSdfSpread(library, 8));
  CHECK(ztextLibrarySetSdfSpread(library, 1) == ZTEXT_RESULT_INVALID_ARGUMENT,
        "a spread below FreeType's range should be refused");
  CHECK(ztextLibrarySetSdfSpread(library, 33) == ZTEXT_RESULT_INVALID_ARGUMENT,
        "a spread above FreeType's range should be refused");

  // NULL handles are tolerated by every destructor.
  ztextShaperDestroy(NULL);
  ztextFaceDestroy(NULL);
  ztextFontDestroy(NULL);
  ztextLineDestroy(NULL);
  ztextLibraryDestroy(NULL);
  ztextParagraphDestroy(NULL);

  ztextShaperDestroy(shaper);
  ztextFaceDestroy(face);
  ztextFontDestroy(the_font);
  ztextLibraryDestroy(library);

  printf("  allocations: %zu total, %zu live, %zu bytes live\n",
         counters.total_allocations, counters.live_blocks, counters.live_bytes);
  CHECK(counters.total_allocations > 0,
        "the installed allocator was never called -- the seam is not in use");
  CHECK(counters.live_blocks == 0, "%zu blocks leaked", counters.live_blocks);
  CHECK(counters.live_bytes == 0, "%zu bytes leaked", counters.live_bytes);

  ztextSetAllocator(NULL);

  // These install allocators of their own, so they come after the balance
  // check above rather than inside it.
  if (checkPerLibraryAllocator(font, font_size) != 0) failures++;
  if (checkSteadyStateAllocations(font, font_size) != 0) failures++;

  // Injection sweep last: it walks a failure point across the whole pipeline.
  if (runInjectionSweep(font, font_size) != 0) failures++;
  ztextSetAllocator(NULL);

  free(font);

  if (failures != 0) {
    printf("c smoke: %d FAILED\n", failures);
    return 1;
  }
  printf("c smoke: ok\n");
  return 0;
}
