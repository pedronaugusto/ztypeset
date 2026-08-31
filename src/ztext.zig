//! ztext -- text shaping and glyph rasterisation for Zig.
//!
//! Four vendored upstreams behind one package: FreeType rasterises, HarfBuzz
//! shapes, SheenBidi orders, libunibreak segments. ztext owns no atlas, no layout, no line breaking
//! and no rich text -- a host owns those, and keeping them out is what makes
//! this reusable.
//!
//! ```zig
//! const ztext = @import("ztext");
//!
//! // Copied, not borrowed: ztext keeps its own copy for as long as any handle
//! // can reach it. Installing also warms the caches the upstreams keep for the
//! // life of the process, so this allocator only ever sees ztext's working set.
//! try ztext.setAllocator(gpa_state.allocator());
//! defer ztext.resetAllocator();
//!
//! const library = try ztext.Library.init();
//! defer library.deinit();
//!
//! // The bytes are BORROWED and must outlive the font. Handles have no
//! // destruction order: whichever of a pair goes second frees what they share.
//! const font = try library.createFont(font_bytes, 0);
//! defer font.deinit();
//!
//! // A face is the font at one size. Make one per size you draw; a second size
//! // costs a size, not another parse.
//! const face = try font.face(0, 16);
//! defer face.deinit();
//!
//! const shaper = try ztext.Shaper.init();
//! defer shaper.deinit();
//!
//! const paragraph = try ztext.Paragraph.init(text, .{});
//! defer paragraph.deinit();
//!
//! // shapingRuns, not visualRuns: one visual run can span several scripts, and
//! // HarfBuzz shapes one script at a time.
//! for (paragraph.shapingRuns()) |run| {
//!     // shapeRun, not shape: the PARAGRAPH owns the text, so a run can only be
//!     // applied to the text it came from, HarfBuzz sees the characters either
//!     // side of the run, direction and script come from the run itself, and
//!     // text a paragraph already validated is not validated again per run.
//!     const glyphs = try shaper.shapeRun(face, paragraph, run, .{});
//!     for (glyphs) |glyph| {
//!         const bitmap = try face.renderGlyph(glyph.glyph_id, .a8, .light, 0, 0);
//!         // ... into your atlas, before the next call on this face.
//!         _ = bitmap;
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

/// The vendored upstreams, pinned. One home; see src/pins.zig.
pub const pins = @import("pins.zig");

//=============================================================================
// Public surface
//=============================================================================

pub const Error = error_mod.Error;
pub const resultName = error_mod.name;
pub const lastErrorDetail = error_mod.lastDetail;

/// Populates the process-global caches the upstreams never free before exit,
/// so a tracking allocator installed afterwards sees a balanced heap.
///
/// `setAllocator` calls this before it installs anything, so a host never has
/// to know to. Two caches are out of its reach because both need a real face:
/// shape one throwaway run before installing if you audit and use either. See
/// `ffi/ztext.h` and UPSTREAM.md for what they are.
pub fn warmup() void {
    c.ztextWarmup();
}

pub const setAllocator = memory_mod.setAllocator;
pub const resetAllocator = memory_mod.resetAllocator;

/// FreeType's own character-map encodings, as four-character tags; see
/// `Font.selectCharmapEncoding`.
pub const charmap_none = c.charmap_none;
pub const charmap_ms_symbol = c.charmap_ms_symbol;
pub const charmap_unicode = c.charmap_unicode;
pub const charmap_sjis = c.charmap_sjis;
pub const charmap_prc = c.charmap_prc;
pub const charmap_big5 = c.charmap_big5;
pub const charmap_wansung = c.charmap_wansung;
pub const charmap_johab = c.charmap_johab;
pub const charmap_adobe_standard = c.charmap_adobe_standard;
pub const charmap_adobe_expert = c.charmap_adobe_expert;
pub const charmap_adobe_custom = c.charmap_adobe_custom;
pub const charmap_adobe_latin_1 = c.charmap_adobe_latin_1;
pub const charmap_old_latin_2 = c.charmap_old_latin_2;
pub const charmap_apple_roman = c.charmap_apple_roman;

/// FreeType's and HarfBuzz's own reference synthetic styles, as fractions of
/// the em; see `Face.setSyntheticBold`.
pub const synthetic_bold_default = c.synthetic_bold_default;
pub const synthetic_oblique_default = c.synthetic_oblique_default;

