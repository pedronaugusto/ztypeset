//! Comptime cross-check: the hand-written externs in `c.zig` against the real
//! C header.
//!
//! `c.zig` is written by hand so the wrapper gets exactly the types it wants
//! and the shipped module never runs translate-c. The cost of hand-writing is
//! drift, and nothing in either compiler notices when this file's twin stops
//! matching `ffi/ztext.h`.
//!
//! This closes that by `@cImport`-ing the header — in a test only, so the
//! shipped module stays translate-c-free — and comparing the two namespaces
//! declaration by declaration. There is **no hand-written list of what to
//! check**: every public declaration in `c.zig` is discovered by reflection,
//! paired with its counterpart by naming convention, and compared. A
//! declaration that fits no category is a compile error rather than a silent
//! pass, so the check cannot quietly stop covering something.
//!
//! The naming conventions are therefore load-bearing, not cosmetic:
//!
//!   * a type `Foo`            pairs with `ZtextFoo`
//!   * a function `ztextFoo`   pairs with itself
//!   * a constant `foo_bar`    pairs with `ZTEXT_FOO_BAR`
//!   * an enum `Foo`'s field `bar` pairs with `ZTEXT_FOO_BAR`
//!
//! A declaration that breaks the convention fails this check, which is the
//! pressure that keeps the two sides legible as twins.
//!
//! ## What it does not catch
//!
//! translate-c renders every C pointer as `[*c]T`, while `c.zig` writes the
//! pointer it means (`*T`, `?*const T`, `[*]T`). Pointee types are therefore
//! compared only by size and alignment: a `float *` declared here as `*i32`
//! passes. `tests/c_smoke.c` drives the same scenarios as the Zig suite
//! through the header itself, which is what covers that residue.
//!
//! It also compares this build's externs against this build's *header*, not
//! against the *library*. Those two diverge when the header is preprocessed
//! with different macros than the library was compiled with.
//!
//! For ztext that gap is narrow and the narrowness is measured, not assumed:
//! `ffi/ztext.h` includes only `<stddef.h>` and `<stdint.h>` and is sensitive
//! to exactly one macro, `ZTEXT_SHARED`, which changes the `ZTEXT_API`
//! attribute and no type. Every FreeType and HarfBuzz configuration macro
//! reaches the implementation, never the installed header. `ztextAbiProbe`
//! covers the residue from the other side, by comparing these declarations
//! against what the compiled library actually does rather than what the header
//! says.

const std = @import("std");
const c = @import("c.zig");

const h = @cImport({
    @cInclude("ztext.h");
});

/// Functions the header defines inline, which emit no symbol and so have
/// nothing for `c.zig` to declare. ztext has none; the list exists so that
/// adding one is a one-line change rather than a puzzle about why the reverse
/// sweep started failing.
const header_inline_fns = [_][]const u8{};

//=============================================================================
// Name conventions, computed rather than tabulated
//=============================================================================

/// `ClusterLevel` -> `CLUSTER_LEVEL`, `default_align` -> `DEFAULT_ALIGN`.
fn screaming(comptime name: []const u8) []const u8 {
    comptime {
        var out: []const u8 = "";
        var prev_lower = false;
        for (name) |ch| {
            if (std.ascii.isUpper(ch)) {
                if (prev_lower) out = out ++ "_";
                out = out ++ [_]u8{ch};
                prev_lower = false;
            } else if (ch == '_') {
                out = out ++ "_";
                prev_lower = false;
            } else {
                out = out ++ [_]u8{std.ascii.toUpper(ch)};
                prev_lower = true;
            }
        }
        return out;
    }
}

fn typeCName(comptime name: []const u8) []const u8 {
    return "Ztext" ++ name;
}

fn constCName(comptime name: []const u8) []const u8 {
    return "ZTEXT_" ++ screaming(name);
}

fn fieldCName(comptime type_name: []const u8, comptime field_name: []const u8) []const u8 {
    return "ZTEXT_" ++ screaming(type_name) ++ "_" ++ screaming(field_name);
}

//=============================================================================
// Comparison primitives
//
// Every failure is a compile error naming both sides, because a build that
// cannot state which declaration drifted is a guard that costs more to read
// than the drift it found.
//=============================================================================

fn fail(comptime msg: []const u8) void {
    @compileError("ztext ABI drift: " ++ msg);
}

fn theirDecl(comptime name: []const u8, comptime because: []const u8) type {
    if (!@hasDecl(h, name)) {
        fail("`" ++ because ++ "` in src/c.zig expects `" ++ name ++
            "` in ffi/ztext.h, which does not declare it");
    }
    return @TypeOf(@field(h, name));
}

