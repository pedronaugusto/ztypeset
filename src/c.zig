//! Hand-written declarations mirroring `ffi/ztext.h`.
//!
//! Written by hand rather than produced by `@cImport` so the package stays
//! translate-c-free and every type is exactly the shape the rest of the
//! wrapper wants -- a `@cImport` of these headers would also leak translate-c
//! types into ztext's public API and make every target depend on translate-c
//! behaving.
//!
//! The cost of hand-writing is drift: nothing in either compiler checks that
//! this file still agrees with the header. That gap is closed by
//! `ztextAbiLayout`, asserted in the test at the bottom of `ztext.zig` -- if a
//! field moves on either side, the test fails loudly instead of corrupting
//! memory quietly.

const std = @import("std");

//=============================================================================
// Results
//=============================================================================

/// Non-exhaustive on purpose. ztext ships a `-Dshared=true` mode and the
/// header warns about header/library skew, so a newer library really can
/// return a code this build has never heard of. A closed enum would make that
/// `panic: invalid enum value` in a safe build and undefined behaviour in a
/// fast one; the `_` turns it into an error the caller can handle.
pub const Result = enum(c_int) {
    ok = 0,
    out_of_memory = 1,
    invalid_argument = 2,
    invalid_utf8 = 3,
    bad_font = 4,
    unsupported = 5,
    glyph_not_found = 6,
    render_failed = 7,
    shape_failed = 8,
    bidi_failed = 9,
    buffer_too_small = 10,
    _,
};

//=============================================================================
// Plain data
//=============================================================================

pub const Allocator = extern struct {
    allocate: ?*const fn (
        user: ?*anyopaque,
        size: usize,
        alignment: usize,
    ) callconv(.c) ?*anyopaque,
    reallocate: ?*const fn (
        user: ?*anyopaque,
        block: ?*anyopaque,
        old_size: usize,
        new_size: usize,
        alignment: usize,
    ) callconv(.c) ?*anyopaque,
    deallocate: ?*const fn (
        user: ?*anyopaque,
        block: ?*anyopaque,
        size: usize,
        alignment: usize,
    ) callconv(.c) void,
    user: ?*anyopaque,
};

pub const Direction = enum(c_int) {
    auto = 0,
    ltr = 1,
    rtl = 2,
    ttb = 3,
    btt = 4,
};

/// The values a break entry holds. Byte-sized, because they live in an array
/// of one byte per text byte; `types.Break` is the enum a caller reads them
/// through. Declared here so the ABI cross-check pairs each with its macro.
pub const break_none: u8 = 0;
pub const break_allowed: u8 = 1;
pub const break_mandatory: u8 = 2;

pub const ClusterLevel = enum(c_int) {
    monotone_graphemes = 0,
    monotone_characters = 1,
    characters = 2,
    graphemes = 3,
};

pub const BaseDirection = enum(c_int) {
    auto = 0,
    ltr = 1,
    rtl = 2,
};

pub const RenderMode = enum(c_int) {
    a8 = 0,
    sdf = 1,
};

pub const Hinting = enum(c_int) {
    normal = 0,
    light = 1,
    none = 2,
};

pub const Feature = extern struct {
    tag: u32,
    value: u32,
    start: u32,
    end: u32,
};

pub const ShapeParams = extern struct {
    direction: Direction,
    script: u32,
    language: ?[*:0]const u8,
    features: ?[*]const Feature,
    feature_count: usize,
    cluster_level: ClusterLevel,
    use_freetype_metrics: c_int,
};

pub const Glyph = extern struct {
    glyph_id: u32,
    cluster: u32,
    x_advance: f32,
    y_advance: f32,
    x_offset: f32,
    y_offset: f32,
};

pub const FaceMetrics = extern struct {
    ascender: f32,
    descender: f32,
    line_height: f32,
    max_advance: f32,
    underline_position: f32,
    underline_thickness: f32,
    units_per_em: u32,
    num_glyphs: u32,
    pixel_size: f32,
};

pub const Extents = extern struct {
    x_min: f32,
    y_min: f32,
    x_max: f32,
    y_max: f32,
    x_advance: f32,
    y_advance: f32,
};

pub const VisualRun = extern struct {
    offset: u32,
    length: u32,
    level: u8,
};

pub const ScriptRun = extern struct {
    offset: u32,
    length: u32,
    script: u32,
};

pub const ShapingRun = extern struct {
    offset: u32,
    length: u32,
    script: u32,
    level: u8,
};

