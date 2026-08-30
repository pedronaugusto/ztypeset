//! End-to-end tests against the committed OFL fonts.
//!
//! The golden numbers here were measured, not predicted: they are what this
//! exact combination of vendored HarfBuzz, vendored FreeType and these exact
//! font bytes produces. `ci/verify-vendor.sh` checks the font hashes, so a
//! golden that starts failing means one of those three moved -- which is
//! precisely the change worth reviewing rather than absorbing.
//!
//! Every test installs `std.testing.allocator`, so any allocation ztext or an
//! upstream fails to return is a test failure. That works because installing
//! an allocator populates the upstreams' process-lifetime caches before it
//! swaps, and because `warmProcessCaches` below reaches the two that need a
//! face and so cannot be reached from inside ztext; see UPSTREAM.md.

const std = @import("std");
const ztext = @import("ztext.zig");
const fonts = @import("fonts");

const ppem: u32 = 32;

/// The two process-lifetime caches `ztext.warmup()` cannot reach on its own,
/// touched here on the default allocator before the test allocator goes in.
///
/// Both need a real face, which warmup has no way to obtain:
///
///   * HarfBuzz's FreeType font-functions singleton, built the first time any
///     face is asked for `use_freetype_metrics`.
///   * One entry in HarfBuzz's language intern table per DISTINCT language
///     tag ever passed. Small (tens of bytes each) and bounded by the number
///     of locales a host ships, but permanent until exit.
///
/// A host that shapes without a language and never asks for FreeType metrics
/// allocates neither. Both are recorded in UPSTREAM.md.
fn warmProcessCaches() !void {
    const library = try ztext.Library.init();
    defer library.deinit();
    const font = try library.createFont(fonts.hebrew, 0);
    defer font.deinit();
    const face = try font.face(0, ppem);
    defer face.deinit();
    const shaper = try ztext.Shaper.init();
    defer shaper.deinit();

    _ = try shaper.shape(face, "a", .{ .use_freetype_metrics = true });
    for (test_languages) |language| {
        _ = try shaper.shape(face, "a", .{ .language = language });
    }
}

/// Every language tag the suite uses, so the intern table is populated before
/// anything is counted.
const test_languages = [_][:0]const u8{ "en", "tr", "de", "ar" };

/// Everything a test needs, torn down in one place.
const Fixture = struct {
    library: ztext.Library,
    shaper: ztext.Shaper,

    fn init() !Fixture {
        try warmProcessCaches();
        try ztext.setAllocator(std.testing.allocator);
        return .{
            .library = try ztext.Library.init(),
            .shaper = try ztext.Shaper.init(),
        };
    }

    fn deinit(self: Fixture) void {
        self.shaper.deinit();
        self.library.deinit();
        ztext.resetAllocator();
    }

    /// A face at the suite's default size, for the many tests that want one
    /// face and have nothing to say about the font.
    ///
    /// The font handle is released immediately and the face keeps it alive,
    /// which means every test built on this exercises the destroy-the-font-
    /// first path rather than only the tidy order.
    fn face(self: Fixture, bytes: []const u8) !ztext.Face {
        const font = try self.library.createFont(bytes, 0);
        defer font.deinit();
        return font.face(0, ppem);
    }

    /// A face at a caller-chosen size, same lifetime bargain.
    fn faceAt(self: Fixture, bytes: []const u8, size: f32) !ztext.Face {
        const font = try self.library.createFont(bytes, 0);
        defer font.deinit();
        return font.face(0, size);
    }
};

/// Advances are 26.6 fixed point converted to float, so every expected value
/// is an exact multiple of 1/64. A sixteenth of that is a generous tolerance
/// that still catches a one-unit drift.
const tolerance: f32 = 1.0 / 1024.0;

fn expectAdvances(glyphs: []const ztext.Glyph, expected: []const f32) !void {
    try std.testing.expectEqual(expected.len, glyphs.len);
    for (glyphs, expected) |glyph, want| {
        try std.testing.expectApproxEqAbs(want, glyph.x_advance, tolerance);
    }
}

fn expectGlyphIds(glyphs: []const ztext.Glyph, expected: []const u32) !void {
    try std.testing.expectEqual(expected.len, glyphs.len);
    for (glyphs, expected) |glyph, want| {
        try std.testing.expectEqual(want, glyph.glyph_id);
    }
}

//=============================================================================
// Faces
//=============================================================================

test "a face reports the metrics its font actually declares" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.hebrew);
    defer face.deinit();

    try std.testing.expectEqualStrings("Noto Sans Hebrew", face.font.familyName());
    try std.testing.expectEqualStrings("Regular", face.font.styleName());

    const metrics = try face.metrics();
    try std.testing.expectEqual(@as(u32, 1000), metrics.units_per_em);
    try std.testing.expectEqual(@as(u32, 151), metrics.num_glyphs);
    try std.testing.expectEqual(@as(f32, ppem), metrics.pixel_size);
    try std.testing.expectApproxEqAbs(@as(f32, 35.0), metrics.ascender, tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, -10.0), metrics.descender, tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 44.0), metrics.line_height, tolerance);

    // The sign convention is load-bearing and easy to "fix" wrongly.
    try std.testing.expect(metrics.descender < 0);
    try std.testing.expect(metrics.ascender > 0);
}

test "a fractional pixel size is honoured rather than rounded" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // 9 pt at a 150% scale factor. A UI that rounds this to 19 drifts against
    // every other element laid out on the same scale, which is the whole
    // reason this takes a float.
    try face.setPixelSize(0, 18.75);
    try std.testing.expectEqual(@as(f32, 18.75), (try face.metrics()).pixel_size);

    // Advances are the thing that has to move continuously: they are what a
    // layout adds up. Measured at three sizes a quarter-pixel apart, they must
    // be strictly increasing -- which they cannot be if the size were rounded
    // on the way in.
    var widths: [3]f32 = undefined;
    for ([_]f32{ 18.25, 18.5, 18.75 }, 0..) |size, i| {
        try face.setPixelSize(0, size);
        _ = try fixture.shaper.shape(face, "Hamburgefonstiv", .{ .direction = .ltr });
        widths[i] = (try fixture.shaper.extents(face)).x_advance;
    }
    try std.testing.expect(widths[0] < widths[1]);
    try std.testing.expect(widths[1] < widths[2]);

    // Ink follows too: the same glyph rasterises taller as the size grows
    // through values that are not whole pixels.
    const glyph = face.font.glyphIndex('H');
    try face.setPixelSize(0, 18.0);
    const small = try face.renderGlyph(glyph, .a8, .none, 0, 0);
    const small_height = small.height;
    try face.setPixelSize(0, 18.5);
    const larger = try face.renderGlyph(glyph, .a8, .none, 0, 0);
    try std.testing.expect(larger.height > small_height);
}

test "FreeType grid-fits face metrics even when the size does not" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // Worth pinning because it looks like a ztext bug and is not. FreeType
    // computes a face's ascender, descender, height and max_advance with
    // FT_PIX_CEIL/FLOOR/ROUND -- see ft_recompute_scaled_metrics in
    // src/base/ftobjs.c, under a GRID_FIT_METRICS that is #defined
    // unconditionally a hundred lines above it, so there is no build option
    // that turns it off.
    //
    // So a host laying out at a fractional size gets advances that move
    // smoothly and a line height that steps in whole pixels. That is
    // FreeType's answer, passed through rather than smoothed over; a host
    // wanting a fractional leading should scale units_per_em itself.
    try face.setPixelSize(0, 18.0);
    const at_18 = try face.metrics();
    try face.setPixelSize(0, 18.5);
    const at_18_5 = try face.metrics();

    try std.testing.expectEqual(at_18.line_height, at_18_5.line_height);
    try std.testing.expectEqual(at_18.ascender, at_18_5.ascender);
    try std.testing.expectEqual(@as(f32, 18.5), at_18_5.pixel_size);

    // Whole pixels, all of them.
    try std.testing.expectEqual(@floor(at_18_5.line_height), at_18_5.line_height);
    try std.testing.expectEqual(@floor(at_18_5.ascender), at_18_5.ascender);
    try std.testing.expectEqual(@floor(at_18_5.descender), at_18_5.descender);
}

test "a pixel size is quantised to 1/64, and says so" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // FreeType's own resolution is 26.6 fixed point, so a size that is not a
    // multiple of 1/64 is reported as what was actually used rather than as
    // what was asked for. 18.7 * 64 rounds to 1197, which is 18.703125.
    try face.setPixelSize(0, 18.7);
    try std.testing.expectEqual(@as(f32, 18.703125), (try face.metrics()).pixel_size);

    // A zero axis copies the other, in both directions.
    try face.setPixelSize(24.5, 0);
    try std.testing.expectEqual(@as(f32, 24.5), (try face.metrics()).pixel_size);
}

test "a pixel size that cannot mean anything is refused" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const nonsense = [_]struct { w: f32, h: f32 }{
        .{ .w = 0, .h = 0 },
        .{ .w = -12, .h = -12 },
        .{ .w = 0, .h = -1 },
        .{ .w = std.math.nan(f32), .h = std.math.nan(f32) },
        .{ .w = std.math.inf(f32), .h = std.math.inf(f32) },
        .{ .w = 0, .h = 16385 },
        // Below 1/64 px, which quantises to nothing at all. Refused rather
        // than accepted as a face that renders empty bitmaps.
        .{ .w = 0, .h = 0.001 },
    };
    for (nonsense) |case| {
        try std.testing.expectError(
            ztext.Error.InvalidArgument,
            face.setPixelSize(case.w, case.h),
        );
    }

    // None of them changed the size the face already had.
    try std.testing.expectEqual(@as(f32, ppem), (try face.metrics()).pixel_size);

    // The smallest size that does mean something is accepted.
    try face.setPixelSize(0, 1.0 / 64.0);
    try std.testing.expectEqual(@as(f32, 1.0 / 64.0), (try face.metrics()).pixel_size);
}

test "a face always has a size, so there is no unsized state to refuse" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.latin, 0);
    defer font.deinit();

    // The size is an argument to `face`, not a second call that might be
    // forgotten, so a face that exists can always be measured and rendered.
    // A size the face could not have is refused at creation instead.
    try std.testing.expectError(ztext.Error.InvalidArgument, font.face(0, 0));
    try std.testing.expectError(ztext.Error.InvalidArgument, font.face(0, -3));

    const face = try font.face(0, ppem);
    defer face.deinit();
    _ = try face.metrics();
    _ = try fixture.shaper.shape(face, "x", .{});
    _ = try face.renderGlyph(face.font.glyphIndex('x'), .a8, .normal, 0, 0);
}

test "a font and its faces may be destroyed in either order" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // Font first. The face keeps it alive, so everything below still works --
    // which is what makes the two handles free of an ordering rule.
    {
        const font = try fixture.library.createFont(fonts.latin, 0);
        const face = try font.face(0, ppem);
        font.deinit();

        _ = try face.metrics();
        try std.testing.expectEqualStrings("Noto Sans", face.font.familyName());
        _ = try fixture.shaper.shape(face, "abc", .{});

        // What a released font cannot do is hand out more faces.
        try std.testing.expectError(ztext.Error.InvalidArgument, font.face(0, ppem));

        face.deinit();
    }

    // Faces first, which is the order a reader would guess.
    {
        const font = try fixture.library.createFont(fonts.latin, 0);
        const a = try font.face(0, 16);
        const b = try font.face(0, 32);
        a.deinit();
        b.deinit();
        font.deinit();
    }
}

test "faces of one font share the parse but not the size" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.latin, 0);
    defer font.deinit();

    const small = try font.face(0, 16);
    defer small.deinit();
    const large = try font.face(0, 48);
    defer large.deinit();

    // Sharing an FT_Face must not mean sharing its scaled state: the two must
    // report their own sizes no matter which was touched last.
    try std.testing.expectEqual(@as(f32, 16), (try small.metrics()).pixel_size);
    try std.testing.expectEqual(@as(f32, 48), (try large.metrics()).pixel_size);
    try std.testing.expectEqual(@as(f32, 16), (try small.metrics()).pixel_size);

    // And the same for rendering, which goes through the shared glyph slot.
    const glyph = font.glyphIndex('H');
    const big = try large.renderGlyph(glyph, .a8, .none, 0, 0);
    const big_height = big.height;
    const wee = try small.renderGlyph(glyph, .a8, .none, 0, 0);
    try std.testing.expect(big_height > wee.height);

    // Interleaving must not let one face's size leak into the other's.
    try std.testing.expectEqual(big_height, (try large.renderGlyph(glyph, .a8, .none, 0, 0)).height);

    // Shaping too: advances come from each face's own HarfBuzz font.
    _ = try fixture.shaper.shape(small, "Hamburgefonstiv", .{ .direction = .ltr });
    const small_width = (try fixture.shaper.extents(small)).x_advance;
    _ = try fixture.shaper.shape(large, "Hamburgefonstiv", .{ .direction = .ltr });
    const large_width = (try fixture.shaper.extents(large)).x_advance;
    try std.testing.expect(large_width > small_width * 2.5);
}

test "a rendered bitmap survives anything but the next render on its own face" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.latin, 0);
    defer font.deinit();
    const face = try font.face(0, ppem);
    defer face.deinit();
    const sibling = try font.face(0, 12);
    defer sibling.deinit();
    const twin = try font.face(0, ppem);
    defer twin.deinit();

    // Two faces of one font hold two live bitmaps at once.
    //
    // Why that needs a copy is settled upstream rather than here: a font's
    // faces share one FT_GlyphSlot, ft_smooth_render FREES the slot's previous
    // buffer before allocating the next (src/smooth/ftsmooth.c:589-608), and
    // FT_Load_Glyph does the same by way of ft_glyphslot_clear
    // (src/base/ftobjs.c:926 -> :582). So a pointer into the slot -- which
    // a borrowed pointer would be -- is freed by the next load through ANY
    // face of the font.
    //
    // A test cannot observe that deterministically: reading freed memory
    // usually returns the old bytes. What these assertions pin is the contract
    // the copy provides -- two faces' bitmaps are independent, correct, and
    // undisturbed by everything that touches the shared slot -- so a
    // regression shows up as a wrong answer even where it would not show up as
    // a crash.
    const h = font.glyphIndex('H');
    const big = try face.renderGlyph(h, .a8, .normal, 0, 0);
    const big_rows = ztext.bitmapRows(big).?;
    const twin_bitmap = try twin.renderGlyph(h, .a8, .normal, 0, 0);
    const twin_rows = ztext.bitmapRows(twin_bitmap).?;
    try std.testing.expect(big_rows.ptr != twin_rows.ptr);
    try std.testing.expectEqualSlices(u8, big_rows, twin_rows);

    const wee = try sibling.renderGlyph(h, .a8, .normal, 0, 0);
    const wee_rows = ztext.bitmapRows(wee).?;
    try std.testing.expect(big_rows.ptr != wee_rows.ptr);
    try std.testing.expect(big.height > wee.height);
    try std.testing.expect(big_rows.len > wee_rows.len);

    var checksum: u64 = 0;
    for (big_rows) |px| checksum +%= px;
    try std.testing.expect(checksum != 0);

    // Everything that touches the shared slot, and none of it may disturb the
    // pixels either face is holding.
    _ = try sibling.renderGlyph(font.glyphIndex('W'), .a8, .normal, 0, 0);
    _ = try face.glyphExtents(font.glyphIndex('W'), .normal);
    _ = try fixture.shaper.shape(face, "unrelated", .{ .use_freetype_metrics = true });
    _ = try fixture.shaper.extents(face);

    var after: u64 = 0;
    for (big_rows) |px| after +%= px;
    try std.testing.expectEqual(checksum, after);

    // The face's own next render is the one thing that does replace it, and
    // the same glyph must come back the same way.
    const again = try face.renderGlyph(h, .a8, .normal, 0, 0);
    try std.testing.expectEqualSlices(u8, big_rows, ztext.bitmapRows(again).?);
}

test "a rasterised bitmap is tightly packed and top-down" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // Copying out of the slot normalises FreeType's pitch, so a consumer that
    // ignores the sign cannot render upside down. Asserted rather than
    // assumed, because the guarantee is now ztext's rather than FreeType's.
    for ("HWjgq") |ch| {
        const bitmap = try face.renderGlyph(face.font.glyphIndex(ch), .a8, .normal, 0, 0);
        try std.testing.expectEqual(@as(i32, @intCast(bitmap.width)), bitmap.pitch);
        try std.testing.expect(ztext.bitmapRows(bitmap) != null);
    }
}

//=============================================================================
// Segmentation
//
// UAX #14 and #29. The tests that matter here are the ones a UI would fail on:
// wrapping between words rather than inside them, and a caret that moves by
// grapheme rather than by character.
//=============================================================================

test "line breaks are offered between words, never inside one" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const text = "hello wonderful world";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    const breaks = paragraph.lineBreaks();
    try std.testing.expectEqual(text.len, breaks.len);

    // After each space, and at the end. Nowhere else -- a break inside
    // "wonderful" is what an ASCII-space approximation gets wrong the moment
    // the text is not English.
    for (breaks, 0..) |b, i| {
        const expected: ztext.Break = if (i == text.len - 1)
            .mandatory
        else if (text[i] == ' ')
            .allowed
        else
            .none;
        try std.testing.expectEqual(expected, b);
    }
}

test "a line break is offered where no space is" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // The case that makes UAX #14 worth vendoring rather than approximating.
    // A hyphen permits a break after it and a slash after it, with no space
    // anywhere -- and CJK breaks between almost every pair of ideographs.
    const cases = [_]struct { text: []const u8, at: usize }{
        .{ .text = "well-known", .at = 4 },
        .{ .text = "and/or", .at = 3 },
        .{ .text = "\u{4ECA}\u{65E5}\u{306F}", .at = 2 },
    };
    for (cases) |case| {
        const paragraph = try ztext.Paragraph.init(case.text, .{});
        defer paragraph.deinit();
        try std.testing.expectEqual(
            ztext.Break.allowed,
            paragraph.lineBreaks()[case.at],
        );
    }
}

test "a caret moves by grapheme, not by character" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // "e" + COMBINING ACUTE is one grapheme in two characters and three
    // bytes. A caret that steps by character lands between the letter and its
    // accent, which is the bug this API exists to prevent.
    const text = "ae\u{301}b";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    try std.testing.expectEqual(@as(usize, 1), paragraph.nextGrapheme(0));
    try std.testing.expectEqual(@as(usize, 4), paragraph.nextGrapheme(1));
    try std.testing.expectEqual(@as(usize, 5), paragraph.nextGrapheme(4));

    // And back the same way, so a caret returns along the path it came.
    try std.testing.expectEqual(@as(usize, 4), paragraph.previousGrapheme(5));
    try std.testing.expectEqual(@as(usize, 1), paragraph.previousGrapheme(4));
    try std.testing.expectEqual(@as(usize, 0), paragraph.previousGrapheme(1));

    // Walking off either end stays put rather than wrapping or going out of
    // range, which is what lets a UI hold the arrow key down.
    try std.testing.expectEqual(text.len, paragraph.nextGrapheme(text.len));
    try std.testing.expectEqual(@as(usize, 0), paragraph.previousGrapheme(0));

    // A full round trip visits every boundary and terminates.
    var at: usize = 0;
    var steps: usize = 0;
    while (at < text.len) : (steps += 1) {
        const next = paragraph.nextGrapheme(at);
        try std.testing.expect(next > at);
        at = next;
    }
    try std.testing.expectEqual(@as(usize, 3), steps);
}