fn sameSizeAndAlign(
    comptime what: []const u8,
    comptime Ours: type,
    comptime Theirs: type,
) void {
    if (@sizeOf(Ours) != @sizeOf(Theirs)) {
        fail(what ++ " is " ++ std.fmt.comptimePrint("{d}", .{@sizeOf(Ours)}) ++
            " bytes in src/c.zig but " ++ std.fmt.comptimePrint("{d}", .{@sizeOf(Theirs)}) ++
            " in ffi/ztext.h");
    }
    if (@alignOf(Ours) != @alignOf(Theirs)) {
        fail(what ++ " has alignment " ++ std.fmt.comptimePrint("{d}", .{@alignOf(Ours)}) ++
            " in src/c.zig but " ++ std.fmt.comptimePrint("{d}", .{@alignOf(Theirs)}) ++
            " in ffi/ztext.h");
    }
}

/// Compares two function types by the only things translate-c preserves:
/// how many parameters there are and how each one is passed.
fn checkFnType(
    comptime what: []const u8,
    comptime Ours: type,
    comptime Theirs: type,
) void {
    const ours = @typeInfo(Ours).@"fn";
    const theirs = @typeInfo(Theirs).@"fn";

    if (ours.params.len != theirs.params.len) {
        fail(what ++ " takes " ++ std.fmt.comptimePrint("{d}", .{ours.params.len}) ++
            " parameters in src/c.zig but " ++ std.fmt.comptimePrint("{d}", .{theirs.params.len}) ++
            " in ffi/ztext.h");
    }

    inline for (ours.params, theirs.params, 0..) |op, tp, i| {
        const OP = op.type orelse fail(what ++ " has an untyped parameter in src/c.zig");
        const TP = tp.type orelse fail(what ++ " has an untyped parameter in ffi/ztext.h");
        sameSizeAndAlign(
            what ++ " parameter " ++ std.fmt.comptimePrint("{d}", .{i}),
            OP,
            TP,
        );
    }

    const OR = ours.return_type orelse fail(what ++ " has no return type in src/c.zig");
    const TR = theirs.return_type orelse fail(what ++ " has no return type in ffi/ztext.h");
    sameSizeAndAlign(what ++ " return value", OR, TR);
}

/// Struct layout, compared field by NAME rather than by position.
///
/// This is the distinction that makes the check worth having. Two same-sized
/// adjacent fields swapping places leaves the *sequence* of offsets identical,
/// so a positional comparison — or a digest folded over offsets alone — passes
/// a swap that silently reinterprets both fields. Pairing each name with its
/// own offset is what catches it.
fn checkStructLayout(
    comptime what: []const u8,
    comptime Ours: type,
    comptime Theirs: type,
) void {
    sameSizeAndAlign(what, Ours, Theirs);

    const ours = @typeInfo(Ours).@"struct";
    const theirs = switch (@typeInfo(Theirs)) {
        .@"struct" => |s| s,
        else => fail(what ++ " is a struct in src/c.zig but not in ffi/ztext.h"),
    };

    if (ours.fields.len != theirs.fields.len) {
        fail(what ++ " has " ++ std.fmt.comptimePrint("{d}", .{ours.fields.len}) ++
            " fields in src/c.zig but " ++ std.fmt.comptimePrint("{d}", .{theirs.fields.len}) ++
            " in ffi/ztext.h");
    }

    inline for (ours.fields) |f| {
        if (!@hasField(Theirs, f.name)) {
            fail(what ++ " has field `" ++ f.name ++ "` in src/c.zig, which ffi/ztext.h does not");
        }
        if (@offsetOf(Ours, f.name) != @offsetOf(Theirs, f.name)) {
            fail(what ++ "." ++ f.name ++ " is at byte " ++
                std.fmt.comptimePrint("{d}", .{@offsetOf(Ours, f.name)}) ++ " in src/c.zig but " ++
                std.fmt.comptimePrint("{d}", .{@offsetOf(Theirs, f.name)}) ++ " in ffi/ztext.h");
        }
        sameSizeAndAlign(
            what ++ "." ++ f.name,
            f.type,
            @FieldType(Theirs, f.name),
        );
    }
}

