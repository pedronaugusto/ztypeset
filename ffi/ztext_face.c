//===----------------------------------------------------------------------===//
// ztext -- library and face lifetime, sizing and metrics.
//===----------------------------------------------------------------------===//

#include "ztext_internal.h"

//===----------------------------------------------------------------------===//
// Library
//===----------------------------------------------------------------------===//

ZtextResult ztextLibraryCreate(ZtextLibrary** out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  *out = NULL;

  ZtextLibrary* library = ZTEXT_NEW(ZtextLibrary);
  if (library == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;

  // FT_New_Library rather than FT_Init_FreeType, because only the former
  // accepts an FT_Memory. This is the whole reason FreeType's allocation is
  // per-library here instead of process-wide.
  ztextInitFtMemory(library);
  const FT_Error error = FT_New_Library(&library->memory, &library->ft);
  if (error != FT_Err_Ok) {
    ztextFree(library);
    return ztextFromFtError(error);
  }

  FT_Add_Default_Modules(library->ft);

  // Deliberately NOT FT_Set_Default_Properties, which FT_Init_FreeType calls.
  // That function reads the FREETYPE_PROPERTIES environment variable and lets
  // it change the interpreter version, the autohinter's warping and more. An
  // engine whose glyph rasterisation depends on an environment variable has no
  // reproducible output, and ztext's own golden tests would inherit that.
  // Anything ztext should expose is exposed as an API instead.

  *out = library;
  return ZTEXT_RESULT_OK;
}

void ztextLibraryDestroy(ZtextLibrary* library) {
  if (library == NULL) return;

  // FT_Done_Library frees through library->memory, so the record and the
  // allocator behind it must outlive this call -- which they do, both being
  // embedded in the struct freed on the next line.
  FT_Done_Library(library->ft);

  // Through the library's own allocator, which is the one the handle was
  // allocated from: ztextInitFtMemory copied the then-current global into it
  // immediately after this struct was allocated. A library is therefore
  // entirely self-consistent even if the process-wide allocator is replaced
  // during its lifetime.
  ztextFreeWith(&library->allocator, library);
}

ZtextResult ztextLibrarySetSdfSpread(ZtextLibrary* library, uint32_t spread) {
  if (library == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  // FreeType clamps out-of-range values silently; refusing here means a caller
  // that asks for 100 finds out rather than quietly getting 32.
  if (spread < 2u || spread > 32u) return ZTEXT_RESULT_INVALID_ARGUMENT;
  FT_Int value = (FT_Int)spread;

  // FreeType registers TWO signed-distance-field renderers, each with its own
  // copy of the property: `sdf` renders from an outline, `bsdf` from an
  // existing bitmap (src/sdf/ftsdfrend.c). ztext always loads with
  // FT_LOAD_NO_BITMAP, so in practice only `sdf` runs -- but setting one and
  // not the other would make this function a half-truth the day that changes.
  FT_Error error = FT_Property_Set(library->ft, "sdf", "spread", &value);
  if (error != FT_Err_Ok) return ztextFromFtError(error);
  error = FT_Property_Set(library->ft, "bsdf", "spread", &value);
  return ztextFromFtError(error);
}

//===----------------------------------------------------------------------===//
// Format sniffing
//
// FreeType answers "unknown file format" both for a format no compiled driver
// handles and for bytes that are not a font at all. Telling those apart is
// useful -- one means "cook this differently", the other means "your asset is
// corrupt" -- and only ztext knows which drivers it left out, so it does the
// telling rather than pushing the ambiguity to the caller.
//===----------------------------------------------------------------------===//

static bool tagIs(const unsigned char* data, const char* tag) {
  return data[0] == (unsigned char)tag[0] && data[1] == (unsigned char)tag[1] &&
         data[2] == (unsigned char)tag[2] && data[3] == (unsigned char)tag[3];
}

/// Returns a description when the bytes are a font format this build cannot
/// read, NULL when they should be handed to FreeType.
static const char* unsupportedFormat(const void* data, size_t size) {
  if (size < 4u) return NULL;
  const unsigned char* p = (const unsigned char*)data;

  if (tagIs(p, "wOFF")) return "WOFF is not compiled in; cook to TTF or OTF";
  if (tagIs(p, "wOF2")) return "WOFF2 is not compiled in; cook to TTF or OTF";
  if (tagIs(p, "%!PS")) return "Type 1 / PostScript fonts are not compiled in";
  if (p[0] == 0x80u && p[1] == 0x01u) return "PFB fonts are not compiled in";
  if (tagIs(p, "\1fcp")) return "PCF bitmap fonts are not compiled in";
  if (tagIs(p, "STAR")) return "BDF bitmap fonts are not compiled in";

  return NULL;
}

ZtextResult ztextLibraryCountFaces(ZtextLibrary* library, const void* data,
                                   size_t size, uint32_t* out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  *out = 0u;
  if (library == NULL || data == NULL || size == 0u) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }
  if (size > (size_t)UINT_MAX || size > (size_t)LONG_MAX) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  const char* unsupported = unsupportedFormat(data, size);
  if (unsupported != NULL) {
    ztextSetErrorDetail(unsupported);
    return ZTEXT_RESULT_UNSUPPORTED;
  }

  // A face index of -1 is FreeType's documented way to ask about the file
  // rather than a face in it: the returned handle carries num_faces and
  // nothing else worth using.
  FT_Face probe = NULL;
  const FT_Error error = FT_New_Memory_Face(library->ft, (const FT_Byte*)data,
                                            (FT_Long)size, -1, &probe);
  if (error != FT_Err_Ok) return ztextFromFtError(error);

  *out = (uint32_t)probe->num_faces;
  FT_Done_Face(probe);
  return ZTEXT_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Font
//===----------------------------------------------------------------------===//

/// Frees the font once nothing needs it: the caller has let it go AND its last
/// face has been destroyed. Called from both sides, so neither order leaks and
/// neither double-frees.
static void releaseFont(ZtextFont* font) {
  if (!font->destroy_requested || font->live_faces != 0u) return;

  ZtextLibrary* library = font->library;
  if (font->hb_face != NULL) hb_face_destroy(font->hb_face);
  // The axis table came from FreeType's allocator, so it goes back through
  // FreeType rather than through ztextFreeWith -- FT_Get_MM_Var allocates it
  // with the library's memory record and FT_Done_MM_Var is the only thing
  // that knows its internal shape.
  if (font->mm != NULL) FT_Done_MM_Var(library->ft, font->mm);
  ztextFreeWith(&library->allocator, font->coords);
  FT_Done_Face(font->ft);
  ztextFreeWith(&library->allocator, font);
}

/// Fetches the font's `fvar` axes and the coordinates it starts at, if it has
/// any. A static font leaves `mm` NULL and is not an error.
///
/// The two arrays are sized from `num_axis`, which comes from `fvar`'s
/// axisCount and is therefore a 16-bit quantity -- so neither multiplication
/// below can overflow, and neither needs a limit invented for it.
///
/// Fetched once rather than per query because FT_Get_MM_Var allocates and
/// copies the whole table every time it is called, and "how many axes does
/// this font have?" is a question a layout pass asks freely.
///
/// A failure here is a real failure rather than "treat it as static": on a
/// face FreeType has already flagged as variable, FT_Get_MM_Var fails only for
/// a broken table or for want of memory, and quietly downgrading either would
/// hand back a font whose axes exist but can never be reached.
static ZtextResult initVariations(ZtextFont* font) {
  if (!FT_HAS_MULTIPLE_MASTERS(font->ft)) return ZTEXT_RESULT_OK;

  const FT_Error error = FT_Get_MM_Var(font->ft, &font->mm);
  if (error != FT_Err_Ok) return ztextFromFtError(error);

  const size_t num_axis = (size_t)font->mm->num_axis;
  // One block for both representations, the 16.16 array first so the floats
  // that follow it are aligned by construction on every target.
  void* block = ztextAllocWith(
      &font->library->allocator,
      num_axis * (sizeof(FT_Fixed) + sizeof(float)), ZTEXT_DEFAULT_ALIGN);
  if (block == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;
  font->coords = (FT_Fixed*)block;
  font->design = (float*)(font->coords + num_axis);

  // A fresh face sits at the axis defaults, so this should both succeed and
  // agree with `def`. The fallback is here because an array left unwritten
  // would afterwards be read as a design value, which is a worse failure than
  // a missing one.
  if (FT_Get_Var_Design_Coordinates(font->ft, font->mm->num_axis,
                                    font->coords) != FT_Err_Ok) {
    for (size_t i = 0; i < num_axis; i++) {
      font->coords[i] = font->mm->axis[i].def;
    }
  }
  for (size_t i = 0; i < num_axis; i++) {
    font->design[i] = (float)font->coords[i] / 65536.0f;
  }
  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFontCreateFromMemory(ZtextLibrary* library, const void* data,
                                      size_t size, uint32_t face_index,
                                      ZtextFont** out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  *out = NULL;
  if (library == NULL || data == NULL || size == 0u) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }
  // HarfBuzz's blob length is an unsigned int. A font larger than that is not
  // a real case, but silently truncating one would be a memory-safety bug, so
  // it is refused explicitly.
  if (size > (size_t)UINT_MAX) return ZTEXT_RESULT_INVALID_ARGUMENT;

  const char* unsupported = unsupportedFormat(data, size);
  if (unsupported != NULL) {
    ztextSetErrorDetail(unsupported);
    return ZTEXT_RESULT_UNSUPPORTED;
  }

  // From the library's allocator, not the process-wide one, so everything a
  // font owns lives and dies with the same allocator its FT_Face memory does.
  ZtextFont* font = (ZtextFont*)ztextAllocWith(
      &library->allocator, sizeof(ZtextFont), ZTEXT_DEFAULT_ALIGN);
  if (font == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;
  memset(font, 0, sizeof(*font));
  font->library = library;

  const FT_Error error =
      FT_New_Memory_Face(library->ft, (const FT_Byte*)data, (FT_Long)size,
                         (FT_Long)face_index, &font->ft);
  if (error != FT_Err_Ok) {
    ztextFreeWith(&library->allocator, font);
    return ztextFromFtError(error);
  }

  // HarfBuzz reads the same bytes independently, through its own table reader.
  // The blob borrows them under the same lifetime rule the caller already
  // signed up to for FreeType, so there is no second contract to explain.
  hb_blob_t* blob = hb_blob_create((const char*)data, (unsigned int)size,
                                   HB_MEMORY_MODE_READONLY, NULL, NULL);

  // hb_face_create_or_fail, NOT hb_face_create. The latter answers a blob its
  // sanitiser rejected -- or an out-of-range index -- with a perfectly normal
  // face object that simply has no tables. Shaping through that returns
  // .notdef for every character with no error anywhere, while FreeType goes on
  // rasterising the same font correctly, so the symptom presents as a font
  // bug rather than a load failure. `_or_fail` returns NULL for both cases.
  font->hb_face = hb_face_create_or_fail(blob, face_index);
  hb_blob_destroy(blob);

  // The glyph count is a second, independent check: a face that survived
  // sanitisation but has no glyphs cannot shape anything either.
  if (font->hb_face == NULL || hb_face_get_glyph_count(font->hb_face) == 0u) {
    if (font->hb_face != NULL) hb_face_destroy(font->hb_face);
    FT_Done_Face(font->ft);
    ztextFreeWith(&library->allocator, font);
    ztextSetErrorDetail(
        "HarfBuzz rejected the font tables that FreeType accepted");
    return ZTEXT_RESULT_BAD_FONT;
  }

  const ZtextResult variations = initVariations(font);
  if (variations != ZTEXT_RESULT_OK) {
    if (font->mm != NULL) FT_Done_MM_Var(library->ft, font->mm);
    ztextFreeWith(&library->allocator, font->coords);
    hb_face_destroy(font->hb_face);
    FT_Done_Face(font->ft);
    ztextFreeWith(&library->allocator, font);
    return variations;
  }

  *out = font;
  return ZTEXT_RESULT_OK;
}

void ztextFontDestroy(ZtextFont* font) {
  if (font == NULL || font->destroy_requested) return;
  font->destroy_requested = true;
  releaseFont(font);
}

const char* ztextFontFamilyName(const ZtextFont* font) {
  if (font == NULL || font->ft->family_name == NULL) return "";
  return font->ft->family_name;
}

const char* ztextFontStyleName(const ZtextFont* font) {
  if (font == NULL || font->ft->style_name == NULL) return "";
  return font->ft->style_name;
}

uint32_t ztextFontGlyphIndex(const ZtextFont* font, uint32_t codepoint) {
  if (font == NULL) return 0u;
  return (uint32_t)FT_Get_Char_Index(font->ft, (FT_ULong)codepoint);
}

uint32_t ztextFontGlyphCount(const ZtextFont* font) {
  return font == NULL ? 0u : (uint32_t)font->ft->num_glyphs;
}

uint32_t ztextFontUnitsPerEm(const ZtextFont* font) {
  return font == NULL ? 0u : (uint32_t)font->ft->units_per_EM;
}

/// True for a character that must never start a new font run.
///
/// Two groups, both from HarfBuzz's own Unicode data rather than a table
/// hand-copied here:
///
///   Format characters -- ZWJ, ZWNJ, the bidi controls, soft hyphen. A font
///   is not expected to have glyphs for them, HarfBuzz removes them during
///   shaping, and breaking a run at one would split a ligature or a joining
///   form for no reason.
///
///   Variation selectors, which are marks and would otherwise be caught by
///   the mark rule below, but are worth naming: they modify the character
///   BEFORE them and belong to the same run by definition.
static bool ignorableForCoverage(hb_unicode_funcs_t* unicode, uint32_t cp) {
  if (cp >= 0xFE00u && cp <= 0xFE0Fu) return true;
  if (cp >= 0xE0100u && cp <= 0xE01EFu) return true;
  return hb_unicode_general_category(unicode, cp) ==
         HB_UNICODE_GENERAL_CATEGORY_FORMAT;
}

/// True for a combining mark, which attaches to what precedes it.
static bool isMark(hb_unicode_funcs_t* unicode, uint32_t cp) {
  switch (hb_unicode_general_category(unicode, cp)) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
      return true;
    default:
      return false;
  }
}

ZtextResult ztextFontCoveredPrefix(const ZtextFont* font, const char* utf8,
                                   size_t length, size_t* out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  *out = 0u;
  if (font == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (utf8 == NULL && length != 0u) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (!ztextIsValidUtf8(utf8, length)) return ZTEXT_RESULT_INVALID_UTF8;
  if (length == 0u) return ZTEXT_RESULT_OK;

  hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();

  size_t i = 0u;
  size_t boundary = 0u;
  while (i < length) {
    uint32_t cp = 0u;
    const size_t step = ztextDecodeUtf8(utf8 + i, length - i, &cp);

    if (!ignorableForCoverage(unicode, cp) &&
        FT_Get_Char_Index(font->ft, (FT_ULong)cp) == 0u) {
      break;
    }
    i += step;

    // A prefix may only END where the next character does not attach to the
    // one before it. Stopping mid-cluster would hand a base to one font and
    // its marks to another, which is the mistake this function exists to make
    // hard to write.
    if (i == length) {
      boundary = i;
      break;
    }
    uint32_t next = 0u;
    ztextDecodeUtf8(utf8 + i, length - i, &next);
    if (!isMark(unicode, next)) boundary = i;
  }

  *out = boundary;
  return ZTEXT_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Face
//===----------------------------------------------------------------------===//

void ztextFaceActivate(const ZtextFace* face) {
  FT_Activate_Size(face->ft_size);
}

static void destroyFaceParts(ZtextFace* face) {
  if (face->hb_ft_font != NULL) {
    hb_font_destroy(face->hb_ft_font);
    face->hb_ft_font = NULL;
  }
  if (face->hb_font != NULL) {
    hb_font_destroy(face->hb_font);
    face->hb_font = NULL;
  }
  if (face->ft_size != NULL) {
    FT_Done_Size(face->ft_size);
    face->ft_size = NULL;
  }
  ztextArrayFree(&face->bitmap, 1u);
}

ZtextResult ztextFaceCreate(ZtextFont* font, float width, float height,
                            ZtextFace** out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  *out = NULL;
  if (font == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  // A font the caller has already let go of is not a font to build on. This
  // is the one thing the order-free destruction above cannot make harmless,
  // so it is an error rather than a surprise.
  if (font->destroy_requested) {
    ztextSetErrorDetail("the font has already been destroyed");
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  ZtextLibrary* library = font->library;
  ZtextFace* face = (ZtextFace*)ztextAllocWith(
      &library->allocator, sizeof(ZtextFace), ZTEXT_DEFAULT_ALIGN);
  if (face == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;
  memset(face, 0, sizeof(*face));
  face->font = font;

  const FT_Error error = FT_New_Size(font->ft, &face->ft_size);
  if (error != FT_Err_Ok) {
    ztextFreeWith(&library->allocator, face);
    return ztextFromFtError(error);
  }

  face->hb_font = hb_font_create(font->hb_face);
  if (face->hb_font == NULL || face->hb_font == hb_font_get_empty()) {
    destroyFaceParts(face);
    ztextFreeWith(&library->allocator, face);
    return ZTEXT_RESULT_OUT_OF_MEMORY;
  }
  // HarfBuzz's own OpenType implementation: advances derived linearly from
  // design units, and an immutable font object.
  hb_ot_font_set_funcs(face->hb_font);

  // Counted and linked before the size is set, because ztextFaceDestroy below
  // is what unwinds a failure from here on and it decrements and unlinks.
  font->live_faces += 1u;
  face->next = font->faces;
  font->faces = face;

  // A font whose axes were moved before this face existed still has to reach
  // it. HarfBuzz keeps variation coordinates on the hb_font_t, not on the
  // shared hb_face_t, so a face created after the change would otherwise
  // shape the default instance under outlines FreeType is varying.
  ztextFaceApplyVariations(face);

  const ZtextResult sized = ztextFaceSetPixelSize(face, width, height);
  if (sized != ZTEXT_RESULT_OK) {
    ztextFaceDestroy(face);
    return sized;
  }

  *out = face;
  return ZTEXT_RESULT_OK;
}

void ztextFaceDestroy(ZtextFace* face) {
  if (face == NULL) return;
  ZtextFont* font = face->font;
  ZtextLibrary* library = font->library;

  // Out of the font's list before the memory goes, or ztextFontSetVariations
  // would walk into it. Singly linked and unordered, so this is the textbook
  // trailing-pointer removal and needs no more than it.
  for (ZtextFace** link = &font->faces; *link != NULL; link = &(*link)->next) {
    if (*link == face) {
      *link = face->next;
      break;
    }
  }

  destroyFaceParts(face);
  ztextFreeWith(&library->allocator, face);

  font->live_faces -= 1u;
  releaseFont(font);
}

ZtextFont* ztextFaceFont(const ZtextFace* face) {
  return face == NULL ? NULL : face->font;
}

/// Pixels to FreeType's 26.6, rounded to nearest. Returns 0 for anything
/// out of range, including NaN -- which is why the comparisons are written
/// positively rather than as negated bounds.
static int32_t toFixed266(float pixels) {
  if (!(pixels > 0.0f) || !(pixels <= 16384.0f)) return 0;
  return (int32_t)(pixels * 64.0f + 0.5f);
}

/// Pushes a validated 26.6 size at FreeType and at HarfBuzz.
///
/// Separate from ztextFaceSetPixelSize because ztextFontSetVariations has to
/// re-run exactly this: MVAR moves the ascender, the descender and the line
/// height, and those are computed when the size is set, so a face whose font
/// has just changed instance is carrying scaled metrics for the previous one.
/// Re-running it from the STORED 26.6 values rather than from floats means
/// nothing is re-derived through a round trip.
static ZtextResult setPixelSizeFixed(ZtextFace* face, int32_t fixed_width,
                                     int32_t fixed_height) {
  // FT_Set_Char_Size acts on whichever FT_Size is current, so this face's own
  // has to be made current first -- otherwise a font's second face would
  // resize its first.
  ztextFaceActivate(face);

  // FT_Set_Char_Size rather than FT_Set_Pixel_Sizes: the latter takes whole
  // pixels only. With a resolution of 0 FreeType substitutes 72 dpi
  // (freetype.h), at which one point is one pixel -- so a 26.6 char size
  // passes straight through as exact fractional pixels.
  const FT_Error error =
      FT_Set_Char_Size(face->font->ft, fixed_width, fixed_height, 0, 0);
  if (error != FT_Err_Ok) return ztextFromFtError(error);

  face->pixel_width = fixed_width;
  face->pixel_height = fixed_height;
  // A resize invalidates any shaped run measured against this face: the
  // advances came from the old size and the ink bounds would come from the
  // new one. Bumping the generation makes that a refusal rather than a
  // plausible-looking mixture.
  face->generation = ztextNextGeneration();

  // HarfBuzz reports positions in units of scale/upem per design unit, so a
  // scale that is already 26.6 pixels yields 26.6 fixed-point positions --
  // which is what the shaper converts to float.
  hb_font_set_scale(face->hb_font, (int)fixed_width, (int)fixed_height);
  // ppem is whole-pixel by definition in HarfBuzz's API, and it only selects
  // bitmap strikes and hinting behaviour, so rounding here loses nothing the
  // scale above does not already carry exactly.
  hb_font_set_ppem(face->hb_font, (unsigned int)((fixed_width + 32) / 64),
                   (unsigned int)((fixed_height + 32) / 64));

  // The FreeType-backed font, if one has been built, takes its scale from the
  // FT_Face and must be told the face changed.
  if (face->hb_ft_font != NULL) hb_ft_font_changed(face->hb_ft_font);

  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFaceSetPixelSize(ZtextFace* face, float width, float height) {
  if (face == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  // A zero axis copies the other, so this has to happen before the range
  // check -- and 0 is the one non-positive value that is not an error.
  if (width == 0.0f) width = height;
  if (height == 0.0f) height = width;

  // Rejects zero, negatives, NaN, infinity, anything above 16384 px, and
  // anything so small it would quantise to nothing.
  const int32_t fixed_width = toFixed266(width);
  const int32_t fixed_height = toFixed266(height);
  if (fixed_width == 0 || fixed_height == 0) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  return setPixelSizeFixed(face, fixed_width, fixed_height);
}

ZtextResult ztextFaceMetrics(const ZtextFace* face, ZtextFaceMetrics* out) {
  if (face == NULL || out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  const FT_Face ft = face->font->ft;
  out->units_per_em = ft->units_per_EM;
  out->num_glyphs = (uint32_t)ft->num_glyphs;
  out->pixel_size = (float)face->pixel_height / 64.0f;

  // The metrics below live on the FT_Size, and a sibling face may have been
  // the last to use this FT_Face.
  ztextFaceActivate(face);

  // 26.6 fixed point throughout.
  const FT_Size_Metrics* metrics = &ft->size->metrics;
  out->ascender = (float)metrics->ascender / 64.0f;
  out->descender = (float)metrics->descender / 64.0f;
  out->line_height = (float)metrics->height / 64.0f;
  out->max_advance = (float)metrics->max_advance / 64.0f;

  // Underline is a design-unit value in the face and has to be scaled by hand;
  // FreeType does not do it for you.
  if (out->units_per_em != 0u) {
    const float scale =
        ((float)face->pixel_height / 64.0f) / (float)out->units_per_em;
    out->underline_position = (float)ft->underline_position * scale;
    out->underline_thickness = (float)ft->underline_thickness * scale;
  }

  // FT_Size_Metrics has no vertical counterpart -- FreeType computes only the
  // four horizontal fields above from `hhea`. `vhea`'s Ascender/Descender/
  // Line_Gap extend along the SAME axis hhea's max_advance does (how far a
  // column sits from its baseline), so they scale with x_scale; its
  // advance_Height_Max extends along the axis hhea's ascender/descender do
  // (how far one glyph's vertical advance can be), so it scales with y_scale
  // -- the mirror image of how ft_recompute_scaled_metrics scales hhea.
  const TT_VertHeader* vert = NULL;
  if (FT_HAS_VERTICAL(ft)) {
    vert = (const TT_VertHeader*)FT_Get_Sfnt_Table(ft, FT_SFNT_VHEA);
  }
  if (vert != NULL) {
    out->vert_ascender =
        (float)FT_MulFix(vert->Ascender, metrics->x_scale) / 64.0f;
    out->vert_descender =
        (float)FT_MulFix(vert->Descender, metrics->x_scale) / 64.0f;
    const float line_gap =
        (float)FT_MulFix(vert->Line_Gap, metrics->x_scale) / 64.0f;
    out->vert_line_height = out->vert_ascender - out->vert_descender + line_gap;
    out->vert_max_advance =
        (float)FT_MulFix(vert->advance_Height_Max, metrics->y_scale) / 64.0f;
    out->has_vertical_metrics = 1u;
  } else {
    // Synthesised from ascender and descender: the same span HarfBuzz's own
    // vertical-advance fallback uses when a font has no vmtx
    // (hb_ot_get_glyph_v_advances), so a shaped run's synthesised advances
    // land in this range too.
    const float span = out->ascender - out->descender;
    out->vert_ascender = span / 2.0f;
    out->vert_descender = -span / 2.0f;
    out->vert_line_height = span;
    out->vert_max_advance = span;
    out->has_vertical_metrics = 0u;
  }

  return ZTEXT_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Variable fonts
//
// The setting is per FONT because FreeType makes it so: variation coordinates
// live on the FT_Face, and every face of a font shares one. See ztext.h for
// what that means for a caller.
//
// The half that is ztext's own work is keeping HarfBuzz in step. HarfBuzz
// keeps its coordinates on the hb_font_t, one per face, so FreeType's copy and
// HarfBuzz's are two places that can disagree -- and when they do, shaping
// describes one instance while rasterisation describes another. Nothing
// reports an error and the text merely spaces wrongly, so the only defence is
// to write both from one place, which is what these functions do.
//===----------------------------------------------------------------------===//

/// Index of the axis carrying `tag`, or `num_axis` when this font has no such
/// axis -- one past the end, so the caller has to look at the result.
///
/// Linear because `num_axis` is a single digit in every real font; a map would
/// be more code than the loop it would replace.
static FT_UInt findAxis(const ZtextFont* font, uint32_t tag) {
  const FT_UInt num_axis = font->mm->num_axis;
  for (FT_UInt i = 0; i < num_axis; i++) {
    if ((uint32_t)font->mm->axis[i].tag == tag) return i;
  }
  return num_axis;
}

/// FreeType's 16.16 to a design value.
static float fixedToDesign(FT_Fixed value) {
  return (float)value / 65536.0f;
}

/// A design value to FreeType's 16.16, rounded to nearest.
///
/// Through double rather than float: a float carries 24 bits of mantissa and a
/// 16.16 coordinate on an axis running to 1000 needs 26, so multiplying in
/// float would quietly drop the last bits of anything but a round number.
static FT_Fixed designToFixed(float value) {
  const double scaled = (double)value * 65536.0;
  return (FT_Fixed)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
}

void ztextFaceApplyVariations(ZtextFace* face) {
  const ZtextFont* font = face->font;
  if (font->mm == NULL) return;

  // Design coordinates in the font's own axis order -- the same array
  // FreeType was handed, only in float. Passing design rather than normalised
  // values means no tag lookup here and, more usefully, no second
  // normalisation that could disagree with FreeType's own.
  hb_font_set_var_coords_design(face->hb_font, font->design,
                                font->mm->num_axis);

  // The FreeType-backed font needs telling separately, and it genuinely does:
  // hb_ft_font_changed reads coordinates back off the FT_Face only when
  // HarfBuzz was configured with HAVE_FT_GET_VAR_BLEND_COORDINATES, which
  // upstream's build defines and ztext's does not. Its ADVANCES come from
  // FreeType either way, but the shape plan is built from the hb_font_t's own
  // coordinates -- so without this, a run shaped with use_freetype_metrics
  // would resolve GSUB feature variations against the default instance.
  if (face->hb_ft_font != NULL) {
    hb_font_set_var_coords_design(face->hb_ft_font, font->design,
                                  font->mm->num_axis);
  }
}

uint32_t ztextFontAxisCount(const ZtextFont* font) {
  if (font == NULL || font->mm == NULL) return 0u;
  return (uint32_t)font->mm->num_axis;
}

ZtextResult ztextFontAxis(const ZtextFont* font, uint32_t index,
                          ZtextVariationAxis* out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (font == NULL || font->mm == NULL || index >= font->mm->num_axis) {
    ztextSetErrorDetail("no such variation axis in this font");
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  const FT_Var_Axis* axis = &font->mm->axis[index];
  out->tag = (uint32_t)axis->tag;
  out->min_value = fixedToDesign(axis->minimum);
  out->default_value = fixedToDesign(axis->def);
  out->max_value = fixedToDesign(axis->maximum);
  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFontVariation(const ZtextFont* font, uint32_t index,
                               float* out) {
  if (out == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  *out = 0.0f;
  if (font == NULL || font->mm == NULL || index >= font->mm->num_axis) {
    ztextSetErrorDetail("no such variation axis in this font");
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  *out = font->design[index];
  return ZTEXT_RESULT_OK;
}

ZtextResult ztextFontSetVariations(ZtextFont* font,
                                   const ZtextVariation* values,
                                   size_t count) {
  if (font == NULL) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (values == NULL && count != 0u) return ZTEXT_RESULT_INVALID_ARGUMENT;
  if (font->mm == NULL) {
    ztextSetErrorDetail("this font has no variable axes");
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }

  const FT_UInt num_axis = font->mm->num_axis;

  // The whole request is checked before any of it is applied, so a call whose
  // third axis is misspelled leaves the first two where they were rather than
  // half-moving the font.
  for (size_t i = 0; i < count; i++) {
    const FT_UInt axis = findAxis(font, values[i].tag);
    if (axis == num_axis) {
      ztextSetErrorDetail("no such variation axis in this font");
      return ZTEXT_RESULT_INVALID_ARGUMENT;
    }
    const float value = values[i].value;
    // Written as two positive comparisons rather than a negated range so that
    // NaN, which compares false against everything, takes this branch instead
    // of slipping through. Infinities fail the bounds outright.
    if (!(value >= fixedToDesign(font->mm->axis[axis].minimum)) ||
        !(value <= fixedToDesign(font->mm->axis[axis].maximum))) {
      ztextSetErrorDetail("variation value is outside the axis range");
      return ZTEXT_RESULT_INVALID_ARGUMENT;
    }
  }

  ZtextLibrary* library = font->library;
  // Built to one side and committed only once FreeType has accepted it. The
  // font's own array could have been written in place and rolled back, but a
  // rollback path that runs only when FreeType fails is a path nothing tests.
  FT_Fixed* wanted = (FT_Fixed*)ztextAllocWith(
      &library->allocator, (size_t)num_axis * sizeof(FT_Fixed),
      ZTEXT_DEFAULT_ALIGN);
  if (wanted == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;

  // Starting from where the font already is, not from the defaults: a weight
  // slider and a width slider are two controls, and moving one must not snap
  // the other back.
  memcpy(wanted, font->coords, (size_t)num_axis * sizeof(FT_Fixed));
  for (size_t i = 0; i < count; i++) {
    // A tag named twice in one call is not rejected -- the later value simply
    // wins, which is what this loop does anyway. That is not written down as a
    // guarantee, so it is not a behaviour anything has to preserve.
    wanted[findAxis(font, values[i].tag)] = designToFixed(values[i].value);
  }

  const FT_Error error =
      FT_Set_Var_Design_Coordinates(font->ft, num_axis, wanted);
  if (error != FT_Err_Ok) {
    ztextFreeWith(&library->allocator, wanted);
    return ztextFromFtError(error);
  }

  memcpy(font->coords, wanted, (size_t)num_axis * sizeof(FT_Fixed));
  ztextFreeWith(&library->allocator, wanted);
  for (FT_UInt i = 0; i < num_axis; i++) {
    font->design[i] = fixedToDesign(font->coords[i]);
  }

  ZtextResult result = ZTEXT_RESULT_OK;
  for (ZtextFace* face = font->faces; face != NULL; face = face->next) {
    ztextFaceApplyVariations(face);

    // Bumped here as well as inside the resize below, so an already-measured
    // run is refused even in the unreachable case where FreeType declines the
    // size it accepted a moment ago.
    face->generation = ztextNextGeneration();

    // MVAR can move the ascender, the descender and the line height, and
    // FreeType computes those when the size is set -- so the size has to be
    // set again for this instance. Nothing else recomputes them, and a face
    // left alone would report the previous instance's leading.
    const ZtextResult sized =
        setPixelSizeFixed(face, face->pixel_width, face->pixel_height);
    // Kept going rather than returned from: the axes are already FreeType's,
    // so stopping half way would leave the remaining faces describing the
    // instance the font has left behind.
    if (sized != ZTEXT_RESULT_OK && result == ZTEXT_RESULT_OK) result = sized;
  }
  return result;
}
