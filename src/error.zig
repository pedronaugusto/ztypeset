//! Translation between the C result enum and a Zig error set.

const std = @import("std");
const c = @import("c.zig");

pub const Error = error{
    /// The installed allocator returned null, or an upstream reported an
    /// allocation failure of its own.
    OutOfMemory,
    /// A null handle, an empty buffer, an out-of-range index, or a face with
    /// no pixel size set.
    InvalidArgument,
    /// The text was not well-formed in the encoding it was passed in.
    InvalidText,
    /// FreeType refused the bytes: not a font, truncated, or broken.
    BadFont,
    /// A font in a format this build does not compile support for.
    Unsupported,
    /// The glyph index is not present in the face.
    GlyphNotFound,
    /// The glyph loaded but could not be rasterised.
    RenderFailed,
    /// HarfBuzz could not shape the run.
    ShapeFailed,
    /// SheenBidi could not analyse the paragraph.
    BidiFailed,
    /// The destination buffer was too small.
    BufferTooSmall,
    /// A result code this build does not know, which means the library is
    /// newer than the declarations compiled against it. Only reachable with a
    /// shared build; see `ffi/ztext.h` on version skew.
    UnknownResult,
};

/// Turns a C result into a Zig error, or void on success.
///
/// `c.Result` is non-exhaustive, so this needs an `else` -- but the property
/// that used to give (adding a result to the C enum without handling it here
/// is a compile error) is not lost: `AbiLayout.result_count` is asserted
/// against the Zig enum's field count in the ABI test, so a new result that
/// nobody mapped fails there instead.
pub fn check(result: c.Result) Error!void {
    return switch (result) {
        .ok => {},
        .out_of_memory => Error.OutOfMemory,
        .invalid_argument => Error.InvalidArgument,
        .invalid_text => Error.InvalidText,
        .bad_font => Error.BadFont,
        .unsupported => Error.Unsupported,
        .glyph_not_found => Error.GlyphNotFound,
        .render_failed => Error.RenderFailed,
        .shape_failed => Error.ShapeFailed,
        .bidi_failed => Error.BidiFailed,
        .buffer_too_small => Error.BufferTooSmall,
        _ => Error.UnknownResult,
    };
}

/// Borrowed, static description of a result, for logging.
pub fn name(result: c.Result) [:0]const u8 {
    return std.mem.span(c.ztextResultName(result));
}

/// What an upstream said about the most recent failure on this thread, or ""
/// if it said nothing.
///
/// ztext's error set is flat, so this is where the detail FreeType had and the
/// enum could not carry ends up. Diagnostics only -- the string is not stable
/// across versions and nothing should branch on it.
pub fn lastDetail() [:0]const u8 {
    return std.mem.span(c.ztextLastErrorDetail());
}
