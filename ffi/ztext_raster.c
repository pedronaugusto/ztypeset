//===----------------------------------------------------------------------===//
// ztext -- turning a glyph index into pixels.
//===----------------------------------------------------------------------===//

#include "ztext_internal.h"

/// FreeType load flags for a hinting mode.
///
/// FT_LOAD_NO_BITMAP is unconditional: without it a face carrying embedded
/// bitmap strikes can answer with a 1-bit or pre-rendered bitmap instead of a
/// rasterised outline, and the pixel format ztext promises would depend on
/// which glyph you asked for. ztext always rasterises outlines. Colour and
/// bitmap strikes are out of scope -- see README.
static FT_Int32 loadFlags(ZtextHinting hinting, ZtextRenderMode mode) {
  if (mode == ZTEXT_RENDER_MODE_SDF) {
    // A hinted outline has been moved onto the pixel grid for one specific
    // size. Baking that into a distance field, whose entire purpose is to be
    // sampled at other sizes, is self-defeating -- so SDF ignores the caller's
    // hinting rather than honouring a request that cannot be meant.
    return FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
  }

  switch (hinting) {
    case ZTEXT_HINTING_LIGHT:
      return FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP;
    case ZTEXT_HINTING_NONE:
      return FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
    case ZTEXT_HINTING_NORMAL:
    default:
      return FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP;
  }
}

// FT_GlyphSlot_Embolden's and FT_GlyphSlot_Oblique's own reference strengths
// (ftsynth.c). That module is documented by FreeType itself as "a code
// resource... copied into the application", not a stable API, so ztext keeps
// only the outline-format arithmetic it actually reaches -- every glyph here
// loads with FT_LOAD_NO_BITMAP, so the bitmap-format branch ftsynth.c also
// carries never applies.
#define ZTEXT_EMBOLDEN_DELTA 0x0AAA
#define ZTEXT_OBLIQUE_SLANT 0x0366A

/// Widens and/or shears the just-loaded glyph in place, so every caller that
/// loads through this face -- render, extents, outline decomposition -- sees
/// the same synthesised shape.
static void applySyntheticStyle(const ZtextFace* face, FT_GlyphSlot slot) {
  if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return;
  if (!face->synthetic_bold && !face->synthetic_oblique) return;

  if (face->synthetic_bold) {
    const FT_Size_Metrics* metrics = &face->font->ft->size->metrics;
    const FT_Pos xstr = (FT_Pos)metrics->x_ppem * ZTEXT_EMBOLDEN_DELTA / 1024;
    const FT_Pos ystr = (FT_Pos)metrics->y_ppem * ZTEXT_EMBOLDEN_DELTA / 1024;
    FT_Outline_EmboldenXY(&slot->outline, xstr, ystr);

    // FT_Outline_EmboldenXY moves the outline only; the advance has to widen
    // by the same amount by hand, or bold text overlaps.
    if (slot->advance.x) slot->advance.x += xstr;
    if (slot->advance.y) slot->advance.y += ystr;
    slot->metrics.horiAdvance += xstr;
    slot->metrics.vertAdvance += ystr;
  }

  if (face->synthetic_oblique) {
    // A shear, not a rotation: the advance is untouched, because slanting
    // does not change how far the pen moves.
    FT_Matrix transform;
    transform.xx = 0x10000L;
    transform.yx = 0;
    transform.xy = ZTEXT_OBLIQUE_SLANT;
    transform.yy = 0x10000L;
    FT_Outline_Transform(&slot->outline, &transform);
  }

  // Neither call above updates the glyph's cached ink metrics -- they move
  // the outline and nothing else -- so ztextFaceGlyphExtents would otherwise
  // keep reporting the pre-synthesis bounds. Recomputed from the transformed
  // outline's own control-point box, which is what ztextFaceGlyphExtents
  // reads through slot->metrics.
  FT_BBox cbox;
  FT_Outline_Get_CBox(&slot->outline, &cbox);
  slot->metrics.horiBearingX = cbox.xMin;
  slot->metrics.horiBearingY = cbox.yMax;
  slot->metrics.width = cbox.xMax - cbox.xMin;
  slot->metrics.height = cbox.yMax - cbox.yMin;
}

