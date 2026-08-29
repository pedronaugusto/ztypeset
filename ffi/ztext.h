//===----------------------------------------------------------------------===//
// ztext -- a C ABI over FreeType, HarfBuzz and SheenBidi.
//
// This header is the contract between the C implementation and the Zig wrapper
// in ../src. Unlike a binding over a C++ library, it is not here because Zig
// cannot call the upstreams -- all three expose C APIs, and build.zig installs
// their headers so a C or C++ host can use them directly, which for anything
// beyond ztext's scope is the better choice.
//
// It is here for one reason: FreeType's FT_FaceRec and FT_GlyphSlotRec are
// large, deeply nested, and partly conditional on FreeType's own configuration
// macros. Hand-mirroring those in Zig would put silent memory corruption one
// re-vendor away. Stopping them at a C boundary means the C compiler checks
// every upstream layout change for free, and only small, flat, ztext-owned
// structs cross into Zig.
//
// Ownership, uniformly:
//   *Create    allocates through the installed allocator; caller owns the
//              handle and must pass it to the matching *Destroy.
//   *Destroy   accepts NULL.
//   accessors  return pointers that borrow from the handle and die with it,
//              or sooner where noted.
//
// ORDERING: a ZtextFont and every ZtextFace made from it must be destroyed
// BEFORE the ZtextLibrary they came from. FT_Done_Library destroys every face
// still registered with it, so destroying the library first leaves the others
// operating on freed memory. This is FreeType's rule and ztext does not hide
// it; a library outliving its fonts is the only supported order.
//
// Fonts and faces have NO order between them. Whichever is released second
// frees the font, so a caller that destroys a font before its faces gets
// correct behaviour rather than a dangling FT_Face. Creating a face from a
// font already destroyed is an error, not undefined.
//
// Threading: see "Thread safety" below. Read it -- FT_Face is not thread-safe
// and ztext does not pretend otherwise.
//===----------------------------------------------------------------------===//

#ifndef ZTEXT_H_
#define ZTEXT_H_

#include <stddef.h>
#include <stdint.h>

/* Exported deliberately and narrowly.
 *
 * A shared ztext is built with -fvisibility=hidden, so the ~10 000 FreeType,
 * HarfBuzz and SheenBidi symbols linked into it stay internal. Without that,
 * loading libztext.so alongside a system libfreetype -- anything that pulls in
 * pango, cairo or fontconfig does -- lets the two interpose on each other, and
 * one library's FT_Face ends up inside the other's functions.
 *
 * A consumer of an MSVC DLL must define ZTEXT_SHARED before including this
 * header, so the declarations become dllimport. Everywhere else nothing is
 * needed. */
#if defined(_MSC_VER) && defined(ZTEXT_SHARED)
#ifdef ZTEXT_BUILD
#define ZTEXT_API __declspec(dllexport)
#else
#define ZTEXT_API __declspec(dllimport)
#endif
#elif defined(ZTEXT_SHARED) && (defined(__GNUC__) || defined(__clang__))
#define ZTEXT_API __attribute__((visibility("default")))
#else
#define ZTEXT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Thread safety
//
// FreeType's FT_Library and FT_Face are not internally synchronised, and that
// restriction propagates:
//
//   * A ZtextLibrary, every ZtextFont made from it, and every ZtextFace made
//     from those, belong to ONE thread. The faces of a font share its FT_Face
//     and its single glyph slot, so they are not independent even though they
//     are separate handles. Use one ZtextLibrary per thread rather than
//     sharing one behind a lock; FreeType's own documentation recommends the
//     same.
//   * ztextFaceRenderGlyph returns pixels the FACE owns, copied out of that
//     shared slot. They are valid until the next ztextFaceRenderGlyph on the
//     same face, and nothing else invalidates them -- not a call on a sibling
//     face, not shaping, not measuring. Copying is what buys that; see the
//     note on ZtextGlyphBitmap.
//   * A ZtextShaper holds scratch for one shaping call at a time. Give each
//     thread its own; they are cheap.
//   * ZtextParagraph does not touch FreeType at all. Once created it is
//     immutable and may be read from several threads.
//
// The allocator installed by ztextSetAllocator is process-wide (HarfBuzz's
// seam is compile-time, so it cannot be otherwise) and must therefore be
// thread-safe if ztext is used from more than one thread.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Version
//===----------------------------------------------------------------------===//

#define ZTEXT_VERSION_MAJOR 0
#define ZTEXT_VERSION_MINOR 1
#define ZTEXT_VERSION_PATCH 0

/// Version of the ztext binding, packed as (major<<16)|(minor<<8)|patch.
/// Compare against the ZTEXT_VERSION_* macros to detect a header/library skew.
ZTEXT_API uint32_t ztextVersion(void);

/// Versions of the vendored upstreams, same packing. These report what was
/// actually compiled in, not what UPSTREAM.md claims.
ZTEXT_API uint32_t ztextFreetypeVersion(void);
ZTEXT_API uint32_t ztextHarfbuzzVersion(void);
ZTEXT_API uint32_t ztextSheenbidiVersion(void);
ZTEXT_API uint32_t ztextUnibreakVersion(void);

/// Packs four characters into an OpenType tag, big-endian as the specs write
/// them: ZTEXT_TAG('l','i','g','a').
#define ZTEXT_TAG(a, b, c, d)                                       \
  ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) |       \
              ((uint32_t)(c) << 8) | (uint32_t)(d)))

//===----------------------------------------------------------------------===//
// Results
//===----------------------------------------------------------------------===//

typedef enum ZtextResult {
  ZTEXT_RESULT_OK = 0,
  /// The allocator returned NULL, or an upstream reported an allocation
  /// failure of its own.
  ZTEXT_RESULT_OUT_OF_MEMORY = 1,
  /// A NULL handle, a zero-length buffer, an out-of-range index, or a
  /// non-finite scalar.
  ZTEXT_RESULT_INVALID_ARGUMENT = 2,
  /// The text was not well-formed UTF-8. Checked by ztext before any of it
  /// reaches HarfBuzz or SheenBidi.
  ZTEXT_RESULT_INVALID_UTF8 = 3,
  /// FreeType refused the bytes: not a font, truncated, or structurally
  /// broken.
  ZTEXT_RESULT_BAD_FONT = 4,
  /// A recognisable font in a format this build does not compile support for
  /// -- WOFF, WOFF2, Type 1, bitmap-only formats. See UPSTREAM.md.
  ZTEXT_RESULT_UNSUPPORTED = 5,
  /// The glyph index is not present in the face.
  ZTEXT_RESULT_GLYPH_NOT_FOUND = 6,
  /// FreeType loaded the glyph but could not rasterise it.
  ZTEXT_RESULT_RENDER_FAILED = 7,
  /// HarfBuzz could not shape the run.
  ZTEXT_RESULT_SHAPE_FAILED = 8,
  /// SheenBidi could not analyse the paragraph.
  ZTEXT_RESULT_BIDI_FAILED = 9,
  /// A caller-provided output buffer was too small.
  ZTEXT_RESULT_BUFFER_TOO_SMALL = 10,
} ZtextResult;

/// Static, never-NULL description of a result code. Borrowed; do not free.
ZTEXT_API const char* ztextResultName(ZtextResult result);