pub const Charmap = types_mod.Charmap;
pub const Matrix = types_mod.Matrix;
pub const matrix_identity = types_mod.matrix_identity;
pub const rotation = types_mod.rotation;
pub const scaling = types_mod.scaling;
pub const shear = types_mod.shear;
pub const LineCap = types_mod.LineCap;
pub const LineJoin = types_mod.LineJoin;
pub const StrokeStyle = types_mod.StrokeStyle;
pub const Stroke = types_mod.Stroke;
pub const stroke_none = types_mod.stroke_none;
pub const outline = types_mod.outline;
pub const Glyph = types_mod.Glyph;
pub const GlyphFlag = types_mod.GlyphFlag;
pub const glyphHas = types_mod.glyphHas;
pub const bitmapChannels = types_mod.bitmapChannels;
pub const Segmentation = types_mod.Segmentation;
pub const segmentation = types_mod.segmentation;
pub const segmentationHas = types_mod.segmentationHas;
pub const Feature = types_mod.Feature;
pub const feature_global = types_mod.feature_global;
pub const Extents = types_mod.Extents;
pub const FaceMetrics = types_mod.FaceMetrics;
pub const GlyphBitmap = types_mod.GlyphBitmap;
pub const BitmapFormat = types_mod.BitmapFormat;
pub const OutlineFuncs = types_mod.OutlineFuncs;
pub const Metric = types_mod.Metric;
pub const VariationAxis = types_mod.VariationAxis;
pub const Variation = types_mod.Variation;
pub const VisualRun = types_mod.VisualRun;
pub const ScriptRun = types_mod.ScriptRun;
pub const Direction = types_mod.Direction;
pub const ClusterLevel = types_mod.ClusterLevel;
pub const BaseDirection = types_mod.BaseDirection;
pub const Encoding = types_mod.Encoding;
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
    _ = @import("pins.zig");
    // Reads README.md, this file and examples/quickstart.zig, so it belongs
    // here for the same reason abi_check does: the shipped module must not
    // embed the repository's documentation.
    _ = @import("example_test.zig");
}

/// `glyph_bitmap` -> `GlyphBitmap`, `result` -> `Result`.
fn pascal(comptime snake: []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        var upper = true;
        for (snake) |ch| {
            if (ch == '_') {
                upper = true;
                continue;
            }
            out = out ++ [_]u8{if (upper) std.ascii.toUpper(ch) else ch};
            upper = false;
        }
        return out;
    }
}

/// The `c.zig` type a `ZtextAbiLayout` field prefix names.
fn layoutType(comptime prefix: []const u8) type {
    if (!@hasDecl(c, pascal(prefix))) {
        @compileError("ZtextAbiLayout has a field named for `" ++ prefix ++
            "`, so src/c.zig must declare the type `" ++ pascal(prefix) ++
            "`, and it does not");
    }
    return @field(c, pascal(prefix));
}

/// What `ZtextAbiLayout.<name>` must hold, derived from the field's own name.
///
/// There is no list of checks here, and that is the point: the NAME is the
/// rule, so a field added to `ZtextAbiLayout` is checked the moment it exists
/// and a field whose name fits no rule is a compile error. Four fields --
/// `charmap_size`, `charmap_align`, `matrix_size`, `matrix_align` -- were
/// added to that struct and checked by nothing at all, which is what a
/// hand-written list of expectations does eventually.
///
///   * `layout_size`             -> `@sizeOf(AbiLayout)`
///   * `<type>_size` / `_align`  -> `@sizeOf` / `@alignOf` of `c.<Type>`
///   * `<type>_offset_<field>`   -> `@offsetOf(c.<Type>, "<field>")`
///   * `<enum>_last`             -> its last enumerator's value
///   * `<enum>_count`            -> how many enumerators it has
fn layoutExpectation(comptime name: []const u8) usize {
    @setEvalBranchQuota(100_000);
    if (comptime std.mem.eql(u8, name, "layout_size")) return @sizeOf(c.AbiLayout);

    if (comptime std.mem.indexOf(u8, name, "_offset_")) |at| {
        const Owner = layoutType(name[0..at]);
        const member = name[at + "_offset_".len ..];
        if (!@hasField(Owner, member)) {
            @compileError("ZtextAbiLayout." ++ name ++ " names a field `" ++ member ++
                "` that " ++ @typeName(Owner) ++ " does not have");
        }
        return @offsetOf(Owner, member);
    }
    if (comptime std.mem.endsWith(u8, name, "_size")) {
        return @sizeOf(layoutType(name[0 .. name.len - "_size".len]));
    }
    if (comptime std.mem.endsWith(u8, name, "_align")) {
        return @alignOf(layoutType(name[0 .. name.len - "_align".len]));
    }
    if (comptime std.mem.endsWith(u8, name, "_last")) {
        const fields = @typeInfo(layoutType(name[0 .. name.len - "_last".len])).@"enum".fields;
        return @intCast(fields[fields.len - 1].value);
    }
    if (comptime std.mem.endsWith(u8, name, "_count")) {
        return @typeInfo(layoutType(name[0 .. name.len - "_count".len])).@"enum".fields.len;
    }
    @compileError("ZtextAbiLayout." ++ name ++
        " ends in none of _size, _align, _offset_<field>, _last or _count, so " ++
        "src/ztext.zig has no rule for what it should hold");
}

