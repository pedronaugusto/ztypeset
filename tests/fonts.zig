//! The committed test fonts, embedded so the suite needs no files at runtime
//! and no path on the command line.
//!
//! All four are Noto, licensed under the SIL Open Font License 1.1. Their
//! provenance, the exact source URL and a SHA-256 for each are recorded in
//! `fonts/PROVENANCE.md`, and `ci/verify-vendor.sh` checks those hashes -- the
//! golden shaping results in the suite are only meaningful against these exact
//! bytes.
//!
//! Each is here for a reason:
//!
//!   latin   Noto Sans. Standard ligatures and real kerning pairs, so `liga`
//!           and `kern` can be turned on and off and the difference observed.
//!   arabic  Noto Naskh Arabic. Cursive joining -- the same letter takes an
//!           initial, medial, final or isolated form depending on neighbours,
//!           which is the shaping behaviour no advance table can fake.
//!   hebrew  Noto Sans Hebrew. Right-to-left WITHOUT joining, so a bug in
//!           direction handling cannot hide behind a bug in joining.
//!   variable
//!           Noto Sans Hebrew again, as a variable font with `wdth` and
//!           `wght` axes. Two axes rather than one, so a test can prove that
//!           moving one leaves the other alone -- and the same script as
//!           `hebrew`, so the two can be compared directly. It is covered by
//!           the SAME licence file as `hebrew`, since it is the same family
//!           from the same upstream repository; there is deliberately no
//!           second copy of the OFL for it.

const std = @import("std");

pub const latin = @embedFile("fonts/NotoSans-Regular.ttf");
pub const arabic = @embedFile("fonts/NotoNaskhArabic-Regular.ttf");
pub const hebrew = @embedFile("fonts/NotoSansHebrew-Regular.ttf");
pub const variable = @embedFile("fonts/NotoSansHebrew[wdth,wght].ttf");

//===----------------------------------------------------------------------===//
// A font with variation sequences, built here
//===----------------------------------------------------------------------===//

/// One entry for `withVariationSequences`.
pub const VariationSequence = struct {
    /// The base character.
    base: u21,
    /// The variation selector that follows it.
    selector: u21,
    /// The glyph the pair names, or 0 to record the pair as a DEFAULT
    /// mapping -- "this font draws the sequence, with the base character's
    /// own glyph and no glyph of its own".
    glyph: u16,
};