/// The last message an upstream produced ON THIS THREAD, or "" if there was
/// none.
///
/// ztext's result enum is flat by design, which loses detail FreeType has and
/// ztext does not: "unknown file format" and "invalid table" both arrive as
/// ZTEXT_RESULT_BAD_FONT. This returns FreeType's own string for the most
/// recent failure so a log line can say which.
///
/// The storage is thread-local, so it must be read on the thread that got the
/// error -- a logger running elsewhere sees "". Borrowed, overwritten by the
/// next failing call, and for diagnostics only: never branch on it.
ZTEXT_API const char* ztextLastErrorDetail(void);

//===----------------------------------------------------------------------===//
// Allocator seam
//
// The three upstreams that allocate all allow it to be redirected, and all
// do it differently:
//
//   FreeType   an FT_MemoryRec per FT_Library. ztext captures the installed
//              allocator when a ZtextLibrary is created, so FreeType memory is
//              genuinely per-library rather than global.
//   SheenBidi  a global default allocator object (its creation functions take
//              no allocator argument).
//   HarfBuzz   compile-time only: four macros resolved when the library is
//              built. Necessarily process-wide.
//
// HarfBuzz is the binding constraint, so ztextSetAllocator is process-wide.
// That is surfaced here rather than hidden behind a per-object parameter that
// could not be honoured.
//
// All three also free without a size -- FT_Free_Func, hb_free_impl and
// SBAllocatorDeallocateBlockFunc each receive only a pointer. Rather than push
// that asymmetry onto every host, ztext records the size and alignment in a
// header ahead of each block and hands them back on deallocate. A host with a
// sized allocator (Zig's std.mem.Allocator, a pool, an arena with accounting)
// therefore needs no bookkeeping of its own.
//===----------------------------------------------------------------------===//

typedef struct ZtextAllocator {
  /// Must return a block of at least `size` bytes aligned to `alignment`
  /// (always a power of two), or NULL on failure. `size` is never 0.
  void* (*allocate)(void* user, size_t size, size_t alignment);

  /// Optional; may be NULL. Provided so a host that can grow a block in place
  /// is not forced through a copy.
  ///
  /// Return NULL to decline -- because the block cannot grow in place, or
  /// because there is no memory. ztext does not distinguish the two: it falls
  /// back to allocate, copy, deallocate either way, and only then reports
  /// failure. `block` must be left valid and untouched when declining, which
  /// is what makes that fallback safe.
  ///
  /// Declining is expected, not exceptional: Zig's std.mem.Allocator only
  /// ever resizes in place and declines the moment a block would have to
  /// move.
  void* (*reallocate)(void* user, void* block, size_t old_size,
                      size_t new_size, size_t alignment);

  /// Frees a block from `allocate` or `reallocate`, with the size and
  /// alignment it was allocated with. Never called with a NULL block.
  void (*deallocate)(void* user, void* block, size_t size, size_t alignment);

  /// Opaque host pointer, passed back unmodified.
  void* user;
} ZtextAllocator;

/// Installs a process-wide allocator for all subsequent ztext allocation.
///
/// Call it before creating anything, and do not swap it while live handles
/// exist -- a handle is freed through whichever allocator is installed at
/// destruction time, except for FreeType memory, which each ZtextLibrary
/// captured at creation. Passing NULL restores malloc/free.
///
/// `alloc` is copied by value; the caller need not keep the struct alive, but
/// `user` must outlive every handle allocated through it.
///
/// Returns ZTEXT_RESULT_INVALID_ARGUMENT if `allocate` or `deallocate` is NULL,
/// in which case the previously installed allocator is left untouched.
ZTEXT_API ZtextResult ztextSetAllocator(const ZtextAllocator* alloc);

/// Populates the process-global caches the upstreams never free before exit,
/// so a host can install a tracking allocator afterwards and still see a
/// balanced heap.
///
/// Optional. Nothing needs it to work correctly; it exists because two of the
/// upstreams keep a cache for the life of the process -- HarfBuzz interns
/// language tags in a list it frees from an atexit handler, and SheenBidi's
/// allocator object is created once and kept -- and a host auditing its own
/// allocations would otherwise attribute those to whatever happened to be
/// running when they were first touched. Both are bounded and small; see
/// UPSTREAM.md.
///
/// Two such caches are out of its reach, because both are built from a real
/// face that warm-up has no way to obtain: HarfBuzz's FreeType font-functions
/// singleton, created by the first shape with `use_freetype_metrics` (about
/// 200 bytes), and one intern-table entry per DISTINCT language tag ever
/// passed (tens of bytes each). A host that uses neither allocates neither; a
/// host that audits and uses either can warm them by shaping one throwaway run
/// before installing its allocator.
///
/// Call it before ztextSetAllocator if you intend to audit. Safe to call more
/// than once, and safe never to call.
ZTEXT_API void ztextWarmup(void);

//===----------------------------------------------------------------------===//
// Library, fonts and faces
//
// Three handles, because there are three lifetimes:
//
//   ZtextLibrary   FreeType's modules and the allocator they were built with.
//   ZtextFont      one font image, parsed once. Its only mutable state is
//                  its variation axes, which are per-font because FreeType
//                  keeps them on the FT_Face; see "Variable fonts".
//   ZtextFace      that font at one size. Cheap; make one per size you draw.
//
// The split is not decoration. A ZtextFace used to carry its own FT_Face and
// its own hb_face_t, so four sizes of one font meant four full parses --
// measured at 47 KB per additional size. Sharing what does not depend on the
// size costs a face 296 bytes of HarfBuzz instead of ~14 KB, and about 5 KB
// less of FreeType. It is also the only arrangement in which a size-
// independent question -- the family name, the glyph count, whether a
// character is covered -- is asked of something that does not have a size.
//
// Both upstreams model it the same way (hb_face_t/hb_font_t, FT_Face/FT_Size),
// which is the other reason to: a reader who knows either one already knows
// this.
//===----------------------------------------------------------------------===//

/// Owns an FT_Library and the FreeType modules registered in it. Not
/// thread-safe; see "Thread safety".
typedef struct ZtextLibrary ZtextLibrary;

/// One parsed font image, shared by every face made from it.
///
/// Immutable apart from its variation axes, which live here rather than on a
/// face because FreeType keeps them on the shared FT_Face.
typedef struct ZtextFont ZtextFont;

/// One font at one pixel size. Everything that depends on the size, and
/// nothing that does not.
typedef struct ZtextFace ZtextFace;

ZTEXT_API ZtextResult ztextLibraryCreate(ZtextLibrary** out);
ZTEXT_API void ztextLibraryDestroy(ZtextLibrary* library);

/// Creates a font from an image already in memory.
///
/// There is deliberately no path-based entry point. A host feeding fonts out
/// of an asset pack has the bytes, not a path, and a file API would have to
/// carry FreeType's stream layer and its own error surface for no one's
/// benefit.
///
/// `data` is BORROWED, not copied: FreeType and HarfBuzz both read tables out
/// of it lazily for as long as the font is alive. The buffer must outlive the
/// ZtextFont and every face made from it, and must not move or be written to
/// in the meantime. This is FreeType's contract for FT_New_Memory_Face and
/// ztext passes it through rather than hiding a copy the caller did not ask
/// for.
///
/// `face_index` selects a face inside a collection (.ttc); use 0 otherwise.
/// ztextLibraryCountFaces says how many there are.
ZTEXT_API ZtextResult ztextFontCreateFromMemory(ZtextLibrary* library,
                                                const void* data, size_t size,
                                                uint32_t face_index,
                                                ZtextFont** out);

