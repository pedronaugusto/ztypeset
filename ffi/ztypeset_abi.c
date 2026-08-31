//===----------------------------------------------------------------------===//
// ztypeset -- the ABI guard, in both directions.
//
// Downwards, at the upstreams: static assertions that fail the BUILD if a
// re-vendor changes the shape of something ztypeset depends on. These are the
// reason the upstream types stop at this boundary -- a Zig side mirroring
// FT_FaceRec by hand would get no such check.
//
// Upwards, at the Zig wrapper: ztypesetAbiLayout reports what this translation
// unit actually compiled to, so src/c.zig can assert its hand-written externs
// still agree. A field reordered on one side and not the other is silent
// memory corruption; here it is a failing test.
//===----------------------------------------------------------------------===//

#include "ztypeset_internal.h"

//===----------------------------------------------------------------------===//
// Assumptions about the vendored upstreams
//===----------------------------------------------------------------------===//

// HarfBuzz reports positions in 26.6 fixed point through hb_position_t, and
// ztypeset divides them by 64 into float. A wider or narrower type, or a change
// of fixed-point scale, would silently rescale every advance in the package.
//===----------------------------------------------------------------------===//
// ffi/ztypeset_ftoption.h's claims, in a form the build refuses
//
// Every switch that file turns off or on is a claim about the binary, and a
// claim in a comment is a comment. One of them was false for the life of the
// package: the resource-fork guessing heuristics were described as dropped
// and were compiled into every build, because the paragraph that dropped
// FT_CONFIG_OPTION_MAC_FONTS assumed a second switch went with it.
//
// These are here rather than in ztypeset_ftoption.h because that file IS the
// definition -- a file cannot check itself -- and this translation unit is
// where ztypeset already asserts what its upstreams compiled to.
//===----------------------------------------------------------------------===//

#ifdef FT_CONFIG_OPTION_USE_ZLIB
#error "ztypeset builds without FT_CONFIG_OPTION_USE_ZLIB"
#endif
#ifdef FT_CONFIG_OPTION_SVG
#error "ztypeset builds without FT_CONFIG_OPTION_SVG"
#endif
#ifdef FT_CONFIG_OPTION_MAC_FONTS
#error "ztypeset builds without FT_CONFIG_OPTION_MAC_FONTS"
#endif
#ifdef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK
#error \
    "ztypeset builds without FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK"
#endif
#ifndef FT_CONFIG_OPTION_ERROR_STRINGS
#error "ztypeset builds WITH FT_CONFIG_OPTION_ERROR_STRINGS"
#endif
#ifndef FT_CONFIG_OPTION_USE_HARFBUZZ
#error "ztypeset builds WITH FT_CONFIG_OPTION_USE_HARFBUZZ"
#endif

_Static_assert(sizeof(hb_position_t) == 4,
               "hb_position_t is no longer 32-bit; check the 26.6 conversions "
               "in ztypeset_shape.c");

// hb_tag_t is what ZtypesetFeature::tag and ZtypesetShapeParams::script are
// passed through as.
_Static_assert(sizeof(hb_tag_t) == 4, "hb_tag_t is no longer 32-bit");

// ztypeset converts ZtypesetFeature into hb_feature_t field by field. The
// assertion is on the field types, not on the struct layout, because the
// conversion does not depend on layout -- only on each field being able to hold
// what ztypeset puts in it.
_Static_assert(sizeof(((hb_feature_t*)0)->tag) == 4, "hb_feature_t::tag");
_Static_assert(sizeof(((hb_feature_t*)0)->value) == 4, "hb_feature_t::value");
_Static_assert(sizeof(((hb_feature_t*)0)->start) >= 4, "hb_feature_t::start");
_Static_assert(sizeof(((hb_feature_t*)0)->end) >= 4, "hb_feature_t::end");