/// A copy of `ttf` carrying one extra cmap subtable, format 14, that names
/// `sequences`. The caller owns the returned bytes.
///
/// None of the four fonts above has a format-14 subtable -- the test that uses
/// this asserts that of the font it starts from rather than trusting it -- and
/// without one, two of the three answers `ztypesetFontVariantGlyphIndex` can give
/// are unreachable: a sequence with a glyph of its own, and a sequence the
/// font records as the default. Only "no such sequence" can be observed, which
/// would leave the interesting half of the entry point untested.
///
/// Built here rather than vendored because it is a few hundred bytes of table
/// this file can explain, against a fifth font with its own provenance, its
/// own licence and its own pin -- and because a fixture whose contents are
/// stated in the test is a better fixture than one whose contents have to be
/// looked up.
///
/// `sequences` must be sorted by `selector` and then by `base`, which is what
/// the format requires and what FreeType's validator checks
/// (`tt_cmap14_validate`).
pub fn withVariationSequences(
    gpa: std.mem.Allocator,
    ttf: []const u8,
    sequences: []const VariationSequence,
) ![]u8 {
    std.debug.assert(sequences.len > 0);

    // ---- the format-14 subtable ------------------------------------------
    //
    // uint16 format, uint32 length, uint32 numVarSelectorRecords, then one
    // 11-byte record per selector: uint24 varSelector and two Offset32 into
    // this same subtable. Offsets are from the start of the subtable, so the
    // sizes have to be known before the header can be written -- hence the
    // measuring pass.
    var selectors: std.ArrayList(u21) = .empty;
    defer selectors.deinit(gpa);
    for (sequences, 0..) |s, i| {
        if (i > 0) {
            const prev = sequences[i - 1];
            std.debug.assert(s.selector > prev.selector or
                (s.selector == prev.selector and s.base > prev.base));
        }
        if (selectors.items.len == 0 or
            selectors.items[selectors.items.len - 1] != s.selector)
        {
            try selectors.append(gpa, s.selector);
        }
    }

    const header_len = 10 + 11 * selectors.items.len;
    var f14: std.ArrayList(u8) = .empty;
    defer f14.deinit(gpa);
    var body: std.ArrayList(u8) = .empty;
    defer body.deinit(gpa);

    try put16(&f14, gpa, 14);
    // Length and record count, filled once the body is measured; the length
    // is patched below because it is not known yet.
    try put32(&f14, gpa, 0);
    try put32(&f14, gpa, @intCast(selectors.items.len));

    for (selectors.items) |selector| {
        var defaults: usize = 0;
        var nondefaults: usize = 0;
        for (sequences) |s| {
            if (s.selector != selector) continue;
            if (s.glyph == 0) {
                defaults += 1;
            } else {
                nondefaults += 1;
            }
        }

        const default_off: u32 = if (defaults == 0)
            0
        else
            @intCast(header_len + body.items.len);
        if (defaults > 0) {
            try put32(&body, gpa, @intCast(defaults));
            for (sequences) |s| {
                if (s.selector != selector or s.glyph != 0) continue;
                // uint24 startUnicodeValue, uint8 additionalCount. One
                // character per range: a range would say nothing this test
                // does not already say.
                try put24(&body, gpa, s.base);
                try body.append(gpa, 0);
            }
        }

        const nondefault_off: u32 = if (nondefaults == 0)
            0
        else
            @intCast(header_len + body.items.len);
        if (nondefaults > 0) {
            try put32(&body, gpa, @intCast(nondefaults));
            for (sequences) |s| {
                if (s.selector != selector or s.glyph == 0) continue;
                try put24(&body, gpa, s.base);
                try put16(&body, gpa, s.glyph);
            }
        }

        try put24(&f14, gpa, selector);
        try put32(&f14, gpa, default_off);
        try put32(&f14, gpa, nondefault_off);
    }

    std.debug.assert(f14.items.len == header_len);
    try f14.appendSlice(gpa, body.items);
    std.mem.writeInt(u32, f14.items[2..6], @intCast(f14.items.len), .big);

    return withExtraCmapSubtable(gpa, ttf, 0, 5, f14.items);
}