/// Releases the caller's claim on the font.
///
/// The font's memory goes when its last face does, so this may be called
/// before or after ztextFaceDestroy with the same result. Faces created from
/// it stay fully usable; only ztextFaceCreate stops working.
ZTEXT_API void ztextFontDestroy(ZtextFont* font);

/// Borrowed family and style names, "" when the font does not name itself.
/// Valid while the font is alive.
ZTEXT_API const char* ztextFontFamilyName(const ZtextFont* font);
ZTEXT_API const char* ztextFontStyleName(const ZtextFont* font);

/// Glyph index for a Unicode scalar in the font's character map, or 0
/// (.notdef) if it has none. Note that shaping does its own mapping -- this is
/// for callers checking coverage before choosing a fallback font.
ZTEXT_API uint32_t ztextFontGlyphIndex(const ZtextFont* font,
                                       uint32_t codepoint);

/// Number of glyphs, and design units per em. The latter is 0 for a font with
/// no scalable outlines.
ZTEXT_API uint32_t ztextFontGlyphCount(const ZtextFont* font);
ZTEXT_API uint32_t ztextFontUnitsPerEm(const ZtextFont* font);

/// How many leading bytes of `utf8` this font can draw, for a host walking its
/// own fallback list.
///
/// ztext does not own that list, because which font to fall back to is a
/// policy question -- a UI's answer differs from a document reader's, and both
/// differ from a game's. What ztext owns is the part that is not a policy
/// question and is easy to get wrong:
///
///   * The prefix never ends in the middle of a cluster. A combining mark can
///     never start a run, so a base and its marks always go to the same font.
///     Splitting there is the classic fallback bug: the accent renders in a
///     different typeface from the letter, or lands on the wrong side of it.
///   * Format characters -- ZWJ, ZWNJ, the bidi controls, soft hyphen -- are
///     treated as covered by every font. They rarely have glyphs, HarfBuzz
///     removes them while shaping, and breaking a run at one would split a
///     ligature or a joining form for nothing. Variation selectors likewise.
///
/// So the loop a host writes is: ask this, shape and draw that prefix with
/// this font, then advance and ask the next font in the list. A prefix of 0
/// means this font cannot start the text at all -- move on. It is never
/// partial through a character.
///
/// Rejects malformed UTF-8 with ZTEXT_RESULT_INVALID_UTF8, like everything
/// else that takes text.
ZTEXT_API ZtextResult ztextFontCoveredPrefix(const ZtextFont* font,
                                             const char* utf8, size_t length,
                                             size_t* out);

// Variable fonts.
//
// A variable font carries an `fvar` table naming a few axes -- weight, width,
// optical size -- and one set of outlines interpolated across the range each
// axis declares. Choosing a coordinate on every axis picks one instance out of
// that continuum, which is what the four functions below do.
//
// The setting belongs to the FONT, not to a face, and that is FreeType's
// arrangement rather than a choice ztext made: FT_Set_Var_Design_Coordinates
// takes an FT_Face, and every face of a font shares one of those. So setting
// an axis moves every face of the font at once. A per-face API would read
// correctly right up to the moment a second size existed, which is a worse
// place to learn it.
//
// Both halves are set together, and that is the whole reason this is an API
// rather than a paragraph in the README. HarfBuzz's hb_font_t carries
// variation coordinates of its own, so setting FreeType's alone leaves shaping
// describing one instance and rasterisation another -- advances from a regular
// weight under the outlines of a bold one. Nothing reports an error; the text
// simply spaces wrongly, which is the class of defect this package exists to
// make unreachable. ztextFontSetVariations updates FreeType and the HarfBuzz
// font of every live face in one call, so the two cannot come apart.
//
// Values are DESIGN coordinates -- the numbers `fvar` itself names, 400 for a
// regular weight and 700 for a bold one -- not the normalised -1..1 the
// OpenType internals work in. A value outside an axis's range is refused
// rather than clamped, the same bargain ztextLibrarySetSdfSpread makes, so a
// caller asking for a weight the font does not have finds out.

/// One variable axis, in design units.
typedef struct ZtextVariationAxis {
  /// Four-character tag: 'wght', 'wdth', 'slnt', 'opsz', 'ital'. Build one
  /// with ZTEXT_TAG.
  uint32_t tag;
  float min_value;
  float default_value;
  float max_value;
} ZtextVariationAxis;

/// One axis set to one value.
typedef struct ZtextVariation {
  uint32_t tag;
  float value;
} ZtextVariation;

/// Number of variable axes, or 0 for a static font -- which is not an error,
/// just the answer.
ZTEXT_API uint32_t ztextFontAxisCount(const ZtextFont* font);

/// Describes axis `index`, which must be below ztextFontAxisCount.
///
/// The axes are in the font's own order, and that order is what
/// ztextFontVariation indexes too.
ZTEXT_API ZtextResult ztextFontAxis(const ZtextFont* font, uint32_t index,
                                    ZtextVariationAxis* out);

/// Moves the named axes, leaving every axis not named where it already was.
///
/// Starting from the current setting rather than from the defaults is the
/// behaviour a host wants: a weight slider and a width slider are two
/// controls, and moving one must not snap the other back.
///
/// Refused, with the font left exactly as it was:
///
///   * a tag that is not an axis of this font -- silently ignoring it would
///     make a typo in a four-character constant invisible;
///   * a value outside the axis's [min, max], or one that is not finite --
///     FreeType clamps such a value, ztext refuses it, exactly as with the
///     SDF spread;
///   * a font with no axes at all.
///
/// The whole request is checked before any of it is applied, so a rejection
/// never leaves half the axes moved.
///
/// This invalidates every run already measured against a face of this font:
/// HVAR moves the advances and MVAR can move the ascender, so a run measured
/// before the change is not one that can be laid out after it.
ZTEXT_API ZtextResult ztextFontSetVariations(ZtextFont* font,
                                             const ZtextVariation* values,
                                             size_t count);

/// Current design value of axis `index`, which starts at the axis default.
ZTEXT_API ZtextResult ztextFontVariation(const ZtextFont* font, uint32_t index,
                                         float* out);

/// Creates a face: this font, at this size.
///
/// A face is never sizeless -- the size is part of what it is -- so there is
/// no state in which measuring or rendering has to be refused for want of one.
/// See ztextFaceSetPixelSize for what the size arguments accept.
///
/// Faces of one font share its FT_Face, and therefore its single glyph slot
/// and its one thread. They do not share a size, a HarfBuzz font, or a glyph
/// bitmap.
ZTEXT_API ZtextResult ztextFaceCreate(ZtextFont* font, float width,
                                      float height, ZtextFace** out);

ZTEXT_API void ztextFaceDestroy(ZtextFace* face);

/// The font this face was made from, borrowed. Never NULL for a live face.
ZTEXT_API ZtextFont* ztextFaceFont(const ZtextFace* face);