// ZtypesetGlyphFlag republishes HarfBuzz's glyph flags under ztypeset's names,
// and ztypesetShaperShape copies the mask straight across with no translation
// table. That is only sound while the two agree value for value, so each pair
// is asserted rather than trusted -- a renumbering upstream would otherwise
// turn "safe to break here" into "unsafe to concat here" silently, and the
// symptom would be a paragraph re-shaped at the wrong places or not at all.
//
// HB_GLYPH_FLAG_DEFINED is asserted too, because it is the mask
// ztypesetShaperShape relies on to drop any bit HarfBuzz gains before ztypeset
// has a name for it.
_Static_assert((int)ZTYPESET_GLYPH_FLAG_UNSAFE_TO_BREAK ==
                   (int)HB_GLYPH_FLAG_UNSAFE_TO_BREAK,
               "HB_GLYPH_FLAG_UNSAFE_TO_BREAK moved");
_Static_assert((int)ZTYPESET_GLYPH_FLAG_UNSAFE_TO_CONCAT ==
                   (int)HB_GLYPH_FLAG_UNSAFE_TO_CONCAT,
               "HB_GLYPH_FLAG_UNSAFE_TO_CONCAT moved");
_Static_assert((int)ZTYPESET_GLYPH_FLAG_SAFE_TO_INSERT_TATWEEL ==
                   (int)HB_GLYPH_FLAG_SAFE_TO_INSERT_TATWEEL,
               "HB_GLYPH_FLAG_SAFE_TO_INSERT_TATWEEL moved");
_Static_assert((int)ZTYPESET_GLYPH_FLAG_DEFINED == (int)HB_GLYPH_FLAG_DEFINED,
               "HarfBuzz defines a glyph flag ztypeset does not republish; "
               "add it to ZtypesetGlyphFlag rather than widening the mask");

