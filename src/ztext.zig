//! ztext -- text shaping and glyph rasterisation for Zig.
//!
//! Four vendored upstreams behind one package: FreeType rasterises, HarfBuzz
//! shapes, SheenBidi orders, libunibreak segments. ztext owns no atlas, no layout, no line breaking
//! and no rich text -- a host owns those, and keeping them out is what makes
//! this reusable.
//!
//! ```zig
//! // Warm the caches the upstreams keep for the life of the process, so a
//! // tracking allocator installed next sees only ztext's working set.
//! ztext.warmup();
//!
//! const gpa = gpa_state.allocator();
//! try ztext.setAllocator(&gpa);
//! defer ztext.resetAllocator();
//!
//! const library = try ztext.Library.init();
//! defer library.deinit();
//!
//! // The bytes are BORROWED and must outlive the font. The font and its
//! // faces must be destroyed before the library, but not before each other.
//! const font = try library.createFont(font_bytes, 0);
//! defer font.deinit();
//!
//! // A face is the font at one size; make one per size you draw.
//! const face = try font.face(0, 16);
//! defer face.deinit();
//!
//! const shaper = try ztext.Shaper.init();
//! defer shaper.deinit();
//!
//! const paragraph = try ztext.Paragraph.init(text, .auto);
//! defer paragraph.deinit();
//!
//! // shapingRuns, not visualRuns: a visual run can span several scripts, and
//! // HarfBuzz shapes one script at a time.
//! for (paragraph.shapingRuns()) |run| {
//!     const glyphs = try shaper.shape(face, text[run.offset..][0..run.length], .{
//!         .direction = ztext.runDirection(run.level),
//!         .script = run.script,
//!     });
//!     for (glyphs) |glyph| {
//!         const bitmap = try face.renderGlyph(glyph.glyph_id, .a8, .light, 0, 0);
//!         _ = bitmap; // ... into your atlas, before this face's next render.
//!     }
//! }
//! ```

const std = @import("std");

pub const c = @import("c.zig");

const error_mod = @import("error.zig");
const types_mod = @import("types.zig");
const memory_mod = @import("memory.zig");
const face_mod = @import("face.zig");
const shaper_mod = @import("shaper.zig");
const bidi_mod = @import("bidi.zig");

//=============================================================================
// Public surface
//=============================================================================

pub const Error = error_mod.Error;
pub const resultName = error_mod.name;
pub const lastErrorDetail = error_mod.lastDetail;

/// Populates the process-global caches the upstreams never free before exit,
/// so a tracking allocator installed afterwards sees a balanced heap.
///
/// Optional; nothing needs it to work correctly. See `ffi/ztext.h` and
/// UPSTREAM.md for what those caches are and why they exist.
pub fn warmup() void {
    c.ztextWarmup();
}

pub const setAllocator = memory_mod.setAllocator;
pub const resetAllocator = memory_mod.resetAllocator;

pub const Glyph = types_mod.Glyph;
pub const Feature = types_mod.Feature;
pub const feature_global = types_mod.feature_global;
pub const Extents = types_mod.Extents;
pub const FaceMetrics = types_mod.FaceMetrics;
pub const GlyphBitmap = types_mod.GlyphBitmap;
pub const OutlineFuncs = types_mod.OutlineFuncs;
pub const VariationAxis = types_mod.VariationAxis;
pub const Variation = types_mod.Variation;
pub const VisualRun = types_mod.VisualRun;
pub const ScriptRun = types_mod.ScriptRun;
pub const Direction = types_mod.Direction;
pub const ClusterLevel = types_mod.ClusterLevel;
pub const BaseDirection = types_mod.BaseDirection;
pub const RenderMode = types_mod.RenderMode;
pub const Hinting = types_mod.Hinting;
pub const tag = types_mod.tag;

pub const Library = face_mod.Library;
pub const Font = face_mod.Font;
pub const Face = face_mod.Face;
pub const bitmapRows = face_mod.bitmapRows;