test "the C library agrees with the extern declarations in c.zig" {
    // This is the guard that makes hand-written externs safe. Every field the
    // Zig side believes in is checked against what the C translation unit
    // compiled to. A reordered field fails here rather than in production.
    //
    // Every field, by construction: the sweep is over `ZtextAbiLayout`'s own
    // fields and each one's expectation comes from its name, so there is no
    // way to add a field the check does not cover.
    var layout: c.AbiLayout = undefined;
    c.ztextAbiLayout(&layout);

    comptime var checked: usize = 0;
    inline for (@typeInfo(c.AbiLayout).@"struct".fields) |field| {
        const got: usize = @field(layout, field.name);
        const want = comptime layoutExpectation(field.name);
        if (got != want) {
            std.debug.print(
                "ZtextAbiLayout.{s}: the library says {d}, src/c.zig computes {d}\n",
                .{ field.name, got, want },
            );
            return error.AbiLayoutDisagrees;
        }
        comptime checked += 1;
    }
    // A sweep that silently matched nothing would be indistinguishable from
    // one that matched everything.
    try std.testing.expectEqual(
        @typeInfo(c.AbiLayout).@"struct".fields.len,
        checked,
    );
}

/// The bytes of one probe field, as an integer, whatever its type.
///
/// One representation for every field, so a pointer, a float, an enum and an
/// integer can be compared to each other -- which is what makes "no two
/// fields of one type share a marker" a question that can be asked at all.
fn markerOf(value: anytype) u64 {
    var out: u64 = 0;
    for (std.mem.asBytes(&value), 0..) |byte, i| {
        if (i >= 8) break;
        out |= @as(u64, byte) << @intCast(i * 8);
    }
    return out;
}

