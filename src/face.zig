//! Library, font and face lifetime, sizing, metrics and rasterisation.
//!
//! Three types because there are three lifetimes: a `Library` owns FreeType's
//! modules, a `Font` is one parsed font image, and a `Face` is that font at one
//! size. Everything that does not depend on the size lives on the font and is
//! parsed once -- which is how both upstreams model it, and what keeps a second
//! size from costing a second full parse.
//!
//! Ordering: a `Font` and its faces must be destroyed before the `Library` they
//! came from. That is FreeType's rule, not ztext's, and it cannot be checked --
//! see `Library.deinit`. Between a font and its faces there is no order at all.

const std = @import("std");
const c = @import("c.zig");
const err = @import("error.zig");
const types = @import("types.zig");

/// Owns a FreeType library and the modules registered in it.
///
/// NOT thread-safe, and neither is any face made from it. Use one per thread
/// rather than sharing one behind a lock -- that is FreeType's own advice, and
/// a lock would serialise glyph rasterisation across the whole process.
pub const Library = struct {
    handle: *c.Library,

    pub fn init() err.Error!Library {
        var handle: *c.Library = undefined;
        try err.check(c.ztextLibraryCreate(&handle));
        return .{ .handle = handle };
    }

    /// Releases the library.
    ///
    /// There is no order to get right: a `Font` or `Face` still alive keeps
    /// the library's FreeType handle alive with it, and whichever is released
    /// last frees it. Creating a font from a library already released is
    /// `error.InvalidArgument`, not undefined behaviour.
    ///
    /// This is not FreeType's own rule -- `FT_Done_Library` destroys any face
    /// still registered with it -- and it is why `ZtextLibrary` counts its
    /// live fonts. A struct holding a library and a face can release them in
    /// field order without a comment explaining which comes first.
    pub fn deinit(self: Library) void {
        c.ztextLibraryDestroy(self.handle);
    }

    /// Loads a font from an image already in memory.
    ///
    /// `bytes` is BORROWED: FreeType and HarfBuzz both read tables out of it
    /// for as long as the font is alive, so the slice must outlive the
    /// returned `Font` and every face made from it, and must not move or be
    /// written to meanwhile. This is FreeType's own contract and ztext passes
    /// it through rather than hiding a copy nobody asked for.
    pub fn createFont(self: Library, bytes: []const u8, face_index: u32) err.Error!Font {
        var handle: *c.Font = undefined;
        try err.check(c.ztextFontCreateFromMemory(
            self.handle,
            bytes.ptr,
            bytes.len,
            face_index,
            &handle,
        ));
        return .{ .handle = handle, .library = self };
    }

    /// Half-width of the distance-field ramp in pixels, for `RenderMode.sdf`.
    /// FreeType accepts 2..32; anything else is `InvalidArgument` rather than
    /// a silent clamp.
    pub fn setSdfSpread(self: Library, spread: u32) err.Error!void {
        try err.check(c.ztextLibrarySetSdfSpread(self.handle, spread));
    }

    /// Number of faces in a font image: 1 for a plain TTF or OTF, more for a
    /// TrueType collection. `createFont`'s index must be below this.
    pub fn countFaces(self: Library, bytes: []const u8) err.Error!u32 {
        var out: u32 = 0;
        try err.check(c.ztextLibraryCountFaces(self.handle, bytes.ptr, bytes.len, &out));
        return out;
    }
};