pub const Shaper = shaper_mod.Shaper;
pub const ShapeParams = shaper_mod.Params;

pub const Paragraph = bidi_mod.Paragraph;
pub const Line = bidi_mod.Line;
pub const Break = types_mod.Break;
pub const ShapingRun = types_mod.ShapingRun;
pub const runDirection = bidi_mod.runDirection;

/// Build options the C library was actually compiled with, so a consumer can
/// branch on them instead of assuming.
pub const options = @import("ztext_options");

//=============================================================================
// Versions
//=============================================================================

pub const Version = struct {
    major: u8,
    minor: u8,
    patch: u8,

    fn unpack(packed_value: u32) Version {
        return .{
            .major = @truncate(packed_value >> 16),
            .minor = @truncate(packed_value >> 8),
            .patch = @truncate(packed_value),
        };
    }

    pub fn format(self: Version, writer: *std.Io.Writer) std.Io.Writer.Error!void {
        try writer.print("{d}.{d}.{d}", .{ self.major, self.minor, self.patch });
    }
};

/// Version of these bindings.
pub fn version() Version {
    return Version.unpack(c.ztextVersion());
}

/// Versions of the vendored upstreams, as compiled in -- not as UPSTREAM.md
/// claims.
pub fn freetypeVersion() Version {
    return Version.unpack(c.ztextFreetypeVersion());
}

pub fn harfbuzzVersion() Version {
    return Version.unpack(c.ztextHarfbuzzVersion());
}

pub fn sheenbidiVersion() Version {
    return Version.unpack(c.ztextSheenbidiVersion());
}

/// Version of the vendored libunibreak, which supplies UAX #14 and #29.
pub fn unibreakVersion() Version {
    return Version.unpack(c.ztextUnibreakVersion());
}

//=============================================================================
// Tests
//=============================================================================

test {
    // Pull every module in so its own tests are discovered and run.
    _ = error_mod;
    _ = types_mod;
    _ = memory_mod;
    _ = face_mod;
    _ = shaper_mod;
    _ = bidi_mod;
    _ = @import("integration_test.zig");
    // Compares c.zig against the real header. Imported here, inside `test`, so
    // the shipped module never analyses it and never runs translate-c.
    _ = @import("abi_check.zig");
}

