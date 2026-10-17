//! Type information as the code reads it: one list of fields, each with its
//! name and its type or value. Zig 0.17 gives it as parallel lists of names,
//! types and values; these join them again, at compile time.

pub const StructField = struct { name: [:0]const u8, type: type };
pub const EnumField = struct { name: [:0]const u8, value: comptime_int };
pub const Decl = struct { name: [:0]const u8 };

pub fn structFields(comptime T: type) [@typeInfo(T).@"struct".field_names.len]StructField {
    const info = @typeInfo(T).@"struct";
    var out: [info.field_names.len]StructField = undefined;
    for (info.field_names, info.field_types, 0..) |name, Field, i| out[i] = .{ .name = name, .type = Field };
    return out;
}

pub fn enumFields(comptime T: type) [@typeInfo(T).@"enum".field_names.len]EnumField {
    const info = @typeInfo(T).@"enum";
    var out: [info.field_names.len]EnumField = undefined;
    for (info.field_names, info.field_values, 0..) |name, value, i| out[i] = .{ .name = name, .value = value };
    return out;
}

pub fn decls(comptime T: type) [@typeInfo(T).@"struct".decl_names.len]Decl {
    const info = @typeInfo(T).@"struct";
    var out: [info.decl_names.len]Decl = undefined;
    for (info.decl_names, 0..) |name, i| out[i] = .{ .name = name };
    return out;
}

test "fields, values and declarations come back as one list" {
    const std = @import("std");
    const S = struct {
        a: u8,
        b: bool,
        pub const c = 1;
    };
    const E = enum(u8) { x = 3, y = 7 };
    const f = comptime structFields(S);
    try std.testing.expectEqualStrings("b", f[1].name);
    try std.testing.expect(f[1].type == bool);
    try std.testing.expectEqual(7, comptime enumFields(E)[1].value);
    try std.testing.expectEqualStrings("c", (comptime decls(S))[0].name);
}