/// Enumerator values, paired by the `ZTEXT_<TYPE>_<FIELD>` convention.
///
/// translate-c flattens a C enum to an integer alias and loses which
/// enumerators belonged to it, so the values cannot be recovered from the type.
/// The convention is what puts them back together — and it is why the header's
/// enumerators are named strictly, with no readable-but-irregular exceptions.
fn checkEnumValues(
    comptime what: []const u8,
    comptime Ours: type,
    comptime ours_name: []const u8,
) void {
    inline for (@typeInfo(Ours).@"enum".fields) |f| {
        const cname = fieldCName(ours_name, f.name);
        _ = theirDecl(cname, what ++ "." ++ f.name);
        if (@as(i128, @field(h, cname)) != @as(i128, f.value)) {
            fail(what ++ "." ++ f.name ++ " is " ++
                std.fmt.comptimePrint("{d}", .{f.value}) ++ " in src/c.zig but " ++ cname ++
                " is " ++ std.fmt.comptimePrint("{d}", .{@field(h, cname)}) ++ " in ffi/ztext.h");
        }
    }
}

//=============================================================================
// The sweep
//=============================================================================

const Counts = struct {
    types: usize = 0,
    functions: usize = 0,
    constants: usize = 0,
    fields: usize = 0,
    enumerators: usize = 0,
};

/// Every public declaration in `c.zig`, classified and compared. The `else`
/// arms are compile errors: a declaration this does not know how to check is a
/// hole in the guard, and a hole should stop the build rather than be counted
/// as a pass.
fn sweepOurs() Counts {
    comptime {
        var n = Counts{};

        for (@typeInfo(c).@"struct".decls) |d| {
            const Decl = @TypeOf(@field(c, d.name));

            // ---- types -----------------------------------------------------
            if (Decl == type) {
                const Ours = @field(c, d.name);
                const cname = typeCName(d.name);
                const what = "type " ++ d.name;
                n.types += 1;

                switch (@typeInfo(Ours)) {
                    .@"opaque" => {
                        // Nothing to compare but existence: an opaque handle
                        // has no layout on either side, which is the point.
                        const Theirs = theirDecl(cname, what);
                        if (Theirs != type) fail(cname ++ " is not a type in ffi/ztext.h");
                        if (@typeInfo(@field(h, cname)) != .@"opaque") {
                            fail(what ++ " is opaque in src/c.zig but not in ffi/ztext.h");
                        }
                    },
                    .@"struct" => |s| {
                        _ = theirDecl(cname, what);
                        const Theirs = @field(h, cname);
                        switch (s.layout) {
                            .@"extern" => {
                                checkStructLayout(what, Ours, Theirs);
                                n.fields += s.fields.len;
                            },
                            .@"packed" => fail(what ++ " is a packed struct. ztext has no " ++
                                "bit-mask types today, so there is no tested code here to " ++
                                "compare one with -- add a case rather than letting it pass."),
                            .auto => fail(what ++ " has automatic layout, so it has no defined " ++
                                "ABI; declare it extern or packed"),
                        }
                    },
                    .@"enum" => |e| {
                        _ = theirDecl(cname, what);
                        sameSizeAndAlign(what, Ours, @field(h, cname));
                        checkEnumValues(what, Ours, d.name);
                        n.enumerators += e.fields.len;
                    },
                    .int, .float, .bool => {
                        _ = theirDecl(cname, what);
                        const Theirs = @field(h, cname);
                        sameSizeAndAlign(what, Ours, Theirs);
                        const oi = @typeInfo(Ours);
                        const ti = @typeInfo(Theirs);
                        if (oi == .int and ti == .int and oi.int.signedness != ti.int.signedness) {
                            fail(what ++ " is " ++ @typeName(Ours) ++ " in src/c.zig but " ++
                                @typeName(Theirs) ++ " in ffi/ztext.h");
                        }
                    },
                    .pointer => {
                        // A callback typedef. translate-c makes every C
                        // function pointer optional; unwrap before comparing.
                        _ = theirDecl(cname, what);
                        const Theirs = @field(h, cname);
                        sameSizeAndAlign(what, Ours, Theirs);
                        const OursFn = @typeInfo(Ours).pointer.child;
                        const TheirsOpt = @typeInfo(Theirs);
                        const TheirsPtr = if (TheirsOpt == .optional) TheirsOpt.optional.child else Theirs;
                        checkFnType(what, OursFn, @typeInfo(TheirsPtr).pointer.child);
                    },
                    else => fail("type " ++ d.name ++ " is a " ++
                        @tagName(@typeInfo(Ours)) ++ ", which this check does not know how to " ++
                        "compare against the header"),
                }
                continue;
            }

            // ---- functions -------------------------------------------------
            if (@typeInfo(Decl) == .@"fn") {
                if (@typeInfo(Decl).@"fn".calling_convention == .auto) {
                    // A Zig helper, not part of the C ABI. c.zig should not
                    // really have these, but one is not drift.
                    continue;
                }
                const what = "function " ++ d.name;
                _ = theirDecl(d.name, what);
                checkFnType(what, Decl, @TypeOf(@field(h, d.name)));
                n.functions += 1;
                continue;
            }

            // ---- float constants -------------------------------------------
            if (@typeInfo(Decl) == .float or @typeInfo(Decl) == .comptime_float) {
                const cname = constCName(d.name);
                const what = "constant " ++ d.name;
                _ = theirDecl(cname, what);
                const ours_val: f64 = @field(c, d.name);
                const theirs_val: f64 = @field(h, cname);
                // Exact, not approximate: both sides are the same decimal
                // literal, so anything else is a typo in one of them.
                if (ours_val != theirs_val) {
                    fail(what ++ " is " ++ std.fmt.comptimePrint("{d}", .{ours_val}) ++
                        " in src/c.zig but " ++ cname ++ " is " ++
                        std.fmt.comptimePrint("{d}", .{theirs_val}) ++ " in ffi/ztext.h");
                }
                n.constants += 1;
                continue;
            }

            // ---- constants -------------------------------------------------
            if (@typeInfo(Decl) == .int or @typeInfo(Decl) == .comptime_int or
                @typeInfo(Decl) == .@"enum")
            {
                const cname = constCName(d.name);
                const what = "constant " ++ d.name;
                _ = theirDecl(cname, what);
                const ours_val: i128 = @intCast(if (@typeInfo(Decl) == .@"enum")
                    @intFromEnum(@field(c, d.name))
                else
                    @field(c, d.name));
                const theirs_val: i128 = @intCast(@field(h, cname));
                if (ours_val != theirs_val) {
                    fail(what ++ " is " ++ std.fmt.comptimePrint("{d}", .{ours_val}) ++
                        " in src/c.zig but " ++ cname ++ " is " ++
                        std.fmt.comptimePrint("{d}", .{theirs_val}) ++ " in ffi/ztext.h");
                }
                n.constants += 1;
                continue;
            }

            fail("src/c.zig declares `" ++ d.name ++ "` as a " ++ @tagName(@typeInfo(Decl)) ++
                ", which this check does not know how to compare. Add a case rather than " ++
                "leaving it unchecked.");
        }

        return n;
    }
}

