//===----------------------------------------------------------------------===//
// ztext -- the ABI guard, in both directions.
//
// Downwards, at the upstreams: static assertions that fail the BUILD if a
// re-vendor changes the shape of something ztext depends on. These are the
// reason the upstream types stop at this boundary -- a Zig side mirroring
// FT_FaceRec by hand would get no such check.
//
// Upwards, at the Zig wrapper: ztextAbiLayout reports what this translation
// unit actually compiled to, so src/c.zig can assert its hand-written externs
// still agree. A field reordered on one side and not the other is silent
// memory corruption; here it is a failing test.
//===----------------------------------------------------------------------===//

#include "ztext_internal.h"

//===----------------------------------------------------------------------===//
// Assumptions about the vendored upstreams
//===----------------------------------------------------------------------===//

// HarfBuzz reports positions in 26.6 fixed point through hb_position_t, and
// ztext divides them by 64 into float. A wider or narrower type, or a change
// of fixed-point scale, would silently rescale every advance in the package.
_Static_assert(sizeof(hb_position_t) == 4,
               "hb_position_t is no longer 32-bit; check the 26.6 conversions "
               "in ztext_shape.c");

// hb_tag_t is what ZtextFeature::tag and ZtextShapeParams::script are passed
// through as.
_Static_assert(sizeof(hb_tag_t) == 4, "hb_tag_t is no longer 32-bit");

// ztext converts ZtextFeature into hb_feature_t field by field. The assertion
// is on the field types, not on the struct layout, because the conversion does
// not depend on layout -- only on each field being able to hold what ztext
// puts in it.
_Static_assert(sizeof(((hb_feature_t*)0)->tag) == 4, "hb_feature_t::tag");
_Static_assert(sizeof(((hb_feature_t*)0)->value) == 4, "hb_feature_t::value");
_Static_assert(sizeof(((hb_feature_t*)0)->start) >= 4, "hb_feature_t::start");
_Static_assert(sizeof(((hb_feature_t*)0)->end) >= 4, "hb_feature_t::end");

// ZtextGlyphFlag republishes HarfBuzz's glyph flags under ztext's names, and
// ztextShaperShapeUtf8 copies the mask straight across with no translation
// table. That is only sound while the two agree value for value, so each pair
// is asserted rather than trusted -- a renumbering upstream would otherwise
// turn "safe to break here" into "unsafe to concat here" silently, and the
// symptom would be a paragraph re-shaped at the wrong places or not at all.
//
// HB_GLYPH_FLAG_DEFINED is asserted too, because it is the mask
// ztextShaperShapeUtf8 relies on to drop any bit HarfBuzz gains before ztext
// has a name for it.
_Static_assert((int)ZTEXT_GLYPH_FLAG_UNSAFE_TO_BREAK ==
                   (int)HB_GLYPH_FLAG_UNSAFE_TO_BREAK,
               "HB_GLYPH_FLAG_UNSAFE_TO_BREAK moved");
_Static_assert((int)ZTEXT_GLYPH_FLAG_UNSAFE_TO_CONCAT ==
                   (int)HB_GLYPH_FLAG_UNSAFE_TO_CONCAT,
               "HB_GLYPH_FLAG_UNSAFE_TO_CONCAT moved");
_Static_assert((int)ZTEXT_GLYPH_FLAG_SAFE_TO_INSERT_TATWEEL ==
                   (int)HB_GLYPH_FLAG_SAFE_TO_INSERT_TATWEEL,
               "HB_GLYPH_FLAG_SAFE_TO_INSERT_TATWEEL moved");
_Static_assert((int)ZTEXT_GLYPH_FLAG_DEFINED == (int)HB_GLYPH_FLAG_DEFINED,
               "HarfBuzz defines a glyph flag ztext does not republish; add it "
               "to ZtextGlyphFlag rather than widening the mask");

// hb_glyph_extents_t is read directly in ztextShaperExtents, including the
// documented convention that height is negative when y grows up.
_Static_assert(sizeof(((hb_glyph_extents_t*)0)->height) ==
                   sizeof(hb_position_t),
               "hb_glyph_extents_t no longer uses hb_position_t");

// FreeType's scaled metrics are 26.6 in FT_Pos, divided by 64 throughout.
_Static_assert(sizeof(FT_Pos) == sizeof(long) || sizeof(FT_Pos) == 8,
               "FT_Pos changed size; check the 26.6 conversions");

// ztextFaceRenderGlyph hands out FT_Bitmap's own buffer and pitch. ztext.h
// documents that a negative pitch means a bottom-up bitmap, so the type has to
// stay SIGNED -- a width check alone would pass an upstream change to
// `unsigned int`, and every bottom-up glyph would then render as a buffer
// overrun's worth of garbage.
_Static_assert(sizeof(((FT_Bitmap*)0)->pitch) == sizeof(int),
               "FT_Bitmap::pitch is no longer int-sized");
