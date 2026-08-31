//===----------------------------------------------------------------------===//
// ztypeset -- turning a glyph index into pixels.
//===----------------------------------------------------------------------===//

#include "ztypeset_internal.h"

/// FreeType load flags for a hinting mode.
///
/// FT_LOAD_NO_BITMAP is unconditional: without it a face carrying embedded
/// bitmap strikes can answer with a 1-bit or pre-rendered bitmap instead of a
/// rasterised outline, and the pixel format ztypeset promises would depend on
/// which glyph you asked for. ztypeset always rasterises outlines. Colour and
/// bitmap strikes are out of scope -- see README.
static FT_Int32 loadFlags(ZtypesetHinting hinting, ZtypesetRenderMode mode) {
  if (mode == ZTYPESET_RENDER_MODE_SDF) {
    // A hinted outline has been moved onto the pixel grid for one specific
    // size. Baking that into a distance field, whose entire purpose is to be
    // sampled at other sizes, is self-defeating -- so SDF ignores the caller's
    // hinting rather than honouring a request that cannot be meant.
    return FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
  }

  switch (hinting) {
    case ZTYPESET_HINTING_LIGHT:
      // Light hinting is its own target: vertical only, and unrelated to the
      // grid the glyph is about to be sampled on.
      return FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP;
    case ZTYPESET_HINTING_NONE:
      return FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
    case ZTYPESET_HINTING_NORMAL:
    default:
      break;
  }

  // Normal hinting is hinting FOR a grid, so the grid the glyph will be
  // sampled on is part of the request.
  switch (mode) {
    case ZTYPESET_RENDER_MODE_LCD:
      return FT_LOAD_TARGET_LCD | FT_LOAD_NO_BITMAP;
    case ZTYPESET_RENDER_MODE_LCD_V:
      return FT_LOAD_TARGET_LCD_V | FT_LOAD_NO_BITMAP;
    case ZTYPESET_RENDER_MODE_A8:
    case ZTYPESET_RENDER_MODE_SDF:
    default:
      return FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP;
  }
}

/// The render mode ztypeset promises, as FreeType's own.
static FT_Render_Mode ftRenderMode(ZtypesetRenderMode mode) {
  switch (mode) {
    case ZTYPESET_RENDER_MODE_SDF:
      return FT_RENDER_MODE_SDF;
    case ZTYPESET_RENDER_MODE_LCD:
      return FT_RENDER_MODE_LCD;
    case ZTYPESET_RENDER_MODE_LCD_V:
      return FT_RENDER_MODE_LCD_V;
    case ZTYPESET_RENDER_MODE_A8:
    default:
      return FT_RENDER_MODE_NORMAL;
  }
}

static ZtypesetBitmapFormat bitmapFormatOf(ZtypesetRenderMode mode) {
  switch (mode) {
    case ZTYPESET_RENDER_MODE_SDF:
      return ZTYPESET_BITMAP_FORMAT_SDF;
    case ZTYPESET_RENDER_MODE_LCD:
      return ZTYPESET_BITMAP_FORMAT_LCD;
    case ZTYPESET_RENDER_MODE_LCD_V:
      return ZTYPESET_BITMAP_FORMAT_LCD_V;
    case ZTYPESET_RENDER_MODE_A8:
    default:
      return ZTYPESET_BITMAP_FORMAT_A8;
  }
}

uint32_t ztypesetBitmapFormatChannels(ZtypesetBitmapFormat format) {
  switch (format) {
    case ZTYPESET_BITMAP_FORMAT_A8:
    case ZTYPESET_BITMAP_FORMAT_SDF:
      return 1u;
    case ZTYPESET_BITMAP_FORMAT_LCD:
    case ZTYPESET_BITMAP_FORMAT_LCD_V:
      return 3u;
    default:
      return 0u;
  }
}

/// The pixel mode FreeType must have produced for `format`, so a silent change
/// upstream is an error here rather than garbage pixels at the consumer.
static FT_Pixel_Mode ftPixelMode(ZtypesetBitmapFormat format) {
  switch (format) {
    case ZTYPESET_BITMAP_FORMAT_LCD:
      return FT_PIXEL_MODE_LCD;
    case ZTYPESET_BITMAP_FORMAT_LCD_V:
      return FT_PIXEL_MODE_LCD_V;
    case ZTYPESET_BITMAP_FORMAT_A8:
    case ZTYPESET_BITMAP_FORMAT_SDF:
    default:
      return FT_PIXEL_MODE_GRAY;
  }
}