/// Changes this face's size in pixels. Passing 0 for one axis copies the
/// other.
///
/// A face already has a size from ztextFaceCreate; this is for a face that
/// follows a changing scale factor, and it invalidates any run measured
/// against the face.
///
/// Fractional sizes are real, not rounded away: 9 pt at a 150% scale factor is
/// 18.75 px, and a UI that rounds that to 19 drifts against every other
/// element on the same scaled layout. The value is quantised to 1/64 px, which
/// is FreeType's own resolution, and anything that would quantise to zero is
/// ZTEXT_RESULT_INVALID_ARGUMENT rather than a face that renders nothing.
///
/// Non-finite values are refused. So is anything above 16384 px, which is a
/// caller error long before it is a FreeType one.
ZTEXT_API ZtextResult ztextFaceSetPixelSize(ZtextFace* face, float width,
                                            float height);

/// Scaled face-wide metrics.
///
/// One thing to know before laying out at a fractional size: FreeType
/// GRID-FITS the four scaled fields below to whole pixels, while advances and
/// ink move continuously. Set 18.0 px and then 18.5 px and `line_height` is
/// the same number both times, even though every advance changed. That is
/// FreeType's own behaviour (ft_recompute_scaled_metrics, and a
/// GRID_FIT_METRICS that is defined unconditionally rather than by a build
/// option), passed through rather than smoothed over. A host that wants a
/// fractional leading should compute it from `units_per_em` itself.
typedef struct ZtextFaceMetrics {
  /// Distance from the baseline to the top of the typical glyph, in pixels,
  /// positive upward. Grid-fitted; see above.
  float ascender;
  /// Baseline to the bottom, in pixels, NEGATIVE downward -- the sign
  /// FreeType uses, kept rather than flipped so a reader comparing against
  /// FreeType's documentation is not misled. Grid-fitted.
  float descender;
  /// Recommended baseline-to-baseline distance, in pixels. Grid-fitted.
  float line_height;
  /// Widest advance in the face, in pixels. Grid-fitted.
  float max_advance;
  float underline_position;
  float underline_thickness;
  /// Design units per em, and the glyph count. Both are properties of the
  /// font rather than of this size; they are repeated here because a caller
  /// deciding a layout wants them in the same answer, and are on ZtextFont
  /// too for a caller that has no face yet.
  uint32_t units_per_em;
  uint32_t num_glyphs;
  /// The vertical pixel size currently set. Quantised to 1/64 px, so it may
  /// differ from what was asked for in the last bit.
  float pixel_size;

  /// Column-direction analogues of the four fields above, for a host laying
  /// out vertical text: how far a column extends either side of its
  /// baseline, the recommended column-to-column distance, and the widest
  /// per-glyph vertical advance. Real when the font has a `vhea`/`vmtx`
  /// (`has_vertical_metrics` is nonzero); otherwise synthesised from
  /// `ascender` and `descender` -- the same span HarfBuzz's own vertical
  /// advance fallback uses, so a shaped run's advances land in the same
  /// range as these.
  float vert_ascender;
  float vert_descender;
  float vert_line_height;
  float vert_max_advance;
  /// Nonzero when the four fields above are read from the font's own
  /// vhea/vmtx tables rather than synthesised.
  uint32_t has_vertical_metrics;
} ZtextFaceMetrics;

ZTEXT_API ZtextResult ztextFaceMetrics(const ZtextFace* face,
                                       ZtextFaceMetrics* out);

//===----------------------------------------------------------------------===//
// Shaping
//===----------------------------------------------------------------------===//

/// Reusable shaping scratch: a HarfBuzz buffer and the glyph array converted
/// out of it.
///
/// A user interface shapes the same strings every frame, so the buffer is
/// owned by a handle the caller keeps rather than allocated per call. After
/// the first few calls a steady-state shape allocates nothing.
typedef struct ZtextShaper ZtextShaper;

typedef enum ZtextDirection {
  /// Let HarfBuzz infer it from the script. Prefer passing the direction from
  /// bidi analysis instead: inference is per-script and cannot know that this
  /// run sits inside an RTL paragraph.
  ZTEXT_DIRECTION_AUTO = 0,
  ZTEXT_DIRECTION_LTR = 1,
  ZTEXT_DIRECTION_RTL = 2,
  ZTEXT_DIRECTION_TTB = 3,
  ZTEXT_DIRECTION_BTT = 4,
} ZtextDirection;

/// How finely clusters are allowed to be split. The default merges a base and
/// its combining marks into one cluster, which is what a caret and a selection
/// highlight want.
typedef enum ZtextClusterLevel {
  ZTEXT_CLUSTER_LEVEL_MONOTONE_GRAPHEMES = 0,
  ZTEXT_CLUSTER_LEVEL_MONOTONE_CHARACTERS = 1,
  ZTEXT_CLUSTER_LEVEL_CHARACTERS = 2,
  /// Group by grapheme without forcing monotone order. Useful when you intend
  /// to reorder glyphs yourself and want the grouping without the constraint.
  ZTEXT_CLUSTER_LEVEL_GRAPHEMES = 3,
} ZtextClusterLevel;

/// One OpenType feature setting. `start`/`end` are byte offsets into the run
/// the feature applies to; use 0 and ZTEXT_FEATURE_GLOBAL for the whole run.
typedef struct ZtextFeature {
  /// ZTEXT_TAG('l','i','g','a') and friends.
  uint32_t tag;
  /// 0 disables, 1 enables, higher values select an alternate where the
  /// feature takes one.
  uint32_t value;
  uint32_t start;
  uint32_t end;
} ZtextFeature;

#define ZTEXT_FEATURE_GLOBAL ((uint32_t)0xFFFFFFFFu)

typedef struct ZtextShapeParams {
  ZtextDirection direction;
  /// ISO 15924 script as a tag -- ZTEXT_TAG('A','r','a','b'). 0 asks HarfBuzz
  /// to guess from the text.
  uint32_t script;
  /// BCP 47 language tag, NULL for none. Affects language-specific features
  /// such as Turkish dotless i.
  const char* language;
  const ZtextFeature* features;
  size_t feature_count;
  ZtextClusterLevel cluster_level;
  /// 0 (default) takes metrics from HarfBuzz's own OpenType table reader:
  /// advances scale linearly from design units, so layout does not shift when
  /// hinting changes, and the shaping font is immutable.
  ///
  /// 1 takes them from FreeType instead, computed from the same face that
  /// will rasterise, with hinting on -- so advances match a hinted raster at
  /// the cost of hinting-dependent layout. Pair it with ZTEXT_HINTING_NORMAL
  /// when rendering; mixing hinted advances with an unhinted raster, or the
  /// reverse, is what makes text drift.
  ///
  /// The two sources are close but not identical: ztext's suite measures the
  /// gap over a 15-glyph run and asserts it stays under one pixel.
  int use_freetype_metrics;
} ZtextShapeParams;

/// One positioned glyph. Advances and offsets are in pixels at the face's
/// current size, y-up.
typedef struct ZtextGlyph {
  uint32_t glyph_id;
  /// Byte offset into the UTF-8 passed to ztextShaperShapeUtf8 -- not a
  /// codepoint index. Several glyphs may share a cluster (one character
  /// decomposing) and several characters may share one (a ligature).
  uint32_t cluster;
  float x_advance;
  float y_advance;
  float x_offset;
  float y_offset;
} ZtextGlyph;

