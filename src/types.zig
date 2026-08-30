//! The plain-data types, re-exported from the C declarations.
//!
//! Deliberately aliases rather than second declarations: a parallel Zig struct
//! would be a third place for the layout to drift, and the `ztextAbiLayout`
//! test already proves `c.zig` agrees with the header. One definition, checked
//! once.

const c = @import("c.zig");

/// One positioned glyph. Advances and offsets are in pixels at the face's
/// current size, y-up.
///
/// `cluster` is a BYTE offset into the UTF-8 that was shaped, not a codepoint
/// index. Several glyphs may share a cluster (one character decomposing) and
/// several characters may share one (a ligature).
///
/// `flags` is a bit mask; read it through `glyphHas`.
pub const Glyph = c.Glyph;

/// One bit of `Glyph.flags`: what shaping learned about the text around a
/// glyph. See `ffi/ztext.h` for each flag's full meaning -- and for why ztext
/// always produces all three rather than only the one HarfBuzz gives away.
pub const GlyphFlag = c.GlyphFlag;

/// Whether `glyph` carries `flag`.
///
/// A named predicate rather than an open-coded `& @intFromEnum(...)` at every
/// call site: the mask is the kind of expression that is wrong exactly once
/// and then copied, and a line-breaker that reads the wrong bit produces
/// correct-looking text with the wrong breaks in it.
pub fn glyphHas(glyph: Glyph, flag: GlyphFlag) bool {
    const bit: u32 = @intCast(@intFromEnum(flag));
    return glyph.flags & bit != 0;
}

/// One OpenType feature setting. `start`/`end` are byte offsets into the run;
/// use `0` and `feature_global` for the whole run.
pub const Feature = c.Feature;

pub const feature_global: u32 = 0xFFFF_FFFF;

/// Ink bounds plus the pen movement. `x_advance` is the sum of the glyph
/// advances, which is not the same as the ink width.
pub const Extents = c.Extents;

/// Face-wide metrics at the current pixel size. `descender` is NEGATIVE, which
/// is FreeType's convention and is kept rather than flipped.
pub const FaceMetrics = c.FaceMetrics;

/// A rasterised glyph. `pixels` borrows the face's glyph slot and is valid
/// only until the next render or shape on that face.
///
/// `format` says how to read those pixels and is set even when there are
/// none, so it never has to be inferred from the `RenderMode` that was asked
/// for.
pub const GlyphBitmap = c.GlyphBitmap;

/// How to read a `GlyphBitmap`'s bytes. Deliberately not `RenderMode`: what
/// was requested and what came back are two facts, and one day they may
/// differ.
pub const BitmapFormat = c.BitmapFormat;

/// Callbacks for `Face.decomposeOutline`: one per outline command, points in
/// 26.6 fixed point, plus a `user` pointer passed back unmodified.
pub const OutlineFuncs = c.OutlineFuncs;

/// A maximal span of one embedding level, in visual order.
/// Where a boundary is permitted, required, or absent.
///
/// Byte-sized on purpose: these arrive as arrays of one byte per text byte,
/// and this is the type that reads them without a cast at every use. The
/// values are cross-checked against the C macros through `c.break_*`.
pub const Break = enum(u8) {
    none = c.break_none,
    allowed = c.break_allowed,
    mandatory = c.break_mandatory,
};

pub const VisualRun = c.VisualRun;

/// A maximal span of one script, in logical order. `script` is an ISO 15924
/// tag, ready to hand to `ShapeParams.script`.
pub const ScriptRun = c.ScriptRun;

/// A span ready to shape: one direction, one script, in visual order.
///
/// The intersection of `VisualRun` and `ScriptRun`, which is the one part of
/// itemisation that is easy to get subtly wrong -- inside a right-to-left
/// visual run the script pieces have to come out in reverse.
pub const ShapingRun = c.ShapingRun;

/// One variable axis, in design units -- the numbers the font's `fvar` table
/// itself names, not the normalised -1..1 the OpenType internals work in.
pub const VariationAxis = c.VariationAxis;

/// One axis set to one value, for `Font.setVariations`.
pub const Variation = c.Variation;

pub const Direction = c.Direction;
pub const ClusterLevel = c.ClusterLevel;
pub const BaseDirection = c.BaseDirection;
pub const RenderMode = c.RenderMode;
pub const Hinting = c.Hinting;

/// Packs four characters into an OpenType tag, big-endian as the specs write
/// them: `tag("liga")`, `tag("Arab")`.
///
/// Neither `comptime` nor sentinel-terminated: script tags are routinely
/// carried at runtime, and a `*const [4:0]u8` would accept only a string
/// literal -- `tag(&some_runtime_array)` would not compile, which is exactly
/// the case this is meant to serve.
pub fn tag(name: *const [4]u8) u32 {
    return (@as(u32, name[0]) << 24) | (@as(u32, name[1]) << 16) |
        (@as(u32, name[2]) << 8) | @as(u32, name[3]);
}

test "tag packs big-endian, from a literal or a runtime array" {
    const std = @import("std");
    try std.testing.expectEqual(@as(u32, 0x6C696761), tag("liga"));
    try std.testing.expectEqual(@as(u32, 0x41726162), tag("Arab"));

    var runtime: [4]u8 = .{ 'A', 'r', 'a', 'b' };
    try std.testing.expectEqual(@as(u32, 0x41726162), tag(&runtime));
}