test "an emoji joined with ZWJ is one grapheme" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // The case everyone's hand-rolled caret gets wrong. Two people joined by
    // U+200D are one grapheme of eleven bytes; backspace must delete all of
    // it, not half a family.
    const text = "\u{1F468}\u{200D}\u{1F469}";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    try std.testing.expectEqual(text.len, paragraph.nextGrapheme(0));
    try std.testing.expectEqual(@as(usize, 0), paragraph.previousGrapheme(text.len));
}

test "word boundaries are where a double-click would select" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const text = "one two";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    const words = paragraph.wordBreaks();
    try std.testing.expectEqual(text.len, words.len);
    // A boundary after "one", after the space, and at the end -- and never
    // MANDATORY, which only line breaking uses.
    try std.testing.expectEqual(ztext.Break.allowed, words[2]);
    try std.testing.expectEqual(ztext.Break.allowed, words[3]);
    try std.testing.expectEqual(ztext.Break.none, words[0]);
    for (words) |w| try std.testing.expect(w != .mandatory);
}

test "the documented wrap loop covers a paragraph exactly once" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // The whole point of the package, end to end: break opportunities from
    // ztext, the width decision from the host, reordering per line from
    // ztext, shaping with context from ztext.
    const text = "the quick brown fox jumps over the lazy dog";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();
    const breaks = paragraph.lineBreaks();

    const limit: f32 = 120;
    var start: usize = 0;
    var lines: usize = 0;
    var covered: usize = 0;

    while (start < text.len) {
        // The furthest allowed break that still fits, or the first one if
        // even that does not -- a host must always make progress.
        var chosen: usize = 0;
        var i: usize = start;
        while (i < text.len) : (i += 1) {
            if (breaks[i] == .none) continue;
            _ = try fixture.shaper.shapeRange(face, text, start, i + 1 - start, .{
                .direction = .ltr,
                .script = ztext.tag("Latn"),
            });
            const width = (try fixture.shaper.extents(face)).x_advance;
            if (width <= limit or chosen == 0) chosen = i + 1;
            if (width > limit) break;
        }
        try std.testing.expect(chosen > start);

        const line = try paragraph.line(start, chosen - start);
        defer line.deinit();
        for (line.shapingRuns()) |run| {
            _ = try fixture.shaper.shapeRun(face, paragraph, run, .{});
            covered += run.length;
        }

        start = chosen;
        lines += 1;
    }

    try std.testing.expect(lines >= 3);
    try std.testing.expectEqual(text.len, covered);
}

//=============================================================================
// Vertical direction
//
// ttb and btt are in the public enum, so they are promises. These say exactly
// how much of one: the plumbing reaches HarfBuzz and vertical advances come
// back, and nothing beyond that is claimed, because none of the committed
// fonts has real vertical metrics to claim it with.
//=============================================================================

test "vertical direction produces vertical advances" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // Horizontal first, as the control.
    _ = try fixture.shaper.shape(face, "Hi", .{ .direction = .ltr });
    for (fixture.shaper.glyphs()) |g| {
        try std.testing.expect(g.x_advance > 0);
        try std.testing.expectEqual(@as(f32, 0), g.y_advance);
    }

    // Top-to-bottom: the pen moves down instead, so x stops advancing and y
    // starts. Noto Sans has no vmtx, so these are the advances HarfBuzz
    // SYNTHESISES from the font's ascender and descender -- which is the
    // honest extent of what ztext can demonstrate with the fonts it ships.
    for ([_]ztext.Direction{ .ttb, .btt }) |direction| {
        _ = try fixture.shaper.shape(face, "Hi", .{ .direction = direction });
        // The shaper reports back the direction it was given, not a
        // normalisation of it: btt stays btt.
        try std.testing.expectEqual(direction, fixture.shaper.direction());
        try std.testing.expect(fixture.shaper.glyphs().len == 2);
        for (fixture.shaper.glyphs()) |g| {
            try std.testing.expectEqual(@as(f32, 0), g.x_advance);
            try std.testing.expect(g.y_advance != 0);
        }
    }
}

test "vertical face metrics are synthesised for a font with no vmtx" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // ZtextFaceMetrics now carries column-direction analogues of ascender,
    // descender, line_height and max_advance, plus has_vertical_metrics
    // saying whether they came from a real vhea/vmtx or were synthesised.
    //
    // None of the committed fonts has a vmtx, so this test can only exercise
    // the synthesised path -- it asserts the flag says so rather than
    // pretending the real path (a genuine CJK vertical font) is covered here.
    const metrics = try face.metrics();
    try std.testing.expect(metrics.ascender > 0);
    try std.testing.expect(metrics.line_height > 0);
    try std.testing.expectEqual(
        @as(usize, 14),
        @typeInfo(ztext.FaceMetrics).@"struct".fields.len,
    );

    try std.testing.expectEqual(@as(u32, 0), metrics.has_vertical_metrics);
    // Synthesised from ascender and descender, so the column-direction span
    // stays in the same ballpark as the horizontal one -- not identical, but
    // neither zero nor wildly different.
    try std.testing.expect(metrics.vert_ascender > 0);
    try std.testing.expect(metrics.vert_descender < 0);
    try std.testing.expect(metrics.vert_line_height > 0);
    try std.testing.expect(metrics.vert_max_advance > 0);
    try std.testing.expectApproxEqAbs(
        metrics.vert_ascender - metrics.vert_descender,
        metrics.vert_line_height,
        0.01,
    );
}

//=============================================================================
// Run context
//
// Shaping a run without the text around it is the mistake `coveredPrefix`
// invites, so these are the tests that make it impossible to ship.
//=============================================================================

test "shaping a run with context matches shaping the whole text" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.arabic);
    defer face.deinit();

    // A fully joining Arabic word, where every interior letter's glyph depends
    // on both of its neighbours. Latin would pass this test with the bug in.
    const word = "\u{645}\u{631}\u{62D}\u{628}\u{627}";
    const params: ztext.ShapeParams = .{
        .direction = .rtl,
        .script = ztext.tag("Arab"),
    };

    var whole: [16]u32 = undefined;
    _ = try fixture.shaper.shape(face, word, params);
    const whole_len = fixture.shaper.glyphs().len;
    for (fixture.shaper.glyphs(), 0..) |g, i| whole[i] = g.glyph_id;

    // EVERY split point on a character boundary, not just one. Each half
    // shaped with the whole word as context must reproduce exactly the glyphs
    // the unsplit word produced for those bytes.
    var checked: usize = 0;
    var split: usize = 2;
    while (split < word.len) : (split += 2) {
        var seen: [16]u32 = undefined;
        var count: usize = 0;

        // Visual order is right-to-left, so the LATER bytes are drawn first.
        for ([_][2]usize{ .{ split, word.len - split }, .{ 0, split } }) |part| {
            _ = try fixture.shaper.shapeRange(face, word, part[0], part[1], params);
            for (fixture.shaper.glyphs()) |g| {
                seen[count] = g.glyph_id;
                count += 1;
            }
        }

        try std.testing.expectEqualSlices(u32, whole[0..whole_len], seen[0..count]);
        checked += 1;
    }
    try std.testing.expect(checked >= 4);
}

test "shaping a run WITHOUT context is measurably different" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.arabic);
    defer face.deinit();

    // The negative half of the test above. If this ever stops differing, the
    // one above has stopped proving anything -- either HarfBuzz changed, or
    // the context is no longer reaching it.
    const word = "\u{645}\u{631}\u{62D}\u{628}\u{627}";
    const params: ztext.ShapeParams = .{
        .direction = .rtl,
        .script = ztext.tag("Arab"),
    };

    var with_context: [8]u32 = undefined;
    _ = try fixture.shaper.shapeRange(face, word, 0, 6, params);
    const n = fixture.shaper.glyphs().len;
    for (fixture.shaper.glyphs(), 0..) |g, i| with_context[i] = g.glyph_id;

    _ = try fixture.shaper.shape(face, word[0..6], params);
    var without: [8]u32 = undefined;
    for (fixture.shaper.glyphs(), 0..) |g, i| without[i] = g.glyph_id;

    // Same letters, same count, different glyphs: the letter adjacent to the
    // split takes a medial form when it can see what follows and a final one
    // when it cannot.
    try std.testing.expectEqual(n, fixture.shaper.glyphs().len);
    try std.testing.expect(!std.mem.eql(u32, with_context[0..n], without[0..n]));
}

test "a run range is refused when it is out of bounds or splits a character" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.hebrew);
    defer face.deinit();

    const text = "\u{5E9}\u{5DC}\u{5D5}\u{5DD}";
    const bad = [_][2]usize{
        .{ text.len + 1, 0 },
        .{ 0, text.len + 1 },
        .{ 2, text.len },
        .{ 1, 2 }, // starts inside a character
        .{ 0, 3 }, // ends inside a character
    };
    for (bad) |range| {
        try std.testing.expectError(
            ztext.Error.InvalidArgument,
            fixture.shaper.shapeRange(face, text, range[0], range[1], .{}),
        );
    }

    // The whole text, and each character boundary, are all fine.
    var offset: usize = 0;
    while (offset <= text.len) : (offset += 2) {
        _ = try fixture.shaper.shapeRange(face, text, offset, text.len - offset, .{});
    }
}

test "clusters from a run are offsets into the whole text" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // A run's clusters index the buffer the caller passed, not the run, so
    // they can be used against the same slice a ShapingRun's offsets are.
    const text = "Hello world";
    const run: ztext.ShapingRun =
        .{ .offset = 6, .length = 5, .script = ztext.tag("Latn"), .level = 0 };
    const glyphs =
        try fixture.shaper.shapeRange(face, text, run.offset, run.length, .{});

    try std.testing.expectEqual(@as(usize, 5), glyphs.len);
    for (glyphs) |g| {
        try std.testing.expect(g.cluster >= run.offset);
        try std.testing.expect(g.cluster < run.offset + run.length);
    }
    try std.testing.expectEqual(@as(u32, 6), glyphs[0].cluster);
}

//=============================================================================
// Coverage and fallback
//=============================================================================

test "covered prefix stops where the font does" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const latin = try fixture.library.createFont(fonts.latin, 0);
    defer latin.deinit();
    const hebrew = try fixture.library.createFont(fonts.hebrew, 0);
    defer hebrew.deinit();

    // Noto Sans has no Hebrew; Noto Sans Hebrew has no Latin letters. Between
    // them the boundary is unambiguous, which is what makes them a fixture for
    // this and not just a convenience.
    const text = "abc \u{5E9}\u{5DC}\u{5D5}\u{5DD}";
    try std.testing.expectEqual(@as(usize, 4), try latin.coveredPrefix(text));
    try std.testing.expectEqual(@as(usize, 0), try hebrew.coveredPrefix(text));
    try std.testing.expectEqual(@as(usize, 8), try hebrew.coveredPrefix(text[4..]));

    // A font that covers everything says so, and an empty string is 0 rather
    // than an error.
    try std.testing.expectEqual(text.len - 4, try hebrew.coveredPrefix(text[4..]));
    try std.testing.expectEqual(@as(usize, 3), try latin.coveredPrefix("abc"));
    try std.testing.expectEqual(@as(usize, 0), try latin.coveredPrefix(""));

    try std.testing.expectError(
        ztext.Error.InvalidText,
        latin.coveredPrefix(&[_]u8{ 0xC3, 0x28 }),
    );
}

test "the documented fallback loop covers text no single font can" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const chain = [_][]const u8{ fonts.latin, fonts.hebrew, fonts.arabic };
    var fontlist: [3]ztext.Font = undefined;
    for (chain, 0..) |bytes, i| fontlist[i] = try fixture.library.createFont(bytes, 0);
    defer for (fontlist) |f| f.deinit();

    // Three scripts, none of which any one font here covers.
    const text = "ab \u{5E9}\u{5DC} \u{645}\u{631} cd";

    var start: usize = 0;
    var pieces: usize = 0;
    while (start < text.len) {
        var advanced = false;
        for (fontlist) |f| {
            const covered = try f.coveredPrefix(text[start..]);
            if (covered == 0) continue;
            start += covered;
            pieces += 1;
            advanced = true;
            break;
        }
        // The loop terminating is the property that matters: a covered prefix
        // is either 0 (try the next font) or forward progress, never a
        // fraction of a character.
        try std.testing.expect(advanced);
    }
    try std.testing.expectEqual(text.len, start);
    try std.testing.expect(pieces >= 3);
}

test "a covered prefix never splits a base from its marks" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const latin = try fixture.library.createFont(fonts.latin, 0);
    defer latin.deinit();

    // U+0301 COMBINING ACUTE ACCENT after a letter Noto Sans has. Both are
    // covered, so the whole thing is -- four bytes, the accent taking two.
    try std.testing.expectEqual(
        @as(usize, 4),
        try latin.coveredPrefix("a\u{301}b"),
    );

    // U+05B0 HEBREW POINT SHEVA is a mark Noto Sans does NOT have. The letter
    // before it is covered, but stopping between them would send the base to
    // one font and the mark to another -- so the prefix stops before the base.
    try std.testing.expectEqual(
        @as(usize, 1),
        try latin.coveredPrefix("a" ++ "b\u{5B0}"),
    );

    // And a run that is nothing but an uncovered mark is 0, not 1.
    try std.testing.expectEqual(@as(usize, 0), try latin.coveredPrefix("\u{5B0}"));
}

test "format characters never break a run" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const arabic = try fixture.library.createFont(fonts.arabic, 0);
    defer arabic.deinit();

    // U+200D ZERO WIDTH JOINER between two Arabic letters. Fonts rarely map
    // it, HarfBuzz removes it while shaping, and breaking the run there would
    // split a joining form for nothing.
    const joined = "\u{645}\u{200D}\u{631}";
    try std.testing.expectEqual(joined.len, try arabic.coveredPrefix(joined));

    // Same for a bidi control and a soft hyphen, which are Format too.
    for ([_][]const u8{ "\u{644}\u{200F}\u{627}", "\u{644}\u{AD}\u{627}" }) |text| {
        try std.testing.expectEqual(text.len, try arabic.coveredPrefix(text));
    }
}

//=============================================================================
// Variable fonts
//
// The one thing worth proving here is that both halves of the pipeline move
// together. FreeType keeps variation coordinates on the FT_Face and HarfBuzz
// keeps its own on each hb_font_t, so it is entirely possible to set one and
// not the other -- and the result is not a crash or an error but text whose
// advances describe a Light while its outlines describe a Black. It looks
// almost right, which is what makes it expensive.
//
// The goldens below were measured against `fonts.variable` at `ppem`, not
// predicted. Both of its axes default to an END of their range rather than to
// the middle, so its style name is "Light".
//=============================================================================

/// The Hebrew word the variable-font tests shape, and the letter they
/// rasterise out of it. Shared so "the advance changed" and "the ink changed"
/// are demonstrably about the same text at the same size.
const variable_word = "\u{5E9}\u{5DC}\u{5D5}\u{5DD}";
const variable_params: ztext.ShapeParams = .{ .direction = .rtl, .script = ztext.tag("Hebr") };

test "a variable font reports the axes it declares" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.variable, 0);
    defer font.deinit();

    try std.testing.expectEqual(@as(u32, 2), font.axisCount());

    // In the font's own order, which is the order `variation` indexes too. A
    // binding that sorted these, or that looked them up by tag and handed back
    // an index, would make that a second thing to keep in step.
    const weight = try font.axis(0);
    try std.testing.expectEqual(ztext.tag("wght"), weight.tag);
    try std.testing.expectApproxEqAbs(@as(f32, 100), weight.min_value, tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 100), weight.default_value, tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 900), weight.max_value, tolerance);

    const width = try font.axis(1);
    try std.testing.expectEqual(ztext.tag("wdth"), width.tag);
    try std.testing.expectApproxEqAbs(@as(f32, 62.5), width.min_value, tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 100), width.default_value, tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 100), width.max_value, tolerance);

    // Both defaults sit at an end of their range, so this font's default
    // instance is Thin-to-Light and normal width -- worth pinning, because a
    // test that expects an axis to have room in both directions would pass
    // here for the wrong reason.
    try std.testing.expectEqualStrings("Light", font.styleName());
    try std.testing.expectApproxEqAbs(@as(f32, 100), try font.variation(0), tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 100), try font.variation(1), tolerance);

    // One past the end is an error rather than a zeroed struct, which would
    // read as a perfectly plausible axis pinned at 0.
    try std.testing.expectError(ztext.Error.InvalidArgument, font.axis(2));
    try std.testing.expectError(ztext.Error.InvalidArgument, font.variation(2));
}

test "a static font has no axes and refuses to pretend otherwise" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // The same family as `fonts.variable`, shipped as a single instance.
    const font = try fixture.library.createFont(fonts.hebrew, 0);
    defer font.deinit();

    try std.testing.expectEqual(@as(u32, 0), font.axisCount());
    try std.testing.expectError(ztext.Error.InvalidArgument, font.axis(0));
    try std.testing.expectError(ztext.Error.InvalidArgument, font.variation(0));
    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        font.setVariations(&.{.{ .tag = ztext.tag("wght"), .value = 700 }}),
    );

    // Including an empty request, which could have been a harmless no-op. A
    // host that reaches this call at all is holding the wrong font, and an
    // error here is cheaper than working out later why the text never changed.
    try std.testing.expectError(ztext.Error.InvalidArgument, font.setVariations(&.{}));
}