ZTEXT_API ZtextResult ztextShaperCreate(ZtextShaper** out);
ZTEXT_API void ztextShaperDestroy(ZtextShaper* shaper);

/// Shapes one run of UTF-8 with one face, one direction and one script.
///
/// This is a run shaper, not a paragraph shaper: it does not itemise. Split
/// text into runs with ztextParagraph* first, then call this once per run.
///
/// The text is validated as UTF-8 before HarfBuzz sees it and rejected with
/// ZTEXT_RESULT_INVALID_UTF8 if it is malformed, rather than silently
/// substituting replacement characters.
///
/// Results replace whatever the shaper held before.
/// `text` is the whole paragraph and `[run_offset, run_offset + run_length)`
/// is the part to shape. Characters outside that range are NOT shaped, but
/// they ARE seen -- which is the difference between correct and nearly correct
/// at a run boundary.
///
/// This matters the moment a host splits a word, which ztextFontCoveredPrefix
/// invites it to do. Shaping the two halves of an Arabic word separately gives
/// the letter before the split a final form and the letter after it an initial
/// form, when both should be medial: five letters of "marhaba" split in the
/// middle come back with two wrong glyphs, silently, in a run that otherwise
/// looks fine. Handing over the surrounding text costs nothing and removes the
/// whole class.
///
/// To shape a standalone string, pass 0 and `length`.
///
/// Cluster values are byte offsets into `text` -- the whole buffer, not the
/// run -- so they index the same slice a ZtextShapingRun's offsets do.
ZTEXT_API ZtextResult ztextShaperShapeUtf8(ZtextShaper* shaper, ZtextFace* face,
                                           const char* text, size_t length,
                                           size_t run_offset, size_t run_length,
                                           const ZtextShapeParams* params);

/// Number of glyphs from the last successful shape.
ZTEXT_API size_t ztextShaperGlyphCount(const ZtextShaper* shaper);

/// Borrowed glyph array in visual order. Valid until the next shape on this
/// shaper, or its destruction.
///
/// NULL when nothing has been shaped -- and also when the last shape succeeded
/// but produced no glyphs, as shaping empty text does. Use
/// ztextShaperGlyphCount to tell those apart; it is the count that is
/// authoritative.
ZTEXT_API const ZtextGlyph* ztextShaperGlyphs(const ZtextShaper* shaper);

/// Direction actually used, which is what AUTO resolved to.
ZTEXT_API ZtextDirection ztextShaperDirection(const ZtextShaper* shaper);

typedef struct ZtextExtents {
  /// Ink bounds relative to the run origin, in pixels, y-up. Empty runs and
  /// runs of blank glyphs report x_min == x_max.
  float x_min;
  float y_min;
  float x_max;
  float y_max;
  /// Sum of the glyph advances -- what to move the pen by, which is not the
  /// same as the ink width.
  float x_advance;
  float y_advance;
} ZtextExtents;

/// Extents of the last successful shape.
///
/// `face` must be the face the run was shaped with, still at the size it was
/// shaped at. It is a parameter rather than something the shaper remembers on
/// your behalf, deliberately: a stored face pointer would dangle the moment
/// the face was destroyed, and the compiler would never mention it. Passing it
/// makes the dependency part of the call, and ztext checks it -- a different
/// face, or the same face resized since, is ZTEXT_RESULT_INVALID_ARGUMENT
/// rather than a plausible mixture of one font's ink and another's advances.
///
/// Metrics come from whichever source the shape used, so extents and advances
/// cannot disagree about which font they describe.
///
/// Costs one glyph-extents query per glyph, so cache the result rather than
/// asking every frame.
ZTEXT_API ZtextResult ztextShaperExtents(const ZtextShaper* shaper,
                                         ZtextFace* face, ZtextExtents* out);

//===----------------------------------------------------------------------===//
// Bidi and script itemisation
//
// The Unicode Bidirectional Algorithm decides the visual order of mixed
// left-to-right and right-to-left text. Shaping needs its output twice over:
// runs must be shaped in the direction the algorithm assigns, and mixed-script
// text must be split before shaping because HarfBuzz shapes one script at a
// time.
//
// ztext stops at runs. Line breaking, justification and where those runs end
// up on screen are a layout engine's job, not this package's.
//===----------------------------------------------------------------------===//

/// One analysed paragraph. Immutable once created, and independent of any
/// face, library or thread.
typedef struct ZtextParagraph ZtextParagraph;

typedef enum ZtextBaseDirection {
  /// Derive the base level from the first strong character, per rule P2/P3.
  ZTEXT_BASE_DIRECTION_AUTO = 0,
  ZTEXT_BASE_DIRECTION_LTR = 1,
  ZTEXT_BASE_DIRECTION_RTL = 2,
} ZtextBaseDirection;

/// A maximal span of one embedding level, in VISUAL order: run 0 is leftmost
/// for an LTR base, rightmost for RTL. Offsets are byte offsets into the
/// paragraph text.
typedef struct ZtextVisualRun {
  uint32_t offset;
  uint32_t length;
  /// Even levels run left-to-right, odd right-to-left.
  uint8_t level;
} ZtextVisualRun;

/// A maximal span of one script, in LOGICAL order. Offsets are byte offsets.
typedef struct ZtextScriptRun {
  uint32_t offset;
  uint32_t length;
  /// ISO 15924 as a tag, ready to hand to ZtextShapeParams::script.
  uint32_t script;
} ZtextScriptRun;

/// A span ready to hand to the shaper: one direction, one script.
///
/// This is the intersection of the two run lists below, in VISUAL order, and
/// it exists because computing it is the one part of itemisation a caller
/// cannot get right by inspection. Visual runs are in visual order and script
/// runs are in logical order; inside a right-to-left visual run the script
/// pieces have to come out backwards. Getting that wrong produces text that
/// looks almost correct.
///
/// Iterate these, shape each with `direction` from `level` and `script` as
/// given, and lay the results out left to right.
typedef struct ZtextShapingRun {
  uint32_t offset;
  uint32_t length;
  /// ISO 15924 as a tag, for ZtextShapeParams::script.
  uint32_t script;
  /// Even runs left-to-right, odd right-to-left.
  uint8_t level;
} ZtextShapingRun;

/// Analyses one paragraph of UTF-8.
///
/// `text` is read during the call only; the paragraph copies what it needs and
/// does not borrow the buffer. That differs from ztextFontCreateFromMemory on
/// purpose -- a paragraph is small and copying it removes a lifetime the
/// caller would otherwise have to track.
///
/// Rejects malformed UTF-8 with ZTEXT_RESULT_INVALID_UTF8. Text containing a
/// paragraph separator is analysed as a single paragraph up to the first one;
/// split beforehand if that is not what you want.
ZTEXT_API ZtextResult ztextParagraphCreateUtf8(const char* text, size_t length,
                                               ZtextBaseDirection base,
                                               ZtextParagraph** out);

ZTEXT_API void ztextParagraphDestroy(ZtextParagraph* paragraph);

