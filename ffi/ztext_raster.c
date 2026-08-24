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
  return ztextFromFtError(error);
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
                                 ZtextGlyphBitmap* out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  const ZtextResult loaded = loadGlyph(face, glyph_id, hinting, mode);
  if (loaded != ZTEXT_RESULT_OK) return loaded;

  FT_GlyphSlot slot = face->font->ft->glyph;
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