/// What `ztextAbiProbe` writes into each field of each probed struct.
///
/// The one home for the whole expectation. It is keyed by name and the sweep
/// below requires an entry for EVERY field of every probed struct, so a field
/// added to any of them is a compile error here rather than a value nothing
/// checks -- which is what five of ZtextFaceMetrics' fields were: vertical
/// ascender, descender, line height, max advance and the has-vertical flag
/// were in the probe, in the header and in src/c.zig, and in no expectation.
///
/// The markers are written on the Zig side against src/c.zig's own idea of
/// where each field sits. That is the whole point: the C compiler laid the
/// struct out from ffi/ztext.h and this reads it back through a layout
/// declared independently, so two same-typed fields that swapped places --
/// which no size, alignment or offset can see -- come back holding each
/// other's marker.
const probe_markers = [_]struct { []const u8, u64 }{
    .{ "allocator.allocate", markerOf(@as(usize, 0x111)) },
    .{ "allocator.reallocate", markerOf(@as(usize, 0x222)) },
    .{ "allocator.deallocate", markerOf(@as(usize, 0x333)) },
    .{ "allocator.user", markerOf(@as(usize, 0x444)) },
    .{ "feature.tag", markerOf(@as(u32, 0x101)) },
    .{ "feature.value", markerOf(@as(u32, 0x102)) },
    .{ "feature.start", markerOf(@as(u32, 0x103)) },
    .{ "feature.end", markerOf(@as(u32, 0x104)) },
    .{ "shape_params.direction", markerOf(c.Direction.ttb) },
    .{ "shape_params.script", markerOf(@as(u32, 0x105)) },
    .{ "shape_params.language", markerOf(@as(usize, 0x555)) },
    .{ "shape_params.features", markerOf(@as(usize, 0x666)) },
    .{ "shape_params.feature_count", markerOf(@as(usize, 0x107)) },
    .{ "shape_params.cluster_level", markerOf(c.ClusterLevel.characters) },
    .{ "shape_params.use_freetype_metrics", markerOf(@as(c_int, 0x108)) },
    .{ "glyph.glyph_id", markerOf(@as(u32, 0x201)) },
    .{ "glyph.cluster", markerOf(@as(u32, 0x202)) },
    .{ "glyph.flags", markerOf(@as(u32, 0x207)) },
    .{ "glyph.x_advance", markerOf(@as(f32, 203.25)) },
    .{ "glyph.y_advance", markerOf(@as(f32, 204.25)) },
    .{ "glyph.x_offset", markerOf(@as(f32, 205.25)) },
    .{ "glyph.y_offset", markerOf(@as(f32, 206.25)) },
    .{ "face_metrics.ascender", markerOf(@as(f32, 301.25)) },
    .{ "face_metrics.descender", markerOf(@as(f32, 302.25)) },
    .{ "face_metrics.line_height", markerOf(@as(f32, 303.25)) },
    .{ "face_metrics.max_advance", markerOf(@as(f32, 304.25)) },
    .{ "face_metrics.underline_position", markerOf(@as(f32, 305.25)) },
    .{ "face_metrics.underline_thickness", markerOf(@as(f32, 306.25)) },
    .{ "face_metrics.units_per_em", markerOf(@as(u32, 0x307)) },
    .{ "face_metrics.num_glyphs", markerOf(@as(u32, 0x308)) },
    .{ "face_metrics.pixel_size", markerOf(@as(f32, 309.25)) },
    .{ "face_metrics.vert_ascender", markerOf(@as(f32, 310.25)) },
    .{ "face_metrics.vert_descender", markerOf(@as(f32, 311.25)) },
    .{ "face_metrics.vert_line_height", markerOf(@as(f32, 312.25)) },
    .{ "face_metrics.vert_max_advance", markerOf(@as(f32, 313.25)) },
    .{ "face_metrics.has_vertical_metrics", markerOf(@as(u32, 0x30A)) },
    .{ "extents.x_min", markerOf(@as(f32, 401.25)) },
    .{ "extents.y_min", markerOf(@as(f32, 402.25)) },
    .{ "extents.x_max", markerOf(@as(f32, 403.25)) },
    .{ "extents.y_max", markerOf(@as(f32, 404.25)) },
    .{ "extents.x_advance", markerOf(@as(f32, 405.25)) },
    .{ "extents.y_advance", markerOf(@as(f32, 406.25)) },
    .{ "visual_run.offset", markerOf(@as(u32, 0x501)) },
    .{ "visual_run.length", markerOf(@as(u32, 0x502)) },
    .{ "visual_run.level", markerOf(@as(u8, 0x53)) },
    .{ "shaping_run.offset", markerOf(@as(u32, 0x611)) },
    .{ "shaping_run.length", markerOf(@as(u32, 0x612)) },
    .{ "shaping_run.script", markerOf(@as(u32, 0x613)) },
    .{ "shaping_run.level", markerOf(@as(u8, 0x64)) },
    .{ "script_run.offset", markerOf(@as(u32, 0x601)) },
    .{ "script_run.length", markerOf(@as(u32, 0x602)) },
    .{ "script_run.script", markerOf(@as(u32, 0x603)) },
    .{ "glyph_bitmap.pixels", markerOf(@as(usize, 0x777)) },
    .{ "glyph_bitmap.format", markerOf(c.BitmapFormat.sdf) },
    .{ "glyph_bitmap.width", markerOf(@as(u32, 0x701)) },
    .{ "glyph_bitmap.height", markerOf(@as(u32, 0x702)) },
    .{ "glyph_bitmap.pitch", markerOf(@as(i32, -0x703)) },
    .{ "glyph_bitmap.left", markerOf(@as(i32, -0x704)) },
    .{ "glyph_bitmap.top", markerOf(@as(i32, 0x705)) },
    .{ "glyph_bitmap.x_advance", markerOf(@as(f32, 706.25)) },
    .{ "charmap.platform_id", markerOf(@as(u16, 0x801)) },
    .{ "charmap.encoding_id", markerOf(@as(u16, 0x802)) },
    .{ "charmap.encoding", markerOf(@as(u32, 0x803)) },
    .{ "variation_axis.tag", markerOf(@as(u32, 0x804)) },
    .{ "variation_axis.min_value", markerOf(@as(f32, 805.25)) },
    .{ "variation_axis.default_value", markerOf(@as(f32, 806.25)) },
    .{ "variation_axis.max_value", markerOf(@as(f32, 807.25)) },
    .{ "variation.tag", markerOf(@as(u32, 0x808)) },
    .{ "variation.value", markerOf(@as(f32, 809.25)) },
    .{ "matrix.xx", markerOf(@as(f32, 901.25)) },
    .{ "matrix.xy", markerOf(@as(f32, 902.25)) },
    .{ "matrix.yx", markerOf(@as(f32, 903.25)) },
    .{ "matrix.yy", markerOf(@as(f32, 904.25)) },
    .{ "stroke.radius", markerOf(@as(f32, 905.25)) },
    .{ "stroke.miter_limit", markerOf(@as(f32, 906.25)) },
    .{ "stroke.cap", markerOf(c.LineCap.square) },
    .{ "stroke.join", markerOf(c.LineJoin.miter_fixed) },
    .{ "stroke.style", markerOf(c.StrokeStyle.shrunk) },
    .{ "outline_funcs.move_to", markerOf(@as(usize, 0x888)) },
    .{ "outline_funcs.line_to", markerOf(@as(usize, 0x999)) },
    .{ "outline_funcs.conic_to", markerOf(@as(usize, 0xAAA)) },
    .{ "outline_funcs.cubic_to", markerOf(@as(usize, 0xBBB)) },
    .{ "outline_funcs.close", markerOf(@as(usize, 0xCCC)) },
    .{ "outline_funcs.user", markerOf(@as(usize, 0xDDD)) },
};