test "moving an axis moves shaping and rasterisation together" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.variable, 0);
    defer font.deinit();
    const face = try font.face(0, ppem);
    defer face.deinit();

    const shin = font.glyphIndex('\u{5E9}');
    try std.testing.expect(shin != 0);

    const freetype_params: ztext.ShapeParams = .{
        .direction = .rtl,
        .script = ztext.tag("Hebr"),
        .use_freetype_metrics = true,
    };

    // The lightest weight the font has.
    try font.setVariations(&.{.{ .tag = ztext.tag("wght"), .value = 100 }});

    var light_advance: f32 = 0;
    for (try fixture.shaper.shape(face, variable_word, variable_params)) |glyph| {
        light_advance += glyph.x_advance;
    }
    _ = try fixture.shaper.shape(face, variable_word, freetype_params);
    var light_freetype: f32 = 0;
    for (fixture.shaper.glyphs()) |glyph| light_freetype += glyph.x_advance;

    const light_bitmap = try face.renderGlyph(shin, .a8, .none, 0, 0);
    const light_rows = ztext.bitmapRows(light_bitmap) orelse return error.TestUnexpectedResult;
    var light_ink: u64 = 0;
    for (light_rows) |value| light_ink += value;
    const light_width = light_bitmap.width;

    // And the heaviest.
    try font.setVariations(&.{.{ .tag = ztext.tag("wght"), .value = 900 }});

    var black_advance: f32 = 0;
    for (try fixture.shaper.shape(face, variable_word, variable_params)) |glyph| {
        black_advance += glyph.x_advance;
    }
    _ = try fixture.shaper.shape(face, variable_word, freetype_params);
    var black_freetype: f32 = 0;
    for (fixture.shaper.glyphs()) |glyph| black_freetype += glyph.x_advance;

    const black_bitmap = try face.renderGlyph(shin, .a8, .none, 0, 0);
    const black_rows = ztext.bitmapRows(black_bitmap) orelse return error.TestUnexpectedResult;
    var black_ink: u64 = 0;
    for (black_rows) |value| black_ink += value;

    // (a) HarfBuzz's side. These advances come from HVAR, read by HarfBuzz's
    // own table reader out of the hb_font_t's coordinates -- so if
    // ztextFontSetVariations had set FreeType alone, both numbers would be the
    // Light one and this is the assertion that would fail.
    try std.testing.expectApproxEqAbs(@as(f32, 66.65625), light_advance, tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 78.984375), black_advance, tolerance);

    // (b) FreeType's side, independently: heavier strokes put more coverage on
    // the page and widen the bitmap. Had HarfBuzz been set alone, these two
    // would be equal instead.
    try std.testing.expectEqual(@as(u32, 18), light_width);
    try std.testing.expectEqual(@as(u32, 24), black_bitmap.width);
    try std.testing.expectEqual(@as(u64, 15027), light_ink);
    try std.testing.expectEqual(@as(u64, 83215), black_ink);

    // (c) The two sources agreeing at each setting, which is what neither (a)
    // nor (b) can see on its own: each is measured entirely within one
    // upstream, so a build where the two describe DIFFERENT instances passes
    // both. Same tolerance and same reasoning as "FreeType and HarfBuzz
    // metrics agree closely but not exactly" -- hinted advances round per
    // glyph, unhinted ones do not, so the gap is small rather than zero. A gap
    // of several pixels here would mean one side never moved.
    try std.testing.expect(@abs(light_freetype - light_advance) < 1.0);
    try std.testing.expect(@abs(black_freetype - black_advance) < 1.0);

    // Weight is the axis being moved, so the two instances must not be within
    // a rounding error of each other -- otherwise (c) would hold trivially.
    try std.testing.expect(black_advance - light_advance > 10.0);
}

test "setting one axis leaves every other axis where it was" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.variable, 0);
    defer font.deinit();

    // Width first, all the way to its condensed end, then weight. A weight
    // slider and a width slider are two controls in a host's UI, and moving
    // one must not snap the other back to its default.
    try font.setVariations(&.{.{ .tag = ztext.tag("wdth"), .value = 62.5 }});
    try font.setVariations(&.{.{ .tag = ztext.tag("wght"), .value = 700 }});

    try std.testing.expectApproxEqAbs(@as(f32, 700), try font.variation(0), tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 62.5), try font.variation(1), tolerance);

    // Both in one call is the other half of the same promise: neither value
    // is derived from the other, and the order within the call does not
    // matter.
    try font.setVariations(&.{
        .{ .tag = ztext.tag("wdth"), .value = 80 },
        .{ .tag = ztext.tag("wght"), .value = 300 },
    });
    try std.testing.expectApproxEqAbs(@as(f32, 300), try font.variation(0), tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 80), try font.variation(1), tolerance);
}

test "a refused variation leaves the font exactly as it was" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.variable, 0);
    defer font.deinit();

    try font.setVariations(&.{.{ .tag = ztext.tag("wght"), .value = 500 }});

    // Above the maximum, below the minimum, and two values that are not
    // numbers. FreeType clamps every one of these; ztext refuses, so a caller
    // who asks for a weight this font does not have finds out rather than
    // quietly receiving 900 and wondering why the slider stopped responding.
    for ([_]f32{ 901, 99, std.math.inf(f32), -std.math.inf(f32), std.math.nan(f32) }) |bad| {
        try std.testing.expectError(
            ztext.Error.InvalidArgument,
            font.setVariations(&.{.{ .tag = ztext.tag("wght"), .value = bad }}),
        );
        try std.testing.expectApproxEqAbs(@as(f32, 500), try font.variation(0), tolerance);
    }

    // A tag this font has no axis for is refused rather than ignored: a typo
    // inside a four-character constant is otherwise invisible, and "the
    // slider does nothing" is a much worse symptom than an error.
    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        font.setVariations(&.{.{ .tag = ztext.tag("slnt"), .value = 0 }}),
    );
    try std.testing.expectApproxEqAbs(@as(f32, 500), try font.variation(0), tolerance);

    // And the whole request is validated before any of it lands, so a good
    // axis sharing a call with a bad one moves neither. Without that, a host
    // retrying the corrected call would be starting from a half-applied font.
    try std.testing.expectError(ztext.Error.InvalidArgument, font.setVariations(&.{
        .{ .tag = ztext.tag("wdth"), .value = 80 },
        .{ .tag = ztext.tag("wght"), .value = 2000 },
    }));
    try std.testing.expectApproxEqAbs(@as(f32, 500), try font.variation(0), tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 100), try font.variation(1), tolerance);
}

test "a face created after the axes moved inherits them" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.variable, 0);
    defer font.deinit();

    const before = try font.face(0, ppem);
    defer before.deinit();

    try font.setVariations(&.{.{ .tag = ztext.tag("wght"), .value = 900 }});

    // HarfBuzz's coordinates live on the hb_font_t, one per face, so a face
    // built after the change has to be seeded from the font rather than from
    // HarfBuzz's defaults. Nothing about this face went through
    // ztextFontSetVariations, which is the case that is easy to miss.
    const after = try font.face(0, ppem);
    defer after.deinit();

    var before_advance: f32 = 0;
    for (try fixture.shaper.shape(before, variable_word, variable_params)) |glyph| {
        before_advance += glyph.x_advance;
    }
    var after_advance: f32 = 0;
    for (try fixture.shaper.shape(after, variable_word, variable_params)) |glyph| {
        after_advance += glyph.x_advance;
    }

    try std.testing.expectApproxEqAbs(before_advance, after_advance, tolerance);
    // Against the golden rather than only against each other: two faces that
    // both missed the change would agree just as well.
    try std.testing.expectApproxEqAbs(@as(f32, 78.984375), after_advance, tolerance);
}

test "moving an axis invalidates a run already measured against a face" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.variable, 0);
    defer font.deinit();
    const face = try font.face(0, ppem);
    defer face.deinit();

    _ = try fixture.shaper.shape(face, variable_word, variable_params);
    _ = try fixture.shaper.extents(face);

    // HVAR moves the advances and MVAR can move the ascender, so the run just
    // shaped and the ink bounds `extents` would now measure belong to two
    // different instances. The face's generation is bumped for exactly this,
    // and a refusal is the only honest answer.
    try font.setVariations(&.{.{ .tag = ztext.tag("wght"), .value = 900 }});
    try std.testing.expectError(ztext.Error.InvalidArgument, fixture.shaper.extents(face));

    // Re-shaping against the new instance makes it answerable again, at the
    // new advance rather than the old one.
    _ = try fixture.shaper.shape(face, variable_word, variable_params);
    const extents = try fixture.shaper.extents(face);
    try std.testing.expectApproxEqAbs(@as(f32, 78.984375), extents.x_advance, tolerance);
}

//=============================================================================
// Golden shaping
//=============================================================================

test "golden: Latin applies standard ligatures, and turning them off undoes it" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const latin: ztext.ShapeParams = .{ .direction = .ltr, .script = ztext.tag("Latn") };

    // "Waffle" -> W, a, ffl-ligature, e. Four glyphs for six characters is the
    // whole point: an advance table cannot produce this.
    _ = try fixture.shaper.shape(face, "Waffle", latin);
    try expectGlyphIds(fixture.shaper.glyphs(), &.{ 58, 68, 1657, 72 });
    try expectAdvances(fixture.shaper.glyphs(), &.{ 29.125, 17.953125, 30.265625, 18.046875 });

    // The same string with liga off is six glyphs again, and the two `f`s are
    // the same glyph id twice.
    const no_liga = [_]ztext.Feature{
        .{ .tag = ztext.tag("liga"), .value = 0, .start = 0, .end = ztext.feature_global },
    };
    _ = try fixture.shaper.shape(face, "Waffle", .{
        .direction = .ltr,
        .script = ztext.tag("Latn"),
        .features = &no_liga,
    });
    try expectGlyphIds(fixture.shaper.glyphs(), &.{ 58, 68, 73, 73, 79, 72 });

    // "fi" is the simplest case, and pins the ligature glyph itself.
    _ = try fixture.shaper.shape(face, "fi", latin);
    try expectGlyphIds(fixture.shaper.glyphs(), &.{1654});
    try expectAdvances(fixture.shaper.glyphs(), &.{19.265625});
}

test "golden: Latin kerning moves glyphs, and turning it off restores them" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // "AV" is the textbook kerning pair. The glyph ids must not change -- only
    // the advance -- which is what separates kerning from substitution.
    _ = try fixture.shaper.shape(face, "AV", .{ .direction = .ltr, .script = ztext.tag("Latn") });
    try expectGlyphIds(fixture.shaper.glyphs(), &.{ 36, 57 });
    try expectAdvances(fixture.shaper.glyphs(), &.{ 19.171875, 19.203125 });

    const no_kern = [_]ztext.Feature{
        .{ .tag = ztext.tag("kern"), .value = 0, .start = 0, .end = ztext.feature_global },
    };
    _ = try fixture.shaper.shape(face, "AV", .{
        .direction = .ltr,
        .script = ztext.tag("Latn"),
        .features = &no_kern,
    });
    try expectGlyphIds(fixture.shaper.glyphs(), &.{ 36, 57 });
    try expectAdvances(fixture.shaper.glyphs(), &.{ 20.453125, 19.203125 });
}

test "golden: Arabic joins, and the joined forms are not the nominal glyphs" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.arabic);
    defer face.deinit();

    const word = "مرحبا"; // MEEM REH HAH BEH ALEF -- five letters, ten bytes.
    try std.testing.expectEqual(@as(usize, 10), word.len);

    _ = try fixture.shaper.shape(face, word, .{ .direction = .rtl, .script = ztext.tag("Arab") });
    const glyphs = fixture.shaper.glyphs();

    // Six glyphs from five characters: the extra one is a mark, positioned by
    // GPOS with a zero advance.
    try expectGlyphIds(glyphs, &.{ 9, 323, 16, 25, 29, 77 });
    try expectAdvances(glyphs, &.{ 8.09375, 0.0, 9.34375, 20.359375, 12.921875, 14.59375 });
    try std.testing.expectApproxEqAbs(@as(f32, 2.875), glyphs[1].x_offset, tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, -0.984375), glyphs[1].y_offset, tolerance);

    // The behavioural claim, independent of the golden ids: shaping produced
    // contextual forms, NOT the nominal glyphs the character map alone gives.
    // This is the assertion that would have caught the allocator bug that made
    // HarfBuzz silently fall back to nominal glyphs.
    var nominal: [8]u32 = undefined;
    var count: usize = 0;
    var it = std.unicode.Utf8View.initUnchecked(word).iterator();
    while (it.nextCodepoint()) |codepoint| : (count += 1) {
        nominal[count] = face.font.glyphIndex(codepoint);
        try std.testing.expect(nominal[count] != 0);
    }

    var differs: usize = 0;
    for (glyphs) |glyph| {
        var found = false;
        for (nominal[0..count]) |n| {
            if (glyph.glyph_id == n) found = true;
        }
        if (!found) differs += 1;
    }
    try std.testing.expect(differs >= 4);
}

test "golden: shaping reports where a line may be broken, and where it may be elongated" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const arabic = try fixture.face(fonts.arabic);
    defer arabic.deinit();
    const latin = try fixture.face(fonts.latin);
    defer latin.deinit();

    // MEEM REH HAH BEH ALEF, the same word as the joining golden above, in
    // visual order: cluster 8 first.
    _ = try fixture.shaper.shape(arabic, "\u{645}\u{631}\u{62d}\u{628}\u{627}", .{
        .direction = .rtl,
        .script = ztext.tag("Arab"),
    });

    // Measured, not predicted -- what this vendored HarfBuzz produces for
    // these exact font bytes. Five of six glyphs are unsafe to break because
    // each letter's form depends on its neighbours; the two that are only
    // unsafe to CONCAT are the ends of the word, where a change further out
    // could still reach in.
    const arabic_flags = [_]u32{ 0x7, 0x7, 0x7, 0x2, 0x7, 0x2 };
    try std.testing.expectEqual(arabic_flags.len, fixture.shaper.glyphs().len);
    for (fixture.shaper.glyphs(), arabic_flags, 0..) |glyph, want, i| {
        std.testing.expectEqual(want, glyph.flags) catch |e| {
            std.debug.print("arabic glyph {d}: flags 0x{x}\n", .{ i, glyph.flags });
            return e;
        };
    }

    // The claims that survive a re-vendor changing the numbers above.
    //
    // First: every bit set is a bit ztext names. A HarfBuzz that grew a
    // fourth flag would set it here, and a consumer switching on the mask
    // would see a value it has no meaning for.
    for (fixture.shaper.glyphs()) |glyph| {
        try std.testing.expectEqual(@as(u32, 0), glyph.flags & ~@as(u32, 0x7));
    }

    // Second, and this is what the buffer flags in ztext_shape.c buy: the two
    // OPTIONAL flags are actually produced. HarfBuzz emits unsafe-to-break
    // whether or not it is asked; the other two it withholds unless told,
    // and a consumer cannot tell a withheld flag from an absent one.
    var saw_unsafe_to_break = false;
    var saw_unsafe_to_concat = false;
    var saw_tatweel = false;
    for (fixture.shaper.glyphs()) |glyph| {
        if (ztext.glyphHas(glyph, .unsafe_to_break)) saw_unsafe_to_break = true;
        if (ztext.glyphHas(glyph, .unsafe_to_concat)) saw_unsafe_to_concat = true;
        if (ztext.glyphHas(glyph, .safe_to_insert_tatweel)) saw_tatweel = true;
    }
    try std.testing.expect(saw_unsafe_to_break);
    try std.testing.expect(saw_unsafe_to_concat);
    try std.testing.expect(saw_tatweel);

    // Third, the negative half, which is the half a line-breaker acts on.
    // Latin with ligatures and kerning is safe to break at every cluster --
    // so a paragraph of it needs no re-shaping after line breaking at all --
    // and nowhere in it may a tatweel be inserted, elongation being a
    // property of the script rather than of the font.
    _ = try fixture.shaper.shape(latin, "office fluff", .{
        .direction = .ltr,
        .script = ztext.tag("Latn"),
    });
    for (fixture.shaper.glyphs()) |glyph| {
        try std.testing.expect(!ztext.glyphHas(glyph, .unsafe_to_break));
        try std.testing.expect(!ztext.glyphHas(glyph, .safe_to_insert_tatweel));
    }

    // And the flags belong to the CURRENT run: a second shape must not leave
    // the first one's answers behind for the glyphs it happens to reuse.
    _ = try fixture.shaper.shape(latin, "a", .{ .direction = .ltr, .script = ztext.tag("Latn") });
    try std.testing.expectEqual(@as(usize, 1), fixture.shaper.glyphs().len);
    try std.testing.expect(!ztext.glyphHas(fixture.shaper.glyphs()[0], .unsafe_to_break));
}

test "golden: Hebrew is right-to-left without joining" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.hebrew);
    defer face.deinit();

    // SHIN LAMED VAV MEM-FINAL: four letters, four glyphs, no substitution --
    // an RTL script that does NOT join, so a direction bug cannot hide behind
    // a joining bug.
    _ = try fixture.shaper.shape(face, "שלום", .{ .direction = .rtl, .script = ztext.tag("Hebr") });
    try expectGlyphIds(fixture.shaper.glyphs(), &.{ 23, 124, 55, 96 });
    try expectAdvances(fixture.shaper.glyphs(), &.{ 21.890625, 9.3125, 16.703125, 23.359375 });
    try std.testing.expectEqual(ztext.Direction.rtl, fixture.shaper.direction());
}

test "HarfBuzz mirrors paired brackets in an RTL run, so ztext need not" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    // Noto Sans Hebrew has no brackets at all -- both map to .notdef -- so
    // this uses the Latin face. Mirroring is a property of the run's
    // direction, not of its script.
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // Rule L4 of UAX #9: a paired bracket in a right-to-left run is rendered
    // with its MIRROR glyph, so "(" opens on the right. HarfBuzz applies this
    // itself, at shaping time, before OpenType features run
    // (`hb_ot_rotate_chars` in hb-ot-shape.cc). ztext therefore exposes no
    // mirroring API and does no mirroring of its own.
    //
    // That is a claim about a vendored upstream, so it is pinned here rather
    // than asserted in the README alone: if a re-vendor ever moved the
    // behaviour, the visible symptom would be backwards brackets in every RTL
    // line, and nothing else in the suite would notice.
    _ = try fixture.shaper.shape(face, "(", .{ .direction = .ltr });
    const open_ltr = fixture.shaper.glyphs()[0].glyph_id;

    _ = try fixture.shaper.shape(face, ")", .{ .direction = .ltr });
    const close_ltr = fixture.shaper.glyphs()[0].glyph_id;

    // Without this the test would pass on a font that mapped both to .notdef.
    try std.testing.expect(open_ltr != close_ltr);
    try std.testing.expect(open_ltr != 0 and close_ltr != 0);

    _ = try fixture.shaper.shape(face, "(", .{ .direction = .rtl });
    try std.testing.expectEqual(close_ltr, fixture.shaper.glyphs()[0].glyph_id);

    _ = try fixture.shaper.shape(face, ")", .{ .direction = .rtl });
    try std.testing.expectEqual(open_ltr, fixture.shaper.glyphs()[0].glyph_id);
}

test "shaped extents enclose the ink and report the pen movement" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.arabic);
    defer face.deinit();

    _ = try fixture.shaper.shape(face, "مرحبا", .{ .direction = .rtl, .script = ztext.tag("Arab") });
    const extents = try fixture.shaper.extents(face);

    var advance: f32 = 0;
    for (fixture.shaper.glyphs()) |glyph| advance += glyph.x_advance;
    try std.testing.expectApproxEqAbs(advance, extents.x_advance, tolerance);
    try std.testing.expectApproxEqAbs(@as(f32, 65.3125), extents.x_advance, tolerance);

    try std.testing.expect(extents.x_max > extents.x_min);
    try std.testing.expect(extents.y_max > extents.y_min);
    // The mark sits below the baseline, so the ink must too.
    try std.testing.expect(extents.y_min < 0);
}