static ZtextResult loadGlyph(ZtextFace* face, uint32_t glyph_id,
                             ZtextHinting hinting, ZtextRenderMode mode) {
  if (face == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (glyph_id >= (uint32_t)face->font->ft->num_glyphs) {
    return ZTEXT_RESULT_GLYPH_NOT_FOUND;
  }

  // The FT_Face is shared with this font's other faces, so the size to load
  // at is whichever one was activated last -- make it ours.
  ztextFaceActivate(face);

  const FT_Error error = FT_Load_Glyph(face->font->ft, (FT_UInt)glyph_id,
                                       loadFlags(hinting, mode));
  if (error != FT_Err_Ok) return ztextFromFtError(error);

  applySyntheticStyle(face, face->font->ft->glyph);
  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFaceSetSyntheticBold(ZtextFace* face, int enabled) {
  if (face == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  face->synthetic_bold = enabled != 0;
  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFaceSetSyntheticOblique(ZtextFace* face, int enabled) {
  if (face == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  face->synthetic_oblique = enabled != 0;
  return ZTEXT_RESULT_OK;
}

/// Copies the rendered slot into the face's own buffer, tightly packed and
/// top-down.
///
/// Two things happen here that the borrowed pointer could not do. The pixels
/// stop belonging to an FT_Face shared with sibling faces, so the lifetime
/// rule shrinks to "this face"; and a negative pitch -- FreeType's bottom-up
/// bitmaps -- is normalised away, so no consumer can render upside down by
/// ignoring a sign it did not know about.
static ZtextResult copyBitmap(ZtextFace* face, const FT_Bitmap* bitmap,
                              ZtextGlyphBitmap* out) {
  const uint32_t width = bitmap->width;
  const uint32_t height = bitmap->rows;
  if (width == 0u || height == 0u) return ZTEXT_RESULT_OK;

  const size_t row_bytes = (size_t)width;
  if (!ztextArrayReserve(&face->bitmap, row_bytes * (size_t)height, 1u)) {
    return ZTEXT_RESULT_OUT_OF_MEMORY;
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
  out->pitch = (int32_t)row_bytes;
  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFaceRenderGlyph(ZtextFace* face, uint32_t glyph_id,
                                 ZtextRenderMode mode, ZtextHinting hinting,
                                 int32_t offset_x, int32_t offset_y,
                                 ZtextGlyphBitmap* out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  const ZtextResult loaded = loadGlyph(face, glyph_id, hinting, mode);
  if (loaded != ZTEXT_RESULT_OK) return loaded;

  FT_GlyphSlot slot = face->font->ft->glyph;

  // Ignored in SDF mode -- see ztext.h for why baking a sub-pixel shift into
  // a reusable, atlas-sampled field would be pointless.
  if (mode != ZTEXT_RENDER_MODE_SDF && (offset_x != 0 || offset_y != 0)) {
    FT_Outline_Translate(&slot->outline, offset_x, offset_y);
  }
  const FT_Render_Mode render_mode = (mode == ZTEXT_RENDER_MODE_SDF)
                                         ? FT_RENDER_MODE_SDF
                                         : FT_RENDER_MODE_NORMAL;

  const FT_Error error = FT_Render_Glyph(slot, render_mode);
  if (error != FT_Err_Ok) {
    const ZtextResult mapped = ztextFromFtError(error);
    // FreeType reports a rasteriser refusing the outline through the same
    // error space as everything else; anything that got this far is a render
    // failure rather than a bad font.
    return mapped == ZTEXT_RESULT_BAD_FONT ? ZTEXT_RESULT_RENDER_FAILED
                                           : mapped;
  }

  // With FT_LOAD_NO_BITMAP and the smooth or SDF renderer this is always 8-bit
  // grey. Checked rather than assumed, because a silent format change would
  // reach a consumer as garbage pixels, not as an error.
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
    if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY ||
        (gray_levels != 256 && gray_levels != 255)) {
      ztextSetErrorDetail("FreeType produced a bitmap that is not 8-bit grey");
      return ZTEXT_RESULT_RENDER_FAILED;
    }
    const ZtextResult copied = copyBitmap(face, &slot->bitmap, out);
    if (copied != ZTEXT_RESULT_OK) return copied;
  }

  // Written whether or not there were pixels: a caller must be able to read
  // the format of an inkless glyph without a special case, and memset left
  // this at A8 regardless of what was rendered.
  out->format = (mode == ZTEXT_RENDER_MODE_SDF) ? ZTEXT_BITMAP_FORMAT_SDF
                                                : ZTEXT_BITMAP_FORMAT_A8;
  out->width = slot->bitmap.width;
  out->height = slot->bitmap.rows;
  out->left = slot->bitmap_left;
  out->top = slot->bitmap_top;
  out->x_advance = (float)slot->advance.x / 64.0f;
  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFaceGlyphExtents(ZtextFace* face, uint32_t glyph_id,
                                  ZtextHinting hinting, ZtextExtents* out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  const ZtextResult loaded =
      loadGlyph(face, glyph_id, hinting, ZTEXT_RENDER_MODE_A8);
  if (loaded != ZTEXT_RESULT_OK) return loaded;

  // FT_Glyph_Metrics is 26.6 once the glyph has been scaled. y is up, and
  // horiBearingY is the distance from the baseline to the TOP of the ink, so
  // the bottom is that minus the height.
  const FT_Glyph_Metrics* metrics = &face->font->ft->glyph->metrics;
  out->x_min = (float)metrics->horiBearingX / 64.0f;
  out->x_max = (float)(metrics->horiBearingX + metrics->width) / 64.0f;
  out->y_max = (float)metrics->horiBearingY / 64.0f;
  out->y_min = (float)(metrics->horiBearingY - metrics->height) / 64.0f;
  out->x_advance = (float)metrics->horiAdvance / 64.0f;
  out->y_advance = 0.0f;
  return ZTEXT_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Outline decomposition
//===----------------------------------------------------------------------===//

typedef struct DecomposeContext {
  const ZtextOutlineFuncs* funcs;
  ZtextResult result;
  bool started;
} DecomposeContext;

/// FT_Outline_Decompose never calls a "close" event of its own -- a contour
/// ends with an implicit line/conic/cubic back to its start. This is where
/// ztext's close() is synthesised: once before every move_to but the first,
/// and once more after decomposition finishes successfully.
static int decomposeMoveTo(const FT_Vector* to, void* user) {
  DecomposeContext* ctx = (DecomposeContext*)user;
  if (ctx->started) {
    ctx->result = ctx->funcs->close(ctx->funcs->user);
    if (ctx->result != ZTEXT_RESULT_OK) return 1;
  }
  ctx->started = true;
  ctx->result =
      ctx->funcs->move_to(ctx->funcs->user, (int32_t)to->x, (int32_t)to->y);
  return ctx->result == ZTEXT_RESULT_OK ? 0 : 1;
}

static int decomposeLineTo(const FT_Vector* to, void* user) {
  DecomposeContext* ctx = (DecomposeContext*)user;
  ctx->result =
      ctx->funcs->line_to(ctx->funcs->user, (int32_t)to->x, (int32_t)to->y);
  return ctx->result == ZTEXT_RESULT_OK ? 0 : 1;
}

static int decomposeConicTo(const FT_Vector* control, const FT_Vector* to,
                            void* user) {
  DecomposeContext* ctx = (DecomposeContext*)user;
  ctx->result = ctx->funcs->conic_to(ctx->funcs->user, (int32_t)control->x,
                                     (int32_t)control->y, (int32_t)to->x,
                                     (int32_t)to->y);
  return ctx->result == ZTEXT_RESULT_OK ? 0 : 1;
}

static int decomposeCubicTo(const FT_Vector* control1,
                            const FT_Vector* control2, const FT_Vector* to,
                            void* user) {
  DecomposeContext* ctx = (DecomposeContext*)user;
  ctx->result = ctx->funcs->cubic_to(
      ctx->funcs->user, (int32_t)control1->x, (int32_t)control1->y,
      (int32_t)control2->x, (int32_t)control2->y, (int32_t)to->x,
      (int32_t)to->y);
  return ctx->result == ZTEXT_RESULT_OK ? 0 : 1;
}

ZtextResult ztextFaceDecomposeOutline(ZtextFace* face, uint32_t glyph_id,
                                      ZtextHinting hinting,
                                      const ZtextOutlineFuncs* funcs) {
  if (funcs == NULL || funcs->move_to == NULL || funcs->line_to == NULL ||
      funcs->conic_to == NULL || funcs->cubic_to == NULL ||
      funcs->close == NULL) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  const ZtextResult loaded =
      loadGlyph(face, glyph_id, hinting, ZTEXT_RENDER_MODE_A8);
  if (loaded != ZTEXT_RESULT_OK) return loaded;

  // shift = delta = 0: the points FT_Outline_Decompose hands the callbacks
  // below are exactly the outline's own 26.6 coordinates, already scaled to
  // this face's size by FT_Load_Glyph -- nothing to re-derive.
  const FT_Outline_Funcs ft_funcs = {
      decomposeMoveTo, decomposeLineTo, decomposeConicTo, decomposeCubicTo,
      0, 0,
  };
  DecomposeContext ctx;
  ctx.funcs = funcs;
  ctx.result = ZTEXT_RESULT_OK;
  ctx.started = false;

  const FT_Error error = FT_Outline_Decompose(&face->font->ft->glyph->outline,
                                              &ft_funcs, &ctx);
  if (error != FT_Err_Ok) {
    // A callback's own decline surfaces as its own ZtextResult; anything else
    // is FreeType rejecting the outline's structure.
    return ctx.result != ZTEXT_RESULT_OK ? ctx.result : ztextFromFtError(error);
  }

  return ctx.started ? funcs->close(funcs->user) : ZTEXT_RESULT_OK;
}