fn probeMarker(comptime name: []const u8) u64 {
    comptime {
        @setEvalBranchQuota(100_000);
        for (probe_markers) |entry| {
            if (std.mem.eql(u8, entry[0], name)) return entry[1];
        }
        @compileError("ZtextAbiProbe has a field `" ++ name ++
            "` that src/ztext.zig's probe_markers has no entry for, so nothing " ++
            "says what ztextAbiProbe should have written into it");
    }
}

/// Every probed field, with the type it has and the marker meant for it.
///
/// Built from `ZtextAbiProbe` itself rather than from the table, so the table
/// is what has to keep up with the struct and not the other way round.
const ProbeField = struct { name: []const u8, type_name: []const u8, marker: u64 };

const probe_fields = blk: {
    @setEvalBranchQuota(1_000_000);
    var list: [probe_markers.len]ProbeField = undefined;
    var n = 0;
    for (@typeInfo(c.AbiProbe).@"struct".fields) |member| {
        for (@typeInfo(member.type).@"struct".fields) |field| {
            if (n == list.len) {
                @compileError("ZtextAbiProbe has more fields than src/ztext.zig's " ++
                    "probe_markers has entries, so at least one field's marker is " ++
                    "named by nothing");
            }
            const name = member.name ++ "." ++ field.name;
            list[n] = .{
                .name = name,
                .type_name = @typeName(field.type),
                .marker = probeMarker(name),
            };
            n += 1;
        }
    }
    if (n != list.len) {
        @compileError("src/ztext.zig's probe_markers has entries for fields " ++
            "ZtextAbiProbe does not have");
    }
    break :blk list;
};

// No two fields of one type may expect the same marker.
//
// This is a property of the TABLE, not of a run, which is why it is checked
// here and not in the test below: if two same-typed fields expected equal
// markers, a real transposition between them would read back correct and the
// test would pass. A check that can only be satisfied by a well-formed table
// has to reject a malformed one before anything is measured with it.
//
// A marker of zero is rejected for the same reason: zero is what an
// untouched field holds, so an expectation of zero cannot tell "the library
// wrote this" from "the library never touched it".
comptime {
    @setEvalBranchQuota(1_000_000);
    for (probe_fields, 0..) |field, i| {
        if (field.marker == 0) {
            @compileError("ZtextAbiProbe." ++ field.name ++ " expects a marker of " ++
                "zero, which is also what an untouched field holds");
        }
        for (probe_fields[0..i]) |prev| {
            if (prev.marker == field.marker and
                std.mem.eql(u8, prev.type_name, field.type_name))
            {
                @compileError("ZtextAbiProbe." ++ field.name ++ " and " ++ prev.name ++
                    " share the marker, so a swap between them would pass");
            }
        }
    }
}