/// A strength that is a fraction of the em, in 26.6 pixels at `ppem`.
///
/// This is FreeType's own arithmetic with the constant taken out: ftsynth.c
/// computes `ppem * 0x0AAA / 1024`, and 0x0AAA / 65536 is exactly
/// ZTYPESET_SYNTHETIC_BOLD_DEFAULT, so the default strength reproduces it to
/// the unit. It is also HarfBuzz's, which rounds `|scale| * embolden` with
/// scale already 26.6 -- one number, three places that need it.
static FT_Pos scaledStrength(uint32_t ppem, float strength) {
  const double scaled = (double)ppem * 64.0 * (double)strength;
  return (FT_Pos)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

/// A float in FreeType's 16.16 fixed point, which is what FT_Matrix holds.
static FT_Fixed toFixed16(float value) {
  const double scaled = (double)value * 65536.0;
  return (FT_Fixed)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

static bool isIdentity(const ZtypesetMatrix* matrix) {
  return matrix->xx == 1.0f && matrix->xy == 0.0f && matrix->yx == 0.0f &&
         matrix->yy == 1.0f;
}

/// Widens and/or shears the just-loaded glyph in place, in the FONT's own
/// space. Answers whether it changed anything.
///
/// The advances a SHAPED run reports do not come through here at all; they
/// come from HarfBuzz, which is told the same two numbers by
/// ztypesetFaceApplySynthetic and applies them with the same arithmetic.
static bool applySynthetic(const ZtypesetFace* face, FT_GlyphSlot slot) {
  if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return false;
  if (face->synthetic_bold == 0.0f && face->synthetic_oblique == 0.0f) {
    return false;
  }

  if (face->synthetic_bold != 0.0f) {
    const FT_Size_Metrics* metrics = &face->font->ft->size->metrics;
    const FT_Pos xstr =
        scaledStrength((uint32_t)metrics->x_ppem, face->synthetic_bold);
    const FT_Pos ystr =
        scaledStrength((uint32_t)metrics->y_ppem, face->synthetic_bold);
    FT_Outline_EmboldenXY(&slot->outline, xstr, ystr);

    // FT_Outline_EmboldenXY moves the outline only; the advance has to widen
    // by the same amount by hand, or bold text overlaps.
    if (slot->advance.x) slot->advance.x += xstr;
    if (slot->advance.y) slot->advance.y += ystr;
    slot->metrics.horiAdvance += xstr;
    slot->metrics.vertAdvance += ystr;
  }

  if (face->synthetic_oblique != 0.0f) {
    // A shear, not a rotation: the advance is untouched, because slanting
    // does not change how far the pen moves.
    FT_Matrix transform;
    transform.xx = 0x10000L;
    transform.yx = 0;
    transform.xy = toFixed16(face->synthetic_oblique);
    transform.yy = 0x10000L;
    FT_Outline_Transform(&slot->outline, &transform);
  }
  return true;
}


static FT_Stroker_LineCap ftLineCap(ZtypesetLineCap cap) {
  switch (cap) {
    case ZTYPESET_LINE_CAP_ROUND:
      return FT_STROKER_LINECAP_ROUND;
    case ZTYPESET_LINE_CAP_SQUARE:
      return FT_STROKER_LINECAP_SQUARE;
    case ZTYPESET_LINE_CAP_BUTT:
    default:
      return FT_STROKER_LINECAP_BUTT;
  }
}

static FT_Stroker_LineJoin ftLineJoin(ZtypesetLineJoin join) {
  switch (join) {
    case ZTYPESET_LINE_JOIN_BEVEL:
      return FT_STROKER_LINEJOIN_BEVEL;
    case ZTYPESET_LINE_JOIN_MITER:
      return FT_STROKER_LINEJOIN_MITER_VARIABLE;
    case ZTYPESET_LINE_JOIN_MITER_FIXED:
      return FT_STROKER_LINEJOIN_MITER_FIXED;
    case ZTYPESET_LINE_JOIN_ROUND:
    default:
      return FT_STROKER_LINEJOIN_ROUND;
  }
}

/// Grows the face's stroked outline to hold at least this much, keeping
/// whatever it already had room for.
///
/// FT_Outline_New sets n_points and n_contours to the sizes it allocated;
/// the stroker's exports APPEND, so the counts are reset to zero at the point
/// of use and the allocated sizes are remembered here instead.
static ZtypesetResult reserveStroked(ZtypesetFace* face, FT_UInt points,
                                  FT_UInt contours) {
  if (points <= face->stroked_points && contours <= face->stroked_contours) {
    return ZTYPESET_RESULT_OK;
  }
  const FT_UInt want_points =
      points > face->stroked_points ? points : face->stroked_points;
  const FT_UInt want_contours =
      contours > face->stroked_contours ? contours : face->stroked_contours;

  FT_Library ft = face->font->library->ft;
  FT_Outline grown;
  const FT_Error error =
      FT_Outline_New(ft, want_points, (FT_Int)want_contours, &grown);
  if (error != FT_Err_Ok) return ztypesetFromFtError(error);

  if (face->stroked_points != 0u) FT_Outline_Done(ft, &face->stroked);
  face->stroked = grown;
  face->stroked_points = want_points;
  face->stroked_contours = want_contours;
  return ZTYPESET_RESULT_OK;
}

/// Traces this face's pen round the just-loaded glyph and puts the result in
/// the slot, so everything downstream -- metrics, rasteriser, decomposition --
/// sees one stroked outline rather than three that had to agree.
///
/// The slot's outline is left pointing at memory this FACE owns. Every entry
/// point loads the glyph before reading it and FT_Load_Glyph reassigns
/// slot->outline from the driver's own loader, so the borrowed pointer never
/// outlives the call that made it; ztypesetFaceDestroy clears it anyway rather
/// than leave a freed pointer reachable.
static ZtypesetResult applyStroke(ZtypesetFace* face, FT_GlyphSlot slot,
                               bool* stroked) {
  *stroked = false;
  if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return ZTYPESET_RESULT_OK;
  if (face->stroke.radius <= 0.0f) return ZTYPESET_RESULT_OK;
  // A glyph with no ink -- a space -- has no path to trace, and stroking
  // nothing is still nothing.
  if (slot->outline.n_points == 0) return ZTYPESET_RESULT_OK;

  if (face->stroker == NULL) {
    const FT_Error error =
        FT_Stroker_New(face->font->library->ft, &face->stroker);
    if (error != FT_Err_Ok) return ztypesetFromFtError(error);
  }

  // FreeType's own default, and SVG's and PostScript's. Its unit is a
  // multiple of the radius, in 16.16.
  const float miter_limit =
      face->stroke.miter_limit > 0.0f ? face->stroke.miter_limit : 4.0f;
  // FT_Stroker_Set rewinds the stroker, so the borders left by the previous
  // glyph are dropped here rather than accumulated.
  FT_Stroker_Set(face->stroker,
                 (FT_Fixed)ztypesetToFixed266(face->stroke.radius),
                 ftLineCap(face->stroke.cap), ftLineJoin(face->stroke.join),
                 toFixed16(miter_limit));

  FT_Error error = FT_Stroker_ParseOutline(face->stroker, &slot->outline, 0);
  if (error != FT_Err_Ok) return ztypesetFromFtError(error);

  // The BAND is both contours, wound against each other, which is
  // FT_Glyph_Stroke; either one alone is a solid shape, which is
  // FT_Glyph_StrokeBorder. Which of them is "outside" depends on the
  // contour's winding, which is the font format's business and not the
  // caller's -- FreeType answers it.
  const bool whole = face->stroke.style == ZTYPESET_STROKE_STYLE_BAND;
  FT_StrokerBorder border = FT_STROKER_BORDER_LEFT;
  FT_UInt points = 0u;
  FT_UInt contours = 0u;
  if (whole) {
    error = FT_Stroker_GetCounts(face->stroker, &points, &contours);
  } else {
    border = face->stroke.style == ZTYPESET_STROKE_STYLE_SHRUNK
                 ? FT_Outline_GetInsideBorder(&slot->outline)
                 : FT_Outline_GetOutsideBorder(&slot->outline);
    error =
        FT_Stroker_GetBorderCounts(face->stroker, border, &points, &contours);
  }
  if (error != FT_Err_Ok) return ztypesetFromFtError(error);
  if (points == 0u || contours == 0u) return ZTYPESET_RESULT_OK;

  const ZtypesetResult reserved = reserveStroked(face, points, contours);
  if (reserved != ZTYPESET_RESULT_OK) return reserved;

  face->stroked.n_points = 0;
  face->stroked.n_contours = 0;
  if (whole) {
    FT_Stroker_Export(face->stroker, &face->stroked);
  } else {
    FT_Stroker_ExportBorder(face->stroker, border, &face->stroked);
  }

  slot->outline = face->stroked;
  *stroked = true;
  return ZTYPESET_RESULT_OK;
}

/// Maps the glyph with the caller's own 2x2. Answers whether it changed
/// anything.
///
/// No advance is touched here, and that is the decision rather than an
/// omission: a shaped run's advances come from HarfBuzz, which has no matrix to
/// be told about, so transforming FreeType's and not HarfBuzz's would make the
/// two disagree by exactly the caller's matrix. See ztypesetFaceSetTransform.
static bool applyTransform(const ZtypesetFace* face, FT_GlyphSlot slot) {
  if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return false;
  if (isIdentity(&face->transform)) return false;

  // FT_Matrix is written the way the multiply reads: xy is the y term of the
  // x output, which is the same convention as ZtypesetMatrix.
  FT_Matrix matrix;
  matrix.xx = toFixed16(face->transform.xx);
  matrix.xy = toFixed16(face->transform.xy);
  matrix.yx = toFixed16(face->transform.yx);
  matrix.yy = toFixed16(face->transform.yy);
  FT_Outline_Transform(&slot->outline, &matrix);
  return true;
}

/// Recomputes the glyph's cached ink metrics from the outline as it now is.
///
/// Neither of the two steps above updates them -- they move the outline and
/// nothing else -- so ztypesetFaceGlyphExtents would otherwise keep reporting
/// the bounds the glyph had when it was loaded. The control-point box is what
/// ztypesetFaceGlyphExtents reads through slot->metrics.
static void refreshInkMetrics(FT_GlyphSlot slot) {
  FT_BBox cbox;
  FT_Outline_Get_CBox(&slot->outline, &cbox);
  slot->metrics.horiBearingX = cbox.xMin;
  slot->metrics.horiBearingY = cbox.yMax;
  slot->metrics.width = cbox.xMax - cbox.xMin;
  slot->metrics.height = cbox.yMax - cbox.yMin;
}

static ZtypesetResult loadGlyph(ZtypesetFace* face, uint32_t glyph_id,
                             ZtypesetHinting hinting, ZtypesetRenderMode mode) {
  if (face == NULL) return ZTYPESET_RESULT_INVALID_ARGUMENT;
  if (glyph_id >= (uint32_t)face->font->ft->num_glyphs) {
    return ZTYPESET_RESULT_GLYPH_NOT_FOUND;
  }

  // The FT_Face is shared with this font's other faces, so the size to load
  // at is whichever one was activated last -- make it ours.
  ztypesetFaceActivate(face);

  const FT_Error error = FT_Load_Glyph(face->font->ft, (FT_UInt)glyph_id,
                                       loadFlags(hinting, mode));
  if (error != FT_Err_Ok) return ztypesetFromFtError(error);

  // Order is load-bearing, and it is design, then ornament, then map. The
  // two synthetic styles are part of the FONT's design and are applied in its
  // own space -- emboldening after a rotation would be emboldening along the
  // rotated axes, which is not what a bolder font looks like. The pen traces
  // the glyph the font describes, so it comes after them and before the
  // matrix; a pen applied first would be widened by the emboldening, and one
  // applied last would be a pen in the caller's device space.
  FT_GlyphSlot slot = face->font->ft->glyph;
  const bool synthesised = applySynthetic(face, slot);
  bool stroked = false;
  const ZtypesetResult stroke_result = applyStroke(face, slot, &stroked);
  if (stroke_result != ZTYPESET_RESULT_OK) return stroke_result;
  const bool transformed = applyTransform(face, slot);
  if (synthesised || stroked || transformed) refreshInkMetrics(slot);
  return ZTYPESET_RESULT_OK;
}

/// Every number this file converts to fixed point has to be one. NaN compares
/// false with itself, which is how it is caught without <math.h>, and an
/// infinity would reach FreeType's fixed-point conversion as an undefined
/// cast.
static bool isFiniteStrength(float value) {
  return value == value && value > -1.0e30f && value < 1.0e30f;
}

ZtypesetResult ztypesetFaceSetTransform(ZtypesetFace* face,
                                        const ZtypesetMatrix* matrix) {
  if (face == NULL) return ZTYPESET_RESULT_INVALID_ARGUMENT;

  ZtypesetMatrix wanted;
  if (matrix == NULL) {
    wanted.xx = 1.0f;
    wanted.xy = 0.0f;
    wanted.yx = 0.0f;
    wanted.yy = 1.0f;
  } else {
    if (!isFiniteStrength(matrix->xx) || !isFiniteStrength(matrix->xy) ||
        !isFiniteStrength(matrix->yx) || !isFiniteStrength(matrix->yy)) {
      return ZTYPESET_RESULT_INVALID_ARGUMENT;
    }
    wanted = *matrix;
  }

  // No generation bump, and deliberately: the matrix reaches no advance and
  // no HarfBuzz extent, so nothing a shaped run reports goes stale.
  face->transform = wanted;
  return ZTYPESET_RESULT_OK;
}

ZtypesetResult ztypesetFaceTransform(const ZtypesetFace* face,
                                     ZtypesetMatrix* out) {
  if (out == NULL) return ZTYPESET_RESULT_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (face == NULL) return ZTYPESET_RESULT_INVALID_ARGUMENT;
  *out = face->transform;
  return ZTYPESET_RESULT_OK;
}

ZtypesetResult ztypesetFaceSetStroke(ZtypesetFace* face,
                                     const ZtypesetStroke* stroke) {
  if (face == NULL) return ZTYPESET_RESULT_INVALID_ARGUMENT;

  ZtypesetStroke wanted;
  memset(&wanted, 0, sizeof(wanted));
  if (stroke != NULL) {
    if (!isFiniteStrength(stroke->radius) ||
        !isFiniteStrength(stroke->miter_limit)) {
      return ZTYPESET_RESULT_INVALID_ARGUMENT;
    }
    // A radius too large for 26.6 converts to zero, which would read as "no
    // stroke" -- a request refused is better than a request silently
    // dropped.
    if (stroke->radius > 0.0f && ztypesetToFixed266(stroke->radius) == 0) {
      ztypesetSetErrorDetail("the stroke radius is larger than "
                             "FreeType's fixed point can hold");
      return ZTYPESET_RESULT_INVALID_ARGUMENT;
    }
    // Rejected rather than defaulted: a cap this build does not name is a
    // consumer built against a newer header, and quietly drawing a butt cap
    // where it asked for something else is the silent failure this package
    // exists to refuse.
    // Through int, not the enum types: a C enum with no negative enumerator
    // may be compiled unsigned, and `< 0` on it is a comparison the compiler
    // is entitled to fold away -- the check would be gone with a warning
    // nobody had turned on.
    const int cap = (int)stroke->cap;
    const int join = (int)stroke->join;
    const int style = (int)stroke->style;
    if (cap < (int)ZTYPESET_LINE_CAP_BUTT ||
        cap > (int)ZTYPESET_LINE_CAP_SQUARE ||
        join < (int)ZTYPESET_LINE_JOIN_ROUND ||
        join > (int)ZTYPESET_LINE_JOIN_MITER_FIXED ||
        style < (int)ZTYPESET_STROKE_STYLE_BAND ||
        style > (int)ZTYPESET_STROKE_STYLE_SHRUNK) {
      ztypesetSetErrorDetail("the stroke names a cap, join or style this build "
                          "does not have");
      return ZTYPESET_RESULT_INVALID_ARGUMENT;
    }
    wanted = *stroke;
  }

  // No generation bump, for the same reason as the matrix: a pen moves ink
  // and no advance, and nothing a shaped run reports goes stale.
  face->stroke = wanted;
  return ZTYPESET_RESULT_OK;
}

ZtypesetResult ztypesetFaceStroke(const ZtypesetFace* face,
                                  ZtypesetStroke* out) {
  if (out == NULL) return ZTYPESET_RESULT_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (face == NULL) return ZTYPESET_RESULT_INVALID_ARGUMENT;
  *out = face->stroke;
  return ZTYPESET_RESULT_OK;
}

ZtypesetResult ztypesetFaceSetSyntheticBold(ZtypesetFace* face,
                                            float strength) {
  if (face == NULL || !isFiniteStrength(strength)) {
    return ZTYPESET_RESULT_INVALID_ARGUMENT;
  }
  if (face->synthetic_bold == strength) return ZTYPESET_RESULT_OK;
  face->synthetic_bold = strength;
  // Shaped advances move with this now, so a run measured against this face
  // before the change is as stale as one measured before a resize.
  face->generation = ztypesetNextGeneration();
  ztypesetFaceApplySynthetic(face);
  return ZTYPESET_RESULT_OK;
}

ZtypesetResult ztypesetFaceSetSyntheticOblique(ZtypesetFace* face,
                                               float slant) {
  if (face == NULL || !isFiniteStrength(slant)) {
    return ZTYPESET_RESULT_INVALID_ARGUMENT;
  }
  if (face->synthetic_oblique == slant) return ZTYPESET_RESULT_OK;
  face->synthetic_oblique = slant;
  // A shear moves the ink and not the advance, but ztypesetShaperExtents reads
  // ink, so the same staleness applies.
  face->generation = ztypesetNextGeneration();
  ztypesetFaceApplySynthetic(face);
  return ZTYPESET_RESULT_OK;
}

/// Copies the rendered slot into the face's own buffer, tightly packed and
/// top-down.
///
/// Two things happen here that the borrowed pointer could not do. The pixels
/// stop belonging to an FT_Face shared with sibling faces, so the lifetime
/// rule shrinks to "this face"; and a negative pitch -- FreeType's bottom-up
/// bitmaps -- is normalised away, so no consumer can render upside down by
/// ignoring a sign it did not know about.
static ZtypesetResult copyBitmap(ZtypesetFace* face, const FT_Bitmap* bitmap,
                              ZtypesetGlyphBitmap* out) {
  const uint32_t width = bitmap->width;
  const uint32_t height = bitmap->rows;
  if (width == 0u || height == 0u) return ZTYPESET_RESULT_OK;

  const size_t row_bytes = (size_t)width;
  // The face's own allocator, which is its library's: a glyph buffer is part
  // of the face, and a handle's memory does not depend on what happened to be
  // installed the first time it drew something.
  if (!ztypesetArrayReserve(ztypesetAllocatorOf(face), &face->bitmap,
                         row_bytes * (size_t)height, 1u)) {
    return ZTYPESET_RESULT_OUT_OF_MEMORY;
  }
  unsigned char* dst = (unsigned char*)face->bitmap.data;
  face->bitmap.count = row_bytes * (size_t)height;

  const int pitch = bitmap->pitch;
  const unsigned char* src = bitmap->buffer;
  if (pitch < 0) {
    // Rows run bottom-up: the buffer points at the LAST row.
    src += (size_t)(-pitch) * (size_t)(height - 1u);
  }
  const size_t stride = (size_t)(pitch < 0 ? -pitch : pitch);
  for (uint32_t y = 0u; y < height; y++) {
    memcpy(dst + row_bytes * (size_t)y, src, row_bytes);
    src = pitch < 0 ? src - stride : src + stride;
  }

  out->pixels = dst;
  return ZTYPESET_RESULT_OK;
}

ZtypesetResult ztypesetFaceRenderGlyph(ZtypesetFace* face, uint32_t glyph_id,
                                 ZtypesetRenderMode mode,
    ZtypesetHinting hinting,
                                 int32_t offset_x, int32_t offset_y,
                                 ZtypesetGlyphBitmap* out) {
  if (out == NULL) return ZTYPESET_RESULT_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  const ZtypesetResult loaded = loadGlyph(face, glyph_id, hinting, mode);
  if (loaded != ZTYPESET_RESULT_OK) return loaded;

  FT_GlyphSlot slot = face->font->ft->glyph;

  // Ignored in SDF mode -- see ztypeset.h for why baking a sub-pixel shift into
  // a reusable, atlas-sampled field would be pointless.
  if (mode != ZTYPESET_RENDER_MODE_SDF && (offset_x != 0 || offset_y != 0)) {
    FT_Outline_Translate(&slot->outline, offset_x, offset_y);
  }
  const FT_Error error = FT_Render_Glyph(slot, ftRenderMode(mode));
  if (error != FT_Err_Ok) {
    const ZtypesetResult mapped = ztypesetFromFtError(error);
    // FreeType reports a rasteriser refusing the outline through the same
    // error space as everything else; anything that got this far is a render
    // failure rather than a bad font.
    return mapped == ZTYPESET_RESULT_BAD_FONT ? ZTYPESET_RESULT_RENDER_FAILED
                                           : mapped;
  }

  // Written whether or not there were pixels: a caller must be able to read
  // the format of an inkless glyph without a special case, and memset left
  // this at A8 regardless of what was rendered.
  const ZtypesetBitmapFormat format = bitmapFormatOf(mode);
  const uint32_t channels = ztypesetBitmapFormatChannels(format);

  // With FT_LOAD_NO_BITMAP this is always 8 bits per sample. Checked rather
  // than assumed, because a silent format change would reach a consumer as
  // garbage pixels, not as an error.
  //
  // num_grays is accepted as either 255 or 256, because FreeType disagrees
  // with itself: the glyph slot is set up with 256 (base/ftobjs.c:537) and the
  // SDF renderer overwrites it with 255 (sdf/ftsdfrend.c:316, :539). Both
  // produce one
  // byte per pixel over the full 0..255 range, so the difference is a
  // bookkeeping inconsistency upstream rather than a format difference --
  // recorded in UPSTREAM.md. Demanding 256 rejects every SDF glyph, which is
  // how this was found.
  if (slot->bitmap.width != 0u && slot->bitmap.rows != 0u) {
    const int gray_levels = (int)slot->bitmap.num_grays;
    if (slot->bitmap.pixel_mode != (unsigned char)ftPixelMode(format) ||
        (gray_levels != 256 && gray_levels != 255)) {
      ztypesetSetErrorDetail("FreeType produced a bitmap in another format");
      return ZTYPESET_RESULT_RENDER_FAILED;
    }
    // A subpixel bitmap is three samples wide, or three rows tall, per pixel.
    // Checked because the pixel dimensions below divide by three, and a
    // remainder would silently drop the last stripe of the last pixel.
    const uint32_t across = (mode == ZTYPESET_RENDER_MODE_LCD) ? channels : 1u;
    const uint32_t down = (mode == ZTYPESET_RENDER_MODE_LCD_V) ? channels : 1u;
    if (slot->bitmap.width % across != 0u || slot->bitmap.rows % down != 0u) {
      ztypesetSetErrorDetail("FreeType produced a subpixel bitmap of "
                             "a size that is not three samples per pixel");
      return ZTYPESET_RESULT_RENDER_FAILED;
    }
    const ZtypesetResult copied = copyBitmap(face, &slot->bitmap, out);
    if (copied != ZTYPESET_RESULT_OK) return copied;
  }

  out->format = format;
  // In PIXELS, in every format. FreeType counts an LCD bitmap's width in
  // samples and an LCD_V bitmap's height in sub-rows; a consumer that read
  // either as a pixel count would lay the glyph out three times too wide or
  // too tall.
  out->width = (mode == ZTYPESET_RENDER_MODE_LCD)
                   ? slot->bitmap.width / channels
                   : slot->bitmap.width;
  out->height = (mode == ZTYPESET_RENDER_MODE_LCD_V)
                    ? slot->bitmap.rows / channels
                    : slot->bitmap.rows;
  // Bytes per PIXEL row, so pitch * height is the buffer in every format. The
  // copy is tightly packed, and an LCD_V pixel row is three of FreeType's.
  out->pitch = (int32_t)(
      slot->bitmap.width *
      ((mode == ZTYPESET_RENDER_MODE_LCD_V) ? channels : 1u));
  out->left = slot->bitmap_left;
  out->top = slot->bitmap_top;
  out->x_advance = ztypesetFrom266(slot->advance.x);
  return ZTYPESET_RESULT_OK;
}

ZtypesetResult ztypesetFaceGlyphExtents(ZtypesetFace* face, uint32_t glyph_id,
                                  ZtypesetHinting hinting,
    ZtypesetExtents* out) {
  if (out == NULL) return ZTYPESET_RESULT_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  const ZtypesetResult loaded =
      loadGlyph(face, glyph_id, hinting, ZTYPESET_RENDER_MODE_A8);
  if (loaded != ZTYPESET_RESULT_OK) return loaded;

  // FT_Glyph_Metrics is 26.6 once the glyph has been scaled. y is up, and
  // horiBearingY is the distance from the baseline to the TOP of the ink, so
  // the bottom is that minus the height.
  const FT_Glyph_Metrics* metrics = &face->font->ft->glyph->metrics;
  out->x_min = ztypesetFrom266(metrics->horiBearingX);
  out->x_max = ztypesetFrom266(metrics->horiBearingX + metrics->width);
  out->y_max = ztypesetFrom266(metrics->horiBearingY);
  out->y_min = ztypesetFrom266(metrics->horiBearingY - metrics->height);
  out->x_advance = ztypesetFrom266(metrics->horiAdvance);
  out->y_advance = 0.0f;
  return ZTYPESET_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Outline decomposition
//===----------------------------------------------------------------------===//

typedef struct DecomposeContext {
  const ZtypesetOutlineFuncs* funcs;
  ZtypesetResult result;
  bool started;
} DecomposeContext;

/// FT_Outline_Decompose never calls a "close" event of its own -- a contour
/// ends with an implicit line/conic/cubic back to its start. This is where
/// ztypeset's close() is synthesised: once before every move_to but the first,
/// and once more after decomposition finishes successfully.
static int decomposeMoveTo(const FT_Vector* to, void* user) {
  DecomposeContext* ctx = (DecomposeContext*)user;
  if (ctx->started) {
    ctx->result = ctx->funcs->close(ctx->funcs->user);
    if (ctx->result != ZTYPESET_RESULT_OK) return 1;
  }
  ctx->started = true;
  ctx->result =
      ctx->funcs->move_to(ctx->funcs->user, (int32_t)to->x, (int32_t)to->y);
  return ctx->result == ZTYPESET_RESULT_OK ? 0 : 1;
}

static int decomposeLineTo(const FT_Vector* to, void* user) {
  DecomposeContext* ctx = (DecomposeContext*)user;
  ctx->result =
      ctx->funcs->line_to(ctx->funcs->user, (int32_t)to->x, (int32_t)to->y);
  return ctx->result == ZTYPESET_RESULT_OK ? 0 : 1;
}

static int decomposeConicTo(const FT_Vector* control, const FT_Vector* to,
                            void* user) {
  DecomposeContext* ctx = (DecomposeContext*)user;
  ctx->result = ctx->funcs->conic_to(ctx->funcs->user, (int32_t)control->x,
                                     (int32_t)control->y, (int32_t)to->x,
                                     (int32_t)to->y);
  return ctx->result == ZTYPESET_RESULT_OK ? 0 : 1;
}

static int decomposeCubicTo(const FT_Vector* control1,
                            const FT_Vector* control2, const FT_Vector* to,
                            void* user) {
  DecomposeContext* ctx = (DecomposeContext*)user;
  ctx->result = ctx->funcs->cubic_to(
      ctx->funcs->user, (int32_t)control1->x, (int32_t)control1->y,
      (int32_t)control2->x, (int32_t)control2->y, (int32_t)to->x,
      (int32_t)to->y);
  return ctx->result == ZTYPESET_RESULT_OK ? 0 : 1;
}

ZtypesetResult ztypesetFaceDecomposeOutline(ZtypesetFace* face,
                                            uint32_t glyph_id,
                                      ZtypesetHinting hinting,
                                      const ZtypesetOutlineFuncs* funcs) {
  if (funcs == NULL || funcs->move_to == NULL || funcs->line_to == NULL ||
      funcs->conic_to == NULL || funcs->cubic_to == NULL ||
      funcs->close == NULL) {
    return ZTYPESET_RESULT_INVALID_ARGUMENT;
  }

  const ZtypesetResult loaded =
      loadGlyph(face, glyph_id, hinting, ZTYPESET_RENDER_MODE_A8);
  if (loaded != ZTYPESET_RESULT_OK) return loaded;

  // shift = delta = 0: the points FT_Outline_Decompose hands the callbacks
  // below are exactly the outline's own 26.6 coordinates, already scaled to
  // this face's size by FT_Load_Glyph -- nothing to re-derive.
  const FT_Outline_Funcs ft_funcs = {
      decomposeMoveTo, decomposeLineTo, decomposeConicTo, decomposeCubicTo,
      0, 0,
  };
  DecomposeContext ctx;
  ctx.funcs = funcs;
  ctx.result = ZTYPESET_RESULT_OK;
  ctx.started = false;

  const FT_Error error = FT_Outline_Decompose(&face->font->ft->glyph->outline,
                                              &ft_funcs, &ctx);
  if (error != FT_Err_Ok) {
    // A callback's own decline surfaces as its own ZtypesetResult; anything
    // else is FreeType rejecting the outline's structure.
    return ctx.result != ZTYPESET_RESULT_OK ? ctx.result :
    ztypesetFromFtError(error);
  }

  return ctx.started ? funcs->close(funcs->user) : ZTYPESET_RESULT_OK;
}
