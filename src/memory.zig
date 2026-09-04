//! Bridges a Zig `std.mem.Allocator` onto ztypeset's allocator seam.
//!
//! This is a thinner bridge than it would otherwise be, because the C side
//! already does the hard part. FreeType, HarfBuzz and SheenBidi all free with
//! a bare pointer, and Zig's allocator interface needs a size and an alignment
//! back; `ffi/ztypeset_core.c` records both in a header ahead of every block and
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
//! `FT_Memory` at that copy.
//!
//! What it copies includes the `user` pointer, and `user` here is a
//! `std.mem.Allocator`. That value therefore has to outlive every handle any
//! copy of it can reach -- which is a lifetime no caller can compute, since it
//! ends when the last block of a library it destroyed is finally freed. So
//! `setAllocator` takes the allocator BY VALUE and keeps it in a slot of
//! ztypeset's own: one per distinct allocator ever installed, allocated with
//! malloc and never freed. That is the same bargain `ffi/ztypeset_core.c` makes
//! for its registry entries, one level up and for the same reason.
//!
//! A single mutable global would not do. Every captured copy would point at
//! the same slot, so installing a new allocator would retroactively change
//! which heap a live library's FreeType memory belonged to -- the exact defect
//! the per-library capture exists to prevent.

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
    // contract ZtypesetAllocator.reallocate documents -- the old block stays
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

/// One slot per distinct allocator ever installed, never freed.
///
/// The C side copies the bridge struct -- `user` included -- into its registry
/// and into every `Library`, and never frees an entry, because an entry has to
/// outlive the last block it issued. `user` points at one of these, so these
/// need the same lifetime, and ztypeset is the only party in a position to give
/// them one.
var slots: std.ArrayList(*std.mem.Allocator) = .empty;

/// A stable address for `gpa`, allocating a slot the first time it is seen.
///
/// Unsynchronised, deliberately and not by omission: this list mirrors the C
/// side's allocator registry, which is unsynchronised for the same reason.
/// Installing an allocator replaces a process-wide one and is not a
/// concurrent operation -- see "Thread safety" in `ffi/ztypeset.h`. A lock here
/// and none there would promise a safety the seam does not have, which is
/// worse than the honest contract.
fn slotFor(gpa: std.mem.Allocator) err.Error!*std.mem.Allocator {
    // Identity is the pair of pointers, because that is all a
    // std.mem.Allocator is. Installing the same allocator twice reuses its
    // slot, exactly as installing the same ZtypesetAllocator twice reuses its
    // registry entry on the C side.
    for (slots.items) |slot| {
        if (slot.ptr == gpa.ptr and slot.vtable == gpa.vtable) return slot;
    }

    // malloc, deliberately. This has to outlive the last block the allocator
    // issued, and the only allocator still standing at that point is the C
    // runtime's -- the same reasoning, and the same choice, as the registry
    // entry on the other side of the boundary. ztypeset links libc on every
    // target it builds for, so this is always available.
    const backing = std.heap.c_allocator;
    const slot = backing.create(std.mem.Allocator) catch
        return err.Error.OutOfMemory;
    slot.* = gpa;
    slots.append(backing, slot) catch {
        backing.destroy(slot);
        return err.Error.OutOfMemory;
    };
    return slot;
}

/// Routes every subsequent ztypeset allocation through `gpa`.
///
/// COPIED, not borrowed. ztypeset keeps its own copy alive for as long as any
/// handle can reach it, so a temporary is fine:
///
/// ```zig
/// var gpa_state: std.heap.DebugAllocator(.{}) = .init;
/// try ztypeset.setAllocator(gpa_state.allocator());
/// ```
///
/// Process-wide, because HarfBuzz's seam is compile-time and cannot be
/// anything else. Warms the upstreams' process-lifetime caches on the way in,
/// so this allocator is never charged for something that is never freed and
/// `resetAllocator` below has a precondition a host can meet without knowing
/// to.
///
/// SETUP, not an operation. `slotFor` above and the C side's registry are both
/// unsynchronised, deliberately, so this has to be called before any other
/// thread is using ztypeset -- once, at start-up. A thread-safe `gpa` does not
/// cover the install itself: what races is ztypeset's own bookkeeping, not the
/// allocator behind it. See "Thread safety" in `ffi/ztypeset.h`.
///
/// Which of two installed allocators a given block comes from is stated in
/// full beside `ztypesetSetAllocator` in `ffi/ztypeset.h`: handle-owned memory
/// follows the handle, HarfBuzz's follows whatever is installed when it
/// asks.
pub fn setAllocator(gpa: std.mem.Allocator) err.Error!void {
    const slot = try slotFor(gpa);
    const bridge = c.Allocator{
        .allocate = allocate,
        .reallocate = reallocate,
        .deallocate = deallocate,
        .user = @ptrCast(slot),
    };
    try err.check(c.ztypesetSetAllocator(&bridge));
}

/// Restores malloc/free.
///
/// Safe once every handle allocated through the Zig allocator has been
/// destroyed -- a precondition a host can meet, because the upstreams' caches
/// that are never freed were charged to whatever was installed BEFORE
/// `setAllocator`. The two that need a face are the exception; see `warmup`.
pub fn resetAllocator() void {
    _ = c.ztypesetSetAllocator(null);
}

test "the allocator bridge round-trips every alignment ztypeset may ask for" {
    try setAllocator(std.testing.allocator);
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
    try setAllocator(std.testing.allocator);
    defer resetAllocator();

    try std.testing.expect(allocate(@ptrCast(@constCast(&std.testing.allocator)), 32, 3) == null);
    try std.testing.expect(allocate(@ptrCast(@constCast(&std.testing.allocator)), 32, 0) == null);
}

test "the allocator bridge grows a block and keeps its contents" {
    try setAllocator(std.testing.allocator);
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

test "installing the same allocator twice reuses its slot" {
    // The slots are never freed, so a host that installs an allocator per
    // frame -- or a test that installs one per case, as this suite does --
    // must not add one each time. Growth here is a leak with a slow fuse and
    // no allocator to report it.
    try setAllocator(std.testing.allocator);
    const after_first = slots.items.len;
    try std.testing.expect(after_first > 0);

    try setAllocator(std.testing.allocator);
    try std.testing.expectEqual(after_first, slots.items.len);

    resetAllocator();
    try std.testing.expectEqual(after_first, slots.items.len);
}