//=============================================================================
// Clusters
//=============================================================================

test "clusters are byte offsets that stay inside the text and stay monotone" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const cases = [_]struct {
        bytes: []const u8,
        text: []const u8,
        direction: ztext.Direction,
        script: *const [4:0]u8,
    }{
        .{ .bytes = fonts.latin, .text = "Waffle wins", .direction = .ltr, .script = "Latn" },
        .{ .bytes = fonts.arabic, .text = "مرحبا", .direction = .rtl, .script = "Arab" },
        .{ .bytes = fonts.hebrew, .text = "שלום", .direction = .rtl, .script = "Hebr" },
    };

    for (cases) |case| {
        const face = try fixture.face(case.bytes);
        defer face.deinit();
        _ = try fixture.shaper.shape(face, case.text, .{
            .direction = case.direction,
            .script = ztext.tag(case.script),
        });

        const glyphs = fixture.shaper.glyphs();
        try std.testing.expect(glyphs.len > 0);

        for (glyphs) |glyph| {
            // In range, and never pointing at a UTF-8 continuation byte --
            // a cluster must be the start of a character.
            try std.testing.expect(glyph.cluster < case.text.len);
            try std.testing.expect(case.text[glyph.cluster] & 0xC0 != 0x80);
        }

        // Monotone: ascending for LTR output, descending for RTL, because the
        // glyph array is in visual order.
        var previous = glyphs[0].cluster;
        for (glyphs[1..]) |glyph| {
            if (case.direction == .rtl) {
                try std.testing.expect(glyph.cluster <= previous);
            } else {
                try std.testing.expect(glyph.cluster >= previous);
            }
            previous = glyph.cluster;
        }

        // Every character is covered by some cluster: no input silently
        // disappears between the text and the glyphs.
        const first = if (case.direction == .rtl) glyphs[glyphs.len - 1] else glyphs[0];
        try std.testing.expectEqual(@as(u32, 0), first.cluster);
    }
}

test "a ligature merges clusters and the mapping still round-trips" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // Six characters, four glyphs. The ffl ligature must carry the cluster of
    // the FIRST character it swallowed, so a caret placed at byte 2, 3 or 4
    // still lands on it.
    _ = try fixture.shaper.shape(face, "Waffle", .{ .direction = .ltr, .script = ztext.tag("Latn") });
    const glyphs = fixture.shaper.glyphs();
    try std.testing.expectEqual(@as(usize, 4), glyphs.len);
    try std.testing.expectEqual(@as(u32, 0), glyphs[0].cluster); // W
    try std.testing.expectEqual(@as(u32, 1), glyphs[1].cluster); // a
    try std.testing.expectEqual(@as(u32, 2), glyphs[2].cluster); // ffl
    try std.testing.expectEqual(@as(u32, 5), glyphs[3].cluster); // e

    // Multi-byte characters must produce byte offsets, not character indices.
    _ = try fixture.shaper.shape(face, "aéb", .{ .direction = .ltr, .script = ztext.tag("Latn") });
    const accented = fixture.shaper.glyphs();
    try std.testing.expectEqual(@as(usize, 3), accented.len);
    try std.testing.expectEqual(@as(u32, 0), accented[0].cluster);
    try std.testing.expectEqual(@as(u32, 1), accented[1].cluster);
    try std.testing.expectEqual(@as(u32, 3), accented[2].cluster); // not 2
}

//=============================================================================
// Bidi and itemisation
//=============================================================================

test "bidi orders a mixed paragraph into visual runs that tile the text" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const text = "Hello مرحبا world";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    // First strong character is Latin, so the base is left-to-right.
    try std.testing.expectEqual(@as(u8, 0), paragraph.baseLevel());
    try std.testing.expectEqual(ztext.Direction.ltr, paragraph.baseDirection());
    try std.testing.expectEqual(text.len, paragraph.length());
    try std.testing.expectEqual(text.len, paragraph.levels().len);

    const runs = paragraph.visualRuns();
    try std.testing.expectEqual(@as(usize, 3), runs.len);
    try std.testing.expectEqualStrings("Hello ", text[runs[0].offset..][0..runs[0].length]);
    try std.testing.expectEqualStrings("مرحبا", text[runs[1].offset..][0..runs[1].length]);
    try std.testing.expectEqualStrings(" world", text[runs[2].offset..][0..runs[2].length]);

    try std.testing.expectEqual(ztext.Direction.ltr, ztext.runDirection(runs[0].level));
    try std.testing.expectEqual(ztext.Direction.rtl, ztext.runDirection(runs[1].level));
    try std.testing.expectEqual(ztext.Direction.ltr, ztext.runDirection(runs[2].level));

    // The runs must cover every byte exactly once, or a layout engine built on
    // them would drop or double-draw text.
    var covered = std.mem.zeroes([64]bool);
    for (runs) |run| {
        for (run.offset..run.offset + run.length) |i| {
            try std.testing.expect(!covered[i]);
            covered[i] = true;
        }
    }
    for (covered[0..text.len]) |byte| try std.testing.expect(byte);
}

test "bidi resolves an RTL base and reverses the run order" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // First strong character is Arabic, so the base is right-to-left and the
    // Latin word becomes the embedded run.
    const text = "مرحبا Hello";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    try std.testing.expectEqual(@as(u8, 1), paragraph.baseLevel());
    try std.testing.expectEqual(ztext.Direction.rtl, paragraph.baseDirection());

    const runs = paragraph.visualRuns();
    try std.testing.expectEqual(@as(usize, 2), runs.len);
    // Visual order with an RTL base: the Latin run is drawn leftmost, so it
    // comes first even though it is last in the text.
    try std.testing.expectEqualStrings("Hello", text[runs[0].offset..][0..runs[0].length]);
    try std.testing.expectEqual(ztext.Direction.ltr, ztext.runDirection(runs[0].level));
    try std.testing.expectEqual(ztext.Direction.rtl, ztext.runDirection(runs[1].level));

    // Forcing the base the other way must change the answer.
    const forced = try ztext.Paragraph.init(text, .{ .base = .ltr });
    defer forced.deinit();
    try std.testing.expectEqual(@as(u8, 0), forced.baseLevel());
}

test "script itemisation splits a mixed paragraph into shapeable runs" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const text = "Hello مرحبا";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    const runs = paragraph.scriptRuns();
    try std.testing.expectEqual(@as(usize, 2), runs.len);
    try std.testing.expectEqual(ztext.tag("Latn"), runs[0].script);
    try std.testing.expectEqual(ztext.tag("Arab"), runs[1].script);

    // Logical order, contiguous, covering the whole paragraph.
    try std.testing.expectEqual(@as(u32, 0), runs[0].offset);
    try std.testing.expectEqual(runs[0].offset + runs[0].length, runs[1].offset);
    try std.testing.expectEqual(text.len, runs[1].offset + runs[1].length);
}

test "a paragraph stops at the first separator, and says how far it got" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // The bidi algorithm is defined per paragraph, so a buffer holding two of
    // them describes two. ztext analyses the first and reports the length it
    // covered -- which INCLUDES the separator, matching SheenBidi. A caller
    // that ignores `length()` and indexes the whole buffer with these runs
    // would silently drop the second paragraph.
    const text = "abc\ndef";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    try std.testing.expectEqual(@as(usize, 4), paragraph.length());
    try std.testing.expectEqual(@as(usize, 4), paragraph.levels().len);

    var covered: usize = 0;
    for (paragraph.visualRuns()) |run| covered += run.length;
    try std.testing.expectEqual(paragraph.length(), covered);

    // Both CR and LF are separators, but CRLF counts as one boundary.
    const crlf = try ztext.Paragraph.init("abc\r\ndef", .{});
    defer crlf.deinit();
    try std.testing.expectEqual(@as(usize, 5), crlf.length());

    // Text with no separator is covered whole.
    const single = try ztext.Paragraph.init("abc def", .{});
    defer single.deinit();
    try std.testing.expectEqual(@as(usize, 7), single.length());
}

test "an empty paragraph is analysed rather than refused" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const paragraph = try ztext.Paragraph.init("", .{});
    defer paragraph.deinit();
    try std.testing.expectEqual(@as(usize, 0), paragraph.length());
    try std.testing.expectEqual(@as(usize, 0), paragraph.visualRuns().len);
    try std.testing.expectEqual(@as(u8, 0), paragraph.baseLevel());

    const rtl = try ztext.Paragraph.init("", .{ .base = .rtl });
    defer rtl.deinit();
    try std.testing.expectEqual(@as(u8, 1), rtl.baseLevel());
}

test "the paragraph, itemiser and shaper compose into a whole line" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // The pipeline a host is meant to build: order, itemise, shape each piece.
    const text = "Hi שלום";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    const latin_face = try fixture.face(fonts.latin);
    defer latin_face.deinit();
    const hebrew_face = try fixture.face(fonts.hebrew);
    defer hebrew_face.deinit();

    var total_glyphs: usize = 0;
    for (paragraph.visualRuns()) |run| {
        const slice = text[run.offset..][0..run.length];
        const direction = ztext.runDirection(run.level);
        const face = if (direction == .rtl) hebrew_face else latin_face;
        _ = try fixture.shaper.shape(face, slice, .{ .direction = direction });
        total_glyphs += fixture.shaper.glyphs().len;
    }
    try std.testing.expect(total_glyphs >= 6);
}

//=============================================================================
// Lines
//
// UAX #9 resolves embedding levels over a paragraph but applies rules L1 and
// L2 over a LINE, so where the text wraps changes the answer. These are the
// tests that a paragraph's own run list is not a substitute.
//=============================================================================

/// "abc " + shalom + three spaces + shalom.
///
/// Chosen because the three spaces sit between two right-to-left words, which
/// makes them right-to-left over the paragraph, and a line that ends just
/// after them makes them left-to-right by rule L1. That single difference is
/// the whole reason ZtextLine exists, and it is visible as an indent on the
/// wrong side.
const wrapped = "abc \u{5E9}\u{5DC}\u{5D5}\u{5DD}   \u{5E9}\u{5DC}\u{5D5}\u{5DD}";
/// Byte offset just past the three spaces: "abc " is 4, each shalom is 8.
const first_line_end = 4 + 8 + 3;

/// Generic over VisualRun and ShapingRun, which differ in what else they
/// carry but agree on `offset` and `length` -- and tiling is a property of
/// those two alone.
fn expectTiles(runs: anytype, from: usize, to: usize) !void {
    var covered = std.mem.zeroes([64]bool);
    for (runs) |run| {
        try std.testing.expect(run.offset >= from);
        try std.testing.expect(run.offset + run.length <= to);
        for (run.offset..run.offset + run.length) |i| {
            try std.testing.expect(!covered[i]);
            covered[i] = true;
        }
    }
    for (covered[from..to]) |byte| try std.testing.expect(byte);
}

test "a line applies rule L1, which the paragraph's own runs cannot" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const paragraph = try ztext.Paragraph.init(wrapped, .{});
    defer paragraph.deinit();
    try std.testing.expectEqual(@as(u8, 0), paragraph.baseLevel());

    // Over the paragraph the three spaces are enclosed by right-to-left text
    // on both sides, so they resolve right-to-left and land inside the single
    // RTL run.
    const para_runs = paragraph.visualRuns();
    try std.testing.expectEqual(@as(usize, 2), para_runs.len);
    try std.testing.expectEqualStrings("abc ", wrapped[para_runs[0].offset..][0..para_runs[0].length]);
    try std.testing.expectEqual(ztext.Direction.ltr, ztext.runDirection(para_runs[0].level));
    try std.testing.expectEqual(ztext.Direction.rtl, ztext.runDirection(para_runs[1].level));
    // The spaces are inside the RTL run, at level 1.
    try std.testing.expectEqual(@as(u8, 1), paragraph.levels()[first_line_end - 1]);

    // Break the line just after the spaces and rule L1 resets them to the
    // paragraph level, which moves them to the other end of the line.
    const line = try paragraph.line(0, first_line_end);
    defer line.deinit();

    const line_runs = line.visualRuns();
    try std.testing.expectEqual(@as(usize, 3), line_runs.len);
    try std.testing.expectEqualStrings("abc ", wrapped[line_runs[0].offset..][0..line_runs[0].length]);
    try std.testing.expectEqual(ztext.Direction.ltr, ztext.runDirection(line_runs[0].level));
    try std.testing.expectEqual(ztext.Direction.rtl, ztext.runDirection(line_runs[1].level));
    try std.testing.expectEqualStrings("   ", wrapped[line_runs[2].offset..][0..line_runs[2].length]);
    try std.testing.expectEqual(ztext.Direction.ltr, ztext.runDirection(line_runs[2].level));

    // This is the assertion that would fail if lines were derived from the
    // paragraph's runs: the spaces are drawn last, to the right of the Hebrew
    // word, not between "abc " and it.
    try std.testing.expect(line_runs[2].offset > line_runs[1].offset);

    try expectTiles(line_runs, 0, first_line_end);
}

test "a line covers exactly its own range, and offsets stay paragraph-relative" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const paragraph = try ztext.Paragraph.init(wrapped, .{});
    defer paragraph.deinit();

    const second = try paragraph.line(first_line_end, wrapped.len - first_line_end);
    defer second.deinit();

    try std.testing.expectEqual(first_line_end, second.offset());
    try std.testing.expectEqual(wrapped.len - first_line_end, second.length());
    try expectTiles(second.visualRuns(), first_line_end, wrapped.len);

    // Every run indexes the caller's original slice, not a line-local copy.
    for (second.visualRuns()) |run| {
        try std.testing.expect(run.offset >= first_line_end);
    }
}

test "a line spanning the whole paragraph agrees with the paragraph" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // The paragraph's runs are not a different algorithm, they are this same
    // one asked for the whole range -- so the two must agree exactly.
    for ([_][]const u8{ wrapped, "Hello مرحبا world", "\u{5E9}\u{5DC}\u{5D5}\u{5DD} abc" }) |text| {
        const paragraph = try ztext.Paragraph.init(text, .{});
        defer paragraph.deinit();

        const line = try paragraph.line(0, paragraph.length());
        defer line.deinit();

        try std.testing.expectEqualSlices(
            ztext.VisualRun,
            paragraph.visualRuns(),
            line.visualRuns(),
        );
        try std.testing.expectEqualSlices(
            ztext.ShapingRun,
            paragraph.shapingRuns(),
            line.shapingRuns(),
        );
    }
}

test "a line's shaping runs are split by script as well as direction" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // One right-to-left visual run spanning two scripts: Hebrew then Arabic.
    // A shaper handed that as one run would get it wrong, which is what the
    // intersection is for -- and a line has to do it too, not just a paragraph.
    const text = "x \u{5E9}\u{5DC} \u{645}\u{631} y";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    const line = try paragraph.line(0, text.len);
    defer line.deinit();

    const runs = line.shapingRuns();
    try std.testing.expect(runs.len >= 3);

    var scripts = std.mem.zeroes([2]bool);
    for (runs) |run| {
        if (run.script == ztext.tag("Hebr")) scripts[0] = true;
        if (run.script == ztext.tag("Arab")) scripts[1] = true;
    }
    try std.testing.expect(scripts[0] and scripts[1]);
    try expectTiles(runs, 0, text.len);
}

test "a line outlives the paragraph it came from" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // The contract says a line copies what it needs. Destroying the paragraph
    // first is how that is proved rather than asserted -- under the testing
    // allocator a borrowed pointer here would be a use-after-free.
    var line: ztext.Line = undefined;
    {
        const paragraph = try ztext.Paragraph.init(wrapped, .{});
        defer paragraph.deinit();
        line = try paragraph.line(0, first_line_end);
    }
    defer line.deinit();

    try std.testing.expectEqual(@as(usize, 3), line.visualRuns().len);
    try expectTiles(line.visualRuns(), 0, first_line_end);
}

test "a line refuses a range that is out of bounds or splits a character" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const paragraph = try ztext.Paragraph.init(wrapped, .{});
    defer paragraph.deinit();

    try std.testing.expectError(error.InvalidArgument, paragraph.line(0, wrapped.len + 1));
    try std.testing.expectError(error.InvalidArgument, paragraph.line(wrapped.len + 1, 0));
    try std.testing.expectError(error.InvalidArgument, paragraph.line(4, std.math.maxInt(usize)));

    // Byte 5 is the second byte of the first Hebrew letter. Splitting there
    // would hand a shaper half a character, so it is refused rather than
    // reordered.
    try std.testing.expectError(error.InvalidArgument, paragraph.line(5, 4));
    try std.testing.expectError(error.InvalidArgument, paragraph.line(0, 5));

    // The boundary either side of it is fine.
    const before = try paragraph.line(0, 4);
    defer before.deinit();
    const after = try paragraph.line(4, 8);
    defer after.deinit();
}

test "an empty line is analysed rather than refused" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const paragraph = try ztext.Paragraph.init(wrapped, .{});
    defer paragraph.deinit();

    const empty = try paragraph.line(4, 0);
    defer empty.deinit();
    try std.testing.expectEqual(@as(usize, 4), empty.offset());
    try std.testing.expectEqual(@as(usize, 0), empty.length());
    try std.testing.expectEqual(@as(usize, 0), empty.visualRuns().len);
    try std.testing.expectEqual(@as(usize, 0), empty.shapingRuns().len);

    // And so is a line of an empty paragraph, which never reaches SheenBidi.
    const blank = try ztext.Paragraph.init("", .{});
    defer blank.deinit();
    const blank_line = try blank.line(0, 0);
    defer blank_line.deinit();
    try std.testing.expectEqual(@as(usize, 0), blank_line.visualRuns().len);
}

//=============================================================================
// Metrics sources
//=============================================================================

test "FreeType and HarfBuzz metrics agree closely but not exactly" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const text = "Hamburgefonstiv";
    const base: ztext.ShapeParams = .{ .direction = .ltr, .script = ztext.tag("Latn") };

    _ = try fixture.shaper.shape(face, text, base);
    var harfbuzz_total: f32 = 0;
    for (fixture.shaper.glyphs()) |glyph| harfbuzz_total += glyph.x_advance;
    const glyph_count = fixture.shaper.glyphs().len;

    _ = try fixture.shaper.shape(face, text, .{
        .direction = .ltr,
        .script = ztext.tag("Latn"),
        .use_freetype_metrics = true,
    });
    var freetype_total: f32 = 0;
    for (fixture.shaper.glyphs()) |glyph| freetype_total += glyph.x_advance;

    // Same glyphs either way -- only the positioning source changes.
    try std.testing.expectEqual(glyph_count, fixture.shaper.glyphs().len);

    // This is a measurement, not an equality. Hinted advances are rounded to
    // the pixel grid per glyph, unhinted ones are not, so they diverge. The
    // point is that the gap stays under a pixel across a whole word and does
    // not silently become large -- if it does, the two sources have stopped
    // describing the same font and `use_freetype_metrics` is a trap.
    const delta = @abs(freetype_total - harfbuzz_total);
    try std.testing.expect(delta < 1.0);
    try std.testing.expect(harfbuzz_total > 250.0 and harfbuzz_total < 280.0);
}