// ZtypesetMetric's enumerators ARE hb_ot_metrics_tag_t's values --
// ztypesetFaceMetric casts one to the other and passes it straight to HarfBuzz
// -- so the mapping is the identity and the only thing that can go wrong is the
// two lists drifting apart. One assertion per tag, generated from the same
// ZTYPESET_METRIC_LIST the enum is, so adding a metric to the header adds its
// assertion with it and a metric HarfBuzz renames stops the build here rather
// than reading as an unsupported metric at runtime.
#define ZTYPESET_METRIC_ASSERT(name, a, b, c, d)             \
  _Static_assert((int)ZTYPESET_METRIC_##name ==              \
                     (int)HB_OT_METRICS_TAG_##name,       \
                 "HB_OT_METRICS_TAG_" #name " moved");
ZTYPESET_METRIC_LIST(ZTYPESET_METRIC_ASSERT)
#undef ZTYPESET_METRIC_ASSERT

// hb_glyph_extents_t is read directly in ztypesetShaperExtents, including the
// documented convention that height is negative when y grows up.
_Static_assert(sizeof(((hb_glyph_extents_t*)0)->height) ==
                   sizeof(hb_position_t),
               "hb_glyph_extents_t no longer uses hb_position_t");

// FreeType's scaled metrics are 26.6 in FT_Pos, divided by 64 throughout.
_Static_assert(sizeof(FT_Pos) == sizeof(long) || sizeof(FT_Pos) == 8,
               "FT_Pos changed size; check the 26.6 conversions");

// ztypesetFaceRenderGlyph hands out FT_Bitmap's own buffer and pitch.
// ztypeset.h documents that a negative pitch means a bottom-up bitmap, so the
// type has to stay SIGNED -- a width check alone would pass an upstream change
// to `unsigned int`, and every bottom-up glyph would then render as a buffer
// overrun's worth of garbage.
_Static_assert(sizeof(((FT_Bitmap*)0)->pitch) == sizeof(int),
               "FT_Bitmap::pitch is no longer int-sized");
_Static_assert((__typeof__(((FT_Bitmap*)0)->pitch))-1 < 0,
               "FT_Bitmap::pitch is no longer signed");
_Static_assert(sizeof(((FT_Bitmap*)0)->width) == sizeof(unsigned int),
               "FT_Bitmap::width changed type");

// SheenBidi levels are copied wholesale into ztypeset's uint8_t array.
_Static_assert(sizeof(SBLevel) == 1,
               "SBLevel is no longer a byte; ztypeset_bidi.c copies "
               "levels raw");

// SBRun offsets are narrowed to uint32_t on the way out. SBUInteger is
// pointer-sized, so the narrowing is safe only because ztypeset refuses text
// longer than a uint32_t can index.
_Static_assert(sizeof(SBUInteger) >= 4, "SBUInteger is unexpectedly narrow");

// ZtypesetEncoding's enumerators ARE SBStringEncoding's values --
// ztypeset_bidi.c casts one to the other and hands it to SheenBidi -- so the
// mapping is the identity and the only thing that can go wrong is the two lists
// drifting. A renumbering upstream would analyse UTF-16 text as UTF-8: not a
// crash, a paragraph of wrong levels.
_Static_assert((int)ZTYPESET_ENCODING_UTF8 == (int)SBStringEncodingUTF8,
               "SBStringEncodingUTF8 moved");
_Static_assert((int)ZTYPESET_ENCODING_UTF16 == (int)SBStringEncodingUTF16,
               "SBStringEncodingUTF16 moved");
_Static_assert((int)ZTYPESET_ENCODING_UTF32 == (int)SBStringEncodingUTF32,
               "SBStringEncodingUTF32 moved");

// libunibreak names its unit types itself and ztypeset casts its own uint16_t
// and uint32_t text to them. The casts are only sound while the widths agree,
// and a mismatch would read one paragraph's units as another's -- silently,
// because the pointer types would still convert.
_Static_assert(sizeof(utf16_t) == 2,
               "libunibreak's utf16_t is no longer 16 bits; ztypeset_bidi.c "
               "casts uint16_t text to it");
_Static_assert(sizeof(utf32_t) == 4,
               "libunibreak's utf32_t is no longer 32 bits; ztypeset_bidi.c "
               "casts uint32_t text to it");

//===----------------------------------------------------------------------===//
// ztypeset's own layout, for the Zig side to assert against
//===----------------------------------------------------------------------===//

void ztypesetAbiLayout(ZtypesetAbiLayout* out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));

  out->layout_size = (uint32_t)sizeof(ZtypesetAbiLayout);

  out->allocator_size = (uint32_t)sizeof(ZtypesetAllocator);
  out->allocator_align = (uint32_t)_Alignof(ZtypesetAllocator);
  out->allocator_offset_allocate = (uint32_t)offsetof(ZtypesetAllocator,
                                    allocate);
  out->allocator_offset_reallocate =
      (uint32_t)offsetof(ZtypesetAllocator, reallocate);
  out->allocator_offset_deallocate =
      (uint32_t)offsetof(ZtypesetAllocator, deallocate);
  out->allocator_offset_user = (uint32_t)offsetof(ZtypesetAllocator, user);

  out->glyph_size = (uint32_t)sizeof(ZtypesetGlyph);
  out->glyph_align = (uint32_t)_Alignof(ZtypesetGlyph);
  out->glyph_offset_glyph_id = (uint32_t)offsetof(ZtypesetGlyph, glyph_id);
  out->glyph_offset_cluster = (uint32_t)offsetof(ZtypesetGlyph, cluster);
  out->glyph_offset_x_advance = (uint32_t)offsetof(ZtypesetGlyph, x_advance);
  out->glyph_offset_y_advance = (uint32_t)offsetof(ZtypesetGlyph, y_advance);
  out->glyph_offset_x_offset = (uint32_t)offsetof(ZtypesetGlyph, x_offset);
  out->glyph_offset_y_offset = (uint32_t)offsetof(ZtypesetGlyph, y_offset);
  out->glyph_offset_flags = (uint32_t)offsetof(ZtypesetGlyph, flags);

  out->feature_size = (uint32_t)sizeof(ZtypesetFeature);
  out->feature_align = (uint32_t)_Alignof(ZtypesetFeature);
  out->shape_params_size = (uint32_t)sizeof(ZtypesetShapeParams);
  out->shape_params_align = (uint32_t)_Alignof(ZtypesetShapeParams);
  out->shape_params_offset_language =
      (uint32_t)offsetof(ZtypesetShapeParams, language);
  out->shape_params_offset_features =
      (uint32_t)offsetof(ZtypesetShapeParams, features);
  out->shape_params_offset_feature_count =
      (uint32_t)offsetof(ZtypesetShapeParams, feature_count);

  out->face_metrics_size = (uint32_t)sizeof(ZtypesetFaceMetrics);
  out->face_metrics_align = (uint32_t)_Alignof(ZtypesetFaceMetrics);
  out->extents_size = (uint32_t)sizeof(ZtypesetExtents);
  out->extents_align = (uint32_t)_Alignof(ZtypesetExtents);

  out->charmap_size = (uint32_t)sizeof(ZtypesetCharmap);
  out->charmap_align = (uint32_t)_Alignof(ZtypesetCharmap);
  out->matrix_size = (uint32_t)sizeof(ZtypesetMatrix);
  out->matrix_align = (uint32_t)_Alignof(ZtypesetMatrix);
  out->stroke_size = (uint32_t)sizeof(ZtypesetStroke);
  out->stroke_align = (uint32_t)_Alignof(ZtypesetStroke);

  out->visual_run_size = (uint32_t)sizeof(ZtypesetVisualRun);
  out->visual_run_align = (uint32_t)_Alignof(ZtypesetVisualRun);
  out->script_run_size = (uint32_t)sizeof(ZtypesetScriptRun);
  out->script_run_align = (uint32_t)_Alignof(ZtypesetScriptRun);

  out->glyph_bitmap_size = (uint32_t)sizeof(ZtypesetGlyphBitmap);
  out->glyph_bitmap_align = (uint32_t)_Alignof(ZtypesetGlyphBitmap);
  out->glyph_bitmap_offset_pixels =
      (uint32_t)offsetof(ZtypesetGlyphBitmap, pixels);
  out->glyph_bitmap_offset_format =
      (uint32_t)offsetof(ZtypesetGlyphBitmap, format);
  out->glyph_bitmap_offset_pitch = (uint32_t)offsetof(ZtypesetGlyphBitmap,
                                    pitch);
  out->glyph_bitmap_offset_x_advance =
      (uint32_t)offsetof(ZtypesetGlyphBitmap, x_advance);

  // Kept in step by hand with the enum in ztypeset.h. The Zig side asserts its
  // own exhaustive mapping has the same count, so adding a result there and
  // forgetting it here fails a test.
  out->result_count = 11u;

  // Sizes as this compiler chose them, and the last enumerator of each, so a
  // consumer can check both its tag type and its values.
  out->result_size = (uint32_t)sizeof(ZtypesetResult);
  out->result_last = (uint32_t)ZTYPESET_RESULT_BUFFER_TOO_SMALL;
  out->direction_size = (uint32_t)sizeof(ZtypesetDirection);
  out->direction_last = (uint32_t)ZTYPESET_DIRECTION_BTT;
  out->cluster_level_size = (uint32_t)sizeof(ZtypesetClusterLevel);
  out->cluster_level_last = (uint32_t)ZTYPESET_CLUSTER_LEVEL_GRAPHEMES;
  out->base_direction_size = (uint32_t)sizeof(ZtypesetBaseDirection);
  out->base_direction_last = (uint32_t)ZTYPESET_BASE_DIRECTION_RTL;
  out->render_mode_size = (uint32_t)sizeof(ZtypesetRenderMode);
  out->render_mode_last = (uint32_t)ZTYPESET_RENDER_MODE_LCD_V;
  out->hinting_size = (uint32_t)sizeof(ZtypesetHinting);
  out->hinting_last = (uint32_t)ZTYPESET_HINTING_NONE;
  out->bitmap_format_size = (uint32_t)sizeof(ZtypesetBitmapFormat);
  out->bitmap_format_last = (uint32_t)ZTYPESET_BITMAP_FORMAT_LCD_V;
  out->line_cap_size = (uint32_t)sizeof(ZtypesetLineCap);
  out->line_cap_last = (uint32_t)ZTYPESET_LINE_CAP_SQUARE;
  out->line_join_size = (uint32_t)sizeof(ZtypesetLineJoin);
  out->line_join_last = (uint32_t)ZTYPESET_LINE_JOIN_MITER_FIXED;
  out->stroke_style_size = (uint32_t)sizeof(ZtypesetStrokeStyle);
  out->stroke_style_last = (uint32_t)ZTYPESET_STROKE_STYLE_SHRUNK;
  out->encoding_size = (uint32_t)sizeof(ZtypesetEncoding);
  out->encoding_last = (uint32_t)ZTYPESET_ENCODING_UTF32;
  out->segmentation_size = (uint32_t)sizeof(ZtypesetSegmentation);
  out->segmentation_last = (uint32_t)ZTYPESET_SEGMENTATION_ALL;
  out->glyph_flag_size = (uint32_t)sizeof(ZtypesetGlyphFlag);
  // The OR of every flag, not the highest one -- see ztypeset.h.
  out->glyph_flag_last = (uint32_t)ZTYPESET_GLYPH_FLAG_DEFINED;
  out->metric_size = (uint32_t)sizeof(ZtypesetMetric);
  // A count rather than a last value: the enumerators are four-character
  // OpenType tags, so "the highest one" is an accident of spelling and says
  // nothing about how many there are. Counted from ZTYPESET_METRIC_LIST, which
  // is also what the enum is generated from, so this cannot be a number
  // someone forgot to raise.