_Static_assert((__typeof__(((FT_Bitmap*)0)->pitch))-1 < 0,
               "FT_Bitmap::pitch is no longer signed");
_Static_assert(sizeof(((FT_Bitmap*)0)->width) == sizeof(unsigned int),
               "FT_Bitmap::width changed type");

// SheenBidi levels are copied wholesale into ztext's uint8_t array.
_Static_assert(sizeof(SBLevel) == 1,
               "SBLevel is no longer a byte; ztext_bidi.c copies levels raw");

// SBRun offsets are narrowed to uint32_t on the way out. SBUInteger is
// pointer-sized, so the narrowing is safe only because ztext refuses text
// longer than a uint32_t can index.
_Static_assert(sizeof(SBUInteger) >= 4, "SBUInteger is unexpectedly narrow");

//===----------------------------------------------------------------------===//
// ztext's own layout, for the Zig side to assert against
//===----------------------------------------------------------------------===//

void ztextAbiLayout(ZtextAbiLayout* out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));

  out->layout_size = (uint32_t)sizeof(ZtextAbiLayout);

  out->allocator_size = (uint32_t)sizeof(ZtextAllocator);
  out->allocator_align = (uint32_t)_Alignof(ZtextAllocator);
  out->allocator_offset_allocate = (uint32_t)offsetof(ZtextAllocator, allocate);
  out->allocator_offset_reallocate =
      (uint32_t)offsetof(ZtextAllocator, reallocate);
  out->allocator_offset_deallocate =
      (uint32_t)offsetof(ZtextAllocator, deallocate);
  out->allocator_offset_user = (uint32_t)offsetof(ZtextAllocator, user);

  out->glyph_size = (uint32_t)sizeof(ZtextGlyph);
  out->glyph_align = (uint32_t)_Alignof(ZtextGlyph);
  out->glyph_offset_glyph_id = (uint32_t)offsetof(ZtextGlyph, glyph_id);
  out->glyph_offset_cluster = (uint32_t)offsetof(ZtextGlyph, cluster);
  out->glyph_offset_x_advance = (uint32_t)offsetof(ZtextGlyph, x_advance);
  out->glyph_offset_y_advance = (uint32_t)offsetof(ZtextGlyph, y_advance);
  out->glyph_offset_x_offset = (uint32_t)offsetof(ZtextGlyph, x_offset);
  out->glyph_offset_y_offset = (uint32_t)offsetof(ZtextGlyph, y_offset);
  out->glyph_offset_flags = (uint32_t)offsetof(ZtextGlyph, flags);

  out->feature_size = (uint32_t)sizeof(ZtextFeature);
  out->feature_align = (uint32_t)_Alignof(ZtextFeature);
  out->shape_params_size = (uint32_t)sizeof(ZtextShapeParams);
  out->shape_params_align = (uint32_t)_Alignof(ZtextShapeParams);
  out->shape_params_offset_language =
      (uint32_t)offsetof(ZtextShapeParams, language);
  out->shape_params_offset_features =
      (uint32_t)offsetof(ZtextShapeParams, features);
  out->shape_params_offset_feature_count =
      (uint32_t)offsetof(ZtextShapeParams, feature_count);

  out->face_metrics_size = (uint32_t)sizeof(ZtextFaceMetrics);
  out->face_metrics_align = (uint32_t)_Alignof(ZtextFaceMetrics);
  out->extents_size = (uint32_t)sizeof(ZtextExtents);
  out->extents_align = (uint32_t)_Alignof(ZtextExtents);

  out->visual_run_size = (uint32_t)sizeof(ZtextVisualRun);
  out->visual_run_align = (uint32_t)_Alignof(ZtextVisualRun);
  out->script_run_size = (uint32_t)sizeof(ZtextScriptRun);
  out->script_run_align = (uint32_t)_Alignof(ZtextScriptRun);

  out->glyph_bitmap_size = (uint32_t)sizeof(ZtextGlyphBitmap);
  out->glyph_bitmap_align = (uint32_t)_Alignof(ZtextGlyphBitmap);
  out->glyph_bitmap_offset_pixels =
      (uint32_t)offsetof(ZtextGlyphBitmap, pixels);
  out->glyph_bitmap_offset_format =
      (uint32_t)offsetof(ZtextGlyphBitmap, format);
  out->glyph_bitmap_offset_pitch = (uint32_t)offsetof(ZtextGlyphBitmap, pitch);
  out->glyph_bitmap_offset_x_advance =
      (uint32_t)offsetof(ZtextGlyphBitmap, x_advance);

  // Kept in step by hand with the enum in ztext.h. The Zig side asserts its
  // own exhaustive mapping has the same count, so adding a result there and
  // forgetting it here fails a test.
  out->result_count = 11u;

  // Sizes as this compiler chose them, and the last enumerator of each, so a
  // consumer can check both its tag type and its values.
  out->result_size = (uint32_t)sizeof(ZtextResult);
  out->result_last = (uint32_t)ZTEXT_RESULT_BUFFER_TOO_SMALL;
  out->direction_size = (uint32_t)sizeof(ZtextDirection);
  out->direction_last = (uint32_t)ZTEXT_DIRECTION_BTT;
  out->cluster_level_size = (uint32_t)sizeof(ZtextClusterLevel);
  out->cluster_level_last = (uint32_t)ZTEXT_CLUSTER_LEVEL_GRAPHEMES;
  out->base_direction_size = (uint32_t)sizeof(ZtextBaseDirection);
  out->base_direction_last = (uint32_t)ZTEXT_BASE_DIRECTION_RTL;
  out->render_mode_size = (uint32_t)sizeof(ZtextRenderMode);
  out->render_mode_last = (uint32_t)ZTEXT_RENDER_MODE_SDF;
  out->hinting_size = (uint32_t)sizeof(ZtextHinting);
  out->hinting_last = (uint32_t)ZTEXT_HINTING_NONE;
  out->bitmap_format_size = (uint32_t)sizeof(ZtextBitmapFormat);
  out->bitmap_format_last = (uint32_t)ZTEXT_BITMAP_FORMAT_SDF;
  out->glyph_flag_size = (uint32_t)sizeof(ZtextGlyphFlag);
  // The OR of every flag, not the highest one -- see ztext.h.
  out->glyph_flag_last = (uint32_t)ZTEXT_GLYPH_FLAG_DEFINED;
}