test "the C library agrees with the extern declarations in c.zig" {
    // This is the guard that makes hand-written externs safe. Every field the
    // Zig side believes in is checked against what the C translation unit
    // compiled to. A reordered field fails here rather than in production.
    var layout: c.AbiLayout = undefined;
    c.ztextAbiLayout(&layout);

    try std.testing.expectEqual(@as(u32, @sizeOf(c.AbiLayout)), layout.layout_size);

    try std.testing.expectEqual(@as(u32, @sizeOf(c.Allocator)), layout.allocator_size);
    try std.testing.expectEqual(@as(u32, @alignOf(c.Allocator)), layout.allocator_align);
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Allocator, "allocate")),
        layout.allocator_offset_allocate,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Allocator, "reallocate")),
        layout.allocator_offset_reallocate,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Allocator, "deallocate")),
        layout.allocator_offset_deallocate,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Allocator, "user")),
        layout.allocator_offset_user,
    );

    try std.testing.expectEqual(@as(u32, @sizeOf(c.Glyph)), layout.glyph_size);
    try std.testing.expectEqual(@as(u32, @alignOf(c.Glyph)), layout.glyph_align);
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Glyph, "glyph_id")),
        layout.glyph_offset_glyph_id,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Glyph, "cluster")),
        layout.glyph_offset_cluster,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Glyph, "x_advance")),
        layout.glyph_offset_x_advance,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Glyph, "y_advance")),
        layout.glyph_offset_y_advance,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Glyph, "x_offset")),
        layout.glyph_offset_x_offset,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.Glyph, "y_offset")),
        layout.glyph_offset_y_offset,
    );

    try std.testing.expectEqual(@as(u32, @sizeOf(c.Feature)), layout.feature_size);
    try std.testing.expectEqual(@as(u32, @alignOf(c.Feature)), layout.feature_align);
    try std.testing.expectEqual(@as(u32, @sizeOf(c.ShapeParams)), layout.shape_params_size);
    try std.testing.expectEqual(@as(u32, @alignOf(c.ShapeParams)), layout.shape_params_align);
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.ShapeParams, "language")),
        layout.shape_params_offset_language,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.ShapeParams, "features")),
        layout.shape_params_offset_features,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.ShapeParams, "feature_count")),
        layout.shape_params_offset_feature_count,
    );

    try std.testing.expectEqual(@as(u32, @sizeOf(c.FaceMetrics)), layout.face_metrics_size);
    try std.testing.expectEqual(@as(u32, @alignOf(c.FaceMetrics)), layout.face_metrics_align);
    try std.testing.expectEqual(@as(u32, @sizeOf(c.Extents)), layout.extents_size);
    try std.testing.expectEqual(@as(u32, @alignOf(c.Extents)), layout.extents_align);

    try std.testing.expectEqual(@as(u32, @sizeOf(c.VisualRun)), layout.visual_run_size);
    try std.testing.expectEqual(@as(u32, @alignOf(c.VisualRun)), layout.visual_run_align);
    try std.testing.expectEqual(@as(u32, @sizeOf(c.ScriptRun)), layout.script_run_size);
    try std.testing.expectEqual(@as(u32, @alignOf(c.ScriptRun)), layout.script_run_align);

    try std.testing.expectEqual(@as(u32, @sizeOf(c.GlyphBitmap)), layout.glyph_bitmap_size);
    try std.testing.expectEqual(@as(u32, @alignOf(c.GlyphBitmap)), layout.glyph_bitmap_align);
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.GlyphBitmap, "pixels")),
        layout.glyph_bitmap_offset_pixels,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.GlyphBitmap, "pitch")),
        layout.glyph_bitmap_offset_pitch,
    );
    try std.testing.expectEqual(
        @as(u32, @offsetOf(c.GlyphBitmap, "x_advance")),
        layout.glyph_bitmap_offset_x_advance,
    );

    // Enum tag sizes. Not fixed by the C standard: a compiler may choose any
    // type that fits, and `enum(c_int)` declared against an enum the compiler
    // made narrower writes past its parameter slot -- silent stack corruption
    // at the call boundary. Nothing else here would catch it, because sizes
    // and offsets of the STRUCTS stay correct while the enums inside them do
    // not.
    //
    // The last enumerator of each goes with it: a value renumbered in the
    // header without this file following turns every switch into a wrong
    // branch, and no layout check would notice.
    try std.testing.expectEqual(@as(u32, @sizeOf(c.Result)), layout.result_size);
    try std.testing.expectEqual(
        @as(u32, @intCast(@intFromEnum(c.Result.buffer_too_small))),
        layout.result_last,
    );
    try std.testing.expectEqual(@as(u32, @sizeOf(c.Direction)), layout.direction_size);
    try std.testing.expectEqual(
        @as(u32, @intCast(@intFromEnum(c.Direction.btt))),
        layout.direction_last,
    );
    try std.testing.expectEqual(
        @as(u32, @sizeOf(c.ClusterLevel)),
        layout.cluster_level_size,
    );
    try std.testing.expectEqual(
        @as(u32, @intCast(@intFromEnum(c.ClusterLevel.graphemes))),
        layout.cluster_level_last,
    );
    try std.testing.expectEqual(
        @as(u32, @sizeOf(c.BaseDirection)),
        layout.base_direction_size,
    );
    try std.testing.expectEqual(
        @as(u32, @intCast(@intFromEnum(c.BaseDirection.rtl))),
        layout.base_direction_last,
    );
    try std.testing.expectEqual(@as(u32, @sizeOf(c.RenderMode)), layout.render_mode_size);
    try std.testing.expectEqual(
        @as(u32, @intCast(@intFromEnum(c.RenderMode.sdf))),
        layout.render_mode_last,
    );
    try std.testing.expectEqual(@as(u32, @sizeOf(c.Hinting)), layout.hinting_size);
    try std.testing.expectEqual(
        @as(u32, @intCast(@intFromEnum(c.Hinting.none))),
        layout.hinting_last,
    );

    // The Zig error mapping must cover every C result. `c.Result` is
    // non-exhaustive so it can survive a newer shared library, but its NAMED
    // fields must still be exactly the C enumerators -- which is what keeps
    // `error.UnknownResult` a genuine "this build is older" signal rather than
    // a hole someone forgot to fill.
    const result_fields = @typeInfo(c.Result).@"enum".fields;
    try std.testing.expectEqual(@as(u32, result_fields.len), layout.result_count);
}

