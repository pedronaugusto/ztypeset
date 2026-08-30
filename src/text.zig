//! How a caller's text reaches the C ABI: a pointer, a length in code units,
//! and which encoding those units are in.
//!
//! The encoding is not a parameter any Zig entry point takes. It is read off
//! the ELEMENT TYPE of the slice -- `u8` is UTF-8, `u16` is UTF-16, `u32` is
//! UTF-32 -- because that is the one piece of information a Zig caller cannot
//! get wrong and a C caller has to be trusted with. Handing `[]const u16` to
//! something expecting UTF-8 does not compile here; across the C ABI the same
//! mistake is a paragraph of plausible, wrong embedding levels.
//!
//! One home for the three-way switch, so an entry point cannot support two
//! encodings and be forgotten for the third.

const std = @import("std");
const c = @import("c.zig");

/// A caller's text as the ABI takes it. `len` is in code units, never bytes.
pub const View = struct {
    ptr: ?*const anyopaque,
    len: usize,
    encoding: c.Encoding,
};

/// The view for `text`: a slice, or a pointer to an array, of `u8`, `u16` or
/// `u32`. String literals are pointers to arrays, which is why both are here.
pub fn view(text: anytype) View {
    const T = @TypeOf(text);
    return switch (@typeInfo(T)) {
        .pointer => |pointer| switch (pointer.size) {
            .slice => of(pointer.child, text),
            .one => switch (@typeInfo(pointer.child)) {
                .array => |array| of(array.child, text),
                else => @compileError(complaint(T)),
            },
            else => @compileError(complaint(T)),
        },
        else => @compileError(complaint(T)),
    };
}

fn complaint(comptime T: type) []const u8 {
    return "ztext: text must be a slice of u8 (UTF-8), u16 (UTF-16) or u32 " ++
        "(UTF-32), or a pointer to an array of one of those; got " ++
        @typeName(T);
}

fn of(comptime Unit: type, text: anytype) View {
    const encoding: c.Encoding = switch (Unit) {
        u8 => .utf8,
        u16 => .utf16,
        u32 => .utf32,
        else => @compileError(complaint(@TypeOf(text))),
    };
    const slice: []const Unit = text;
    return .{ .ptr = slice.ptr, .len = slice.len, .encoding = encoding };
}

test "the encoding comes from the element type" {
    try std.testing.expectEqual(c.Encoding.utf8, view("abc").encoding);
    try std.testing.expectEqual(c.Encoding.utf8, view(@as([]const u8, "abc")).encoding);
    try std.testing.expectEqual(c.Encoding.utf16, view(&[_]u16{ 'a', 'b' }).encoding);
    try std.testing.expectEqual(c.Encoding.utf32, view(&[_]u32{'a'}).encoding);
    try std.testing.expectEqual(@as(usize, 3), view("abc").len);
    // Code UNITS, not bytes: this is the property every offset the C side
    // reports back is counted in.
    try std.testing.expectEqual(@as(usize, 2), view(&[_]u16{ 'a', 'b' }).len);
}
