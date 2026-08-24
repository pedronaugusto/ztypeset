//===----------------------------------------------------------------------===//
// ztext -- shaping one run through HarfBuzz.
//===----------------------------------------------------------------------===//

#include "ztext_internal.h"

//===----------------------------------------------------------------------===//
// Enum translation
//===----------------------------------------------------------------------===//

static hb_direction_t toHbDirection(ZtextDirection direction) {
  switch (direction) {
    case ZTEXT_DIRECTION_LTR:
      return HB_DIRECTION_LTR;
    case ZTEXT_DIRECTION_RTL:
      return HB_DIRECTION_RTL;
    case ZTEXT_DIRECTION_TTB:
      return HB_DIRECTION_TTB;
    case ZTEXT_DIRECTION_BTT:
      return HB_DIRECTION_BTT;
    case ZTEXT_DIRECTION_AUTO:
    default:
      return HB_DIRECTION_INVALID;
  }
}

static ZtextDirection fromHbDirection(hb_direction_t direction) {
  switch (direction) {
    case HB_DIRECTION_LTR:
      return ZTEXT_DIRECTION_LTR;
    case HB_DIRECTION_RTL:
      return ZTEXT_DIRECTION_RTL;
    case HB_DIRECTION_TTB:
      return ZTEXT_DIRECTION_TTB;
    case HB_DIRECTION_BTT:
      return ZTEXT_DIRECTION_BTT;
    default:
      return ZTEXT_DIRECTION_AUTO;
  }
}

static hb_buffer_cluster_level_t toHbClusterLevel(ZtextClusterLevel level) {
  switch (level) {
    case ZTEXT_CLUSTER_LEVEL_MONOTONE_CHARACTERS:
      return HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS;
    case ZTEXT_CLUSTER_LEVEL_CHARACTERS:
      return HB_BUFFER_CLUSTER_LEVEL_CHARACTERS;
    case ZTEXT_CLUSTER_LEVEL_GRAPHEMES:
      return HB_BUFFER_CLUSTER_LEVEL_GRAPHEMES;
    case ZTEXT_CLUSTER_LEVEL_MONOTONE_GRAPHEMES:
    default:
      return HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES;
  }
}

//===----------------------------------------------------------------------===//
// The FreeType-backed shaping font
//===----------------------------------------------------------------------===//

/// Builds (once) the hb_font_t that asks FreeType for metrics.
///
/// hb_ft_font_create wraps the face ztext already owns. The alternative,
/// hb_ft_font_set_funcs, opens an FT_Face of its own from an FT_Library of its
/// own -- which would take metrics from a different face than the one being
/// rasterised, defeating the only reason to ask FreeType at all.
///
/// Its glyph callbacks load through the font's SHARED FT_Face, so this face's
/// size has to be active whenever HarfBuzz is handed it -- at creation, where
/// hb-ft caches the scale, and again before every shape.
static hb_font_t* freetypeFont(ZtextFace* face) {
  ztextFaceActivate(face);
  if (face->hb_ft_font != NULL) return face->hb_ft_font;

  hb_font_t* font = hb_ft_font_create(face->font->ft, NULL);
  if (font == NULL || font == hb_font_get_empty()) return NULL;

  // hb-ft defaults its load flags to FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING, so
  // out of the box it returns the same unhinted advances hb-ot-font already
  // gives -- with a lock and a cache in the way. Since the point of this path
  // is to match a hinted raster, hinting is switched on explicitly.
  hb_ft_font_set_load_flags(font, FT_LOAD_DEFAULT);
  hb_ft_font_changed(font);

  face->hb_ft_font = font;
  // Built lazily, so it can come into existence long after the font's axes
  // were moved -- and hb_ft_font_create does not read them off the FT_Face in
  // this build. Assigned first, because that is what tells the helper below
  // there is a second HarfBuzz font to write.
  ztextFaceApplyVariations(face);
  return font;
}

//===----------------------------------------------------------------------===//
// Shaper
//===----------------------------------------------------------------------===//