pub const VariationAxis = extern struct {
    tag: u32,
    min_value: f32,
    default_value: f32,
    max_value: f32,
};

pub const Variation = extern struct {
    tag: u32,
    value: f32,
};

pub const GlyphBitmap = extern struct {
    pixels: ?[*]const u8,
    width: u32,
    height: u32,
    pitch: i32,
    left: i32,
    top: i32,
    x_advance: f32,
};

pub const AbiLayout = extern struct {
    layout_size: u32,

    allocator_size: u32,
    allocator_align: u32,
    allocator_offset_allocate: u32,
    allocator_offset_reallocate: u32,
    allocator_offset_deallocate: u32,
    allocator_offset_user: u32,

    glyph_size: u32,
    glyph_align: u32,
    glyph_offset_glyph_id: u32,
    glyph_offset_cluster: u32,
    glyph_offset_x_advance: u32,
    glyph_offset_y_advance: u32,
    glyph_offset_x_offset: u32,
    glyph_offset_y_offset: u32,

    feature_size: u32,
    feature_align: u32,
    shape_params_size: u32,
    shape_params_align: u32,
    shape_params_offset_language: u32,
    shape_params_offset_features: u32,
    shape_params_offset_feature_count: u32,

    face_metrics_size: u32,
    face_metrics_align: u32,
    extents_size: u32,
    extents_align: u32,

    visual_run_size: u32,
    visual_run_align: u32,
    script_run_size: u32,
    script_run_align: u32,

    glyph_bitmap_size: u32,
    glyph_bitmap_align: u32,
    glyph_bitmap_offset_pixels: u32,
    glyph_bitmap_offset_pitch: u32,
    glyph_bitmap_offset_x_advance: u32,

    result_count: u32,

    result_size: u32,
    result_last: u32,
    direction_size: u32,
    direction_last: u32,
    cluster_level_size: u32,
    cluster_level_last: u32,
    base_direction_size: u32,
    base_direction_last: u32,
    render_mode_size: u32,
    render_mode_last: u32,
    hinting_size: u32,
    hinting_last: u32,
};

pub const AbiProbe = extern struct {
    allocator: Allocator,
    feature: Feature,
    shape_params: ShapeParams,
    glyph: Glyph,
    face_metrics: FaceMetrics,
    extents: Extents,
    visual_run: VisualRun,
    script_run: ScriptRun,
    shaping_run: ShapingRun,
    glyph_bitmap: GlyphBitmap,
};

//=============================================================================
// Opaque handles
//=============================================================================

pub const Library = opaque {};
pub const Font = opaque {};
pub const Face = opaque {};
pub const Shaper = opaque {};
pub const Paragraph = opaque {};
pub const Line = opaque {};

//=============================================================================
// Entry points
//=============================================================================

pub extern fn ztextVersion() u32;
pub extern fn ztextFreetypeVersion() u32;
pub extern fn ztextHarfbuzzVersion() u32;
pub extern fn ztextSheenbidiVersion() u32;
pub extern fn ztextUnibreakVersion() u32;
pub extern fn ztextResultName(result: Result) [*:0]const u8;
pub extern fn ztextLastErrorDetail() [*:0]const u8;
pub extern fn ztextSetAllocator(alloc: ?*const Allocator) Result;
pub extern fn ztextWarmup() void;
pub extern fn ztextAbiLayout(out: *AbiLayout) void;
pub extern fn ztextAbiProbe(out: *AbiProbe) void;

pub extern fn ztextLibraryCreate(out: **Library) Result;
pub extern fn ztextLibraryDestroy(library: ?*Library) void;
pub extern fn ztextLibrarySetSdfSpread(library: *Library, spread: u32) Result;
pub extern fn ztextLibraryCountFaces(library: *Library, data: [*]const u8, size: usize, out: *u32) Result;

pub extern fn ztextFontCreateFromMemory(
    library: *Library,
    data: [*]const u8,
    size: usize,
    face_index: u32,
    out: **Font,
) Result;
pub extern fn ztextFontDestroy(font: ?*Font) void;
pub extern fn ztextFontFamilyName(font: *const Font) [*:0]const u8;
pub extern fn ztextFontStyleName(font: *const Font) [*:0]const u8;
pub extern fn ztextFontGlyphIndex(font: *const Font, codepoint: u32) u32;
pub extern fn ztextFontGlyphCount(font: *const Font) u32;
pub extern fn ztextFontUnitsPerEm(font: *const Font) u32;
pub extern fn ztextFontCoveredPrefix(
    font: *const Font,
    utf8: [*]const u8,
    length: usize,
    out: *usize,
) Result;
pub extern fn ztextFontAxisCount(font: *const Font) u32;
pub extern fn ztextFontAxis(font: *const Font, index: u32, out: *VariationAxis) Result;
pub extern fn ztextFontSetVariations(
    font: *Font,
    values: ?[*]const Variation,
    count: usize,
) Result;
pub extern fn ztextFontVariation(font: *const Font, index: u32, out: *f32) Result;