/// One parsed font image, shared by every face made from it.
///
/// The home of everything that does not depend on a size: the names, the glyph
/// count, the character map. Making a second `Face` from it costs a size, not
/// another parse.
///
/// Immutable apart from `setVariations`, which is here rather than on `Face`
/// because FreeType keeps variation coordinates on the shared `FT_Face`.
pub const Font = struct {
    handle: *c.Font,
    /// The library this font came from. Carried so the dependency is visible
    /// in the type rather than only in prose: a font must not outlive it.
    library: Library,

    /// Releases this handle's claim on the font.
    ///
    /// May be called before or after its faces' `deinit` with the same result:
    /// the font's memory goes when the last of them does. Faces already made
    /// stay usable; only `face` stops working.
    pub fn deinit(self: Font) void {
        c.ztextFontDestroy(self.handle);
    }

    /// This font at one size. See `Face.setPixelSize` for what the arguments
    /// accept; passing 0 for one axis copies the other.
    pub fn face(self: Font, width: f32, height: f32) err.Error!Face {
        var handle: *c.Face = undefined;
        try err.check(c.ztextFaceCreate(self.handle, width, height, &handle));
        return .{ .handle = handle, .font = self };
    }

    /// Borrowed, valid while the font is alive. `""` when the font does not
    /// name itself.
    pub fn familyName(self: Font) [:0]const u8 {
        return std.mem.span(c.ztextFontFamilyName(self.handle));
    }

    pub fn styleName(self: Font) [:0]const u8 {
        return std.mem.span(c.ztextFontStyleName(self.handle));
    }

    /// Glyph index for a Unicode scalar, or 0 (.notdef) when the font has no
    /// mapping for it. Shaping does its own mapping; this is for checking
    /// coverage before choosing a fallback font.
    pub fn glyphIndex(self: Font, codepoint: u21) u32 {
        return c.ztextFontGlyphIndex(self.handle, codepoint);
    }

    /// Glyph index for a base character followed by a VARIATION SELECTOR --
    /// cmap format 14, which is what U+FE0E/U+FE0F and the Ideographic
    /// Variation Sequences use.
    ///
    /// Nonzero exactly when this font draws this exact pair -- including the
    /// case where the font records the pair as its default and the answer is
    /// the base character's own glyph. 0 for every way it does not; see
    /// `ffi/ztext.h` for the three of them.
    pub fn variantGlyphIndex(self: Font, codepoint: u21, selector: u21) u32 {
        return c.ztextFontVariantGlyphIndex(self.handle, codepoint, selector);
    }

    pub fn glyphCount(self: Font) u32 {
        return c.ztextFontGlyphCount(self.handle);
    }

    /// Design units per em, or 0 for a font with no scalable outlines.
    pub fn unitsPerEm(self: Font) u32 {
        return c.ztextFontUnitsPerEm(self.handle);
    }

    /// Number of variable axes this font declares, or 0 for a static one --
    /// which is an answer, not an error.
    pub fn axisCount(self: Font) u32 {
        return c.ztextFontAxisCount(self.handle);
    }

    /// Describes one axis, in the font's own order. `index` must be below
    /// `axisCount`; anything else is `error.InvalidArgument`.
    pub fn axis(self: Font, index: u32) err.Error!types.VariationAxis {
        var out: types.VariationAxis = undefined;
        try err.check(c.ztextFontAxis(self.handle, index, &out));
        return out;
    }

    /// Moves the named axes, leaving every axis not named where it was.
    ///
    /// The setting belongs to the FONT rather than to a face, because
    /// FreeType keeps variation coordinates on the `FT_Face` that all this
    /// font's faces share. Every one of them moves, and every one of them has
    /// its HarfBuzz side moved with it -- which is the point of the call. Set
    /// only FreeType's half and shaping describes one instance while
    /// rasterisation describes another, with nothing to report it but text
    /// that spaces wrongly.
    ///
    /// `error.InvalidArgument`, with the font untouched, for a tag this font
    /// has no axis for, a value outside that axis's `[min_value, max_value]`
    /// or one that is not finite, and for a font with no axes at all. The
    /// request is checked whole before any of it is applied.
    ///
    /// This invalidates every run already measured against a face of this
    /// font: `Shaper.extents` on one is `error.InvalidArgument` afterwards
    /// rather than a mixture of the two instances.
    pub fn setVariations(self: Font, values: []const types.Variation) err.Error!void {
        try err.check(c.ztextFontSetVariations(
            self.handle,
            if (values.len == 0) null else values.ptr,
            values.len,
        ));
    }

    /// Current design value of one axis, which starts at the axis default.
    pub fn variation(self: Font, index: u32) err.Error!f32 {
        var out: f32 = 0;
        try err.check(c.ztextFontVariation(self.handle, index, &out));
        return out;
    }

    /// Number of NAMED INSTANCES this font declares -- the points in its axis
    /// space that its own designers named. 0 for a static font, and 0 for a
    /// variable font that names none.
    pub fn namedInstanceCount(self: Font) u32 {
        return c.ztextFontNamedInstanceCount(self.handle);
    }

    /// Design coordinates of one named instance, one per axis, in `axis`
    /// order, written into `values` and returned as the prefix that was used.
    ///
    /// `error.BufferTooSmall` when `values` is shorter than `axisCount`, with
    /// nothing written.
    pub fn namedInstanceCoords(
        self: Font,
        index: u32,
        values: []f32,
    ) err.Error![]const f32 {
        // `values.ptr` even for an empty slice: passing null would ask for the
        // count instead, which would come back as success having written
        // nothing -- a caller that ignored the returned slice would then be
        // reading whatever was already in its buffer.
        var count: usize = values.len;
        try err.check(c.ztextFontNamedInstanceCoords(
            self.handle,
            index,
            values.ptr,
            &count,
        ));
        return values[0..count];
    }

    /// Bytes the instance's subfamily name occupies as UTF-8, excluding the
    /// NUL that `namedInstanceName` also writes.
    pub fn namedInstanceNameLen(self: Font, index: u32) err.Error!usize {
        var size: usize = 0;
        try err.check(c.ztextFontNamedInstanceName(self.handle, index, null, &size));
        return size;
    }

    /// Writes the instance's subfamily name into `buffer` as UTF-8 and returns
    /// the part of it that was written, not counting the NUL.
    ///
    /// `buffer` must hold the name AND its NUL, so it needs
    /// `namedInstanceNameLen` + 1 bytes; anything less is
    /// `error.BufferTooSmall`. `error.Unsupported` when the lookup yields
    /// nothing at all -- see `ffi/ztext.h` for the two ways that happens.
    pub fn namedInstanceName(
        self: Font,
        index: u32,
        buffer: []u8,
    ) err.Error![]const u8 {
        var size: usize = buffer.len;
        try err.check(c.ztextFontNamedInstanceName(
            self.handle,
            index,
            buffer.ptr,
            &size,
        ));
        return buffer[0..size];
    }

    /// Moves every axis to a named instance's coordinates in one step.
    ///
    /// The same bargain as `setVariations`, because it is the same commit
    /// path: every face of this font moves with it, and every run already
    /// measured against one of them is refused afterwards.
    pub fn setNamedInstance(self: Font, index: u32) err.Error!void {
        try err.check(c.ztextFontSetNamedInstance(self.handle, index));
    }

    /// How many leading bytes of `utf8` this font can draw, for a host walking
    /// its own fallback list.
    ///
    /// ztext does not own the list -- which font to fall back to is a policy
    /// question -- but it owns the part that is not: the prefix never ends
    /// inside a cluster, so a base and its combining marks always go to the
    /// same font, and format characters like ZWJ never break a run.
    ///
    /// A prefix of 0 means this font cannot start the text at all.
    pub fn coveredPrefix(self: Font, utf8: []const u8) err.Error!usize {
        var out: usize = 0;
        try err.check(c.ztextFontCoveredPrefix(self.handle, utf8.ptr, utf8.len, &out));
        return out;
    }
};