test "every field of every shared struct lands where Zig expects it" {
    // Sizes and alignments cannot catch two same-typed fields swapping places:
    // `ascender` and `descender` are both floats, and transposing them changes
    // no size, no alignment and no offset the layout test looks at, while
    // turning every line of text upside down. The C side writes a distinct
    // marker into each field; this reads them back.
    var probe: c.AbiProbe = undefined;
    c.ztextAbiProbe(&probe);

    try std.testing.expectEqual(@as(usize, 0x111), @intFromPtr(probe.allocator.allocate.?));
    try std.testing.expectEqual(@as(usize, 0x222), @intFromPtr(probe.allocator.reallocate.?));
    try std.testing.expectEqual(@as(usize, 0x333), @intFromPtr(probe.allocator.deallocate.?));
    try std.testing.expectEqual(@as(usize, 0x444), @intFromPtr(probe.allocator.user.?));

    try std.testing.expectEqual(@as(u32, 0x101), probe.feature.tag);
    try std.testing.expectEqual(@as(u32, 0x102), probe.feature.value);
    try std.testing.expectEqual(@as(u32, 0x103), probe.feature.start);
    try std.testing.expectEqual(@as(u32, 0x104), probe.feature.end);

    try std.testing.expectEqual(c.Direction.ttb, probe.shape_params.direction);
    try std.testing.expectEqual(@as(u32, 0x105), probe.shape_params.script);
    try std.testing.expectEqual(@as(usize, 0x555), @intFromPtr(probe.shape_params.language.?));
    try std.testing.expectEqual(@as(usize, 0x666), @intFromPtr(probe.shape_params.features.?));
    try std.testing.expectEqual(@as(usize, 0x107), probe.shape_params.feature_count);
    try std.testing.expectEqual(c.ClusterLevel.characters, probe.shape_params.cluster_level);
    try std.testing.expectEqual(@as(c_int, 0x108), probe.shape_params.use_freetype_metrics);

    try std.testing.expectEqual(@as(u32, 0x201), probe.glyph.glyph_id);
    try std.testing.expectEqual(@as(u32, 0x202), probe.glyph.cluster);
    try std.testing.expectEqual(@as(f32, 203.25), probe.glyph.x_advance);
    try std.testing.expectEqual(@as(f32, 204.25), probe.glyph.y_advance);
    try std.testing.expectEqual(@as(f32, 205.25), probe.glyph.x_offset);
    try std.testing.expectEqual(@as(f32, 206.25), probe.glyph.y_offset);

    try std.testing.expectEqual(@as(f32, 301.25), probe.face_metrics.ascender);
    try std.testing.expectEqual(@as(f32, 302.25), probe.face_metrics.descender);
    try std.testing.expectEqual(@as(f32, 303.25), probe.face_metrics.line_height);
    try std.testing.expectEqual(@as(f32, 304.25), probe.face_metrics.max_advance);
    try std.testing.expectEqual(@as(f32, 305.25), probe.face_metrics.underline_position);
    try std.testing.expectEqual(@as(f32, 306.25), probe.face_metrics.underline_thickness);
    try std.testing.expectEqual(@as(u32, 0x307), probe.face_metrics.units_per_em);
    try std.testing.expectEqual(@as(u32, 0x308), probe.face_metrics.num_glyphs);
    try std.testing.expectEqual(@as(f32, 309.25), probe.face_metrics.pixel_size);

    try std.testing.expectEqual(@as(f32, 401.25), probe.extents.x_min);
    try std.testing.expectEqual(@as(f32, 402.25), probe.extents.y_min);
    try std.testing.expectEqual(@as(f32, 403.25), probe.extents.x_max);
    try std.testing.expectEqual(@as(f32, 404.25), probe.extents.y_max);
    try std.testing.expectEqual(@as(f32, 405.25), probe.extents.x_advance);
    try std.testing.expectEqual(@as(f32, 406.25), probe.extents.y_advance);

    try std.testing.expectEqual(@as(u32, 0x501), probe.visual_run.offset);
    try std.testing.expectEqual(@as(u32, 0x502), probe.visual_run.length);
    try std.testing.expectEqual(@as(u8, 0x53), probe.visual_run.level);

    try std.testing.expectEqual(@as(u32, 0x611), probe.shaping_run.offset);
    try std.testing.expectEqual(@as(u32, 0x612), probe.shaping_run.length);
    try std.testing.expectEqual(@as(u32, 0x613), probe.shaping_run.script);
    try std.testing.expectEqual(@as(u8, 0x64), probe.shaping_run.level);

    try std.testing.expectEqual(@as(u32, 0x601), probe.script_run.offset);
    try std.testing.expectEqual(@as(u32, 0x602), probe.script_run.length);
    try std.testing.expectEqual(@as(u32, 0x603), probe.script_run.script);

    try std.testing.expectEqual(@as(usize, 0x777), @intFromPtr(probe.glyph_bitmap.pixels.?));
    try std.testing.expectEqual(@as(u32, 0x701), probe.glyph_bitmap.width);
    try std.testing.expectEqual(@as(u32, 0x702), probe.glyph_bitmap.height);
    try std.testing.expectEqual(@as(i32, -0x703), probe.glyph_bitmap.pitch);
    try std.testing.expectEqual(@as(i32, -0x704), probe.glyph_bitmap.left);
    try std.testing.expectEqual(@as(i32, 0x705), probe.glyph_bitmap.top);
    try std.testing.expectEqual(@as(f32, 706.25), probe.glyph_bitmap.x_advance);
}

test "version reporting is wired up" {
    const v = version();
    try std.testing.expectEqual(@as(u8, 0), v.major);
    try std.testing.expectEqual(@as(u8, 1), v.minor);

    // Pinned in UPSTREAM.md; these assert the library actually compiled the
    // versions the documentation claims, rather than whatever was on the
    // include path.
    const ft = freetypeVersion();
    try std.testing.expectEqual(@as(u8, 2), ft.major);
    try std.testing.expectEqual(@as(u8, 14), ft.minor);

    const hb = harfbuzzVersion();
    try std.testing.expectEqual(@as(u8, 14), hb.major);

    const sb = sheenbidiVersion();
    try std.testing.expectEqual(@as(u8, 3), sb.major);

    const ub = unibreakVersion();
    try std.testing.expectEqual(@as(u8, 7), ub.major);
    try std.testing.expectEqual(@as(u8, 0), ub.minor);
}

test "result names are never empty" {
    inline for (@typeInfo(c.Result).@"enum".fields) |field| {
        const name = resultName(@enumFromInt(field.value));
        try std.testing.expect(name.len > 0);
    }
}
