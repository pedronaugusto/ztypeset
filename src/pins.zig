//! The vendored upstreams, pinned. **This file is the one home for those
//! facts**, and everything else derives from it.
//!
//! The pin used to be written in four places -- UPSTREAM.md's table, the
//! `verify` calls in ci/verify-vendor.sh, the assertions in src/ztypeset.zig and
//! the assertions in the downstream consumer -- with nothing comparing them.
//! Three of the four could go stale without a single check going red, and the
//! one assertion that existed checked a major version only: `harfbuzz.major
//! == 14` passes for every HarfBuzz release in a year, so "the library
//! actually compiled the version the documentation claims" was not what it
//! proved.
//!
//! Now:
//!   - `ci/verify-vendor.sh` reads the tag, commit and URL from here and
//!     fetches those exact commits.
//!   - the suite asserts the versions the LINKED libraries report against
//!     `version` below, all three components of it.
//!   - `ci/measurements.sh --check` compares UPSTREAM.md's table with this
//!     file, so the prose cannot drift from the pin either.
//!
//! Shape matters: each pin is one line, because ci/verify-vendor.sh reads this
//! file with grep and sed. A pin split across lines would be read as missing,
//! and a missing pin fails rather than passing quietly -- but keep them on one
//! line anyway, and run ci/verify-vendor.sh after touching this file.

const std = @import("std");

pub const Version = struct {
    major: u8,
    minor: u8,
    patch: u8,

    pub fn eql(self: Version, other: Version) bool {
        return self.major == other.major and self.minor == other.minor and
            self.patch == other.patch;
    }

    pub fn format(self: Version, writer: *std.Io.Writer) std.Io.Writer.Error!void {
        try writer.print("{d}.{d}.{d}", .{ self.major, self.minor, self.patch });
    }
};

pub const Pin = struct {
    /// The directory under `libs/`, and the name ci/verify-vendor.sh prints.
    name: []const u8,
    /// The version the upstream reports at runtime, all three components.
    /// Where an upstream numbers itself with two (libunibreak is 7.0), the
    /// third is 0 -- one shape for the fact, so nothing has to format it two
    /// ways. That divergence was a finding: the C consumer printed `7.0` and
    /// the Zig one `7.0.0` for the same number.
    version: Version,
    url: []const u8,
    /// The release tag. A tag can be moved, which is why the commit is here
    /// too and is what the vendor check actually compares.
    tag: []const u8,
    /// The tag's PEELED target: the commit, not the tag object.
    commit: []const u8,
};

pub const freetype: Pin = .{ .name = "freetype", .version = .{ .major = 2, .minor = 14, .patch = 3 }, .url = "https://gitlab.freedesktop.org/freetype/freetype.git", .tag = "VER-2-14-3", .commit = "0a0221a1347e2f1e07c395263540026e9a0aa7c7" };
pub const harfbuzz: Pin = .{ .name = "harfbuzz", .version = .{ .major = 14, .minor = 4, .patch = 0 }, .url = "https://github.com/harfbuzz/harfbuzz.git", .tag = "14.4.0", .commit = "36cb489cb02ce4b92099669ba9f9bea348eff93f" };
pub const sheenbidi: Pin = .{ .name = "sheenbidi", .version = .{ .major = 3, .minor = 0, .patch = 0 }, .url = "https://github.com/Tehreer/SheenBidi.git", .tag = "v3.0.0", .commit = "cfe430e7375a7845b679adae9d51dac6deaa8858" };
pub const libunibreak: Pin = .{ .name = "libunibreak", .version = .{ .major = 7, .minor = 0, .patch = 0 }, .url = "https://github.com/adah1972/libunibreak.git", .tag = "libunibreak_7_0", .commit = "3ce4bfa3129ff3738046a44a6db533d2ce25af2b" };

pub const all = [_]Pin{ freetype, harfbuzz, sheenbidi, libunibreak };

test "every pin is complete" {
    for (all) |pin| {
        try std.testing.expect(pin.name.len > 0);
        try std.testing.expect(pin.url.len > 0);
        try std.testing.expect(pin.tag.len > 0);
        // A git object name, so exactly forty lowercase hex digits. An
        // abbreviated one is ambiguous and a truncated one silently matches
        // nothing when ci/verify-vendor.sh greps UPSTREAM.md for it.
        try std.testing.expectEqual(@as(usize, 40), pin.commit.len);
        for (pin.commit) |ch| {
            try std.testing.expect((ch >= '0' and ch <= '9') or (ch >= 'a' and ch <= 'f'));
        }
    }
}
