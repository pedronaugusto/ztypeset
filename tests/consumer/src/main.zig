//! A downstream consumer of the `ztypeset` Zig module.
const std = @import("std");
const ztypeset = @import("ztypeset");

const fonts = @import("fonts");

pub fn main() !void {
    var gpa_state = std.heap.DebugAllocator(.{}){};
    defer std.debug.assert(gpa_state.deinit() == .ok);

    try ztypeset.setAllocator(gpa_state.allocator());
    defer ztypeset.resetAllocator();

    const library = try ztypeset.Library.init();
    defer library.deinit();
    const font = try library.createFont(fonts.hebrew, 0);
    defer font.deinit();
    const face = try font.face(0, 24);
    defer face.deinit();

    const shaper = try ztypeset.Shaper.init();
    defer shaper.deinit();

    const paragraph = try ztypeset.Paragraph.init("a שלום b", .{});
    defer paragraph.deinit();
    if (paragraph.visualRuns().len != 3) return error.UnexpectedRunCount;
    // At least one per visual run; a run whose script changes mid-way splits
    // further, which is the whole point of the list.
    if (paragraph.shapingRuns().len < paragraph.visualRuns().len) {
        return error.UnexpectedShapingRunCount;
    }

    // The documented pipeline, end to end, as a consumer would write it.
    var glyph_total: usize = 0;
    for (paragraph.shapingRuns()) |run| {
        // The paragraph owns the text, so there is no slice to get wrong and
        // every run is shaped with the characters either side of it in view.
        const glyphs = try shaper.shapeRun(face, paragraph, run, .{});
        if (shaper.direction() != ztypeset.runDirection(run.level)) {
            return error.RunDirectionIgnored;
        }
        for (glyphs) |glyph| {
            const bitmap = try face.renderGlyph(glyph.glyph_id, .a8, .light, 0, 0);
            if (ztypeset.bitmapRows(bitmap)) |rows| glyph_total += rows.len;
        }
    }
    if (glyph_total == 0) return error.NoInk;

    // `{f}` exercises Version.format, which the in-repo suite never reaches.
    std.debug.print(
        "zig consumer ok: ztypeset {f}, freetype {f}, harfbuzz {f}, sheenbidi {f}, unibreak {f}\n",
        .{ ztypeset.version(), ztypeset.freetypeVersion(), ztypeset.harfbuzzVersion(), ztypeset.sheenbidiVersion(), ztypeset.unibreakVersion() },
    );
}