//=============================================================================
// Rasterisation
//=============================================================================

test "A8 rasterisation produces coverage with ink in it" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('o');
    try std.testing.expect(glyph != 0);

    const bitmap = try face.renderGlyph(glyph, .a8, .none, 0, 0);
    try std.testing.expect(bitmap.width > 0 and bitmap.height > 0);
    try std.testing.expect(bitmap.pitch > 0);

    const rows = ztext.bitmapRows(bitmap) orelse return error.TestUnexpectedResult;
    var max: u8 = 0;
    var lit: usize = 0;
    for (rows) |value| {
        max = @max(max, value);
        if (value != 0) lit += 1;
    }
    try std.testing.expectEqual(@as(u8, 255), max);
    try std.testing.expect(lit > rows.len / 8);

    // A blank glyph has no ink and says so rather than inventing a bitmap.
    const space = face.font.glyphIndex(' ');
    const blank = try face.renderGlyph(space, .a8, .none, 0, 0);
    try std.testing.expect(blank.pixels == null);
    try std.testing.expect(blank.x_advance > 0);
}

test "SDF is a real distance field, not a coverage bitmap in disguise" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('o');
    const coverage = try face.renderGlyph(glyph, .a8, .none, 0, 0);
    const coverage_width = coverage.width;
    const coverage_height = coverage.height;
    const coverage_left = coverage.left;

    const spread: u32 = 8; // FreeType's default.
    const sdf = try face.renderGlyph(glyph, .sdf, .none, 0, 0);

    // The field extends by the spread on every side, and the bearing moves
    // with it. Getting this wrong puts every glyph in an atlas off by 8px.
    try std.testing.expectEqual(coverage_width + 2 * spread, sdf.width);
    try std.testing.expectEqual(coverage_height + 2 * spread, sdf.height);
    try std.testing.expectEqual(coverage_left - @as(i32, @intCast(spread)), sdf.left);

    const rows = ztext.bitmapRows(sdf) orelse return error.TestUnexpectedResult;
    const pitch: usize = @intCast(sdf.pitch);

    // Scan the middle row of an 'o'. Crossing its left stroke, the field must
    // rise past the 128 mid-point (inside) and fall back below it inside the
    // counter (outside again). A coverage bitmap would be flat 255 across the
    // stroke and flat 0 elsewhere, with no ramp at all.
    const middle = rows[(sdf.height / 2) * pitch ..][0..sdf.width];

    var minimum: u8 = 255;
    var maximum: u8 = 0;
    var crossings: usize = 0;
    var previous_inside = middle[0] >= 128;
    for (middle) |value| {
        minimum = @min(minimum, value);
        maximum = @max(maximum, value);
        const inside = value >= 128;
        if (inside != previous_inside) crossings += 1;
        previous_inside = inside;
    }

    // Left edge in, left edge out, right edge in, right edge out.
    try std.testing.expectEqual(@as(usize, 4), crossings);
    try std.testing.expect(maximum > 128);
    try std.testing.expect(minimum < 16);

    // A ramp, not a step: consecutive samples change gradually.
    var biggest_step: u8 = 0;
    for (middle[1..], middle[0 .. middle.len - 1]) |a, b| {
        const step = if (a > b) a - b else b - a;
        biggest_step = @max(biggest_step, step);
    }
    try std.testing.expect(biggest_step < 64);

    // The spread is a library-wide property and changing it changes the field.
    try fixture.library.setSdfSpread(16);
    const wider = try face.renderGlyph(glyph, .sdf, .none, 0, 0);
    try std.testing.expectEqual(coverage_width + 32, wider.width);
    try fixture.library.setSdfSpread(spread);

    try std.testing.expectError(ztext.Error.InvalidArgument, fixture.library.setSdfSpread(1));
    try std.testing.expectError(ztext.Error.InvalidArgument, fixture.library.setSdfSpread(33));
}

test "a rendered bitmap says which format its bytes are in" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // A8 coverage and an SDF are both one byte per pixel, so a consumer that
    // remembers the wrong mode does not get an error -- it gets a picture,
    // washed out and plausible. The bitmap therefore carries its own format.
    const glyph = face.font.glyphIndex('o');
    try std.testing.expectEqual(
        ztext.BitmapFormat.a8,
        (try face.renderGlyph(glyph, .a8, .none, 0, 0)).format,
    );
    try std.testing.expectEqual(
        ztext.BitmapFormat.sdf,
        (try face.renderGlyph(glyph, .sdf, .none, 0, 0)).format,
    );
    // Back again: the field is written by every render, not left at whatever
    // the last one set.
    try std.testing.expectEqual(
        ztext.BitmapFormat.a8,
        (try face.renderGlyph(glyph, .a8, .none, 0, 0)).format,
    );

    // A glyph with no ink still says what it would have been. Otherwise the
    // format is only meaningful after a NULL check, and a caller batching
    // renders into an atlas would have to special-case the spaces -- which is
    // exactly the special case that gets written once and then forgotten.
    const space = try face.renderGlyph(face.font.glyphIndex(' '), .sdf, .none, 0, 0);
    try std.testing.expectEqual(@as(?[*]const u8, null), space.pixels);
    try std.testing.expectEqual(ztext.BitmapFormat.sdf, space.format);
}

test "glyph extents match the rasterised bitmap" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('H');
    const extents = try face.glyphExtents(glyph, .none);
    const bitmap = try face.renderGlyph(glyph, .a8, .none, 0, 0);

    // The bitmap is the extents rounded outwards to whole pixels, so it can be
    // larger by at most a pixel on each axis but never smaller.
    const ink_width = extents.x_max - extents.x_min;
    const ink_height = extents.y_max - extents.y_min;
    try std.testing.expect(@as(f32, @floatFromInt(bitmap.width)) >= ink_width - 1.0);
    try std.testing.expect(@as(f32, @floatFromInt(bitmap.height)) >= ink_height - 1.0);
    try std.testing.expect(extents.x_advance > 0);
}

//=============================================================================
// Subpixel positioning
//
// offset_x/offset_y are in 26.6, same as everywhere else FreeType's fixed
// point crosses this boundary. `0, 0` is the pin: every other rasterisation
// test in this file calls renderGlyph with a zero offset and none of them
// moved when the parameter was added, which is what "unchanged for an
// existing caller" means in practice.
//=============================================================================

test "a whole-pixel offset shifts the bitmap by exactly one pixel" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('o');
    const plain = try face.renderGlyph(glyph, .a8, .none, 0, 0);
    // Copied out: the face's bitmap buffer is reused and overwritten by the
    // very next renderGlyph on this face, so `plain`'s pixels do not survive
    // the second call below.
    const plain_rows = try std.testing.allocator.dupe(
        u8,
        ztext.bitmapRows(plain) orelse return error.TestUnexpectedResult,
    );
    defer std.testing.allocator.free(plain_rows);

    // 64 in 26.6 is exactly one pixel, so this is a pure repositioning: same
    // shape, same coverage, moved by one pixel on each axis.
    const shifted = try face.renderGlyph(glyph, .a8, .none, 64, 64);
    const shifted_rows = ztext.bitmapRows(shifted) orelse return error.TestUnexpectedResult;

    try std.testing.expectEqual(plain.width, shifted.width);
    try std.testing.expectEqual(plain.height, shifted.height);
    try std.testing.expectEqual(plain.left + 1, shifted.left);
    try std.testing.expectEqual(plain.top + 1, shifted.top);
    try std.testing.expectEqualSlices(u8, plain_rows, shifted_rows);
}

test "a fractional offset changes the antialiasing rather than the pixel grid" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('o');
    const plain = try face.renderGlyph(glyph, .a8, .none, 0, 0);
    const plain_rows = try std.testing.allocator.dupe(
        u8,
        ztext.bitmapRows(plain) orelse return error.TestUnexpectedResult,
    );
    defer std.testing.allocator.free(plain_rows);

    // Half a pixel: too small to move the glyph a whole cell, but the coverage
    // at every partially-covered edge pixel has to change -- that is the
    // entire point of subpixel positioning.
    const half = try face.renderGlyph(glyph, .a8, .none, 32, 0);
    const half_rows = ztext.bitmapRows(half) orelse return error.TestUnexpectedResult;

    var identical = plain.width == half.width and plain.height == half.height;
    if (identical) identical = std.mem.eql(u8, plain_rows, half_rows);
    try std.testing.expect(!identical);
}

test "SDF ignores the subpixel offset" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('o');
    const plain = try face.renderGlyph(glyph, .sdf, .none, 0, 0);
    const plain_rows = try std.testing.allocator.dupe(
        u8,
        ztext.bitmapRows(plain) orelse return error.TestUnexpectedResult,
    );
    defer std.testing.allocator.free(plain_rows);

    // The same fractional offset that changed the A8 bitmap above must do
    // nothing here: SDF is baked once and sampled at any position later, so
    // baking a sub-pixel shift in would be work with no way to undo it.
    const offset = try face.renderGlyph(glyph, .sdf, .none, 32, 32);
    const offset_rows = ztext.bitmapRows(offset) orelse return error.TestUnexpectedResult;

    try std.testing.expectEqual(plain.width, offset.width);
    try std.testing.expectEqual(plain.height, offset.height);
    try std.testing.expectEqual(plain.left, offset.left);
    try std.testing.expectEqual(plain.top, offset.top);
    try std.testing.expectEqualSlices(u8, plain_rows, offset_rows);
}

//=============================================================================
// Glyph outlines as paths
//=============================================================================

const OutlineCollector = struct {
    move_to_count: u32 = 0,
    line_to_count: u32 = 0,
    conic_to_count: u32 = 0,
    cubic_to_count: u32 = 0,
    close_count: u32 = 0,
    min_x: i32 = std.math.maxInt(i32),
    max_x: i32 = std.math.minInt(i32),

    fn track(self: *OutlineCollector, x: i32) void {
        self.min_x = @min(self.min_x, x);
        self.max_x = @max(self.max_x, x);
    }
};

fn outlineMoveTo(user: ?*anyopaque, x: i32, y: i32) callconv(.c) ztext.c.Result {
    _ = y;
    const self: *OutlineCollector = @ptrCast(@alignCast(user.?));
    self.move_to_count += 1;
    self.track(x);
    return .ok;
}

fn outlineLineTo(user: ?*anyopaque, x: i32, y: i32) callconv(.c) ztext.c.Result {
    _ = y;
    const self: *OutlineCollector = @ptrCast(@alignCast(user.?));
    self.line_to_count += 1;
    self.track(x);
    return .ok;
}

fn outlineConicTo(user: ?*anyopaque, control_x: i32, control_y: i32, x: i32, y: i32) callconv(.c) ztext.c.Result {
    _ = control_x;
    _ = control_y;
    _ = y;
    const self: *OutlineCollector = @ptrCast(@alignCast(user.?));
    self.conic_to_count += 1;
    self.track(x);
    return .ok;
}

fn outlineCubicTo(
    user: ?*anyopaque,
    control1_x: i32,
    control1_y: i32,
    control2_x: i32,
    control2_y: i32,
    x: i32,
    y: i32,
) callconv(.c) ztext.c.Result {
    _ = control1_x;
    _ = control1_y;
    _ = control2_x;
    _ = control2_y;
    _ = y;
    const self: *OutlineCollector = @ptrCast(@alignCast(user.?));
    self.cubic_to_count += 1;
    self.track(x);
    return .ok;
}

fn outlineClose(user: ?*anyopaque) callconv(.c) ztext.c.Result {
    const self: *OutlineCollector = @ptrCast(@alignCast(user.?));
    self.close_count += 1;
    return .ok;
}

fn outlineFuncsInto(collector: *OutlineCollector) ztext.OutlineFuncs {
    return .{
        .move_to = outlineMoveTo,
        .line_to = outlineLineTo,
        .conic_to = outlineConicTo,
        .cubic_to = outlineCubicTo,
        .close = outlineClose,
        .user = @ptrCast(collector),
    };
}

test "decomposing a round letter visits curves and closes every contour" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('o');
    var collector = OutlineCollector{};
    const funcs = outlineFuncsInto(&collector);
    try face.decomposeOutline(glyph, .none, &funcs);

    // An 'o' is two contours -- the outer ring and the counter -- each opened
    // by move_to and closed exactly once.
    try std.testing.expect(collector.move_to_count >= 2);
    try std.testing.expectEqual(collector.move_to_count, collector.close_count);
    // It is round, so FT_Outline_Decompose must emit at least one curve.
    try std.testing.expect(collector.conic_to_count > 0 or collector.cubic_to_count > 0);

    // Points are 26.6 in the same space glyphExtents reports.
    const extents = try face.glyphExtents(glyph, .none);
    const min_x_px = @as(f32, @floatFromInt(collector.min_x)) / 64.0;
    const max_x_px = @as(f32, @floatFromInt(collector.max_x)) / 64.0;
    try std.testing.expectApproxEqAbs(extents.x_min, min_x_px, 1.0);
    try std.testing.expectApproxEqAbs(extents.x_max, max_x_px, 1.0);
}

test "an inkless glyph decomposes to nothing" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const space = face.font.glyphIndex(' ');
    var collector = OutlineCollector{};
    const funcs = outlineFuncsInto(&collector);
    try face.decomposeOutline(space, .none, &funcs);

    try std.testing.expectEqual(@as(u32, 0), collector.move_to_count);
    try std.testing.expectEqual(@as(u32, 0), collector.close_count);
}

fn abortingMoveTo(user: ?*anyopaque, x: i32, y: i32) callconv(.c) ztext.c.Result {
    _ = user;
    _ = x;
    _ = y;
    return .out_of_memory;
}

test "a callback's own failure aborts decomposition and propagates" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('o');
    var collector = OutlineCollector{};
    var funcs = outlineFuncsInto(&collector);
    funcs.move_to = abortingMoveTo;

    try std.testing.expectError(ztext.Error.OutOfMemory, face.decomposeOutline(glyph, .none, &funcs));
    // Aborted before the first line/curve of the contour it never opened.
    try std.testing.expectEqual(@as(u32, 0), collector.line_to_count);
}

test "decomposeOutline refuses an incomplete callback set" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('o');
    var collector = OutlineCollector{};
    var funcs = outlineFuncsInto(&collector);
    funcs.close = null;

    try std.testing.expectError(ztext.Error.InvalidArgument, face.decomposeOutline(glyph, .none, &funcs));
}

//=============================================================================
// Synthetic bold and oblique
//=============================================================================

test "synthetic bold widens the ink and the advance, and rasterisation agrees" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('H');
    const plain = try face.glyphExtents(glyph, .none);
    const plain_bitmap = try face.renderGlyph(glyph, .a8, .none, 0, 0);

    try face.setSyntheticBold(true);
    const bold = try face.glyphExtents(glyph, .none);
    const bold_bitmap = try face.renderGlyph(glyph, .a8, .none, 0, 0);
    try face.setSyntheticBold(false);

    // The advance has to widen or bold text overlaps the next glyph.
    try std.testing.expect(bold.x_advance > plain.x_advance);
    // The ink widens too, and ztextFaceGlyphExtents and renderGlyph agree on
    // it -- both flow through the same glyph-loading path.
    try std.testing.expect(bold.x_max - bold.x_min > plain.x_max - plain.x_min);
    try std.testing.expect(bold_bitmap.width >= plain_bitmap.width);

    // Disabling it again is not a one-way trip.
    const restored = try face.glyphExtents(glyph, .none);
    try std.testing.expectEqual(plain.x_advance, restored.x_advance);
}

test "synthetic oblique shears the ink but leaves the advance alone" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('H');
    const upright = try face.glyphExtents(glyph, .none);

    try face.setSyntheticOblique(true);
    const sheared = try face.glyphExtents(glyph, .none);
    try face.setSyntheticOblique(false);

    // A shear does not change how far the pen moves.
    try std.testing.expectEqual(upright.x_advance, sheared.x_advance);
    // But it does move the ink -- ztextFaceGlyphExtents has to recompute the
    // bounds from the sheared outline rather than report the upright ones.
    try std.testing.expect(sheared.x_min != upright.x_min or sheared.x_max != upright.x_max);
}

test "synthetic style reaches outline decomposition, not just render and extents" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const glyph = face.font.glyphIndex('H');

    var plain_collector = OutlineCollector{};
    const plain_funcs = outlineFuncsInto(&plain_collector);
    try face.decomposeOutline(glyph, .none, &plain_funcs);

    try face.setSyntheticBold(true);
    var bold_collector = OutlineCollector{};
    const bold_funcs = outlineFuncsInto(&bold_collector);
    try face.decomposeOutline(glyph, .none, &bold_funcs);
    try face.setSyntheticBold(false);

    const plain_width = plain_collector.max_x - plain_collector.min_x;
    const bold_width = bold_collector.max_x - bold_collector.min_x;
    try std.testing.expect(bold_width > plain_width);
}

//=============================================================================
// Hostile input
//=============================================================================

test "malformed UTF-8 is refused rather than repaired" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const bad = [_][]const u8{
        &.{0xC3}, // truncated two-byte sequence
        &.{ 0xC3, 0x28 }, // bad continuation
        &.{ 0xE2, 0x82 }, // truncated three-byte sequence
        &.{ 0xC0, 0x80 }, // overlong NUL
        &.{ 0xED, 0xA0, 0x80 }, // UTF-16 surrogate half
        &.{ 0xF5, 0x80, 0x80, 0x80 }, // beyond U+10FFFF
        &.{ 0x80, 0x80 }, // stray continuation bytes
        &.{ 'a', 0xFF, 'b' }, // never a valid lead
    };

    for (bad) |text| {
        try std.testing.expectError(
            ztext.Error.InvalidText,
            fixture.shaper.shape(face, text, .{}),
        );
        try std.testing.expectError(
            ztext.Error.InvalidText,
            ztext.Paragraph.init(text, .{}),
        );
    }

    // Valid text that merely looks unusual must still be accepted.
    for ([_][]const u8{ "", "a", "é", "€", "𝄞", "\x00between\x00" }) |text| {
        _ = try fixture.shaper.shape(face, text, .{});
        const paragraph = try ztext.Paragraph.init(text, .{});
        paragraph.deinit();
    }
}

test "non-font bytes are refused" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        fixture.library.createFont("", 0),
    );
    try std.testing.expectError(
        ztext.Error.BadFont,
        fixture.library.createFont("this is definitely not a font" ** 8, 0),
    );

    // A format ztext recognises but does not compile support for is reported
    // as unsupported, not as a broken font -- the difference tells a host to
    // re-cook rather than to go looking for corruption.
    try std.testing.expectError(
        ztext.Error.Unsupported,
        fixture.library.createFont("wOFF" ++ "\x00" ** 60, 0),
    );
    try std.testing.expectError(
        ztext.Error.Unsupported,
        fixture.library.createFont("wOF2" ++ "\x00" ** 60, 0),
    );
    try std.testing.expect(ztext.lastErrorDetail().len > 0);

    // A face index past the end of a real font. FreeType calls this an
    // invalid argument rather than a bad font, which is right -- the bytes are
    // fine, the request is not.
    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        fixture.library.createFont(fonts.hebrew, 99),
    );
}