/// Byte length actually analysed, which is at most `length` -- less if the
/// text contained a paragraph separator.
ZTEXT_API size_t ztextParagraphLength(const ZtextParagraph* paragraph);

/// Resolved base embedding level: even for LTR, odd for RTL.
ZTEXT_API uint8_t ztextParagraphBaseLevel(const ZtextParagraph* paragraph);

/// Borrowed per-byte embedding levels, one entry per byte of
/// ztextParagraphLength. Continuation bytes of a multi-byte character carry
/// the same level as its first byte.
///
/// These are the levels UAX #9 resolves over the PARAGRAPH, before rule L1
/// resets trailing whitespace for a particular line. Where the two differ, a
/// line's own visual runs are the authority -- see ztextLineCreate.
///
/// Valid until the paragraph is destroyed.
ZTEXT_API const uint8_t* ztextParagraphLevels(
    const ZtextParagraph* paragraph);

//===----------------------------------------------------------------------===//
// Segmentation
//
// Where a line may break, where a grapheme cluster ends, where a word ends.
//
// These are here for the same reason bidi is: they are pure functions of the
// text and the Unicode character database, not decisions about layout. ztext
// already owns UAX #9; owning that and not UAX #14 and #29 would be an
// arbitrary line, and it would leave ztextLineCreate -- which takes a byte
// range -- with no way for a caller to find one.
//
// The division of labour is the same as everywhere else here. ztext says where
// a break is PERMITTED; the host decides where one HAPPENS, because that needs
// a width, and a width is not a property of text. Concretely: walk the allowed
// positions, measure with ztextShaperExtents until the next one would not fit,
// and hand the range you chose to ztextLineCreate.
//
// From libunibreak, which allocates nothing and keeps no global state -- so
// unlike the other three upstreams there is no allocator seam and no
// initialisation to get wrong.
//===----------------------------------------------------------------------===//

/// The three values a break entry can hold.
///
/// Macros rather than an enum, and that is deliberate: these live in arrays of
/// one byte per text byte, and C does not let you fix an enum's width. An
/// int-sized enum beside a uint8_t array would be a type that cannot hold its
/// own values -- the kind of near-miss this package spends its guards on.
///
/// No boundary here. Also what a byte inside a multi-byte character reports,
/// since a break there is never a break.
#define ZTEXT_BREAK_NONE 0u
/// A boundary is permitted here.
#define ZTEXT_BREAK_ALLOWED 1u
/// A boundary is REQUIRED here. Line breaks only -- a paragraph's last byte is
/// always mandatory, and so is a U+2028 LINE SEPARATOR within it.
#define ZTEXT_BREAK_MANDATORY 2u

/// Borrowed, one ZTEXT_BREAK_* value per byte of ztextParagraphLength,
/// describing the boundary AFTER that byte. NULL for an empty paragraph.
///
/// So a line may run from `start` to `i + 1` whenever `line_breaks[i]` is not
/// ZTEXT_BREAK_NONE. Valid until the paragraph is destroyed.
///
/// Language tailoring is not exposed: these are the untailored rules. That
/// matters mainly for strict Japanese and Korean line breaking, and adding it
/// later is a parameter rather than a redesign.
ZTEXT_API const uint8_t* ztextParagraphLineBreaks(
    const ZtextParagraph* paragraph);

/// Grapheme cluster boundaries -- what a caret moves by and what backspace
/// deletes. Never ZTEXT_BREAK_MANDATORY.
///
/// This is emphatically not the same as a character: a base plus its combining
/// marks is one grapheme, and so is a regional-indicator pair or an emoji
/// joined with U+200D. Moving a caret by character puts it inside one.
ZTEXT_API const uint8_t* ztextParagraphGraphemeBreaks(
    const ZtextParagraph* paragraph);

/// Word boundaries -- double-click selection, and word-wise caret movement.
/// Never ZTEXT_BREAK_MANDATORY.
ZTEXT_API const uint8_t* ztextParagraphWordBreaks(
    const ZtextParagraph* paragraph);

/// The next and previous grapheme boundary from `offset`, for moving a caret.
///
/// `ztextParagraphNextGrapheme(p, length)` is `length`, and
/// `ztextParagraphPreviousGrapheme(p, 0)` is 0, so a caret walked off either
/// end stays put rather than wrapping or going out of range. An `offset` that
/// is not itself a boundary is snapped outward to one.
///
/// Written as functions rather than left to the caller because the loop is
/// three lines and everyone writes it slightly differently -- usually by
/// stepping a character at a time, which is the bug these exist to prevent.
ZTEXT_API size_t ztextParagraphNextGrapheme(const ZtextParagraph* paragraph,
                                            size_t offset);
ZTEXT_API size_t ztextParagraphPreviousGrapheme(const ZtextParagraph* paragraph,
                                                size_t offset);

/// The paragraph laid out as ONE line.
///
/// Correct whenever the text fits on one, which is most labels, most buttons
/// and every single-line field -- and wrong the moment it wraps. Rules L1 and
/// L2 of UAX #9 are defined over a line, not a paragraph, so where the text
/// breaks changes the answer. Use ztextLineCreate for anything that wraps; see
/// the note there for what actually differs.
ZTEXT_API size_t ztextParagraphVisualRunCount(const ZtextParagraph* paragraph);
ZTEXT_API const ZtextVisualRun* ztextParagraphVisualRuns(
    const ZtextParagraph* paragraph);

/// Script runs are a property of the text, so unlike the runs above they do
/// not change when it wraps. A line reuses these.
ZTEXT_API size_t ztextParagraphScriptRunCount(const ZtextParagraph* paragraph);
ZTEXT_API const ZtextScriptRun* ztextParagraphScriptRuns(
    const ZtextParagraph* paragraph);

/// Visual runs intersected with script runs: what to actually shape, for the
/// paragraph laid out as one line. Same caveat as ztextParagraphVisualRuns.
ZTEXT_API size_t ztextParagraphShapingRunCount(const ZtextParagraph* paragraph);
ZTEXT_API const ZtextShapingRun* ztextParagraphShapingRuns(
    const ZtextParagraph* paragraph);

//===----------------------------------------------------------------------===//
// Lines
//
// One byte range of a paragraph, reordered as its own line.
//
// This exists because bidi reordering is not a property of a paragraph alone.
// UAX #9 resolves embedding levels over the paragraph (rules P through I), but
// then rules L1 and L2 -- the whitespace reset and the run reversal -- are
// applied PER LINE. A run list computed for the whole paragraph is therefore
// the right answer only when the whole paragraph is one line.
//
// The visible difference is trailing whitespace. In "abc <hebrew>   <hebrew>"
// the three spaces sit between two right-to-left words and resolve to level 1,
// so the paragraph puts them in the middle. Break the line just after them and
// L1 resets them to the paragraph level, which moves them to the LEFT-hand end
// of that line. Get this wrong and wrapped right-to-left text has its indent
// on the wrong side -- subtly, only when it wraps, and only in the second
// language a product ships.
//
// ztext does not decide where the breaks go: see the README on UAX #14. It
// takes the ranges a host has already chosen and reorders each correctly.
//===----------------------------------------------------------------------===//

typedef struct ZtextLine ZtextLine;