/// A copy of `ttf` whose cmap carries one extra `(platform, encoding)` record
/// pointing at `subtable`, which the caller has already built.
///
/// The subtables cannot simply be repointed, which is why this exists rather
/// than a few lines at each caller: their offsets are relative to the start of
/// the cmap, so one more encoding record moves every one of them by eight
/// bytes.
fn withExtraCmapSubtable(
    gpa: std.mem.Allocator,
    ttf: []const u8,
    platform: u16,
    encoding: u16,
    subtable: []const u8,
) ![]u8 {
    // ---- the cmap this font already has ----------------------------------
    const num_tables = readU16(ttf, 4);
    var record: usize = 0;
    var cmap_off: usize = 0;
    var cmap_len: usize = 0;
    for (0..num_tables) |i| {
        const at = 12 + 16 * i;
        if (std.mem.eql(u8, ttf[at..][0..4], "cmap")) {
            record = at;
            cmap_off = readU32(ttf, at + 8);
            cmap_len = readU32(ttf, at + 12);
        }
    }
    if (cmap_len == 0) return error.NoCmapTable;
    const cmap = ttf[cmap_off..][0..cmap_len];

    // ---- a new cmap: every subtable it had, plus this one -----------------
    //
    // The subtables cannot simply be repointed. Their offsets are relative to
    // the start of the cmap, and one more encoding record moves every one of
    // them by eight bytes, so the bytes are copied and the offsets recomputed.
    // Two records commonly share one subtable -- (0,3) and (3,1) usually do --
    // so a shared one is copied once and pointed at twice.
    //
    // The records are re-SORTED by platform and encoding, which the format
    // requires and which is load-bearing here rather than tidiness. FreeType
    // chooses a face's default charmap by walking the list BACKWARDS and
    // taking the last one whose encoding is Unicode
    // (find_unicode_charmap, libs/freetype/src/base/ftobjs.c) -- and platform
    // 0 is Apple Unicode, so a (0,5) record appended at the end becomes the
    // default charmap. Its format-14 char_index answers 0 for everything, so
    // the font's ordinary lookups all return .notdef while the variation
    // sequences work perfectly. Sorted, (0,5) sits before (3,1) and the
    // backwards walk finds the format-4 charmap first, which is why every
    // real font with variation sequences works. The same sort is what keeps a
    // (3,0) symbol record below (3,1), so adding one does not change which
    // map a font opens with.
    const Record = struct {
        platform: u16,
        encoding: u16,
        /// Offset within the OLD cmap, or undefined for the new record.
        old_off: u32,
        is_new: bool,

        fn before(_: void, a: @This(), b: @This()) bool {
            if (a.platform != b.platform) return a.platform < b.platform;
            return a.encoding < b.encoding;
        }
    };

    const old_records = readU16(cmap, 2);
    var records: std.ArrayList(Record) = .empty;
    defer records.deinit(gpa);
    for (0..old_records) |i| {
        const at = 4 + 8 * i;
        try records.append(gpa, .{
            .platform = readU16(cmap, at),
            .encoding = readU16(cmap, at + 2),
            .old_off = readU32(cmap, at + 4),
            .is_new = false,
        });
    }
    try records.append(gpa, .{
        .platform = platform,
        .encoding = encoding,
        .old_off = 0,
        .is_new = true,
    });
    std.mem.sort(Record, records.items, {}, Record.before);

    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try out.appendSlice(gpa, ttf);
    while (out.items.len % 4 != 0) try out.append(gpa, 0);
    const new_cmap_off = out.items.len;

    var data: std.ArrayList(u8) = .empty;
    defer data.deinit(gpa);
    var new_offsets: std.ArrayList(u32) = .empty;
    defer new_offsets.deinit(gpa);

    const records_len = 4 + 8 * records.items.len;
    for (records.items) |r| {
        if (r.is_new) {
            // Filled below, once every copied subtable has taken its room.
            try new_offsets.append(gpa, 0);
            continue;
        }
        var found: u32 = 0;
        // Only the records already resolved; the two lists are the same
        // length only once this loop has finished.
        for (records.items[0..new_offsets.items.len], new_offsets.items) |other, off| {
            if (!other.is_new and other.old_off == r.old_off) found = off;
        }
        if (found == 0) {
            found = @intCast(records_len + data.items.len);
            const length = try subtableLength(cmap, r.old_off);
            try data.appendSlice(gpa, cmap[r.old_off..][0..length]);
        }
        try new_offsets.append(gpa, found);
    }
    const new_off: u32 = @intCast(records_len + data.items.len);

    var new_cmap: std.ArrayList(u8) = .empty;
    defer new_cmap.deinit(gpa);
    try put16(&new_cmap, gpa, 0);
    try put16(&new_cmap, gpa, @intCast(records.items.len));
    for (records.items, new_offsets.items) |r, off| {
        try put16(&new_cmap, gpa, r.platform);
        try put16(&new_cmap, gpa, r.encoding);
        try put32(&new_cmap, gpa, if (r.is_new) new_off else off);
    }
    try new_cmap.appendSlice(gpa, data.items);
    try new_cmap.appendSlice(gpa, subtable);

    try out.appendSlice(gpa, new_cmap.items);

    // The directory entry, repointed. Nothing recomputes the checksums: no
    // reader in this package looks at them, and a fixture that pretended to
    // would be claiming a guarantee it does not keep.
    std.mem.writeInt(u32, out.items[record + 8 ..][0..4], @intCast(new_cmap_off), .big);
    std.mem.writeInt(u32, out.items[record + 12 ..][0..4], @intCast(new_cmap.items.len), .big);

    return out.toOwnedSlice(gpa);
}

