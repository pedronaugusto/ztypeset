//! Shaping one run of text through HarfBuzz.

const std = @import("std");
const c = @import("c.zig");
const err = @import("error.zig");
const types = @import("types.zig");
const text_mod = @import("text.zig");
const face_mod = @import("face.zig");

/// What to shape and how.
///
/// The defaults shape a left-to-right run with the script guessed from the
/// text, no language, no explicit features, and metrics from HarfBuzz's own
/// OpenType reader. In practice `direction` and `script` should come from
/// `bidi.Paragraph`, not from the guess.
pub const Params = struct {
    direction: types.Direction = .auto,
    /// ISO 15924 as a tag -- `types.tag("Arab")`. 0 asks HarfBuzz to guess.
    script: u32 = 0,
    /// BCP 47 language tag. Affects language-specific behaviour such as
    /// Turkish dotless i.
    language: ?[:0]const u8 = null,
    features: []const types.Feature = &.{},
    cluster_level: types.ClusterLevel = .monotone_graphemes,
    /// `false` (the default) takes advances from HarfBuzz's own table reader:
    /// they scale linearly from design units, so layout does not move when
    /// hinting changes and the shaping font stays immutable.
    ///
    /// `true` takes them from FreeType, computed from the same face that will
    /// rasterise, with hinting on. Pair it with `Hinting.normal` when
    /// rendering -- mixing hinted advances with an unhinted raster, or the
    /// reverse, is what makes text drift.
    use_freetype_metrics: bool = false,

    fn toC(self: Params) c.ShapeParams {
        return .{
            .direction = self.direction,
            .script = self.script,
            .language = if (self.language) |l| l.ptr else null,
            .features = if (self.features.len == 0) null else self.features.ptr,
            .feature_count = self.features.len,
            .cluster_level = self.cluster_level,
            .use_freetype_metrics = if (self.use_freetype_metrics) 1 else 0,
        };
    }
};

/// Reusable shaping scratch.
///
/// A user interface shapes the same strings every frame, so the HarfBuzz
/// buffer and the converted glyph array live on a handle the caller keeps.
/// Once the arrays have reached their high-water mark, shaping allocates
/// nothing.
///
/// One shaper serves one shaping call at a time; give each thread its own.
pub const Shaper = struct {
    handle: *c.Shaper,

    pub fn init() err.Error!Shaper {
        var handle: *c.Shaper = undefined;
        try err.check(c.ztextShaperCreate(&handle));
        return .{ .handle = handle };
    }

    pub fn deinit(self: Shaper) void {
        c.ztextShaperDestroy(self.handle);
    }

    /// Shapes one run with one face, one direction and one script.
    ///
    /// This does not itemise: split mixed-direction or mixed-script text with
    /// `bidi.Paragraph` first and call this once per run.
    ///
    /// `text` is a slice of `u8`, `u16` or `u32`, and its element type is
    /// what says which encoding it is in. It is rejected with `InvalidText`
    /// if it is malformed, rather than silently substituting replacement
    /// characters the way HarfBuzz would. Returns the shaped glyphs, so the
    /// borrow is visibly tied to this call.
    ///
    /// The slice is owned by the shaper and is invalidated by the next `shape`
    /// on it -- not merely overwritten: the array is grown in place and a
    /// longer run moves it, so a slice kept across a call can point at freed
    /// memory. Copy what you need, or consume it before shaping again.
    pub fn shape(
        self: Shaper,
        target: face_mod.Face,
        text: anytype,
        params: Params,
    ) err.Error![]const types.Glyph {
        const view = text_mod.view(text);
        const c_params = params.toC();
        try err.check(c.ztextShaperShape(
            self.handle,
            target.handle,
            view.ptr,
            view.len,
            view.encoding,
            0,
            view.len,
            &c_params,
        ));
        return self.glyphs();
    }

    /// Shapes one run of `text` WITH the rest of it as context.
    ///
    /// This is the call to use for anything that came out of a `Paragraph` or
    /// a `Line`, and the only correct one once a host splits a word between
    /// fonts -- which `Font.coveredPrefix` invites it to do. Shaping the two
    /// halves of an Arabic word separately gives the letter before the split a
    /// final form and the letter after it an initial form, when both should be
    /// medial. Passing the surrounding text costs nothing and removes the
    /// whole class of error.
    ///
    /// `direction` and `script` come from the run, because that is what a run
    /// is for; `params` supplies the rest. Cluster values are code-unit
    /// offsets into `text`, so they index the slice you already have -- and
    /// `text` must be in the encoding its `Paragraph` was built from, since
    /// `run`'s offsets are counted in that.
    pub fn shapeRun(
        self: Shaper,
        target: face_mod.Face,
        text: anytype,
        run: types.ShapingRun,
        params: Params,
    ) err.Error![]const types.Glyph {
        const view = text_mod.view(text);
        var with_run = params;
        with_run.direction = if (run.level % 2 == 0) .ltr else .rtl;
        with_run.script = run.script;
        const c_params = with_run.toC();
        try err.check(c.ztextShaperShape(
            self.handle,
            target.handle,
            view.ptr,
            view.len,
            view.encoding,
            run.offset,
            run.length,
            &c_params,
        ));
        return self.glyphs();
    }

    /// Glyphs from the last successful shape, in visual order. Empty after a
    /// shape that failed or produced nothing.
    ///
    /// Borrowed, and invalidated -- not just overwritten -- by the next
    /// `shape` on this shaper. Prefer the slice `shape` returns.
    pub fn glyphs(self: Shaper) []const types.Glyph {
        const count = c.ztextShaperGlyphCount(self.handle);
        if (count == 0) return &.{};
        const ptr = c.ztextShaperGlyphs(self.handle) orelse return &.{};
        return ptr[0..count];
    }

    /// The direction actually used, which is what `.auto` resolved to.
    pub fn direction(self: Shaper) types.Direction {
        return c.ztextShaperDirection(self.handle);
    }

    /// Ink bounds and total advance of the last shape.
    ///
    /// `target` must be the face the run was shaped with. It is a parameter
    /// rather than something the shaper remembers, deliberately: a stored face
    /// would dangle the moment it was destroyed and nothing would say so.
    /// Metrics come from whichever source the shape used, so extents and
    /// advances always describe the same font.
    ///
    /// Costs one glyph-extents query per glyph, so cache the result rather
    /// than asking every frame.
    pub fn extents(self: Shaper, target: face_mod.Face) err.Error!types.Extents {
        var out: types.Extents = undefined;
        try err.check(c.ztextShaperExtents(self.handle, target.handle, &out));
        return out;
    }
};