test "every truncated prefix of a font fails cleanly or behaves" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // The smallest committed font, so the sweep is affordable. Every prefix up
    // to 2 KB is tried, then every 251st byte (a prime, so the stride does not
    // land on table boundaries systematically). That is a SAMPLE, not a proof:
    // roughly 2100 of 26860 prefixes are covered.
    const font = fonts.hebrew;
    var loaded: usize = 0;
    var rejected: usize = 0;

    var length: usize = 1;
    while (length < font.len) : (length = if (length < 2048) length + 1 else length + 251) {
        const partial = fixture.library.createFont(font[0..length], 0) catch |e| {
            // Any typed error is acceptable. A crash, a hang or an untyped
            // failure is not, and is what this sweep exists to rule out.
            switch (e) {
                ztext.Error.BadFont,
                ztext.Error.Unsupported,
                ztext.Error.InvalidArgument,
                ztext.Error.OutOfMemory,
                => rejected += 1,
                else => return e,
            }
            continue;
        };
        defer partial.deinit();
        loaded += 1;

        // FreeType reads tables lazily, so a prefix that loads has merely not
        // been caught yet. Push it: everything below must return a value or a
        // typed error, and must not corrupt anything.
        const face = partial.face(0, ppem) catch continue;
        defer face.deinit();
        _ = face.metrics() catch {};
        _ = face.font.glyphIndex('a');
        _ = fixture.shaper.shape(face, "abc שלום", .{}) catch continue;
        for (fixture.shaper.glyphs()) |glyph| {
            _ = face.renderGlyph(glyph.glyph_id, .a8, .normal, 0, 0) catch {};
            _ = face.glyphExtents(glyph.glyph_id, .none) catch {};
        }
        _ = fixture.shaper.extents(face) catch {};
    }

    // Both outcomes must actually occur, or the sweep is not testing what it
    // claims: some prefixes are rejected outright, and some load and then have
    // to survive being used.
    try std.testing.expect(rejected > 0);
    try std.testing.expect(loaded > 0);
}

test "flipping bytes inside a real font never gets past the boundary" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // Mutation over the first 4 KB, which covers the table directory and the
    // head of the tables it points at -- where a doctored count or offset does
    // the most damage. Deterministic positions, so a failure reproduces.
    var mutated = try std.testing.allocator.dupe(u8, fonts.hebrew);
    defer std.testing.allocator.free(mutated);

    var survived: usize = 0;
    var position: usize = 0;
    while (position < 4096) : (position += 37) {
        const original = mutated[position];
        for ([_]u8{ 0x00, 0xFF, original ^ 0x80 }) |replacement| {
            mutated[position] = replacement;
            const damaged = fixture.library.createFont(mutated, 0) catch continue;
            defer damaged.deinit();
            survived += 1;
            const face = damaged.face(0, ppem) catch continue;
            defer face.deinit();
            _ = face.metrics() catch {};
            _ = fixture.shaper.shape(face, "שלום abc", .{}) catch continue;
            for (fixture.shaper.glyphs()) |glyph| {
                if (face.renderGlyph(glyph.glyph_id, .a8, .normal, 0, 0)) |bitmap| {
                    // Whatever comes back must describe itself consistently:
                    // a bitmap claiming pixels must have them, and its rows
                    // must fit the buffer it points at.
                    if (bitmap.width != 0 and bitmap.height != 0) {
                        try std.testing.expect(bitmap.pixels != null);
                        try std.testing.expect(@abs(bitmap.pitch) >= @as(i32, @intCast(bitmap.width)));
                    }
                } else |_| {}
            }
        }
        mutated[position] = original;
    }

    // Some mutations are in padding or in tables nothing reads, so surviving
    // is expected and is not a finding -- the finding would be a crash.
    try std.testing.expect(survived > 0);
}

//=============================================================================
// Reuse
//=============================================================================

test "a shaper is reusable across faces, scripts and directions" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const latin = try fixture.face(fonts.latin);
    defer latin.deinit();
    const arabic = try fixture.face(fonts.arabic);
    defer arabic.deinit();
    const hebrew = try fixture.face(fonts.hebrew);
    defer hebrew.deinit();

    // Interleaving is where stale buffer state would show up: a leftover
    // script or direction from the previous call producing different output
    // than the same call made first.
    _ = try fixture.shaper.shape(arabic, "مرحبا", .{ .direction = .rtl, .script = ztext.tag("Arab") });
    const arabic_first = try std.testing.allocator.dupe(ztext.Glyph, fixture.shaper.glyphs());
    defer std.testing.allocator.free(arabic_first);

    _ = try fixture.shaper.shape(latin, "Waffle", .{ .direction = .ltr, .script = ztext.tag("Latn") });
    _ = try fixture.shaper.shape(hebrew, "שלום", .{ .direction = .rtl, .script = ztext.tag("Hebr") });
    _ = try fixture.shaper.shape(latin, "AV", .{ .direction = .ltr, .script = ztext.tag("Latn") });

    _ = try fixture.shaper.shape(arabic, "مرحبا", .{ .direction = .rtl, .script = ztext.tag("Arab") });
    try std.testing.expectEqualSlices(ztext.Glyph, arabic_first, fixture.shaper.glyphs());
}

test "shaping is stable across repeated calls" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    _ = try fixture.shaper.shape(face, "Waffle wins", .{ .direction = .ltr, .script = ztext.tag("Latn") });
    const first = try std.testing.allocator.dupe(ztext.Glyph, fixture.shaper.glyphs());
    defer std.testing.allocator.free(first);

    for (0..16) |_| {
        _ = try fixture.shaper.shape(face, "Waffle wins", .{ .direction = .ltr, .script = ztext.tag("Latn") });
        try std.testing.expectEqualSlices(ztext.Glyph, first, fixture.shaper.glyphs());
    }
}

test "shaping does not depend on the process locale" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // HarfBuzz's guess_segment_properties reaches for the process locale when
    // no language is set, which would make output differ between machines.
    // ztext seeds the language so it cannot. An explicit language must still
    // be honoured, so the two calls below are allowed to differ from each
    // other -- what must not happen is the default silently BEING a locale.
    _ = try fixture.shaper.shape(face, "fi", .{ .direction = .ltr, .script = ztext.tag("Latn") });
    const default_glyphs = try std.testing.allocator.dupe(ztext.Glyph, fixture.shaper.glyphs());
    defer std.testing.allocator.free(default_glyphs);

    for (test_languages) |language| {
        _ = try fixture.shaper.shape(face, "fi", .{
            .direction = .ltr,
            .script = ztext.tag("Latn"),
            .language = language,
        });
    }

    _ = try fixture.shaper.shape(face, "fi", .{ .direction = .ltr, .script = ztext.tag("Latn") });
    try std.testing.expectEqualSlices(ztext.Glyph, default_glyphs, fixture.shaper.glyphs());
}

test "face counting reports what a font image contains" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // All three committed fonts are plain faces, not collections.
    for ([_][]const u8{ fonts.latin, fonts.arabic, fonts.hebrew }) |bytes| {
        try std.testing.expectEqual(@as(u32, 1), try fixture.library.countFaces(bytes));
    }

    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        fixture.library.countFaces(""),
    );
    try std.testing.expectError(
        ztext.Error.BadFont,
        fixture.library.countFaces("not a font at all, not even close"),
    );
    try std.testing.expectError(
        ztext.Error.Unsupported,
        fixture.library.countFaces("wOFF" ++ "\x00" ** 60),
    );
}

test "extents come from the same metrics source the shape used" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const text = "Hamburgefonstiv";

    // Whichever source is in play, the reported advance must equal the sum of
    // the advances the same call produced. Taking extents from hb-ot-font
    // after shaping with hb-ft would break exactly this, by an amount small
    // enough to pass for rounding.
    for ([_]bool{ false, true }) |use_freetype| {
        _ = try fixture.shaper.shape(face, text, .{
            .direction = .ltr,
            .script = ztext.tag("Latn"),
            .use_freetype_metrics = use_freetype,
        });

        var summed: f32 = 0;
        for (fixture.shaper.glyphs()) |glyph| summed += glyph.x_advance;

        const extents = try fixture.shaper.extents(face);
        try std.testing.expectApproxEqAbs(summed, extents.x_advance, tolerance);
        try std.testing.expect(extents.x_max > extents.x_min);
    }
}

test "extents and glyphs are refused before anything has been shaped" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const fresh = try ztext.Shaper.init();
    defer fresh.deinit();

    try std.testing.expectEqual(@as(usize, 0), fresh.glyphs().len);
    try std.testing.expectEqual(ztext.Direction.auto, fresh.direction());
    try std.testing.expectError(ztext.Error.InvalidArgument, fresh.extents(face));
}

test "versions format the way a log line needs" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // Version.format is public API and nothing else in the suite reaches it.
    //
    // No version LITERAL appears here, and that is the point. This test used
    // to assert "0.1.0" and "2.14.3", which made it a third home for ztext's
    // version and a second home for FreeType's -- and the first bump of
    // either turned a formatting test red for a reason that had nothing to do
    // with formatting. ffi/ztext.h's macros are the one home for the first
    // (gated against build.zig.zon by ci/measurements.sh) and src/pins.zig for
    // the second (gated against the linked libraries by the test below).
    //
    // So: one synthetic value, which this test owns and which pins the digits
    // a real version rarely has, and one real value compared against its own
    // fields.
    var buffer: [64]u8 = undefined;
    const synthetic = ztext.Version{ .major = 3, .minor = 14, .patch = 159 };
    try std.testing.expectEqualStrings(
        "3.14.159",
        try std.fmt.bufPrint(&buffer, "{f}", .{synthetic}),
    );

    var expected: [64]u8 = undefined;
    const v = ztext.version();
    try std.testing.expectEqualStrings(
        try std.fmt.bufPrint(&expected, "{d}.{d}.{d}", .{ v.major, v.minor, v.patch }),
        try std.fmt.bufPrint(&buffer, "{f}", .{v}),
    );
}

test "a font FreeType accepts but HarfBuzz cannot read is refused" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // FreeType and HarfBuzz parse the same bytes independently, and they do
    // not agree on what is acceptable. Setting the sfnt version word to
    // 0x00020000 is a case FreeType's sfnt driver takes and HarfBuzz's
    // sanitiser rejects.
    //
    // Left unchecked this is the worst kind of failure: hb_face_create hands
    // back a normal face object with no tables, every character shapes to
    // .notdef with no error reported, and FreeType goes on rasterising the
    // font perfectly -- so it presents as a broken font rather than a failed
    // load.
    const doctored = try std.testing.allocator.dupe(u8, fonts.hebrew);
    defer std.testing.allocator.free(doctored);
    doctored[0] = 0x00;
    doctored[1] = 0x02;
    doctored[2] = 0x00;
    doctored[3] = 0x00;

    try std.testing.expectError(
        ztext.Error.BadFont,
        fixture.library.createFont(doctored, 0),
    );
    try std.testing.expect(ztext.lastErrorDetail().len > 0);

    // A face index past the end of the collection is the same class of
    // problem and must also be refused rather than yielding a tableless face.
    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        fixture.library.createFont(fonts.hebrew, 7),
    );
}

test "a rejected shape does not leave the previous run queryable" {
    const fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    _ = try fixture.shaper.shape(face, "Hello", .{ .direction = .ltr });
    try std.testing.expectEqual(@as(usize, 5), fixture.shaper.glyphs().len);

    // A caller that logs the error and keeps drawing must not draw the last
    // successful run again. Every rejection path has to clear, not just the
    // ones that fail late.
    try std.testing.expectError(
        ztext.Error.InvalidText,
        fixture.shaper.shape(face, &[_]u8{ 0xFF, 0xFE }, .{}),
    );
    try std.testing.expectEqual(@as(usize, 0), fixture.shaper.glyphs().len);
    try std.testing.expectEqual(ztext.Direction.auto, fixture.shaper.direction());
    try std.testing.expectError(ztext.Error.InvalidArgument, fixture.shaper.extents(face));

    // Same for a rejection that happens before any shaping work at all. This
    // one goes through the C layer directly, because the Zig wrapper makes a
    // feature count without features unrepresentable -- but a C host can hand
    // it over, and the clearing must happen on that path too.
    _ = try fixture.shaper.shape(face, "Hello", .{ .direction = .ltr });
    try std.testing.expectEqual(@as(usize, 5), fixture.shaper.glyphs().len);

    var inconsistent = std.mem.zeroes(ztext.c.ShapeParams);
    inconsistent.feature_count = 3;
    try std.testing.expectEqual(ztext.c.Result.invalid_argument, ztext.c.ztextShaperShape(
        fixture.shaper.handle,
        face.handle,
        "Hello",
        5,
        .utf8,
        0,
        5,
        &inconsistent,
    ));
    try std.testing.expectEqual(@as(usize, 0), fixture.shaper.glyphs().len);
}

test "extents refuse a face the run was not shaped against" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const latin = try fixture.face(fonts.latin);
    defer latin.deinit();
    const hebrew = try fixture.face(fonts.hebrew);
    defer hebrew.deinit();

    _ = try fixture.shaper.shape(latin, "Hamburgefonstiv", .{ .direction = .ltr });
    _ = try fixture.shaper.extents(latin);

    // Measuring against another face would mix one font's ink bounds with
    // another's advances: wrong, plausible, and silent.
    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        fixture.shaper.extents(hebrew),
    );

    // Resizing the face invalidates the run for the same reason -- the
    // advances were computed at the old size.
    try latin.setPixelSize(0, 8);
    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        fixture.shaper.extents(latin),
    );

    // Shaping again at the new size makes it valid once more.
    _ = try fixture.shaper.shape(latin, "Hamburgefonstiv", .{ .direction = .ltr });
    _ = try fixture.shaper.extents(latin);
}

test "a library keeps its own allocator when the global one is replaced" {
    // The wrapper used to hand C a pointer to a single mutable global, which
    // defeated the C side's per-library capture entirely: swapping the
    // allocator and then destroying a library handed its FreeType blocks to an
    // allocator that had never issued them, and Zig's DebugAllocator aborts on
    // exactly that. `setAllocator` copying each allocator into a slot of its
    // own is what makes this work, and this is the test that says so.
    var first_state: std.heap.DebugAllocator(.{}) = .init;
    var second_state: std.heap.DebugAllocator(.{}) = .init;
    const first = first_state.allocator();
    const second = second_state.allocator();

    try ztext.setAllocator(first);
    const library = try ztext.Library.init();
    const font = try library.createFont(fonts.hebrew, 0);
    const face = try font.face(0, 24);

    // Everything from here is nominally the second allocator's.
    try ztext.setAllocator(second);
    const other = try ztext.Library.init();

    // Destroying the first library must return its memory to the allocator it
    // was born with, not to whichever one happens to be installed. The order
    // below is the tidy one; the test after this file's last golden is the one
    // that shows the order does not matter.
    face.deinit();
    font.deinit();
    library.deinit();
    other.deinit();

    ztext.resetAllocator();
    try std.testing.expectEqual(std.heap.Check.ok, first_state.deinit());
    try std.testing.expectEqual(std.heap.Check.ok, second_state.deinit());
}

test "shaping runs split a paragraph into spans a shaper can actually take" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // The case that makes this API necessary: one visual run, three scripts.
    // Shaping straight from visualRuns() hands HarfBuzz a run spanning Latin,
    // Greek and Cyrillic and lets it guess.
    {
        const text = "Hello Ελληνικά мир";
        const paragraph = try ztext.Paragraph.init(text, .{});
        defer paragraph.deinit();

        try std.testing.expectEqual(@as(usize, 1), paragraph.visualRuns().len);
        const runs = paragraph.shapingRuns();
        try std.testing.expectEqual(@as(usize, 3), runs.len);
        try std.testing.expectEqual(ztext.tag("Latn"), runs[0].script);
        try std.testing.expectEqual(ztext.tag("Grek"), runs[1].script);
        try std.testing.expectEqual(ztext.tag("Cyrl"), runs[2].script);
        for (runs) |run| try std.testing.expectEqual(ztext.Direction.ltr, ztext.runDirection(run.level));
    }

    // The subtlety this exists for: inside a right-to-left visual run, the
    // script pieces must come out in REVERSE, because they are drawn
    // right-to-left. Emitting them in logical order would put the Arabic where
    // the Hebrew belongs and still look like plausible text.
    {
        const text = "مرحبا שלום";
        const paragraph = try ztext.Paragraph.init(text, .{});
        defer paragraph.deinit();

        try std.testing.expectEqual(@as(u8, 1), paragraph.baseLevel());
        try std.testing.expectEqual(@as(usize, 1), paragraph.visualRuns().len);

        const runs = paragraph.shapingRuns();
        try std.testing.expectEqual(@as(usize, 2), runs.len);
        try std.testing.expectEqual(ztext.tag("Hebr"), runs[0].script);
        try std.testing.expectEqual(ztext.tag("Arab"), runs[1].script);
        try std.testing.expectEqualStrings("שלום", text[runs[0].offset..][0..runs[0].length]);
        try std.testing.expect(runs[1].offset < runs[0].offset);
    }

    // Nested directions: Latin embedded in an RTL paragraph gets level 2.
    {
        const text = "مرحبا Hello שלום";
        const paragraph = try ztext.Paragraph.init(text, .{});
        defer paragraph.deinit();

        const runs = paragraph.shapingRuns();
        try std.testing.expect(runs.len >= 3);
        var saw_embedded_latin = false;
        for (runs) |run| {
            if (run.level == 2) {
                saw_embedded_latin = true;
                try std.testing.expectEqual(ztext.tag("Latn"), run.script);
                try std.testing.expectEqual(ztext.Direction.ltr, ztext.runDirection(run.level));
            }
        }
        try std.testing.expect(saw_embedded_latin);
    }

    // Whatever the text, the shaping runs must tile it exactly once -- they
    // are what a layout engine draws, so a gap is dropped text and an overlap
    // is doubled text.
    for ([_][]const u8{
        "Hello مرحبا world",
        "مرحبا שלום",
        "Hello Ελληνικά мир",
        "مرحبا Hello שלום",
        "plain latin only",
        "",
    }) |text| {
        const paragraph = try ztext.Paragraph.init(text, .{});
        defer paragraph.deinit();

        var covered = std.mem.zeroes([64]bool);
        for (paragraph.shapingRuns()) |run| {
            try std.testing.expect(run.length > 0);
            for (run.offset..run.offset + run.length) |i| {
                try std.testing.expect(!covered[i]);
                covered[i] = true;
            }
        }
        for (covered[0..paragraph.length()]) |byte| try std.testing.expect(byte);
    }
}