//===----------------------------------------------------------------------===//
// A font with an MS Symbol character map, built here
//===----------------------------------------------------------------------===//

/// A copy of `ttf` carrying one extra cmap subtable at (3, 0), FreeType's
/// FT_ENCODING_MS_SYMBOL, mapping `first` and the codes after it to `glyphs`.
/// The caller owns the returned bytes.
///
/// None of the four fonts above has a non-Unicode charmap -- the test that
/// uses this asserts that of the font it starts from rather than trusting it
/// -- so without one, selecting a charmap could be observed only as a refusal.
/// A symbol map is the case the entry point exists for: an icon font whose
/// glyphs are reachable through no Unicode character at all.
pub fn withSymbolCmap(
    gpa: std.mem.Allocator,
    ttf: []const u8,
    first: u16,
    glyphs: []const u16,
) ![]u8 {
    std.debug.assert(glyphs.len > 0);

    // Subtable format 6, "trimmed table mapping": uint16 format, length,
    // language, firstCode and entryCount, then one glyph id per code. The
    // smallest subtable the format has, and enough for a symbol map, whose
    // codes are one contiguous run in the 0xF000 private-use block by
    // convention.
    var sub: std.ArrayList(u8) = .empty;
    defer sub.deinit(gpa);
    try put16(&sub, gpa, 6);
    try put16(&sub, gpa, @intCast(10 + 2 * glyphs.len));
    try put16(&sub, gpa, 0);
    try put16(&sub, gpa, first);
    try put16(&sub, gpa, @intCast(glyphs.len));
    for (glyphs) |glyph| try put16(&sub, gpa, glyph);

    // Platform 3, encoding 0 is the pair FreeType reads as
    // FT_ENCODING_MS_SYMBOL (tt_get_cmap_info, libs/freetype/src/sfnt/ttcmap.c).
    return withExtraCmapSubtable(gpa, ttf, 3, 0, sub.items);
}

/// Length of the cmap subtable at `off`, whose home is the format's own
/// header. Formats this package's fonts do not use are an error rather than a
/// guess.
fn subtableLength(cmap: []const u8, off: u32) !usize {
    return switch (readU16(cmap, off)) {
        0, 2, 4, 6 => readU16(cmap, off + 2),
        8, 10, 12, 13 => readU32(cmap, off + 4),
        14 => readU32(cmap, off + 2),
        else => error.UnsupportedCmapSubtable,
    };
}

fn readU16(bytes: []const u8, off: usize) u16 {
    return std.mem.readInt(u16, bytes[off..][0..2], .big);
}

fn readU32(bytes: []const u8, off: usize) u32 {
    return std.mem.readInt(u32, bytes[off..][0..4], .big);
}

fn put16(list: *std.ArrayList(u8), gpa: std.mem.Allocator, value: u16) !void {
    try list.appendSlice(gpa, &.{ @intCast(value >> 8), @truncate(value) });
}

fn put24(list: *std.ArrayList(u8), gpa: std.mem.Allocator, value: u21) !void {
    try list.appendSlice(gpa, &.{
        @intCast(value >> 16),
        @truncate(value >> 8),
        @truncate(value),
    });
}

fn put32(list: *std.ArrayList(u8), gpa: std.mem.Allocator, value: u32) !void {
    try list.appendSlice(gpa, &.{
        @intCast(value >> 24),
        @truncate(value >> 16),
        @truncate(value >> 8),
        @truncate(value),
    });
}