//===----------------------------------------------------------------------===//
// Field-by-field probe
//===----------------------------------------------------------------------===//

void ztextAbiProbe(ZtextAbiProbe* out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));

  // Markers are chosen so no two fields anywhere in the struct share one, and
  // so integer and float fields cannot be confused for each other: integers
  // count up from 0x100, floats are their index plus a fraction no integer
  // field carries.
  out->allocator.allocate = (void* (*)(void*, size_t, size_t))(uintptr_t)0x111;
  out->allocator.reallocate =
      (void* (*)(void*, void*, size_t, size_t, size_t))(uintptr_t)0x222;
  out->allocator.deallocate =
      (void (*)(void*, void*, size_t, size_t))(uintptr_t)0x333;
  out->allocator.user = (void*)(uintptr_t)0x444;

  out->feature.tag = 0x101u;
  out->feature.value = 0x102u;
  out->feature.start = 0x103u;
  out->feature.end = 0x104u;

  out->shape_params.direction = (ZtextDirection)3;
  out->shape_params.script = 0x105u;
  out->shape_params.language = (const char*)(uintptr_t)0x555;
  out->shape_params.features = (const ZtextFeature*)(uintptr_t)0x666;
  out->shape_params.feature_count = (size_t)0x107;
  out->shape_params.cluster_level = (ZtextClusterLevel)2;
  out->shape_params.use_freetype_metrics = 0x108;

  out->glyph.glyph_id = 0x201u;
  out->glyph.cluster = 0x202u;
  out->glyph.flags = 0x207u;
  out->glyph.x_advance = 203.25f;
  out->glyph.y_advance = 204.25f;
  out->glyph.x_offset = 205.25f;
  out->glyph.y_offset = 206.25f;

  out->face_metrics.ascender = 301.25f;
  out->face_metrics.descender = 302.25f;
  out->face_metrics.line_height = 303.25f;
  out->face_metrics.max_advance = 304.25f;
  out->face_metrics.underline_position = 305.25f;
  out->face_metrics.underline_thickness = 306.25f;
  out->face_metrics.units_per_em = 0x307u;
  out->face_metrics.num_glyphs = 0x308u;
  out->face_metrics.pixel_size = 309.25f;

  out->extents.x_min = 401.25f;
  out->extents.y_min = 402.25f;
  out->extents.x_max = 403.25f;
  out->extents.y_max = 404.25f;
  out->extents.x_advance = 405.25f;
  out->extents.y_advance = 406.25f;

  out->visual_run.offset = 0x501u;
  out->visual_run.length = 0x502u;
  out->visual_run.level = 0x53u;

  out->script_run.offset = 0x601u;
  out->script_run.length = 0x602u;
  out->script_run.script = 0x603u;

  out->shaping_run.offset = 0x611u;
  out->shaping_run.length = 0x612u;
  out->shaping_run.script = 0x613u;
  out->shaping_run.level = 0x64u;

  out->glyph_bitmap.pixels = (const uint8_t*)(uintptr_t)0x777;
  // The one marker that is not from the counting sequence: `format` is an
  // enum with two enumerators, so the only value that both proves it was
  // written and stays a legal value of its own type is the one memset did not
  // leave behind.
  out->glyph_bitmap.format = ZTEXT_BITMAP_FORMAT_SDF;
  out->glyph_bitmap.width = 0x701u;
  out->glyph_bitmap.height = 0x702u;
  out->glyph_bitmap.pitch = -0x703;
  out->glyph_bitmap.left = -0x704;
  out->glyph_bitmap.top = 0x705;
  out->glyph_bitmap.x_advance = 706.25f;
}
