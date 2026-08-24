//! Bridges a Zig `std.mem.Allocator` onto ztext's allocator seam.
//!
//! This is a thinner bridge than it would otherwise be, because the C side
//! already does the hard part. FreeType, HarfBuzz and SheenBidi all free with
//! a bare pointer, and Zig's allocator interface needs a size and an alignment
//! back; `ffi/ztext_core.c` records both in a header ahead of every block and
//! hands them to `deallocate`, so nothing here has to keep a side table or
//! pad allocations of its own.
//!
//! ## Global state
//!
//! The seam is process-wide because HarfBuzz's is compile-time and cannot be
//! anything else. That is surfaced rather than hidden.
//!
//! FreeType is the exception, and it is genuinely per-library: the C side
//! copies the installed allocator into each `Library` and points FreeType's
//! `FT_Memory` at that copy. `setAllocator` therefore takes a pointer the
//! caller keeps alive rather than writing into a mutable global -- a global
//! would leave every captured copy pointing at the same slot, which is the
//! same thing as not capturing at all.

const std = @import("std");
const c = @import("c.zig");
const err = @import("error.zig");

fn allocate(user: ?*anyopaque, size: usize, alignment: usize) callconv(.c) ?*anyopaque {
    const gpa: *const std.mem.Allocator = @ptrCast(@alignCast(user orelse return null));
    // The C side documents alignment as a power of two and never passes zero;
    // checked anyway, because a bogus alignment would produce a bogus
    // std.mem.Alignment and a corrupt free later.
    if (alignment == 0 or !std.math.isPowerOfTwo(alignment)) return null;

    const want = std.mem.Alignment.fromByteUnits(alignment);
    const block = gpa.rawAlloc(size, want, @returnAddress()) orelse return null;
    return @ptrCast(block);
}

fn reallocate(
    user: ?*anyopaque,
    block: ?*anyopaque,
    old_size: usize,
    new_size: usize,
    alignment: usize,
) callconv(.c) ?*anyopaque {
    const gpa: *const std.mem.Allocator = @ptrCast(@alignCast(user orelse return null));
    if (alignment == 0 or !std.math.isPowerOfTwo(alignment)) return null;
    const existing: [*]u8 = @ptrCast(block orelse return null);

    const want = std.mem.Alignment.fromByteUnits(alignment);
    // rawRemap can grow in place; when it cannot it returns null and the C
    // side falls back to allocate-copy-deallocate, which is exactly the
    // contract ZtextAllocator.reallocate documents -- the old block stays
    // valid on failure.
    const moved = gpa.rawRemap(existing[0..old_size], want, new_size, @returnAddress()) orelse {
        return null;
    };
    return @ptrCast(moved);
}

fn deallocate(
    user: ?*anyopaque,
    block: ?*anyopaque,
    size: usize,
    alignment: usize,
) callconv(.c) void {
    const gpa: *const std.mem.Allocator = @ptrCast(@alignCast(user orelse return));
    const existing: [*]u8 = @ptrCast(block orelse return);
    const want = std.mem.Alignment.fromByteUnits(alignment);
    gpa.rawFree(existing[0..size], want, @returnAddress());
}

/// Routes every subsequent ztext allocation through `gpa`.
///
/// Takes a POINTER, and the pointee must outlive every handle allocated
/// through it. That is not ceremony: the C side copies the allocator into each
/// `Library` so FreeType memory keeps using the allocator the library was
/// created with even after this is called again. If the wrapper wrote into one
/// mutable global and handed C a pointer to it -- the obvious design -- that
/// capture would be defeated, every captured copy would silently follow the
/// newest allocator, and destroying a library after a swap would hand its
/// FreeType blocks to an allocator that never issued them.
///
/// A `const` at function or file scope is the usual thing to point at:
///
/// ```zig
/// var gpa_state: std.heap.DebugAllocator(.{}) = .init;
/// const gpa = gpa_state.allocator();
/// try ztext.setAllocator(&gpa);
/// ```
///
/// Process-wide, because HarfBuzz's seam is compile-time and cannot be
/// anything else. Call `warmup()` first if you intend to audit this
/// allocator's balance.
pub fn setAllocator(gpa: *const std.mem.Allocator) err.Error!void {
    const bridge = c.Allocator{
        .allocate = allocate,
        .reallocate = reallocate,
        .deallocate = deallocate,
        .user = @ptrCast(@constCast(gpa)),
    };
    try err.check(c.ztextSetAllocator(&bridge));
}

/// Restores malloc/free.
///
/// Only safe once every handle allocated through the Zig allocator has been
/// destroyed.
pub fn resetAllocator() void {
    _ = c.ztextSetAllocator(null);
}

test "the allocator bridge round-trips every alignment ztext may ask for" {
    try setAllocator(&std.testing.allocator);
    defer resetAllocator();

    var alignment: usize = 1;
    while (alignment <= 64) : (alignment *= 2) {
        const block = allocate(@ptrCast(@constCast(&std.testing.allocator)), 100, alignment) orelse {
            return error.TestUnexpectedResult;
        };
        try std.testing.expect(@intFromPtr(block) % alignment == 0);
        // Write the payload so a too-small allocation trips the test allocator.
        const bytes: [*]u8 = @ptrCast(block);
        @memset(bytes[0..100], 0xAB);
        deallocate(@ptrCast(@constCast(&std.testing.allocator)), block, 100, alignment);
    }
}

test "the allocator bridge rejects a non-power-of-two alignment" {
    try setAllocator(&std.testing.allocator);
    defer resetAllocator();

    try std.testing.expect(allocate(@ptrCast(@constCast(&std.testing.allocator)), 32, 3) == null);
    try std.testing.expect(allocate(@ptrCast(@constCast(&std.testing.allocator)), 32, 0) == null);
}

test "the allocator bridge grows a block and keeps its contents" {
    try setAllocator(&std.testing.allocator);
    defer resetAllocator();

    const alignment: usize = 8;
    const block = allocate(@ptrCast(@constCast(&std.testing.allocator)), 32, alignment) orelse {
        return error.TestUnexpectedResult;
    };
    const bytes: [*]u8 = @ptrCast(block);
    @memset(bytes[0..32], 0x5A);

    // rawRemap is allowed to fail, and the C side has a fallback for exactly
    // that, so a null here is a legitimate outcome rather than a test failure.
    if (reallocate(@ptrCast(@constCast(&std.testing.allocator)), block, 32, 64, alignment)) |grown| {
        const grown_bytes: [*]u8 = @ptrCast(grown);
        for (grown_bytes[0..32]) |byte| try std.testing.expectEqual(@as(u8, 0x5A), byte);
        deallocate(@ptrCast(@constCast(&std.testing.allocator)), grown, 64, alignment);
    } else {
        deallocate(@ptrCast(@constCast(&std.testing.allocator)), block, 32, alignment);
    }
}