/// Reorders `paragraph`'s bytes `[offset, offset + length)` as one line.
///
/// Offsets are byte offsets into the paragraph, and the runs come back with
/// paragraph-relative offsets too -- not line-relative -- so they index the
/// same buffer the caller already has.
///
/// A zero-length line is legal and has no runs. A range that ends past
/// ztextParagraphLength, or that starts or ends in the middle of a UTF-8
/// character, is ZTEXT_RESULT_INVALID_ARGUMENT rather than a silent
/// half-character.
///
/// The line copies what it needs, so it holds no reference to the paragraph
/// and may outlive it.
ZTEXT_API ZtextResult ztextLineCreate(const ZtextParagraph* paragraph,
                                      size_t offset, size_t length,
                                      ZtextLine** out);

ZTEXT_API void ztextLineDestroy(ZtextLine* line);

ZTEXT_API size_t ztextLineOffset(const ZtextLine* line);
ZTEXT_API size_t ztextLineLength(const ZtextLine* line);

/// Runs of one embedding level, in visual order, with L1 and L2 applied over
/// this line's range.
ZTEXT_API size_t ztextLineVisualRunCount(const ZtextLine* line);
ZTEXT_API const ZtextVisualRun* ztextLineVisualRuns(const ZtextLine* line);

/// This line's visual runs intersected with the paragraph's script runs: what
/// to actually shape.
ZTEXT_API size_t ztextLineShapingRunCount(const ZtextLine* line);
ZTEXT_API const ZtextShapingRun* ztextLineShapingRuns(const ZtextLine* line);

//===----------------------------------------------------------------------===//
// Rasterisation
//===----------------------------------------------------------------------===//

typedef enum ZtextRenderMode {
  /// 8-bit coverage, one byte per pixel, 0 = uncovered.
  ZTEXT_RENDER_MODE_A8 = 0,
  /// FreeType's native signed distance field, 8-bit, 128 at the outline and
  /// rising inside. See README for what it costs and what it is good for --
  /// measured, not assumed.
  ZTEXT_RENDER_MODE_SDF = 1,
} ZtextRenderMode;

typedef enum ZtextHinting {
  /// The face's own hinting where it has any, FreeType's autohinter where it
  /// does not.
  ZTEXT_HINTING_NORMAL = 0,
  /// Vertical hinting only -- crisp baselines, horizontal metrics left alone.
  /// The usual choice when advances come from unhinted shaping.
  ZTEXT_HINTING_LIGHT = 1,
  /// No hinting. Required for SDF, which wants unhinted outlines.
  ZTEXT_HINTING_NONE = 2,
} ZtextHinting;

/// Fakes a bold or an italic on a face that has neither of its own, the way a
/// production stack does. Both apply at glyph LOADING, so
/// ztextFaceGlyphExtents and ztextFaceRenderGlyph always agree on the same
/// widened, sheared glyph -- but not shaping: HarfBuzz's own advance queries
/// bypass this face's glyph loading, so a shaped run's advances do not widen.
///
/// Emboldening widens the glyph, and its advance is widened by the same
/// amount so bold text does not overlap; a shear leaves the advance alone,
/// because slanting does not change how far the pen moves.
///
/// `enabled` is 0 or 1. Applies to every glyph loaded through this face from
/// the next call on; already-rendered bitmaps and already-taken extents are
/// unaffected.
ZTEXT_API ZtextResult ztextFaceSetSyntheticBold(ZtextFace* face, int enabled);
ZTEXT_API ZtextResult ztextFaceSetSyntheticOblique(ZtextFace* face,
                                                   int enabled);

/// Callbacks for ztextFaceDecomposeOutline, one per outline command. Points
/// are in 26.6 fixed point, at this face's current size. Modelled on
/// ZtextAllocator: `user` is passed back unmodified, and a callback other
/// than ZTEXT_RESULT_OK aborts decomposition and becomes the result
/// ztextFaceDecomposeOutline returns.
typedef struct ZtextOutlineFuncs {
  ZtextResult (*move_to)(void* user, int32_t x, int32_t y);
  ZtextResult (*line_to)(void* user, int32_t x, int32_t y);
  ZtextResult (*conic_to)(void* user, int32_t control_x, int32_t control_y,
                          int32_t x, int32_t y);
  ZtextResult (*cubic_to)(void* user, int32_t control1_x, int32_t control1_y,
                          int32_t control2_x, int32_t control2_y, int32_t x,
                          int32_t y);
  /// Emitted once a contour is complete, before the next move_to and after
  /// the last -- FT_Outline_Decompose itself has no such event, only an
  /// implicit line/conic/cubic back to the contour's start.
  ZtextResult (*close)(void* user);
  void* user;
} ZtextOutlineFuncs;

typedef struct ZtextGlyphBitmap {
  /// Owned by the FACE, and valid until the next ztextFaceRenderGlyph on it.
  ///
  /// Nothing else invalidates it: not shaping, not measuring, not a call on
  /// another face of the same font. That is the point of copying rather than
  /// handing back FreeType's glyph slot. A font's faces share one FT_Face and
  /// therefore one slot, so a borrowed pointer would be freed by a sibling
  /// face loading any glyph at all -- a rule that spans handles and would be
  /// violated by code that looks obviously correct.
  ///
  /// The copy is one memcpy of a few hundred bytes against a rasterisation
  /// measured in microseconds, and the buffer is reused, so a steady-state
  /// renderer still allocates nothing.
  ///
  /// NULL for a glyph with no ink, such as a space.
  const uint8_t* pixels;
  uint32_t width;
  uint32_t height;
  /// Bytes per row. Always positive and always tightly packed, because the
  /// copy above normalises FreeType's bottom-up bitmaps on the way out -- so
  /// a consumer cannot render upside down by ignoring a sign.
  int32_t pitch;
  /// Pen-relative position of the bitmap's top-left corner, in pixels, y-up.
  int32_t left;
  int32_t top;
  /// The glyph's own advance at this size, in pixels. For laying out a single
  /// glyph; a shaped run's advances come from shaping and may differ.
  float x_advance;
} ZtextGlyphBitmap;

/// Loads and rasterises one glyph by index.
///
/// SDF mode forces unhinted loading regardless of `hinting`, because a hinted
/// outline produces a distance field that does not match the shape at other
/// sizes -- which is the only reason to want one.
///
/// `offset_x`/`offset_y` place the glyph at a fractional pixel offset, in
/// 26.6 fixed point -- the unit FreeType uses and shaping advances already
/// come back in -- so a host laying out text at fractional x is not forced to
/// snap every glyph to the pixel grid. 0, 0 renders identically to a build
/// without this parameter. Ignored in SDF mode: the field is meant to be
/// sampled at any sub-pixel position later, so baking one in here would be
/// wasted, unrecoverable work -- apply the offset where the field is sampled
/// instead, the same way scale and rotation already are.
ZTEXT_API ZtextResult ztextFaceRenderGlyph(ZtextFace* face, uint32_t glyph_id,
                                           ZtextRenderMode mode,
                                           ZtextHinting hinting,
                                           int32_t offset_x, int32_t offset_y,
                                           ZtextGlyphBitmap* out);