test "the documented pipeline shapes every run of a mixed paragraph" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // Exactly the loop the README and the module doc comment show.
    const latin = try fixture.face(fonts.latin);
    defer latin.deinit();
    const arabic = try fixture.face(fonts.arabic);
    defer arabic.deinit();

    const text = "Hello مرحبا world";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    var total: usize = 0;
    for (paragraph.shapingRuns()) |run| {
        const face = if (run.script == ztext.tag("Arab")) arabic else latin;
        const glyphs = try fixture.shaper.shape(face, text[run.offset..][0..run.length], .{
            .direction = ztext.runDirection(run.level),
            .script = run.script,
        });
        for (glyphs) |glyph| {
            try std.testing.expect(glyph.cluster < run.length);
        }
        total += glyphs.len;
    }
    try std.testing.expect(total >= 17);
}

//=============================================================================
// OpenType metrics, named instances, variation sequences
//=============================================================================

// The design values below are the fonts' own, read out of their OS/2, post and
// hhea tables; the expected pixel value is the design value scaled by
// ppem/upem, and both fonts here are 1000 upem. Written that way rather than
// as bare numbers so a reader can check the arithmetic against the font.
const design_to_px = @as(f32, @floatFromInt(ppem)) / 1000.0;

test "the OpenType metrics a font declares" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // The same `post` table through two libraries, and they do NOT agree --
    // which is the thing worth pinning. HarfBuzz reports underlinePosition as
    // the font stores it, the TOP edge of the stroke; FreeType subtracts half
    // the thickness to get the CENTRE. Noto Sans stores -100 and 50, so the
    // two answers are -100 and -125 design units, and a caller drawing a
    // rectangle needs to know which edge it was handed.
    const metrics = try face.metrics();
    const top_edge = try face.metric(.underline_offset);
    try std.testing.expectApproxEqAbs(@as(f32, -100) * design_to_px, top_edge, 0.05);
    try std.testing.expectApproxEqAbs(
        @as(f32, -125) * design_to_px,
        metrics.underline_position,
        0.05,
    );
    try std.testing.expectApproxEqAbs(
        top_edge - metrics.underline_thickness / 2.0,
        metrics.underline_position,
        0.05,
    );
    // The thickness itself is the one number both read the same way.
    try std.testing.expectApproxEqAbs(
        metrics.underline_thickness,
        try face.metric(.underline_size),
        0.05,
    );

    // OS/2 values FreeType never puts on the FT_Size, which is the reason this
    // entry point exists at all.
    try std.testing.expectApproxEqAbs(
        @as(f32, 536) * design_to_px,
        try face.metric(.x_height),
        0.05,
    );
    try std.testing.expectApproxEqAbs(
        @as(f32, 714) * design_to_px,
        try face.metric(.cap_height),
        0.05,
    );
    try std.testing.expectApproxEqAbs(
        @as(f32, 50) * design_to_px,
        try face.metric(.strikeout_size),
        0.05,
    );
    // Positive: the offset is measured UP from the baseline, which is the
    // font's own convention and is kept rather than flipped.
    try std.testing.expectApproxEqAbs(
        @as(f32, 322) * design_to_px,
        try face.metric(.strikeout_offset),
        0.05,
    );
    try std.testing.expect(try face.metric(.strikeout_offset) > 0);
    try std.testing.expect(try face.metric(.horizontal_descender) < 0);

    // Noto Sans sets USE_TYPO_METRICS and its sTypoAscender equals its `hhea`
    // ascender, so the two tables agree here and this pair does NOT exercise
    // the disagreement the header describes -- no vendored font does. What it
    // does show is the other difference: FreeType rounds the ascender onto a
    // whole pixel when it scales it onto the FT_Size and HarfBuzz does not, so
    // the two answers differ by less than one pixel and neither is wrong.
    const hb_ascender = try face.metric(.horizontal_ascender);
    try std.testing.expectApproxEqAbs(@as(f32, 1069) * design_to_px, hb_ascender, 0.05);
    try std.testing.expect(@abs(metrics.ascender - hb_ascender) < 1.0);
}

test "a metric the font does not declare is an answer, not a failure" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    // No vendored font has a `vhea`, so every vertical metric is genuinely
    // absent -- and a 0 could not be told from a font that declares zero.
    const latin = try fixture.face(fonts.latin);
    defer latin.deinit();
    try std.testing.expectError(ztext.Error.Unsupported, latin.metric(.vertical_ascender));
    try std.testing.expectError(ztext.Error.Unsupported, latin.metric(.vertical_descender));
    try std.testing.expectError(ztext.Error.Unsupported, latin.metric(.vertical_caret_rise));

    // Noto Naskh Arabic carries OS/2 version 4 with sxHeight and sCapHeight
    // both 0, which HarfBuzz reads as "not declared" rather than as zero
    // heights -- an Arabic font has no x-height to declare.
    const arabic = try fixture.face(fonts.arabic);
    defer arabic.deinit();
    try std.testing.expectError(ztext.Error.Unsupported, arabic.metric(.x_height));
    try std.testing.expectError(ztext.Error.Unsupported, arabic.metric(.cap_height));

    // The same font still declares everything else.
    try std.testing.expect(try arabic.metric(.strikeout_size) > 0);

    // With the fallback there is always an answer, and it is a plausible one:
    // an x-height is a positive fraction of the em, not zero and not the em.
    const fallback = try arabic.metricWithFallback(.x_height);
    try std.testing.expect(fallback > 0);
    try std.testing.expect(fallback < @as(f32, @floatFromInt(ppem)));
    // And a vertical ascender exists even for a font with no `vhea`.
    const vertical = try latin.metricWithFallback(.vertical_ascender);
    try std.testing.expect(vertical != 0);
}

test "metrics follow the axes" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.variable, 0);
    defer font.deinit();
    const face = try font.face(0, ppem);
    defer face.deinit();

    // This font has no MVAR, so its x-height does not move -- which is the
    // point of asserting it rather than a change: what must hold is that the
    // value stays a real reading of the font at the current instance, and a
    // reader who expected movement can see here that MVAR is what provides it.
    try font.setNamedInstance(0);
    const thin = try face.metric(.x_height);
    try font.setNamedInstance(8);
    const black = try face.metric(.x_height);
    try std.testing.expectApproxEqAbs(@as(f32, 536) * design_to_px, thin, 0.05);
    try std.testing.expectApproxEqAbs(thin, black, 0.001);
}

test "a metric this build does not name is rejected rather than passed through" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    // Zig cannot build a `Metric` this build has no name for, so the check
    // that matters is exercised from C; see tests/c_smoke.c. What can be shown
    // here is that every name this build does have reaches HarfBuzz, which is
    // the other half of the same guarantee: nothing in the enum is a tag
    // ztextFaceMetric refuses.
    var named: usize = 0;
    inline for (@typeInfo(ztext.Metric).@"enum".fields) |field| {
        const metric: ztext.Metric = @enumFromInt(field.value);
        // Never InvalidArgument: that is reserved for a tag this build does
        // not name, and every one of these is named by definition. Unsupported
        // is a real answer and says the font is quiet about this metric.
        if (face.metric(metric)) |_| {} else |e| {
            if (e != ztext.Error.Unsupported) return e;
        }
        named += 1;
    }
    try std.testing.expectEqual(@as(usize, 28), named);
}

test "named instances are the points in the axis space the designers chose" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.variable, 0);
    defer font.deinit();

    try std.testing.expectEqual(@as(u32, 9), font.namedInstanceCount());

    // Coordinates come back in axis order: wght first, wdth second.
    var coords: [2]f32 = undefined;
    const regular = try font.namedInstanceCoords(3, &coords);
    try std.testing.expectEqual(@as(usize, 2), regular.len);
    try std.testing.expectApproxEqAbs(@as(f32, 400), regular[0], 0.01);
    try std.testing.expectApproxEqAbs(@as(f32, 100), regular[1], 0.01);

    // The name is decoded out of the font's UTF-16BE `name` table, so a
    // caller never meets one.
    var buffer: [32]u8 = undefined;
    try std.testing.expectEqualStrings("Regular", try font.namedInstanceName(3, &buffer));
    try std.testing.expectEqualStrings("Thin", try font.namedInstanceName(0, &buffer));
    try std.testing.expectEqualStrings("Black", try font.namedInstanceName(8, &buffer));

    // Asking for the length first, then a buffer that is exactly one byte
    // short of the name plus its NUL.
    try std.testing.expectEqual(@as(usize, 7), try font.namedInstanceNameLen(3));
    var exact: [8]u8 = undefined;
    try std.testing.expectEqualStrings("Regular", try font.namedInstanceName(3, &exact));
    var tight: [7]u8 = undefined;
    try std.testing.expectError(
        ztext.Error.BufferTooSmall,
        font.namedInstanceName(3, &tight),
    );

    // A buffer shorter than the axis count is refused rather than half filled.
    var one: [1]f32 = undefined;
    try std.testing.expectError(
        ztext.Error.BufferTooSmall,
        font.namedInstanceCoords(3, &one),
    );

    // One past the end, on every entry point that takes an index.
    try std.testing.expectError(ztext.Error.InvalidArgument, font.namedInstanceCoords(9, &coords));
    try std.testing.expectError(ztext.Error.InvalidArgument, font.namedInstanceName(9, &buffer));
    try std.testing.expectError(ztext.Error.InvalidArgument, font.namedInstanceNameLen(9));
    try std.testing.expectError(ztext.Error.InvalidArgument, font.setNamedInstance(9));
}

test "a static font names no instances and refuses to pretend otherwise" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.hebrew, 0);
    defer font.deinit();

    try std.testing.expectEqual(@as(u32, 0), font.namedInstanceCount());
    var coords: [2]f32 = undefined;
    var buffer: [32]u8 = undefined;
    try std.testing.expectError(ztext.Error.InvalidArgument, font.namedInstanceCoords(0, &coords));
    try std.testing.expectError(ztext.Error.InvalidArgument, font.namedInstanceName(0, &buffer));
    try std.testing.expectError(ztext.Error.InvalidArgument, font.setNamedInstance(0));
}

test "choosing a named instance moves the axes and every face with them" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const font = try fixture.library.createFont(fonts.variable, 0);
    defer font.deinit();
    const face = try font.face(0, ppem);
    defer face.deinit();

    try font.setNamedInstance(0); // Thin
    try std.testing.expectApproxEqAbs(@as(f32, 100), try font.variation(0), 0.01);
    try std.testing.expectApproxEqAbs(@as(f32, 100), try font.variation(1), 0.01);
    var thin: f32 = 0;
    for (try fixture.shaper.shape(face, variable_word, variable_params)) |glyph| {
        thin += glyph.x_advance;
    }

    try font.setNamedInstance(8); // Black
    try std.testing.expectApproxEqAbs(@as(f32, 900), try font.variation(0), 0.01);
    try std.testing.expectApproxEqAbs(@as(f32, 100), try font.variation(1), 0.01);
    var black: f32 = 0;
    for (try fixture.shaper.shape(face, variable_word, variable_params)) |glyph| {
        black += glyph.x_advance;
    }

    // Heavier is wider, through HVAR, on a face that already existed when the
    // instance changed.
    try std.testing.expect(black > thin);

    // And it is the same commit path as setVariations: naming the instance's
    // coordinates by tag lands in the same place.
    try font.setVariations(&.{
        .{ .tag = ztext.tag("wght"), .value = 900 },
        .{ .tag = ztext.tag("wdth"), .value = 100 },
    });
    var by_tag: f32 = 0;
    for (try fixture.shaper.shape(face, variable_word, variable_params)) |glyph| {
        by_tag += glyph.x_advance;
    }
    try std.testing.expectApproxEqAbs(black, by_tag, 0.001);
}

test "a variation sequence names its own glyph, the base glyph, or none" {
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const gpa = std.testing.allocator;

    const alef: u21 = '\u{5D0}';
    const bet: u21 = '\u{5D1}';
    const gimel: u21 = '\u{5D2}';

    // The plain font first: it has no format-14 subtable, so every sequence
    // is 0. Asserted rather than assumed -- it is what makes the fixture below
    // the thing that changes the answers.
    const plain = try fixture.library.createFont(fonts.hebrew, 0);
    defer plain.deinit();
    try std.testing.expectEqual(@as(u32, 0), plain.variantGlyphIndex(alef, 0xFE00));

    const bet_glyph: u16 = @intCast(plain.glyphIndex(bet));
    try std.testing.expect(bet_glyph != 0);

    const bytes = try fonts.withVariationSequences(gpa, fonts.hebrew, &.{
        // U+05D0 U+FE00 gets a glyph of its own -- bet's, so the test can tell
        // it apart from alef's.
        .{ .base = alef, .selector = 0xFE00, .glyph = bet_glyph },
        // U+05D1 U+FE01 is recorded as the DEFAULT: the font draws it, with
        // bet's own glyph, and stores no glyph for it.
        .{ .base = bet, .selector = 0xFE01, .glyph = 0 },
    });
    defer gpa.free(bytes);

    const font = try fixture.library.createFont(bytes, 0);
    defer font.deinit();

    // The rebuilt cmap must still be the cmap: every letter this font maps has
    // to map to the same glyph it did before.
    var codepoint: u21 = 0x5D0;
    while (codepoint <= 0x5EA) : (codepoint += 1) {
        try std.testing.expectEqual(plain.glyphIndex(codepoint), font.glyphIndex(codepoint));
    }

    // A sequence with a glyph of its own.
    try std.testing.expectEqual(@as(u32, bet_glyph), font.variantGlyphIndex(alef, 0xFE00));
    try std.testing.expect(font.variantGlyphIndex(alef, 0xFE00) != font.glyphIndex(alef));

    // A sequence recorded as the default: the base character's own glyph,
    // resolved through the Unicode cmap, NOT 0.
    try std.testing.expectEqual(font.glyphIndex(bet), font.variantGlyphIndex(bet, 0xFE01));

    // The three ways to get 0, all of which mean "this font does not draw this
    // pair": a selector the font never mentions, a base character the selector
    // does not list, and the plain font above with no subtable at all.
    try std.testing.expectEqual(@as(u32, 0), font.variantGlyphIndex(alef, 0xFE02));
    try std.testing.expectEqual(@as(u32, 0), font.variantGlyphIndex(gimel, 0xFE00));

    // The base character on its own is unaffected by any of it.
    try std.testing.expectEqual(plain.glyphIndex(alef), font.glyphIndex(alef));
}

test "golden: the autohinter's coverage comes from GSUB, not the cmap alone" {
    // FreeType gives every glyph a STYLE -- a script, a width, a hinting mode
    // -- and the style is what picks the blue zones the autohinter snaps
    // outlines to. There are two ways to decide which glyph belongs to which
    // script, and they do not cover the same glyphs:
    //
    //   * the character map, which reaches only the glyphs some character
    //     names;
    //   * GSUB, which also reaches the glyphs those characters SUBSTITUTE to.
    //
    // The second set is what shaping produces and the first does not contain:
    // an Arabic medial form, a ligature, an Indic conjunct. Through the
    // character map alone each of those falls into a styleless default with no
    // blue zones, so the glyphs a text renderer actually draws are the ones
    // hinted worst. ffi/ztext_ftoption.h defines FT_CONFIG_OPTION_USE_HARFBUZZ
    // so that it is the second, and this test is what holds that macro down.
    //
    // It matters at `light` in particular, which is the autohinter and nothing
    // else for a TrueType face: FT_LOAD_TARGET_LIGHT falls through to the
    // autohinter unless the driver hints lightly itself, and FreeType's CFF
    // driver is the only one that says it does.
    //
    // WHY A WHOLE-FONT SWEEP AND NOT A GLYPH. The first version of this test
    // pinned two rasters: a glyph only shaping can reach, and the isolated
    // form of U+0628. Both are byte-identical with the macro on and off, and
    // the mutation harness duly reported the guard as a hole. What the macro
    // moves is a scattered minority of each font -- so the golden has to be
    // the sweep the differential was measured in. Every one of the three
    // digests below, and every ink total, differs between the two arms.
    const fixture = try Fixture.init();
    defer fixture.deinit();

    const expected = .{
        .{ "arabic", fonts.arabic, LightSweep{
            .glyphs = 1415,
            .refused = 2,
            .first_refused = 262,
            .digest = 0x164ded98f21af7e1,
            .ink = 5415653,
        } },
        .{ "hebrew", fonts.hebrew, LightSweep{
            .glyphs = 151,
            .refused = 0,
            .first_refused = 0,
            .digest = 0x9bdaf335a5a9b362,
            .ink = 342784,
        } },
        .{ "latin", fonts.latin, LightSweep{
            .glyphs = 3884,
            .refused = 0,
            .first_refused = 0,
            .digest = 0xda4e410f0897e11a,
            .ink = 17511770,
        } },
    };

    inline for (expected) |entry| {
        const face = try fixture.faceAt(entry[1], 12.0);
        defer face.deinit();
        try std.testing.expectEqual(entry[2], try lightSweep(face));
    }
}

/// Every glyph of a face rendered with `light` hinting, reduced to numbers a
/// golden can hold.
///
/// `ink` is carried beside `digest` because a digest can only ever say that
/// something moved. Two ink totals are a direction and a magnitude, which is
/// most of what a reader of a failing golden wants.
const LightSweep = struct {
    glyphs: u32,
    /// Glyphs FreeType's rasteriser declines outright. Pinned rather than
    /// tolerated: a refusal appearing or disappearing is a change in what this
    /// package can draw, and it should not be able to hide inside a digest.
    refused: u32,
    /// The lowest id of them, for a reader; the rest are held by `digest`,
    /// which every glyph contributes to in id order -- a refusal included.
    first_refused: u32,
    digest: u64,
    ink: u64,
};

fn lightSweep(face: ztext.Face) !LightSweep {
    const metrics = try face.metrics();
    var sweep = LightSweep{
        .glyphs = metrics.num_glyphs,
        .refused = 0,
        .first_refused = 0,
        .digest = 0,
        .ink = 0,
    };
    var hasher = std.hash.Wyhash.init(0);
    var glyph: u32 = 1;
    while (glyph < metrics.num_glyphs) : (glyph += 1) {
        const bitmap = face.renderGlyph(glyph, .a8, .light, 0, 0) catch |failure| {
            // One kind of refusal, and it comes from FreeType's rasteriser
            // rather than from ztext. Asserted so that a NEW failure mode
            // cannot be absorbed by the count.
            try std.testing.expectEqual(ztext.Error.RenderFailed, failure);
            if (sweep.refused == 0) sweep.first_refused = glyph;
            sweep.refused += 1;
            hasher.update("refused");
            continue;
        };
        hasher.update(std.mem.asBytes(&bitmap.width));
        hasher.update(std.mem.asBytes(&bitmap.height));
        hasher.update(std.mem.asBytes(&bitmap.left));
        hasher.update(std.mem.asBytes(&bitmap.top));
        const rows = ztext.bitmapRows(bitmap) orelse continue;
        const pitch: usize = @intCast(bitmap.pitch);
        var row: usize = 0;
        while (row < bitmap.height) : (row += 1) {
            const line = rows[row * pitch ..][0..bitmap.width];
            hasher.update(line);
            for (line) |byte| sweep.ink += byte;
        }
    }
    sweep.digest = hasher.final();
    return sweep;
}