/// The other direction: a function the header exports that `c.zig` never
/// declared is invisible to the sweep above, because the sweep only walks what
/// `c.zig` has.
fn sweepTheirs() usize {
    comptime {
        var missing: usize = 0;
        var found: usize = 0;

        for (@typeInfo(h).@"struct".decls) |d| {
            // Filter by name BEFORE touching the value: translate-c emits
            // `@compileError` declarations for system macros it cannot render,
            // and evaluating one of those would fail the build for a reason
            // that has nothing to do with ztext.
            if (!std.mem.startsWith(u8, d.name, "ztext")) continue;
            if (@typeInfo(@TypeOf(@field(h, d.name))) != .@"fn") continue;

            var inline_in_header = false;
            for (header_inline_fns) |name| {
                if (std.mem.eql(u8, name, d.name)) inline_in_header = true;
            }
            if (inline_in_header) continue;

            found += 1;
            if (!@hasDecl(c, d.name)) {
                missing += 1;
                fail("ffi/ztext.h exports `" ++ d.name ++ "` but src/c.zig never declares it");
            }
        }
        return found;
    }
}

//=============================================================================
// The test
//
// The comparisons above are compile errors, so reaching this body at all means
// they passed. What is left to assert is that they actually ran: a sweep that
// silently matched nothing would be indistinguishable from a sweep that
// matched everything.
//=============================================================================

test "ABI: src/c.zig agrees with ffi/ztext.h" {
    @setEvalBranchQuota(1_000_000);

    const ours = comptime sweepOurs();
    const theirs = comptime sweepTheirs();

    // Coarse floors, not exact counts: they exist so that a sweep which
    // silently matched nothing is a failure rather than a pass. Exact numbers
    // would make every new declaration a two-line change for no extra safety.
    try std.testing.expect(ours.types >= 18);
    try std.testing.expect(ours.functions >= 40);
    try std.testing.expect(ours.fields >= 60);
    try std.testing.expect(ours.enumerators >= 25);
    try std.testing.expectEqual(ours.functions, theirs);
}
