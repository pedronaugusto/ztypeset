//! The Unicode Bidirectional Algorithm and script itemisation, from SheenBidi.
//!
//! What comes out is the input a shaper needs -- runs of one direction and one
//! script -- not a laid-out line. Line breaking, justification and placement
//! belong to a layout engine.
//!
//! Note the difference between "not a laid-out line" and "not per line". UAX #9
//! resolves embedding levels over a paragraph, but applies rules L1 and L2 --
//! the whitespace reset and the run reversal -- over a LINE. `Paragraph`'s runs
//! are the paragraph laid out as one line; when a host wraps, it hands each
//! range back through `Line`.

const std = @import("std");
const c = @import("c.zig");
const err = @import("error.zig");
const types = @import("types.zig");
const text_mod = @import("text.zig");

/// One analysed paragraph.
///
/// Immutable once created and independent of any face, library or thread, so
/// it can be built once and read from several threads.
pub const Paragraph = struct {
    handle: *c.Paragraph,

    /// Analyses one paragraph.
    ///
    /// `text` is a slice of `u8`, `u16` or `u32` -- UTF-8, UTF-16 or UTF-32 --
    /// and its element type is what says which. Every offset and length this
    /// paragraph reports afterwards is counted in those code units, so a
    /// UTF-16 paragraph's runs index the `[]const u16` you passed.
    ///
    /// `text` is read during the call only; the paragraph copies what it
    /// needs, so there is no lifetime to track afterwards. That differs from
    /// `Library.createFont` on purpose -- a paragraph is small, and copying it
    /// removes a footgun.
    ///
    /// The algorithm is defined per paragraph, so text containing a paragraph
    /// separator is analysed up to the first one. `length()` reports how much
    /// was covered.
    pub fn init(text: anytype, options: Options) err.Error!Paragraph {
        const view = text_mod.view(text);
        var handle: *c.Paragraph = undefined;
        try err.check(c.ztextParagraphCreate(
            view.ptr,
            view.len,
            view.encoding,
            options.base,
            options.segmentation,
            &handle,
        ));
        return .{ .handle = handle };
    }

    /// What to analyse, beyond the text itself.
    pub const Options = struct {
        /// The direction to fall back to when the text itself does not say --
        /// `.auto` reads it from the first strong character, which is UAX #9
        /// rule P2.
        base: types.BaseDirection = .auto,
        /// Which segmentation passes to run, as an OR of `Segmentation`; see
        /// `types.segmentation`. Each pass costs one walk of libunibreak and
        /// one byte per code unit for the paragraph's lifetime, and the
        /// accessor for a pass not asked for returns an empty slice. The
        /// default runs all three, which is what a caller that has not
        /// thought about it wants.
        segmentation: u32 = @intFromEnum(types.Segmentation.all),
    };

    /// The segmentation passes this paragraph ran.
    ///
    /// Reported for the same reason as `encoding`: a paragraph outlives the
    /// call that made it, and an empty break array is otherwise
    /// indistinguishable from a pass that was never run.
    pub fn segmentation(self: Paragraph) u32 {
        return c.ztextParagraphSegmentation(self.handle);
    }

    pub fn deinit(self: Paragraph) void {
        c.ztextParagraphDestroy(self.handle);
    }

    /// Code units actually analysed, which is at most the input length --
    /// less if the text contained a paragraph separator.
    pub fn length(self: Paragraph) usize {
        return c.ztextParagraphLength(self.handle);
    }

    /// The encoding this paragraph was built from, which is the unit every
    /// offset and length above is counted in.
    ///
    /// A run list is routinely passed on without the text it came from, and
    /// reading a UTF-16 paragraph's offsets as bytes indexes half a
    /// character. This is what a consumer checks against.
    pub fn encoding(self: Paragraph) types.Encoding {
        return c.ztextParagraphEncoding(self.handle);
    }

    /// Resolved base embedding level: even for left-to-right, odd for
    /// right-to-left.
    pub fn baseLevel(self: Paragraph) u8 {
        return c.ztextParagraphBaseLevel(self.handle);
    }

    /// Convenience over `baseLevel`, for feeding `Params.direction`.
    pub fn baseDirection(self: Paragraph) types.Direction {
        return if (self.baseLevel() % 2 == 0) .ltr else .rtl;
    }

    /// Per-code-unit embedding levels. Every unit of a multi-unit character
    /// carries the same level as its first.
    ///
    /// These are resolved over the PARAGRAPH, before rule L1 resets trailing
    /// whitespace for a particular line. Where the two differ, `Line`'s visual
    /// runs are the authority.
    pub fn levels(self: Paragraph) []const u8 {
        const len = self.length();
        if (len == 0) return &.{};
        const ptr = c.ztextParagraphLevels(self.handle) orelse return &.{};
        return ptr[0..len];
    }

    /// Where a line MAY break, one entry per code unit, describing the
    /// boundary AFTER that unit.
    ///
    /// ztext says where a break is permitted; you decide where one happens,
    /// because that needs a width. The loop is: walk the non-`.none` positions,
    /// measure with `Shaper.extents` until the next would not fit, then hand
    /// the range you chose to `line`.
    pub fn lineBreaks(self: Paragraph) []const types.Break {
        return self.breakSlice(c.ztextParagraphLineBreaks(self.handle));
    }

    /// Grapheme cluster boundaries -- what a caret moves by and what backspace
    /// deletes. Not the same as characters: a base plus its combining marks is
    /// one grapheme, and so is an emoji joined with U+200D.
    pub fn graphemeBreaks(self: Paragraph) []const types.Break {
        return self.breakSlice(c.ztextParagraphGraphemeBreaks(self.handle));
    }

    /// Word boundaries -- double-click selection and word-wise caret movement.
    pub fn wordBreaks(self: Paragraph) []const types.Break {
        return self.breakSlice(c.ztextParagraphWordBreaks(self.handle));
    }

    fn breakSlice(self: Paragraph, ptr: ?[*]const u8) []const types.Break {
        const len = self.length();
        if (len == 0) return &.{};
        const raw = ptr orelse return &.{};
        return @as([*]const types.Break, @ptrCast(raw))[0..len];
    }

    /// The next grapheme boundary at or after `offset`, for moving a caret
    /// right. Returns `length()` at the end rather than running off it.
    pub fn nextGrapheme(self: Paragraph, offset: usize) usize {
        return c.ztextParagraphNextGrapheme(self.handle, offset);
    }

    /// The previous grapheme boundary before `offset`, for moving a caret
    /// left. Returns 0 at the start.
    pub fn previousGrapheme(self: Paragraph, offset: usize) usize {
        return c.ztextParagraphPreviousGrapheme(self.handle, offset);
    }

    /// Runs of one embedding level in VISUAL order: index 0 is leftmost for an
    /// LTR base, rightmost for RTL.
    ///
    /// This is the paragraph laid out as ONE line, which is right for a label
    /// and wrong for anything that wraps -- see `line`.
    pub fn visualRuns(self: Paragraph) []const types.VisualRun {
        const count = c.ztextParagraphVisualRunCount(self.handle);
        if (count == 0) return &.{};
        const ptr = c.ztextParagraphVisualRuns(self.handle) orelse return &.{};
        return ptr[0..count];
    }

    /// Runs of one script in LOGICAL order.
    pub fn scriptRuns(self: Paragraph) []const types.ScriptRun {
        const count = c.ztextParagraphScriptRunCount(self.handle);
        if (count == 0) return &.{};
        const ptr = c.ztextParagraphScriptRuns(self.handle) orelse return &.{};
        return ptr[0..count];
    }

    /// What to actually shape: visual runs intersected with script runs, in
    /// visual order, each uniform in both direction and script.
    ///
    /// This is the list to iterate for a paragraph that fits on one line.
    /// `visualRuns` and `scriptRuns` are the inputs it is built from and are
    /// exposed for callers doing something else with them -- shaping straight
    /// from `visualRuns` is wrong the moment a run spans two scripts.
    pub fn shapingRuns(self: Paragraph) []const types.ShapingRun {
        const count = c.ztextParagraphShapingRunCount(self.handle);
        if (count == 0) return &.{};
        const ptr = c.ztextParagraphShapingRuns(self.handle) orelse return &.{};
        return ptr[0..count];
    }

    /// Reorders one range of this paragraph as its own line.
    ///
    /// Use this for anything that wraps. `offset` and `len` are code units
    /// within the paragraph, and the runs come back with paragraph-relative
    /// offsets too, so they index the same slice you already have.
    ///
    /// A range that ends past `length()`, or that starts or ends inside a
    /// character, is `error.InvalidArgument`.
    pub fn line(self: Paragraph, offset: usize, len: usize) err.Error!Line {
        var handle: *c.Line = undefined;
        try err.check(c.ztextLineCreate(self.handle, offset, len, &handle));
        return .{ .handle = handle };
    }
};