test "handles have no destruction order: a library may go before its fonts" {
    // The order below is the one FreeType forbids. FT_Done_Library destroys
    // every FT_Face still registered with it, so a library freed at the
    // `library.deinit()` line would leave `font` and `face` reading memory
    // FreeType had already returned -- and every assertion after it would be
    // reading whatever the allocator put there next.
    //
    // ztext counts live fonts instead and defers the library's teardown to
    // whichever handle is released last, so the assertions after that line
    // are the whole test.
    try warmProcessCaches();
    try ztext.setAllocator(std.testing.allocator);
    defer ztext.resetAllocator();

    const library = try ztext.Library.init();
    const font = try library.createFont(fonts.latin, 0);
    const face = try font.face(0, 16);

    library.deinit();

    // A working font and a working face: the FT_Library underneath them is
    // still there, and so is everything they own.
    try std.testing.expectEqualStrings("Noto Sans", font.familyName());
    const glyph = font.glyphIndex('g');
    try std.testing.expect(glyph != 0);
    const bitmap = try face.renderGlyph(glyph, .a8, .normal, 0, 0);
    try std.testing.expect(bitmap.width > 0 and bitmap.height > 0);

    // What order-free destruction does NOT extend to: building something new
    // on a handle the caller has let go of is refused rather than undefined,
    // the same refusal a destroyed font gives.
    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        library.createFont(fonts.latin, 0),
    );

    // And the last release frees the library. std.testing.allocator reports
    // the leak if it does not, which is what makes this a gate rather than a
    // demonstration.
    face.deinit();
    font.deinit();
}

test "a face's glyph buffer belongs to its library, not to whatever is installed" {
    // The allocator seam has two halves. Which allocator FREES a block was
    // already settled -- the block's header names it -- and a test above
    // covers it. This is the other half: which allocator MAKES it.
    //
    // A face's glyph buffer is the sharp case. It is allocated lazily, the
    // first time something is drawn, which can be long after the face was
    // made and under a completely different allocator. Charging it there
    // would put one handle's memory in two heaps, and the only symptom would
    // be a host's own accounting quietly not adding up.
    var first_state: std.heap.DebugAllocator(.{ .enable_memory_limit = true }) = .init;
    var second_state: std.heap.DebugAllocator(.{ .enable_memory_limit = true }) = .init;
    const first = first_state.allocator();
    const second = second_state.allocator();

    try warmProcessCaches();

    try ztext.setAllocator(first);
    const library = try ztext.Library.init();
    const font = try library.createFont(fonts.latin, 0);
    const face = try font.face(0, 32);

    // Everything from here is nominally the second allocator's.
    try ztext.setAllocator(second);
    const before_first = first_state.total_requested_bytes;
    const before_second = second_state.total_requested_bytes;

    const glyph = font.glyphIndex('g');
    const bitmap = try face.renderGlyph(glyph, .a8, .normal, 0, 0);
    try std.testing.expect(bitmap.width > 0 and bitmap.height > 0);

    // Rendering is FreeType and ztext and nothing else -- no HarfBuzz call is
    // reachable from here -- so the second allocator must not have been asked
    // for a single byte, and the first must have been asked for the buffer.
    try std.testing.expectEqual(before_second, second_state.total_requested_bytes);
    try std.testing.expect(first_state.total_requested_bytes > before_first);

    face.deinit();
    font.deinit();
    library.deinit();

    ztext.resetAllocator();
    try std.testing.expectEqual(std.heap.Check.ok, first_state.deinit());
    try std.testing.expectEqual(std.heap.Check.ok, second_state.deinit());
}

/// UTF-16 for `utf8`, in NATIVE byte order -- which is what ztext takes, and
/// what `std.unicode.utf8ToUtf16Le` would not give on a big-endian host.
fn toUtf16(out: []u16, utf8: []const u8) ![]const u16 {
    var count: usize = 0;
    var it = (try std.unicode.Utf8View.init(utf8)).iterator();
    while (it.nextCodepoint()) |cp| {
        if (cp < 0x10000) {
            out[count] = @intCast(cp);
            count += 1;
        } else {
            const rest = cp - 0x10000;
            out[count] = @intCast(0xD800 + (rest >> 10));
            out[count + 1] = @intCast(0xDC00 + (rest & 0x3FF));
            count += 2;
        }
    }
    return out[0..count];
}

/// UTF-32 for `utf8`: one unit per character, so this is the codepoint list.
fn toUtf32(out: []u32, utf8: []const u8) ![]const u32 {
    var count: usize = 0;
    var it = (try std.unicode.Utf8View.init(utf8)).iterator();
    while (it.nextCodepoint()) |cp| : (count += 1) out[count] = cp;
    return out[0..count];
}

test "one text, three encodings: the same answer in different units" {
    // The gate on M4. Every one of the three upstreams has a separate entry
    // point per encoding -- SheenBidi an SBStringEncoding, libunibreak three
    // functions per algorithm, HarfBuzz three hb_buffer_add_* -- so an
    // encoding can be wired into one of them and not the others, and the
    // symptom is not a crash: it is a paragraph of plausible, wrong levels.
    // Nothing but a differential comparison sees that.
    var fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.hebrew);
    defer face.deinit();

    // Latin, Hebrew, a space run between two right-to-left words, and one
    // ASTRAL character -- the case where the three encodings disagree most
    // about length: four bytes, a surrogate pair, one unit.
    const utf8: []const u8 = "a \u{5E9}\u{5DC}\u{5D5}\u{5DD}  \u{5D0} b\u{1D11E}";
    var utf16_storage: [64]u16 = undefined;
    var utf32_storage: [64]u32 = undefined;
    const utf16 = try toUtf16(&utf16_storage, utf8);
    const utf32 = try toUtf32(&utf32_storage, utf8);

    const p8 = try ztext.Paragraph.init(utf8, .{});
    defer p8.deinit();
    const p16 = try ztext.Paragraph.init(utf16, .{});
    defer p16.deinit();
    const p32 = try ztext.Paragraph.init(utf32, .{});
    defer p32.deinit();

    // Each reports the encoding it was built from and a length in ITS units.
    try std.testing.expectEqual(ztext.Encoding.utf8, p8.encoding());
    try std.testing.expectEqual(ztext.Encoding.utf16, p16.encoding());
    try std.testing.expectEqual(ztext.Encoding.utf32, p32.encoding());
    try std.testing.expectEqual(utf8.len, p8.length());
    try std.testing.expectEqual(utf16.len, p16.length());
    try std.testing.expectEqual(utf32.len, p32.length());
    // The three lengths really are different, or this test proves nothing.
    try std.testing.expect(utf8.len != utf16.len and utf16.len != utf32.len);

    // Everything that is a property of the TEXT rather than of its spelling.
    try std.testing.expectEqual(p8.baseLevel(), p16.baseLevel());
    try std.testing.expectEqual(p8.baseLevel(), p32.baseLevel());
    try std.testing.expectEqual(p8.visualRuns().len, p16.visualRuns().len);
    try std.testing.expectEqual(p8.visualRuns().len, p32.visualRuns().len);
    try std.testing.expectEqual(p8.scriptRuns().len, p16.scriptRuns().len);
    try std.testing.expectEqual(p8.scriptRuns().len, p32.scriptRuns().len);
    try std.testing.expectEqual(p8.shapingRuns().len, p16.shapingRuns().len);
    try std.testing.expectEqual(p8.shapingRuns().len, p32.shapingRuns().len);

    // Per CHARACTER, walked in all three at once: the level UAX #9 resolved
    // and the three UAX #14/#29 boundaries after it have to agree. This is
    // what catches one algorithm wired to the wrong upstream entry point --
    // the counts above would not move.
    {
        var at8: usize = 0;
        var at16: usize = 0;
        var at32: usize = 0;
        var it = (try std.unicode.Utf8View.init(utf8)).iterator();
        while (it.nextCodepoint()) |cp| {
            const len8 = std.unicode.utf8CodepointSequenceLength(cp) catch
                unreachable;
            const len16: usize = if (cp < 0x10000) 1 else 2;

            try std.testing.expectEqual(p8.levels()[at8], p16.levels()[at16]);
            try std.testing.expectEqual(p8.levels()[at8], p32.levels()[at32]);

            // The entry at i describes the boundary AFTER unit i, so the
            // character's own boundary is at its LAST unit in each encoding.
            const end8 = at8 + len8 - 1;
            const end16 = at16 + len16 - 1;
            inline for (.{ "lineBreaks", "graphemeBreaks", "wordBreaks" }) |name| {
                const b8 = @field(ztext.Paragraph, name)(p8)[end8];
                const b16 = @field(ztext.Paragraph, name)(p16)[end16];
                const b32 = @field(ztext.Paragraph, name)(p32)[at32];
                try std.testing.expectEqual(b8, b16);
                try std.testing.expectEqual(b8, b32);
            }

            at8 += len8;
            at16 += len16;
            at32 += 1;
        }
        try std.testing.expectEqual(utf8.len, at8);
        try std.testing.expectEqual(utf16.len, at16);
        try std.testing.expectEqual(utf32.len, at32);
    }

    // And the glyphs. Cluster values are code-unit offsets and so differ by
    // construction; everything else -- which glyph, where it goes -- must be
    // identical, because it is the same text.
    for (p8.shapingRuns(), p16.shapingRuns(), p32.shapingRuns()) |r8, r16, r32| {
        const g8 = try fixture.shaper.shapeRun(face, p8, r8, .{});
        var copied: [64]ztext.Glyph = undefined;
        @memcpy(copied[0..g8.len], g8);
        const kept = copied[0..g8.len];

        const g16 = try fixture.shaper.shapeRun(face, p16, r16, .{});
        try std.testing.expectEqual(kept.len, g16.len);
        for (kept, g16) |a, b| {
            try std.testing.expectEqual(a.glyph_id, b.glyph_id);
            try std.testing.expectEqual(a.flags, b.flags);
            try std.testing.expectEqual(a.x_advance, b.x_advance);
            try std.testing.expectEqual(a.y_advance, b.y_advance);
            try std.testing.expectEqual(a.x_offset, b.x_offset);
            try std.testing.expectEqual(a.y_offset, b.y_offset);
        }

        const g32 = try fixture.shaper.shapeRun(face, p32, r32, .{});
        try std.testing.expectEqual(kept.len, g32.len);
        for (kept, g32) |a, b| {
            try std.testing.expectEqual(a.glyph_id, b.glyph_id);
            try std.testing.expectEqual(a.x_advance, b.x_advance);
        }
    }
}

test "a range that would split a character is refused in every encoding" {
    var fixture = try Fixture.init();
    defer fixture.deinit();

    // U+1D11E is one UTF-32 unit, a surrogate pair in UTF-16 and four bytes
    // in UTF-8, so "one past the start of the last character" is a different
    // index in each -- and has to be refused in the two where it is inside a
    // character, allowed in the one where there is nothing to split.
    const utf8: []const u8 = "ab\u{1D11E}";
    var utf16_storage: [8]u16 = undefined;
    const utf16 = try toUtf16(&utf16_storage, utf8);

    const p8 = try ztext.Paragraph.init(utf8, .{});
    defer p8.deinit();
    const p16 = try ztext.Paragraph.init(utf16, .{});
    defer p16.deinit();

    try std.testing.expectError(ztext.Error.InvalidArgument, p8.line(3, 1));
    try std.testing.expectError(ztext.Error.InvalidArgument, p16.line(3, 1));
    const whole8 = try p8.line(0, p8.length());
    whole8.deinit();
    const whole16 = try p16.line(0, p16.length());
    whole16.deinit();

    // UTF-32 has nothing to split: every index is a boundary.
    var utf32_storage: [8]u32 = undefined;
    const utf32 = try toUtf32(&utf32_storage, utf8);
    const p32 = try ztext.Paragraph.init(utf32, .{});
    defer p32.deinit();
    const any = try p32.line(1, 1);
    any.deinit();
}

test "a paragraph run is shaped from the paragraph's own text" {
    // The gate on M5, and on the class of bug the entry point removes: a run
    // list applied to a buffer that is not the one it describes. Here there
    // is no second buffer to get wrong -- the paragraph is the text.
    var fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.hebrew);
    defer face.deinit();

    const text = "abc \u{5E9}\u{5DC}\u{5D5}\u{5DD} def";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    for (paragraph.shapingRuns()) |run| {
        const through_paragraph =
            try fixture.shaper.shapeRun(face, paragraph, run, .{});
        var kept: [64]ztext.Glyph = undefined;
        @memcpy(kept[0..through_paragraph.len], through_paragraph);

        // The same run through the borrowed-text entry point, with the
        // direction and script the run carries spelled out by hand. Identical
        // glyphs: shapeRun is the same shape, not a different one.
        const through_text = try fixture.shaper.shapeRange(
            face,
            text,
            run.offset,
            run.length,
            .{
                .direction = if (run.level % 2 == 0) .ltr else .rtl,
                .script = run.script,
            },
        );
        try std.testing.expectEqualSlices(
            ztext.Glyph,
            kept[0..through_paragraph.len],
            through_text,
        );
    }
}

test "a paragraph run refuses a direction or script the caller also set" {
    // Two sources for one fact is the defect class this package spends most
    // of its guards on. The run carries direction and script; a Params that
    // also carries them is refused rather than quietly losing.
    var fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.latin);
    defer face.deinit();

    const paragraph = try ztext.Paragraph.init("Hello", .{});
    defer paragraph.deinit();
    const run = paragraph.shapingRuns()[0];

    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        fixture.shaper.shapeRun(face, paragraph, run, .{ .direction = .ltr }),
    );
    try std.testing.expectError(
        ztext.Error.InvalidArgument,
        fixture.shaper.shapeRun(face, paragraph, run, .{ .script = ztext.tag("Latn") }),
    );

    // Everything else in Params still applies.
    _ = try fixture.shaper.shapeRun(face, paragraph, run, .{
        .cluster_level = .monotone_characters,
    });
}

test "a run built by hand cannot reach outside its paragraph" {
    var fixture = try Fixture.init();
    defer fixture.deinit();
    const face = try fixture.face(fonts.hebrew);
    defer face.deinit();

    // Runs a paragraph produced are inside it and on character boundaries;
    // a struct a caller filled in is neither by construction.
    const text = "ab\u{5D0}cd";
    const paragraph = try ztext.Paragraph.init(text, .{});
    defer paragraph.deinit();

    const bad = [_][2]u32{
        .{ 99, 1 }, // starts past the end
        .{ 0, 99 }, // runs past the end
        .{ 3, 1 }, // starts inside a character
        .{ 0, 3 }, // ends inside a character
    };
    for (bad) |range| {
        const run: ztext.ShapingRun =
            .{ .offset = range[0], .length = range[1], .script = 0, .level = 0 };
        try std.testing.expectError(
            ztext.Error.InvalidArgument,
            fixture.shaper.shapeRun(face, paragraph, run, .{}),
        );
    }
}

test "a paragraph runs only the segmentation passes it was asked for" {
    // The gate on M6. Every pass costs a walk of libunibreak and a byte per
    // code unit kept for the paragraph's life, and a paragraph used for bidi
    // alone was paying for all three.
    const text = "one two \u{5D0}\u{5D1} three";

    const all = try ztext.Paragraph.init(text, .{});
    defer all.deinit();
    try std.testing.expectEqual(
        @as(u32, @intCast(@intFromEnum(ztext.Segmentation.all))),
        all.segmentation(),
    );
    try std.testing.expectEqual(text.len, all.lineBreaks().len);
    try std.testing.expectEqual(text.len, all.graphemeBreaks().len);
    try std.testing.expectEqual(text.len, all.wordBreaks().len);

    // One pass: the array asked for is identical to the one the full
    // paragraph produced, and the other two do not exist.
    const lines_only = try ztext.Paragraph.init(text, .{
        .segmentation = ztext.segmentation(&.{.lines}),
    });
    defer lines_only.deinit();
    try std.testing.expect(
        ztext.segmentationHas(lines_only.segmentation(), .lines),
    );
    try std.testing.expect(
        !ztext.segmentationHas(lines_only.segmentation(), .words),
    );
    try std.testing.expectEqualSlices(
        ztext.Break,
        all.lineBreaks(),
        lines_only.lineBreaks(),
    );
    try std.testing.expectEqual(@as(usize, 0), lines_only.graphemeBreaks().len);
    try std.testing.expectEqual(@as(usize, 0), lines_only.wordBreaks().len);

    // And the pass in the MIDDLE of the mask, which is where an array laid
    // out at a fixed offset would read the wrong one.
    const words_only = try ztext.Paragraph.init(text, .{
        .segmentation = ztext.segmentation(&.{.words}),
    });
    defer words_only.deinit();
    try std.testing.expectEqualSlices(
        ztext.Break,
        all.wordBreaks(),
        words_only.wordBreaks(),
    );
    try std.testing.expectEqual(@as(usize, 0), words_only.lineBreaks().len);
    try std.testing.expectEqual(@as(usize, 0), words_only.graphemeBreaks().len);

    // None at all: the bidi analysis is untouched by the choice.
    const none = try ztext.Paragraph.init(text, .{
        .segmentation = ztext.segmentation(&.{}),
    });
    defer none.deinit();
    try std.testing.expectEqual(@as(usize, 0), none.lineBreaks().len);
    try std.testing.expectEqual(@as(usize, 0), none.graphemeBreaks().len);
    try std.testing.expectEqual(@as(usize, 0), none.wordBreaks().len);
    try std.testing.expectEqual(all.baseLevel(), none.baseLevel());
    try std.testing.expectEqual(
        all.shapingRuns().len,
        none.shapingRuns().len,
    );
    try std.testing.expectEqualSlices(u8, all.levels(), none.levels());
}

test "without the grapheme pass a caret has nowhere to move" {
    const text = "abc";
    const paragraph = try ztext.Paragraph.init(text, .{
        .segmentation = ztext.segmentation(&.{.lines}),
    });
    defer paragraph.deinit();

    // Not "jump to the end", which is what an absent array used to mean.
    try std.testing.expectEqual(@as(usize, 1), paragraph.nextGrapheme(1));
    try std.testing.expectEqual(@as(usize, 2), paragraph.previousGrapheme(2));
}

test "a segmentation bit this build has no name for is refused" {
    // The same contract as an unknown encoding: a consumer compiled against a
    // newer header is refused rather than quietly given less than it asked
    // for. Through the C entry point, because the Zig mask cannot spell it.
    var handle: *ztext.c.Paragraph = undefined;
    try std.testing.expectEqual(
        ztext.c.Result.invalid_argument,
        ztext.c.ztextParagraphCreate("abc", 3, .utf8, .auto, 0x8, &handle),
    );
}