ZtextResult ztextShaperCreate(ZtextShaper** out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  *out = NULL;

  ZtextShaper* shaper = ZTEXT_NEW(ZtextShaper);
  if (shaper == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;

  shaper->buffer = hb_buffer_create();
  if (shaper->buffer == NULL ||
      !hb_buffer_allocation_successful(shaper->buffer)) {
    hb_buffer_destroy(shaper->buffer);
    ztextFree(shaper);
    return ZTEXT_RESULT_OUT_OF_MEMORY;
  }

  *out = shaper;
  return ZTEXT_RESULT_OK;
}

void ztextShaperDestroy(ZtextShaper* shaper) {
  if (shaper == NULL) return;
  hb_buffer_destroy(shaper->buffer);
  ztextArrayFree(&shaper->glyphs, sizeof(ZtextGlyph));
  ztextArrayFree(&shaper->features, sizeof(hb_feature_t));
  ztextFree(shaper);
}

static bool buildFeatures(ZtextShaper* shaper, const ZtextFeature* features,
                          size_t count) {
  shaper->features.count = 0u;
  if (count == 0u) return true;
  if (!ztextArrayReserve(&shaper->features, count, sizeof(hb_feature_t))) {
    return false;
  }

  hb_feature_t* out = (hb_feature_t*)shaper->features.data;
  for (size_t i = 0u; i < count; i++) {
    // Converted field by field rather than cast: the two structs happen to
    // agree today, and nothing would tell us if that stopped being true.
    out[i].tag = (hb_tag_t)features[i].tag;
    out[i].value = features[i].value;
    out[i].start = features[i].start;
    out[i].end = features[i].end;
  }
  shaper->features.count = count;
  return true;
}

ZtextResult ztextShaperShapeUtf8(ZtextShaper* shaper, ZtextFace* face,
                                 const char* text, size_t length,
                                 size_t run_offset, size_t run_length,
                                 const ZtextShapeParams* params) {
  if (shaper == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;

  // Invalidate before validating. With the reset further down, an argument
  // rejected here would leave the PREVIOUS run's glyphs live and queryable --
  // so a caller that logs the error and carries on draws last frame's text,
  // while a failure that happened later (out of memory, say) correctly
  // cleared them. Two different behaviours for the same "it failed".
  shaper->shaped = false;
  shaper->glyphs.count = 0u;
  shaper->used_freetype_metrics = false;
  shaper->face_generation = 0u;

  if (face == NULL || params == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (text == NULL && length != 0u) return ZTEXT_RESULT_INVALID_ARGUMENT;
  // HarfBuzz counts text in unsigned int.
  if (length > (size_t)INT_MAX) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (params->feature_count != 0u && params->features == NULL) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }
  // Written as subtractions so no caller's arithmetic can overflow into a
  // range that looks valid.
  if (run_offset > length) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (run_length > length - run_offset) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (!ztextIsValidUtf8(text, length)) return ZTEXT_RESULT_INVALID_UTF8;
  // A run that starts or ends inside a character would hand HarfBuzz half of
  // one. Refused rather than shaped, as everywhere else that takes a range.
  if (ztextSplitsUtf8Character(text, length, run_offset) ||
      ztextSplitsUtf8Character(text, length, run_offset + run_length)) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  hb_font_t* font = face->hb_font;
  if (params->use_freetype_metrics != 0) {
    font = freetypeFont(face);
    if (font == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;
    shaper->used_freetype_metrics = true;
  }

  hb_buffer_t* buffer = shaper->buffer;
  hb_buffer_clear_contents(buffer);
  // The WHOLE text goes in and only the run is selected. HarfBuzz shapes the
  // item range and uses everything around it as context, which is what makes
  // joining correct across a boundary the host chose.
  hb_buffer_add_utf8(buffer, text, (int)length, (unsigned int)run_offset,
                     (int)run_length);
  if (!hb_buffer_allocation_successful(buffer)) {
    return ZTEXT_RESULT_OUT_OF_MEMORY;
  }

  hb_buffer_set_cluster_level(buffer, toHbClusterLevel(params->cluster_level));

  if (params->direction != ZTEXT_DIRECTION_AUTO) {
    hb_buffer_set_direction(buffer, toHbDirection(params->direction));
  }
  if (params->script != 0u) {
    hb_buffer_set_script(buffer, hb_script_from_iso15924_tag(
                                     (hb_tag_t)params->script));
  }
  // Language, and the reason this is not simply "set it if the caller gave
  // one".
  //
  // hb_buffer_guess_segment_properties fills in any property still unset, and
  // for language it calls hb_language_get_default(), which reads the process
  // locale through setlocale(LC_CTYPE). Shaping that depends on the user's
  // locale is shaping that differs between two machines running the same
  // build -- unacceptable for an engine, and it would make this package's own
  // golden tests environment-dependent.
  //
  // So the buffer is seeded with "und" (ISO 639-2 for undetermined) before the
  // guess, which stops it reaching for the locale, and cleared afterwards when
  // the caller asked for no language at all. The result is that ztext applies
  // language-specific features only when explicitly told to.
  if (params->language != NULL) {
    hb_buffer_set_language(
        buffer, hb_language_from_string(params->language, -1));
  } else {
    hb_buffer_set_language(buffer, hb_language_from_string("und", 3));
  }

  // Fills in only what was left unset above, so an explicit direction is never
  // overwritten by a guess.
  hb_buffer_guess_segment_properties(buffer);

  if (params->language == NULL) {
    hb_buffer_set_language(buffer, HB_LANGUAGE_INVALID);
  }

  if (!buildFeatures(shaper, params->features, params->feature_count)) {
    return ZTEXT_RESULT_OUT_OF_MEMORY;
  }

  hb_shape(font, buffer,
           shaper->features.count == 0u
               ? NULL
               : (const hb_feature_t*)shaper->features.data,
           (unsigned int)shaper->features.count);

  if (!hb_buffer_allocation_successful(buffer)) {
    return ZTEXT_RESULT_OUT_OF_MEMORY;
  }

  unsigned int glyph_count = 0u;
  const hb_glyph_info_t* infos =
      hb_buffer_get_glyph_infos(buffer, &glyph_count);
  const hb_glyph_position_t* positions =
      hb_buffer_get_glyph_positions(buffer, &glyph_count);
  if (glyph_count != 0u && (infos == NULL || positions == NULL)) {
    return ZTEXT_RESULT_SHAPE_FAILED;
  }

  if (!ztextArrayReserve(&shaper->glyphs, glyph_count, sizeof(ZtextGlyph))) {
    return ZTEXT_RESULT_OUT_OF_MEMORY;
  }

  // HarfBuzz keeps its results in two parallel arrays of its own layout. They
  // are fused here into one array of one small struct, so a consumer walks one
  // cache line per glyph and never sees a HarfBuzz type.
  ZtextGlyph* glyphs = (ZtextGlyph*)shaper->glyphs.data;
  for (unsigned int i = 0u; i < glyph_count; i++) {
    glyphs[i].glyph_id = infos[i].codepoint;  // Post-shaping this IS the index.
    glyphs[i].cluster = infos[i].cluster;
    glyphs[i].x_advance = (float)positions[i].x_advance / 64.0f;
    glyphs[i].y_advance = (float)positions[i].y_advance / 64.0f;
    glyphs[i].x_offset = (float)positions[i].x_offset / 64.0f;
    glyphs[i].y_offset = (float)positions[i].y_offset / 64.0f;
  }
  shaper->glyphs.count = glyph_count;

  shaper->direction = fromHbDirection(hb_buffer_get_direction(buffer));
  shaper->face_generation = face->generation;
  shaper->shaped = true;
  return ZTEXT_RESULT_OK;
}

size_t ztextShaperGlyphCount(const ZtextShaper* shaper) {
  if (shaper == NULL || !shaper->shaped) return 0u;
  return shaper->glyphs.count;
}

const ZtextGlyph* ztextShaperGlyphs(const ZtextShaper* shaper) {
  if (shaper == NULL || !shaper->shaped) return NULL;
  return (const ZtextGlyph*)shaper->glyphs.data;
}

ZtextDirection ztextShaperDirection(const ZtextShaper* shaper) {
  if (shaper == NULL || !shaper->shaped) return ZTEXT_DIRECTION_AUTO;
  return shaper->direction;
}

ZtextResult ztextShaperExtents(const ZtextShaper* shaper, ZtextFace* face,
                               ZtextExtents* out) {
  if (shaper == NULL || face == NULL || out == NULL) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }
  memset(out, 0, sizeof(*out));
  if (!shaper->shaped) return ZTEXT_RESULT_INVALID_ARGUMENT;

  // The face must be the one the run was shaped against, at the size it was
  // shaped at. Measured against a different face -- or the same face after a
  // resize -- the result is ink bounds from one font and advances from
  // another: wrong, plausible, and undetectable by the caller. Generations are
  // compared rather than pointers, because the face that was shaped with may
  // since have been destroyed and its address reused.
  if (face->generation != shaper->face_generation) {
    ztextSetErrorDetail(
        "extents asked for a different face, or the same face resized since "
        "it was shaped");
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  // The same metrics source the shape used. Taking extents from hb-ot-font
  // after shaping with hb-ft would mix two views of the same glyph, and the
  // mismatch would be small enough to be mistaken for rounding.
  hb_font_t* font = face->hb_font;
  if (shaper->used_freetype_metrics) {
    font = freetypeFont(face);
    if (font == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;
  }

  const ZtextGlyph* glyphs = (const ZtextGlyph*)shaper->glyphs.data;
  const size_t count = shaper->glyphs.count;

  float pen_x = 0.0f;
  float pen_y = 0.0f;
  bool any_ink = false;

  for (size_t i = 0u; i < count; i++) {
    hb_glyph_extents_t extents;
    if (hb_font_get_glyph_extents(font, glyphs[i].glyph_id, &extents)) {
      const float x =
          pen_x + glyphs[i].x_offset + (float)extents.x_bearing / 64.0f;
      const float y =
          pen_y + glyphs[i].y_offset + (float)extents.y_bearing / 64.0f;
      // HarfBuzz reports height NEGATIVE in a y-up coordinate system, so the
      // bottom edge is y + height. Both corners are min/maxed rather than
      // assumed, so a font that reports them the other way round still gives
      // a well-formed box.
      const float x2 = x + (float)extents.width / 64.0f;
      const float y2 = y + (float)extents.height / 64.0f;

      const float x_min = x < x2 ? x : x2;
      const float x_max = x < x2 ? x2 : x;
      const float y_min = y < y2 ? y : y2;
      const float y_max = y < y2 ? y2 : y;

      if (!any_ink) {
        out->x_min = x_min;
        out->x_max = x_max;
        out->y_min = y_min;
        out->y_max = y_max;
        any_ink = true;
      } else {
        if (x_min < out->x_min) out->x_min = x_min;
        if (x_max > out->x_max) out->x_max = x_max;
        if (y_min < out->y_min) out->y_min = y_min;
        if (y_max > out->y_max) out->y_max = y_max;
      }
    }

    pen_x += glyphs[i].x_advance;
    pen_y += glyphs[i].y_advance;
  }

  out->x_advance = pen_x;
  out->y_advance = pen_y;
  return ZTEXT_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Warm-up
//===----------------------------------------------------------------------===//

void ztextWarmup(void) {
  // HarfBuzz builds several singletons on first use and frees them from an
  // atexit handler (hb_lazy_loader_t, hb-machinery.hh). They are caches with
  // process lifetime, not leaks -- but they are allocated through whichever
  // allocator happens to be installed the first time something needs them,
  // and they are still live when a host checks its heap at shutdown.
  //
  // Touching them here lets a host populate them BEFORE installing a tracking
  // allocator, so what that allocator then sees is ztext's actual working set.
  // ztext's own allocator-balance test depends on exactly this.
  //
  // Each of the four below was found by running that test and reading the
  // stack of what it reported, rather than by guessing from the source.

  // The list of compiled-in shapers (hb-shape.cc).
  (void)hb_shape_list_shapers();

  // The Unicode character database (hb-ucd.cc), reached through the default
  // unicode functions.
  (void)hb_unicode_funcs_get_default();

  // The language intern table (hb-common.cc). "und" is the tag ztext seeds
  // every unspecified shape with, so this is the entry that would otherwise
  // appear on the first shape.
  (void)hb_language_from_string("und", 3);

  // The OpenType font-functions singleton (hb-ot-font.cc). It is built the
  // first time any font is pointed at hb-ot-font, which ztext does for every
  // face; an empty font is enough to trigger it and owns nothing.
  hb_font_t* probe = hb_font_create(hb_face_get_empty());
  if (probe != NULL) {
    hb_ot_font_set_funcs(probe);
    hb_font_destroy(probe);
  }

  // SheenBidi's allocator object is likewise created once and kept for the
  // life of the process. It is allocated before ztext's seam is live -- by
  // SheenBidi's own default allocator, i.e. malloc -- so it never reaches a
  // host allocator at all; installing it here simply makes when that happens
  // predictable rather than dependent on the first paragraph.
  (void)ztextInstallSheenbidiAllocator();
}