/// One font at one pixel size.
///
/// Faces of a font share its `FT_Face`, and therefore its one glyph slot and
/// its one thread. They do not share a size, a HarfBuzz font, or a rasterised
/// bitmap.
pub const Face = struct {
    handle: *c.Face,
    /// The font this face was made from, carried for the same reason `Font`
    /// carries its library.
    font: Font,

    pub fn deinit(self: Face) void {
        c.ztextFaceDestroy(self.handle);
    }

    /// Changes this face's size in pixels. Passing 0 for one axis copies the
    /// other. A face always has a size, so this is for following a changing
    /// scale factor; it invalidates any run measured against the face.
    ///
    /// Fractional sizes are real: 9 pt at a 150% scale factor is 18.75 px, and
    /// rounding that to 19 drifts against every other element on the same
    /// scaled layout. The value is quantised to 1/64 px, FreeType's own
    /// resolution. Zero, negative, non-finite, above 16384, or so small it
    /// quantises to nothing is `error.InvalidArgument`.
    pub fn setPixelSize(self: Face, width: f32, height: f32) err.Error!void {
        try err.check(c.ztextFaceSetPixelSize(self.handle, width, height));
    }

    pub fn metrics(self: Face) err.Error!types.FaceMetrics {
        var out: types.FaceMetrics = undefined;
        try err.check(c.ztextFaceMetrics(self.handle, &out));
        return out;
    }

    /// One OpenType metric in pixels at this face's current size.
    ///
    /// `metrics` above is FreeType's view: `hhea`, plus `post`'s underline
    /// converted to FreeType's own convention. This is the rest of what
    /// OpenType defines, read through HarfBuzz, which honours the
    /// USE_TYPO_METRICS bit and applies variations. The two disagree in two
    /// places on purpose -- the ascender for a font that sets that bit, and
    /// the underline offset, which is the stroke's top edge here and its
    /// centre there -- and `ffi/ztext.h` says which question each answers.
    ///
    /// `error.Unsupported` when the font's tables do not declare it -- which
    /// is common for x-height and cap-height in older fonts, and for every
    /// vertical metric in a font with no `vhea`. That is an answer: a 0 on its
    /// own could not be told from a font that declares zero.
    pub fn metric(self: Face, which: types.Metric) err.Error!f32 {
        var out: f32 = 0;
        try err.check(c.ztextFaceMetric(self.handle, which, &out));
        return out;
    }

    /// The same, with HarfBuzz's own value synthesised when the font declares
    /// none. Never `error.Unsupported`; the price is that a caller cannot tell
    /// a designed value from an estimate.
    pub fn metricWithFallback(self: Face, which: types.Metric) err.Error!f32 {
        var out: f32 = 0;
        try err.check(c.ztextFaceMetricWithFallback(self.handle, which, &out));
        return out;
    }

    /// Fakes a bold weight for a face with no bold of its own, at FreeType's
    /// own reference strength. Applies to every glyph loaded through this
    /// face from the next call on -- `glyphExtents` and `renderGlyph` always
    /// agree on the same widened glyph, advance included, so bold text does
    /// not overlap.
    ///
    /// Not reflected in shaping: HarfBuzz's own advance queries bypass this
    /// face's glyph loading, so a shaped run's advances do not widen.
    pub fn setSyntheticBold(self: Face, enabled: bool) err.Error!void {
        try err.check(c.ztextFaceSetSyntheticBold(self.handle, @intFromBool(enabled)));
    }

    /// Fakes an italic by shearing the outline about 12 degrees, FreeType's
    /// own reference slant. The advance is untouched, because a shear does
    /// not change how far the pen moves. Same loading-time scope and the same
    /// shaping caveat as `setSyntheticBold`.
    pub fn setSyntheticOblique(self: Face, enabled: bool) err.Error!void {
        try err.check(c.ztextFaceSetSyntheticOblique(self.handle, @intFromBool(enabled)));
    }

    /// Rasterises one glyph.
    ///
    /// The pixels belong to this face and live until its next `renderGlyph`.
    /// Nothing else disturbs them -- not shaping, not measuring, not a call on
    /// a sibling face.
    ///
    /// `offset_x`/`offset_y` place the glyph at a fractional pixel offset, in
    /// 26.6 fixed point -- the unit shaping advances already come back in --
    /// so text laid out at a fractional x is not forced onto the pixel grid.
    /// `0, 0` renders identically to a caller with no notion of subpixel
    /// placement. Ignored in `.sdf` mode: see `ffi/ztext.h`.
    pub fn renderGlyph(
        self: Face,
        glyph_id: u32,
        mode: types.RenderMode,
        hinting: types.Hinting,
        offset_x: i32,
        offset_y: i32,
    ) err.Error!types.GlyphBitmap {
        var out: types.GlyphBitmap = undefined;
        try err.check(c.ztextFaceRenderGlyph(
            self.handle,
            glyph_id,
            mode,
            hinting,
            offset_x,
            offset_y,
            &out,
        ));
        return out;
    }

    /// Metrics for one glyph without rasterising it.
    pub fn glyphExtents(
        self: Face,
        glyph_id: u32,
        hinting: types.Hinting,
    ) err.Error!types.Extents {
        var out: types.Extents = undefined;
        try err.check(c.ztextFaceGlyphExtents(self.handle, glyph_id, hinting, &out));
        return out;
    }

    /// Walks one glyph's outline through `funcs`, for a host that fills its
    /// own shapes -- an offline SDF baker, a path-effect renderer -- rather
    /// than sampling a bitmap. Points arrive in 26.6 fixed point.
    ///
    /// `funcs` is called synchronously and does not need to outlive the call.
    /// A callback returning anything but `.ok` stops decomposition and that
    /// result is what this returns.
    pub fn decomposeOutline(
        self: Face,
        glyph_id: u32,
        hinting: types.Hinting,
        funcs: *const types.OutlineFuncs,
    ) err.Error!void {
        try err.check(c.ztextFaceDecomposeOutline(self.handle, glyph_id, hinting, funcs));
    }
};

/// The rasterised pixels as a slice, or null for a glyph with no ink.
///
/// A free function rather than a method: it takes a bitmap, not a face.
///
/// The slice belongs to the face and dies with its next `renderGlyph`.
///
/// The pitch check is defensive rather than reachable: ztext copies the glyph
/// out of FreeType's slot tightly packed and top-down, so `pitch` is always
/// `width`. It stays because the day a colour format arrives is the day a
/// slice computed from `width` alone would be wrong.
pub fn bitmapRows(bitmap: types.GlyphBitmap) ?[]const u8 {
    const pixels = bitmap.pixels orelse return null;
    if (bitmap.pitch <= 0) return null;
    const pitch: usize = @intCast(bitmap.pitch);
    const total = std.math.mul(usize, pitch, bitmap.height) catch return null;
    return pixels[0..total];
}