#define ZTYPESET_METRIC_ONE(name, a, b, c, d) +1
  out->metric_count = (uint32_t)(0 ZTYPESET_METRIC_LIST(ZTYPESET_METRIC_ONE));
#undef ZTYPESET_METRIC_ONE
}

//===----------------------------------------------------------------------===//
// Field-by-field probe
//===----------------------------------------------------------------------===//

void ztypesetAbiProbe(ZtypesetAbiProbe* out) {
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

  out->shape_params.direction = (ZtypesetDirection)3;
  out->shape_params.script = 0x105u;
  out->shape_params.language = (const char*)(uintptr_t)0x555;
  out->shape_params.features = (const ZtypesetFeature*)(uintptr_t)0x666;
  out->shape_params.feature_count = (size_t)0x107;
  out->shape_params.cluster_level = (ZtypesetClusterLevel)2;
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
  out->face_metrics.vert_ascender = 310.25f;
  out->face_metrics.vert_descender = 311.25f;
  out->face_metrics.vert_line_height = 312.25f;
  out->face_metrics.vert_max_advance = 313.25f;
  out->face_metrics.has_vertical_metrics = 0x30Au;

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
  out->glyph_bitmap.format = ZTYPESET_BITMAP_FORMAT_SDF;
  out->glyph_bitmap.width = 0x701u;
  out->glyph_bitmap.height = 0x702u;
  out->glyph_bitmap.pitch = -0x703;
  out->glyph_bitmap.left = -0x704;
  out->glyph_bitmap.top = 0x705;
  out->glyph_bitmap.x_advance = 706.25f;

  out->charmap.platform_id = (uint16_t)0x801;
  out->charmap.encoding_id = (uint16_t)0x802;
  out->charmap.encoding = 0x803u;

  out->variation_axis.tag = 0x804u;
  out->variation_axis.min_value = 805.25f;
  out->variation_axis.default_value = 806.25f;
  out->variation_axis.max_value = 807.25f;

  out->variation.tag = 0x808u;
  out->variation.value = 809.25f;

  out->matrix.xx = 901.25f;
  out->matrix.xy = 902.25f;
  out->matrix.yx = 903.25f;
  out->matrix.yy = 904.25f;

  out->stroke.radius = 905.25f;
  out->stroke.miter_limit = 906.25f;
  out->stroke.cap = ZTYPESET_LINE_CAP_SQUARE;
  out->stroke.join = ZTYPESET_LINE_JOIN_MITER_FIXED;
  out->stroke.style = ZTYPESET_STROKE_STYLE_SHRUNK;

  out->outline_funcs.move_to =
      (ZtypesetResult(*)(void*, int32_t, int32_t))(uintptr_t)0x888;
  out->outline_funcs.line_to =
      (ZtypesetResult(*)(void*, int32_t, int32_t))(uintptr_t)0x999;
  out->outline_funcs.conic_to =
      (ZtypesetResult(*)(void*, int32_t, int32_t, int32_t,
                      int32_t))(uintptr_t)0xAAA;
  out->outline_funcs.cubic_to =
      (ZtypesetResult(*)(void*, int32_t, int32_t, int32_t, int32_t, int32_t,
                      int32_t))(uintptr_t)0xBBB;
  out->outline_funcs.close = (ZtypesetResult(*)(void*))(uintptr_t)0xCCC;
  out->outline_funcs.user = (void*)(uintptr_t)0xDDD;
}