test "every probed field carries the distinct marker the library wrote" {
    // Sizes and alignments cannot catch two same-typed fields swapping places:
    // `ascender` and `descender` are both floats, and transposing them changes
    // no size, no alignment and no offset the layout test looks at, while
    // turning every line of text upside down. The C side writes a distinct
    // marker into each field; this reads them back.
    //
    // The sweep is over the fields themselves rather than over a list someone
    // maintains, so a field added to any probed struct is a compile error in
    // probe_fields above rather than a value nothing checks. Five of
    // ZtextFaceMetrics' fields were exactly that: the vertical ascender,
    // descender, line height, max advance and the has-vertical flag were in
    // the probe, in the header and in src/c.zig, and in no expectation.
    //
    // Blind spot, stated: the markers say what ztextAbiProbe was WRITTEN to
    // put in each field. If ffi/ztext_abi.c assigned the wrong marker to the
    // right field and the table were updated to match, both would agree and
    // nothing here would notice. What it catches is the two sides disagreeing
    // about WHERE a field is -- which is the failure that ships.
    var probe: c.AbiProbe = undefined;
    c.ztextAbiProbe(&probe);

    var holes: usize = 0;
    var checked: usize = 0;

    inline for (@typeInfo(c.AbiProbe).@"struct".fields) |member| {
        inline for (@typeInfo(member.type).@"struct".fields) |field| {
            const name = member.name ++ "." ++ field.name;
            const want = comptime probeMarker(name);
            const got = markerOf(@field(@field(probe, member.name), field.name));

            if (got == 0) {
                std.debug.print(
                    "ZtextAbiProbe.{s} is zero: ztextAbiProbe never writes it\n",
                    .{name},
                );
                holes += 1;
            } else if (got != want) {
                std.debug.print(
                    "ZtextAbiProbe.{s}: the library wrote {x}, src/ztext.zig expects {x}\n",
                    .{ name, got, want },
                );
                holes += 1;
            }
            checked += 1;
        }
    }

    try std.testing.expectEqual(@as(usize, 0), holes);
    try std.testing.expectEqual(probe_markers.len, checked);
    // A floor, so a sweep that matched nothing is a failure, not a pass.
    try std.testing.expect(checked >= 70);
}

test "the linked upstreams are the pinned ones, to the patch" {
    const v = version();
    try std.testing.expectEqual(@as(u8, 0), v.major);
    try std.testing.expectEqual(@as(u8, 2), v.minor);

    // src/pins.zig is the one home for what libs/ holds, and this asserts the
    // LINKED libraries agree with it -- not that a document says so.
    //
    // All three components, deliberately. This used to check `hb.major == 14`
    // and nothing else, which passes for every HarfBuzz release in a year: a
    // re-vendor could land with the pin untouched and the assertion green.
    const expected = [_]pins.Version{
        pins.freetype.version,
        pins.harfbuzz.version,
        pins.sheenbidi.version,
        pins.libunibreak.version,
    };
    const actual = [_]Version{
        freetypeVersion(),
        harfbuzzVersion(),
        sheenbidiVersion(),
        unibreakVersion(),
    };
    for (expected, actual, pins.all) |want, got, pin| {
        const same = want.major == got.major and want.minor == got.minor and
            want.patch == got.patch;
        if (!same) {
            std.debug.print(
                "{s}: pinned {d}.{d}.{d}, linked {d}.{d}.{d}\n",
                .{ pin.name, want.major, want.minor, want.patch, got.major, got.minor, got.patch },
            );
            return error.LinkedUpstreamIsNotThePinnedOne;
        }
    }
}

test "result names are never empty" {
    inline for (@typeInfo(c.Result).@"enum".fields) |field| {
        const name = resultName(@enumFromInt(field.value));
        try std.testing.expect(name.len > 0);
    }
}