/// Metrics for one glyph without rasterising it.
///
/// It loads the glyph, which invalidates any ZtextGlyphBitmap previously
/// returned for this face -- so size an atlas entry with this BEFORE
/// rasterising, not while holding pixels you still intend to read.
ZTEXT_API ZtextResult ztextFaceGlyphExtents(ZtextFace* face, uint32_t glyph_id,
                                            ZtextHinting hinting,
                                            ZtextExtents* out);

/// Walks one glyph's outline through `funcs`, for a host that fills its own
/// shapes -- an offline SDF baker, a path-effect renderer -- rather than
/// sampling a bitmap. A wrapper over FT_Outline_Decompose; see
/// ZtextOutlineFuncs for the callback shape.
///
/// Subject to this face's synthetic bold and oblique settings, the same as
/// ztextFaceRenderGlyph and ztextFaceGlyphExtents.
ZTEXT_API ZtextResult ztextFaceDecomposeOutline(ZtextFace* face,
                                                uint32_t glyph_id,
                                                ZtextHinting hinting,
                                                const ZtextOutlineFuncs* funcs);

/// Half-width of the distance field ramp, in pixels, for ZTEXT_RENDER_MODE_SDF.
///
/// Accepts 2..32, which is the range FreeType supports; anything else is
/// ZTEXT_RESULT_INVALID_ARGUMENT rather than a silent clamp. The default is 8.
/// Applies to every face made from this library.
ZTEXT_API ZtextResult ztextLibrarySetSdfSpread(ZtextLibrary* library,
                                               uint32_t spread);

/// Number of faces inside a font image, which is 1 for a plain TTF or OTF and
/// more for a TrueType collection (.ttc).
///
/// Call it before ztextFontCreateFromMemory if you intend to iterate a
/// collection; the index that function takes must be below this.
ZTEXT_API ZtextResult ztextLibraryCountFaces(ZtextLibrary* library,
                                             const void* data, size_t size,
                                             uint32_t* out);

//===----------------------------------------------------------------------===//
// ABI layout guard
//
// Any consumer that hand-declares the POD types above -- the Zig wrapper does,
// and so would a C# or Rust binding -- has two declarations that nothing in
// either compiler checks still agree. A field reordered here and not there is
// silent corruption, not a build error. The two functions below let a consumer
// assert against what this library actually compiled to.
//
// ztext's own Zig wrapper additionally compares its externs against THIS
// HEADER at comptime (src/abi_check.zig), by reflection over every public
// declaration, which is a stronger check on the axis it covers: it sees
// function arity and per-parameter sizes, and every enumerator by name rather
// than only the last. What it cannot see is a header preprocessed differently
// from the library it is linked against -- and that is exactly what
// ztextAbiProbe covers, which is why both exist.
//
// A consumer without a comptime view of this header has only what is below,
// which is the reason it is a public API rather than a test fixture. It is not
// grown field-by-field as the API grows; treat it as a self-check on the types
// most likely to be mirrored, not as an exhaustive manifest.
//
// The upstream types are guarded differently and more strongly: static
// assertions in ztext_abi.c fail the BUILD if a re-vendor changes the shape of
// anything ztext casts to or from. That is the whole reason the upstream
// structs stop at this boundary.
//===----------------------------------------------------------------------===//

typedef struct ZtextAbiLayout {
  /// sizeof(ZtextAbiLayout). Read this first: if it disagrees with the
  /// consumer's own sizeof, the struct itself has changed and nothing below
  /// can be trusted.
  uint32_t layout_size;

  uint32_t allocator_size;
  uint32_t allocator_align;
  uint32_t allocator_offset_allocate;
  uint32_t allocator_offset_reallocate;
  uint32_t allocator_offset_deallocate;
  uint32_t allocator_offset_user;

  uint32_t glyph_size;
  uint32_t glyph_align;
  uint32_t glyph_offset_glyph_id;
  uint32_t glyph_offset_cluster;
  uint32_t glyph_offset_x_advance;
  uint32_t glyph_offset_y_advance;
  uint32_t glyph_offset_x_offset;
  uint32_t glyph_offset_y_offset;

  uint32_t feature_size;
  uint32_t feature_align;
  uint32_t shape_params_size;
  uint32_t shape_params_align;
  uint32_t shape_params_offset_language;
  uint32_t shape_params_offset_features;
  uint32_t shape_params_offset_feature_count;

  uint32_t face_metrics_size;
  uint32_t face_metrics_align;
  uint32_t extents_size;
  uint32_t extents_align;

  uint32_t visual_run_size;
  uint32_t visual_run_align;
  uint32_t script_run_size;
  uint32_t script_run_align;

  uint32_t glyph_bitmap_size;
  uint32_t glyph_bitmap_align;
  uint32_t glyph_bitmap_offset_pixels;
  uint32_t glyph_bitmap_offset_pitch;
  uint32_t glyph_bitmap_offset_x_advance;

  /// Number of enumerators in ZtextResult, so a consumer can assert its own
  /// error mapping is exhaustive.
  uint32_t result_count;

  /// Size of each enum type as the C compiler chose it, and the value of each
  /// one's last enumerator.
  ///
  /// An enum's tag size is not fixed by the C standard -- a compiler may pick
  /// anything that fits, and a consumer declaring `enum(c_int)` against a C
  /// enum the compiler made a `char` writes past the end of a parameter slot.
  /// It corrupts the stack silently, at the call boundary, which is the worst
  /// place for it. The last enumerator is reported alongside because a value
  /// renumbered in this header without the consumer noticing turns every
  /// switch on it into a wrong branch.
  uint32_t result_size;
  uint32_t result_last;
  uint32_t direction_size;
  uint32_t direction_last;
  uint32_t cluster_level_size;
  uint32_t cluster_level_last;
  uint32_t base_direction_size;
  uint32_t base_direction_last;
  uint32_t render_mode_size;
  uint32_t render_mode_last;
  uint32_t hinting_size;
  uint32_t hinting_last;
} ZtextAbiLayout;

/// Fills `out` with the layout the library was compiled with. Never fails.
ZTEXT_API void ztextAbiLayout(ZtextAbiLayout* out);

/// Every plain-data type ztext hands across the boundary, in one struct.
///
/// `ztextAbiProbe` fills each field with a distinct value derived from its
/// position, and the Zig side asserts each field reads back the value meant
/// for it. Sizes and alignments alone cannot catch two same-typed fields
/// swapping places -- `ascender` and `descender` are both floats, and
/// transposing them changes no size, no alignment and no offset the other
/// check looks at, while turning every line of text upside down.
///
/// This catches that, and catches a field changing type, with one function
/// instead of an offset table that would have to grow a field at a time.
typedef struct ZtextAbiProbe {
  ZtextAllocator allocator;
  ZtextFeature feature;
  ZtextShapeParams shape_params;
  ZtextGlyph glyph;
  ZtextFaceMetrics face_metrics;
  ZtextExtents extents;
  ZtextVisualRun visual_run;
  ZtextScriptRun script_run;
  ZtextShapingRun shaping_run;
  ZtextGlyphBitmap glyph_bitmap;
} ZtextAbiProbe;

/// Fills every field of every plain-data type with a distinct marker. Never
/// fails.
ZTEXT_API void ztextAbiProbe(ZtextAbiProbe* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ZTEXT_H_