/// One line of a paragraph, reordered over its own range.
///
/// Rules L1 and L2 of UAX #9 are defined per line, so a wrapped paragraph
/// cannot be laid out from `Paragraph.shapingRuns` alone. The visible symptom
/// is trailing whitespace: spaces between two right-to-left words resolve to a
/// right-to-left level over the paragraph, but a line that ends just after
/// them resets them to the base level, which moves them to the other end of
/// that line.
///
/// A line copies what it needs, so it holds no reference to the paragraph it
/// came from and may outlive it.
pub const Line = struct {
    handle: *c.Line,

    pub fn deinit(self: Line) void {
        c.ztextLineDestroy(self.handle);
    }

    /// Code-unit offset of this line within its paragraph.
    pub fn offset(self: Line) usize {
        return c.ztextLineOffset(self.handle);
    }

    /// Length of this line, in the paragraph's code units.
    pub fn length(self: Line) usize {
        return c.ztextLineLength(self.handle);
    }

    /// Runs of one embedding level in VISUAL order, with L1 and L2 applied
    /// over this line.
    pub fn visualRuns(self: Line) []const types.VisualRun {
        const count = c.ztextLineVisualRunCount(self.handle);
        if (count == 0) return &.{};
        const ptr = c.ztextLineVisualRuns(self.handle) orelse return &.{};
        return ptr[0..count];
    }

    /// What to actually shape for this line: its visual runs intersected with
    /// the paragraph's script runs.
    pub fn shapingRuns(self: Line) []const types.ShapingRun {
        const count = c.ztextLineShapingRunCount(self.handle);
        if (count == 0) return &.{};
        const ptr = c.ztextLineShapingRuns(self.handle) orelse return &.{};
        return ptr[0..count];
    }
};

/// Direction implied by an embedding level: even is left-to-right.
///
/// A free function rather than a method, because it takes a run and not a
/// paragraph -- `paragraph.runDirection(run)` does not compile and never did.
pub fn runDirection(level: u8) types.Direction {
    return if (level % 2 == 0) .ltr else .rtl;
}