pub extern fn ztextFaceCreate(
    font: *Font,
    width: f32,
    height: f32,
    out: **Face,
) Result;
pub extern fn ztextFaceDestroy(face: ?*Face) void;
pub extern fn ztextFaceFont(face: *const Face) ?*Font;
pub extern fn ztextFaceSetPixelSize(face: *Face, width: f32, height: f32) Result;
pub extern fn ztextFaceMetrics(face: *const Face, out: *FaceMetrics) Result;
pub extern fn ztextFaceRenderGlyph(
    face: *Face,
    glyph_id: u32,
    mode: RenderMode,
    hinting: Hinting,
    out: *GlyphBitmap,
) Result;
pub extern fn ztextFaceGlyphExtents(
    face: *Face,
    glyph_id: u32,
    hinting: Hinting,
    out: *Extents,
) Result;

pub extern fn ztextShaperCreate(out: **Shaper) Result;
pub extern fn ztextShaperDestroy(shaper: ?*Shaper) void;
pub extern fn ztextShaperShapeUtf8(
    shaper: *Shaper,
    face: *Face,
    text: [*]const u8,
    length: usize,
    run_offset: usize,
    run_length: usize,
    params: *const ShapeParams,
) Result;
pub extern fn ztextShaperGlyphCount(shaper: *const Shaper) usize;
pub extern fn ztextShaperGlyphs(shaper: *const Shaper) ?[*]const Glyph;
pub extern fn ztextShaperDirection(shaper: *const Shaper) Direction;
pub extern fn ztextShaperExtents(shaper: *const Shaper, face: *Face, out: *Extents) Result;

pub extern fn ztextParagraphCreateUtf8(
    text: [*]const u8,
    length: usize,
    base: BaseDirection,
    out: **Paragraph,
) Result;
pub extern fn ztextParagraphDestroy(paragraph: ?*Paragraph) void;
pub extern fn ztextParagraphLength(paragraph: *const Paragraph) usize;
pub extern fn ztextParagraphBaseLevel(paragraph: *const Paragraph) u8;
pub extern fn ztextParagraphLevels(paragraph: *const Paragraph) ?[*]const u8;
pub extern fn ztextParagraphLineBreaks(paragraph: *const Paragraph) ?[*]const u8;
pub extern fn ztextParagraphGraphemeBreaks(paragraph: *const Paragraph) ?[*]const u8;
pub extern fn ztextParagraphWordBreaks(paragraph: *const Paragraph) ?[*]const u8;
pub extern fn ztextParagraphNextGrapheme(paragraph: *const Paragraph, offset: usize) usize;
pub extern fn ztextParagraphPreviousGrapheme(paragraph: *const Paragraph, offset: usize) usize;
pub extern fn ztextParagraphVisualRunCount(paragraph: *const Paragraph) usize;
pub extern fn ztextParagraphVisualRuns(paragraph: *const Paragraph) ?[*]const VisualRun;
pub extern fn ztextParagraphScriptRunCount(paragraph: *const Paragraph) usize;
pub extern fn ztextParagraphScriptRuns(paragraph: *const Paragraph) ?[*]const ScriptRun;
pub extern fn ztextParagraphShapingRunCount(paragraph: *const Paragraph) usize;
pub extern fn ztextParagraphShapingRuns(paragraph: *const Paragraph) ?[*]const ShapingRun;

pub extern fn ztextLineCreate(
    paragraph: *const Paragraph,
    offset: usize,
    length: usize,
    out: **Line,
) Result;
pub extern fn ztextLineDestroy(line: ?*Line) void;
pub extern fn ztextLineOffset(line: *const Line) usize;
pub extern fn ztextLineLength(line: *const Line) usize;
pub extern fn ztextLineVisualRunCount(line: *const Line) usize;
pub extern fn ztextLineVisualRuns(line: *const Line) ?[*]const VisualRun;
pub extern fn ztextLineShapingRunCount(line: *const Line) usize;
pub extern fn ztextLineShapingRuns(line: *const Line) ?[*]const ShapingRun;
