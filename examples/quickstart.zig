//! The example in README.md and in `src/ztext.zig`'s module doc, as a program
//! that runs.
//!
//! It exists because the two copies of it disagreed. The README's loop was
//! corrected to `shapeRun` -- the run shaped in the context of the whole text,
//! so HarfBuzz can see the characters either side -- and the module doc kept
//! shaping `text[run.offset..][0..run.length]`, which is the bug the README
//! says was fixed. Nothing compiled either one, so nothing could tell.
//!
//! Now there is one copy. The regions between the `//<<<` and `//>>>` markers
//! below are the source of both, and `src/example_test.zig` fails if either
//! document has drifted from them. `zig build test` builds and RUNS this file,
//! so an example that no longer compiles is a red build rather than a reader's
//! afternoon.
//!
//! What is deliberately NOT inside the markers is the scaffolding a host
//! supplies anyway: the allocator, the font bytes, the string, and -- for the
//! second region -- the width decision, which is the one thing only the host
//! can make.

const std = @import("std");
const fonts = @import("fonts");

pub fn main() !void {
    var gpa_state: std.heap.DebugAllocator(.{}) = .init;
    defer std.debug.assert(gpa_state.deinit() == .ok);

    const font_bytes = fonts.latin;
    const text = "Hello \u{395}\u{3bb}\u{3bb}\u{3b7}\u{3bd}\u{3b9}\u{3ba}\u{3ac} \u{43c}\u{438}\u{440}";

    //<<<usage
    const ztext = @import("ztext");

    // Copied, not borrowed: ztext keeps its own copy for as long as any handle
    // can reach it. Installing also warms the caches the upstreams keep for the
    // life of the process, so this allocator only ever sees ztext's working set.
    try ztext.setAllocator(gpa_state.allocator());
    defer ztext.resetAllocator();

    const library = try ztext.Library.init();
    defer library.deinit();

    // The bytes are BORROWED and must outlive the font. Handles have no
    // destruction order: whichever of a pair goes second frees what they share.
    const font = try library.createFont(font_bytes, 0);
    defer font.deinit();

    // A face is the font at one size. Make one per size you draw; a second size
    // costs a size, not another parse.
    const face = try font.face(0, 16);
    defer face.deinit();

    const shaper = try ztext.Shaper.init();
    defer shaper.deinit();

    const paragraph = try ztext.Paragraph.init(text, .auto);
    defer paragraph.deinit();

    // shapingRuns, not visualRuns: one visual run can span several scripts, and
    // HarfBuzz shapes one script at a time.
    for (paragraph.shapingRuns()) |run| {
        // shapeRun, not shape: the PARAGRAPH owns the text, so a run can only be
        // applied to the text it came from, HarfBuzz sees the characters either
        // side of the run, direction and script come from the run itself, and
        // text a paragraph already validated is not validated again per run.
        const glyphs = try shaper.shapeRun(face, paragraph, run, .{});
        for (glyphs) |glyph| {
            const bitmap = try face.renderGlyph(glyph.glyph_id, .a8, .light, 0, 0);
            // ... into your atlas, before the next call on this face.
            _ = bitmap;
        }
    }
    //>>>usage

    //<<<wrapping
    const breaks = paragraph.lineBreaks(); // one entry per code unit
    var start: usize = 0;
    while (start < text.len) {
        // Furthest permitted break that still fits. ztext says where a break is
        // ALLOWED; only you know how wide the box is.
        const end = chooseBreak(breaks, start, box_width);

        const line = try paragraph.line(start, end - start);
        defer line.deinit();
        for (line.shapingRuns()) |run| {
            // Shaped and rendered exactly as above, per line this time.
            const glyphs = try shaper.shapeRun(face, paragraph, run, .{});
            _ = glyphs;
        }
        start = end;
    }
    //>>>wrapping
}

/// The host's decision, which is why it is not in the example: ztext reports
/// where a break is permitted and has no opinion about how wide a line is.
///
/// This one takes the furthest allowed break at or before `start + width`, and
/// falls back to the end of the text when there is none -- a real host would
/// measure the shaped advance instead of counting bytes.
fn chooseBreak(
    breaks: []const @import("ztext").Break,
    start: usize,
    width: usize,
) usize {
    var best: usize = 0;
    var i = start;
    const limit = @min(breaks.len, start + width);
    while (i < limit) : (i += 1) {
        if (breaks[i] != .none) best = i + 1;
    }
    if (best <= start) return @min(breaks.len, start + width);
    return best;
}

/// Bytes, because this example measures nothing. See `chooseBreak`.
const box_width: usize = 12;
