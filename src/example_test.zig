//! The documented examples are one text, and this is what says so.
//!
//! `examples/quickstart.zig` is a program `zig build test` compiles and runs.
//! Two regions of it, marked `//<<<name` and `//>>>name`, are the source of
//! every copy: the fenced blocks in README.md and the example in
//! `src/ztext.zig`'s module doc. Each test below extracts a region and
//! requires the document to contain it verbatim.
//!
//! It exists because the copies disagreed and nothing could tell. README's
//! loop had been corrected to `shapeRun` -- the whole text in, the run
//! selecting part of it, so HarfBuzz sees the characters either side -- while
//! the module doc still sliced the text and shaped the slice, which is the
//! defect README's own defect list says was fixed. Two homes for one example,
//! neither compiled.
//!
//! Blind spot, stated: this proves the documents quote the program, and the
//! build proves the program compiles and runs. It does not prove the program
//! is a GOOD example, and it does not read the prose around the fences.

const std = @import("std");

const quickstart = @embedFile("example_quickstart");
const readme = @embedFile("example_readme");
const module_doc = @embedFile("example_module_doc");

/// The text between `//<<<name` and `//>>>name`, with the indentation the
/// markers themselves carry removed from every line.
///
/// The markers are inside a function body in the example, and the documents
/// quote it at column zero; dedenting by the marker's own indentation is what
/// makes those the same text rather than two spellings of it.
fn region(allocator: std.mem.Allocator, name: []const u8) ![]u8 {
    const open = try std.fmt.allocPrint(allocator, "//<<<{s}\n", .{name});
    defer allocator.free(open);
    const close = try std.fmt.allocPrint(allocator, "//>>>{s}", .{name});
    defer allocator.free(close);

    const open_at = std.mem.indexOf(u8, quickstart, open) orelse
        return error.NoSuchRegion;
    const body_at = open_at + open.len;
    const close_at = std.mem.indexOfPos(u8, quickstart, body_at, close) orelse
        return error.RegionNotClosed;

    // The marker's indentation: from the last newline before it to the marker.
    const line_at = if (std.mem.lastIndexOfScalar(u8, quickstart[0..open_at], '\n')) |n|
        n + 1
    else
        0;
    const indent = quickstart[line_at..open_at];

    // close_at points at the closing marker; back up over its indentation too.
    const body = quickstart[body_at .. close_at - indent.len];

    var out: std.ArrayList(u8) = .empty;
    errdefer out.deinit(allocator);
    var lines = std.mem.splitScalar(u8, body, '\n');
    while (lines.next()) |line| {
        const stripped = if (std.mem.startsWith(u8, line, indent))
            line[indent.len..]
        else
            line;
        try out.appendSlice(allocator, stripped);
        try out.append(allocator, '\n');
    }
    // splitScalar yields a final empty piece after the trailing newline.
    if (out.items.len > 0) _ = out.pop();
    return out.toOwnedSlice(allocator);
}

fn expectQuotes(document: []const u8, what: []const u8, body: []const u8) !void {
    if (std.mem.indexOf(u8, document, body) != null) return;
    std.debug.print(
        \\
        \\{s} no longer quotes examples/quickstart.zig verbatim.
        \\
        \\The example is the one home for this text. Copy the region back into
        \\the document rather than editing the region to match the document --
        \\the region is what compiles and runs.
        \\
        \\Expected to find:
        \\{s}
        \\
    , .{ what, body });
    return error.DocumentationHasDriftedFromTheExample;
}

test "README.md quotes the usage example verbatim" {
    const body = try region(std.testing.allocator, "usage");
    defer std.testing.allocator.free(body);
    try expectQuotes(readme, "README.md", body);
}

test "README.md quotes the wrapping example verbatim" {
    const body = try region(std.testing.allocator, "wrapping");
    defer std.testing.allocator.free(body);
    try expectQuotes(readme, "README.md", body);
}

test "the module doc quotes the usage example verbatim" {
    const body = try region(std.testing.allocator, "usage");
    defer std.testing.allocator.free(body);

    // The same text as a `//!` doc comment. An empty line carries no trailing
    // space, because `zig fmt` would strip it and the comparison would then
    // fail on whitespace nobody can see.
    var quoted: std.ArrayList(u8) = .empty;
    defer quoted.deinit(std.testing.allocator);
    var lines = std.mem.splitScalar(u8, body, '\n');
    while (lines.next()) |line| {
        try quoted.appendSlice(std.testing.allocator, if (line.len == 0) "//!" else "//! ");
        try quoted.appendSlice(std.testing.allocator, line);
        try quoted.append(std.testing.allocator, '\n');
    }
    if (quoted.items.len > 0) _ = quoted.pop();

    try expectQuotes(module_doc, "src/ztext.zig's module doc", quoted.items);
}
