//===----------------------------------------------------------------------===//
// ztypeset -- implementation-private declarations shared by the ffi/*.c units.
//
// Not installed and not part of the ABI. Nothing here may appear in ztypeset.h.
//===----------------------------------------------------------------------===//

#ifndef ZTYPESET_INTERNAL_H_
#define ZTYPESET_INTERNAL_H_

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SIZES_H
#include FT_OUTLINE_H
#include FT_STROKER_H
#include FT_TRUETYPE_TABLES_H

#include <hb.h>
#include <hb-ot.h>
#include <hb-ft.h>

#include <SheenBidi/SheenBidi.h>

#include <graphemebreak.h>
#include <linebreak.h>
#include <wordbreak.h>

#include "ztypeset.h"

//===----------------------------------------------------------------------===//
// Allocation
//
// Every allocation ztypeset makes -- its own, FreeType's, HarfBuzz's and
// SheenBidi's -- lands here. All three free without a size, so a header ahead
// of each block records what was asked for; that is what lets
// ZtypesetAllocator::deallocate be handed a size and alignment, which in turn
// is what lets a Zig host use its std.mem.Allocator directly.
//===----------------------------------------------------------------------===//

/// Alignment ztypeset uses when a caller asks for "whatever malloc would give".
/// FreeType, HarfBuzz and SheenBidi all allocate with malloc semantics and
/// never request more.
#define ZTYPESET_DEFAULT_ALIGN (_Alignof(max_align_t))

/// Index into ztypeset's allocator registry, which holds one entry per distinct
/// allocator ever installed. Every block records the index of the allocator
/// that issued it, so "freed through the allocator that made it" is a routing
/// decision ztypeset makes rather than a rule a caller has to keep. See the
/// registry section at the top of ztypeset_core.c.
typedef uint32_t ZtypesetAllocatorId;

/// Entry 0: malloc/free, present before anything runs.
#define ZTYPESET_ALLOCATOR_DEFAULT ((ZtypesetAllocatorId)0u)

/// "Whichever allocator the block itself records" -- the only value a free or
/// a grow of process-wide memory ever passes, because nothing else knows.
#define ZTYPESET_ALLOCATOR_ANY ((ZtypesetAllocatorId)0xFFFFFFFFu)

/// Returned by the registry when it could not take another allocator.
#define ZTYPESET_ALLOCATOR_NONE ((ZtypesetAllocatorId)0xFFFFFFFEu)

/// The allocator ztypesetSetAllocator most recently installed.
ZtypesetAllocatorId ztypesetInstalledAllocator(void);

/// Process-wide memory: new blocks come from the installed allocator, and a
/// grow or a free is routed to whichever allocator the block records.
void* ztypesetAlloc(size_t size, size_t alignment);
void* ztypesetRealloc(void* block, size_t new_size, size_t alignment);
void ztypesetFree(void* block);

/// Memory that belongs to one object rather than to the process, named by the
/// allocator that owns it. The caller's `id` is CHECKED against the one the
/// block records, and a disagreement stops the process -- it can only mean
/// ztypeset allocated a block in one place and released it in another.
void* ztypesetAllocFrom(ZtypesetAllocatorId id, size_t size, size_t alignment);
void* ztypesetReallocFrom(ZtypesetAllocatorId id, void* block, size_t new_size,
                       size_t alignment);
void ztypesetFreeFrom(ZtypesetAllocatorId id, void* block);

/// Zero-initialising allocate, with the multiplication checked for overflow.
void* ztypesetCalloc(size_t count, size_t size);

/// Convenience for the common case: one zeroed object of a type.
#define ZTYPESET_NEW(T) ((T*)ztypesetCalloc(1, sizeof(T)))

/// Wires `library`'s FreeType memory record to ztypeset, recording the
/// allocator installed right now in `library->allocator`.
///
/// The record must outlive the FT_Library built from it, so it lives
/// inside the library rather than on a caller's stack.
void ztypesetInitFtMemory(ZtypesetLibrary* library);

/// Points SheenBidi's global default allocator at ztypeset's. Idempotent.
ZtypesetResult ztypesetInstallSheenbidiAllocator(void);

//===----------------------------------------------------------------------===//
// Error detail
//===----------------------------------------------------------------------===//

/// A process-unique, never-reused value, for detecting that a handle a caller
/// passed is not the one an earlier call was made against.
uint64_t ztypesetNextGeneration(void);

/// Pixels to FreeType's 26.6, rounded to nearest. Returns 0 for anything out
/// of range, including NaN -- so the comparisons are written positively
/// rather than as negated bounds, and 0 is never a value the caller meant.
///
/// One home for the conversion because there are two callers with the same
/// requirement: a face's pixel size and a stroke's radius are both a positive
/// length in pixels that FreeType takes in 26.6, and two copies of it could
/// round or clamp differently.
int32_t ztypesetToFixed266(float pixels);

/// 26.6 fixed point to pixels: the inverse of ztypesetToFixed266, and the one
/// place that conversion is written.
///
/// It was open-coded as `(float)x / 64.0f` at twenty-six sites across three
/// translation units, four of them per glyph in the shaping loop. That is one
/// home per site for a conversion the library performs everywhere, and the
/// count is what makes it worth a function rather than a habit.
///
/// Multiplication by the reciprocal rather than division, and NOT an accuracy
/// trade: 64 is a power of two, so 1/64 is exactly representable and the
/// product is the same value the quotient would round to, for every finite
/// input. What it buys is that an unoptimised build -- Debug, where ztypeset's
/// own sanitiser runs -- does four multiplies per glyph instead of four
/// divides.
///
/// int64_t rather than int32_t because FreeType's FT_Pos is a long: the
/// callers hand over metrics straight out of an FT_Size, and narrowing them
/// here would put a truncation in the one place that is supposed to remove
/// the arithmetic from the call sites.
static inline float ztypesetFrom266(int64_t fixed) {
  return (float)fixed * (1.0f / 64.0f);
}

//===----------------------------------------------------------------------===//
// Face activation
//===----------------------------------------------------------------------===//

/// Makes `face`'s FT_Size the current one on the font's shared FT_Face.
///
/// Every path that reaches FreeType through a face must call this first --
/// loading, rendering, reading scaled metrics, and handing the FreeType-backed
/// hb_font_t to HarfBuzz, whose glyph callbacks load through the same shared
/// face. FT_Activate_Size is a pointer assignment (ftobjs.c), so calling it
/// unconditionally is cheaper than tracking which size is current.
void ztypesetFaceActivate(const ZtypesetFace* face);

/// Pushes the font's current variation coordinates onto `face`'s HarfBuzz
/// fonts, both the OpenType one and the FreeType-backed one if it has been
/// built. A no-op for a font with no axes.
///
/// Called from three places, so it is shared rather than inlined:
/// when the axes move, when a face is created after they moved, and when the
/// FreeType-backed font is built lazily on the first shape that asks for it.
/// Miss any one of them and that face shapes the default instance while
/// FreeType rasterises the chosen one.
void ztypesetFaceApplyVariations(ZtypesetFace* face);

/// Tells both of this face's HarfBuzz fonts what its synthetic bold and
/// oblique are, so a shaped run's advances widen with the ink instead of
/// staying at the unstyled font's widths.
///
/// Called by the setters and by whatever builds the FreeType-backed font,
/// which comes into existence long after a style may have been set.
void ztypesetFaceApplySynthetic(ZtypesetFace* face);

/// Records an upstream's own description of a failure, for
/// ztypesetLastErrorDetail. Never affects control flow.
void ztypesetSetErrorDetail(const char* detail);

/// Maps an FT_Error onto a ZtypesetResult, recording FreeType's error string as
/// the detail on the way through.
ZtypesetResult ztypesetFromFtError(FT_Error error);

//===----------------------------------------------------------------------===//
// Text
//
// The one place in ztypeset that knows how the three encodings differ. Every
// entry point that takes text goes through these four, so an encoding cannot
// be handled in three places and forgotten in a fourth.
//
// HarfBuzz substitutes U+FFFD for malformed input and SheenBidi has its own
// recovery; both are reasonable for a text editor and wrong for an engine
// reading a localisation table, where malformed input means the table is
// corrupt and should say so. ztypeset validates first and refuses.
//===----------------------------------------------------------------------===//

/// Bytes in one code unit of `encoding`: 1, 2 or 4.
///
/// 0 for a value this build does not name, which is how every entry point
/// validates the enum -- a consumer compiled against a newer header cannot
/// smuggle an encoding past a switch that has no arm for it.
size_t ztypesetEncodingUnitSize(ZtypesetEncoding encoding);

/// True if `length` code units at `text` are well-formed in `encoding`: no
/// overlong UTF-8, no unpaired surrogate in either UTF-8 or UTF-16, nothing
/// above U+10FFFF, no truncated sequence at the end.
///
/// True for a zero length whatever `text` is, and false for a NULL `text`
/// with a non-zero one.
bool ztypesetTextIsWellFormed(const void* text, size_t length,
                           ZtypesetEncoding encoding);

/// Decodes the character starting at code unit `index` of already-VALIDATED
/// text, and returns how many units it spans. Never returns 0, so a loop over
/// it always terminates.
///
/// `index >= length` is answered, not undefined: U+FFFD and a step of 1, the
/// same for all three encodings. One comparison here rather than a
/// precondition every caller has to repeat and every future caller has to
/// remember. tests/c_internal.c holds it.
size_t ztypesetTextDecode(const void* text, size_t length,
                          ZtypesetEncoding encoding,
                       size_t index, uint32_t* out);

/// True if code unit `index` falls INSIDE a character rather than on a
/// boundary -- the one way a caller-given range can slice a character in
/// half. False for `index >= length`, which is what lets a caller check both
/// ends of a half-open range with the same call.
///
/// Shared by ztypeset_shape.c (a shaped run) and ztypeset_bidi.c (a line),
/// which both refuse a range that would split a character rather than shape or
/// reorder half of one.
bool ztypesetTextSplitsCharacter(const void* text, size_t length,
                              ZtypesetEncoding encoding, size_t index);

//===----------------------------------------------------------------------===//
// Growable array
//
// Used by the shaper for its converted glyph array and by the paragraph for
// its run lists. Deliberately tiny: capacity in elements, doubling growth,
// and no shrink -- a shaper that reaches steady state should stop allocating.
//===----------------------------------------------------------------------===//

typedef struct ZtypesetArray {
  void* data;
  size_t count;
  size_t capacity;
} ZtypesetArray;

/// The allocator that issued `block`, read back from the block's own header.
///
/// This is what lets an array name its owner without the owner being written
/// down twice. An array lives inside a handle; the handle is a block; the
/// block already records which allocator issued it. A second copy on the
/// handle could disagree with the first, and nothing would notice.
ZtypesetAllocatorId ztypesetAllocatorOf(void* block);

/// Ensures room for `count` elements of `element_size`, growing if needed.
/// Leaves the array untouched and returns false on allocation failure.
///
/// `owner` is the allocator this array's memory belongs to: the one that issued
/// the handle the array lives in, via ztypesetAllocatorOf. It decides the FIRST
/// allocation -- a block that already exists keeps the allocator that issued it
/// through every grow, and passing a different one here is a detected fatal
/// mismatch rather than a silent migration.
bool ztypesetArrayReserve(ZtypesetAllocatorId owner, ZtypesetArray* array,
                          size_t count,
                       size_t element_size);

void ztypesetArrayFree(ZtypesetAllocatorId owner, ZtypesetArray* array,
                    size_t element_size);

//===----------------------------------------------------------------------===//
// Handles
//
// Defined here as real structs so every accessor is statically typed and no
// cast from void* crosses an entry point.
//===----------------------------------------------------------------------===//

struct ZtypesetLibrary {
  /// FreeType holds a pointer to this, so it lives inside the library rather
  /// than on the stack of whoever created it. Its `user` points back at the
  /// library, which is how the allocation shims find `allocator` below.
  struct FT_MemoryRec_ memory;

  /// Registry index of the allocator installed when this library was created.
  ///
  /// This is what makes FreeType's memory genuinely per-library rather than
  /// process-wide: FT_New_Library takes an FT_Memory, and every allocation
  /// FreeType makes for this library -- faces, glyph slots, hinting state --
  /// is routed back through this entry, even if the process-wide allocator is
  /// replaced afterwards. HarfBuzz cannot do this; its seam is compile-time,
  /// which is why the registry routes HarfBuzz's frees by what the block
  /// records instead.
  ZtypesetAllocatorId allocator;

  FT_Library ft;

  /// Fonts still alive, and whether the caller has asked for this library to
  /// go -- the same pair ZtypesetFont keeps for its faces, one level up.
  ///
  /// It is not symmetry for its own sake. FT_Done_Library destroys every
  /// FT_Face still registered with it, so a library freed first would leave
  /// each ZtypesetFont holding a freed FT_Face, a freed FT_Library and a freed
  /// ZtypesetLibrary. Nothing in this package could have caught that: the
  /// memory it reads back is a library's worth of plausible bytes.
  size_t live_fonts;
  bool destroy_requested;
};

struct ZtypesetFont {
  ZtypesetLibrary* library;

  /// One FT_Face, shared by every face of this font. Each ZtypesetFace owns an
  /// FT_Size over it and activates that size before touching FreeType.
  FT_Face ft;

  /// HarfBuzz's own table reader over the same bytes, immutable and shared.
  /// This is where HarfBuzz's lazily-loaded OpenType tables live -- measured
  /// at ~13 KB for Noto Sans -- so sharing it is most of what this split
  /// saves. An hb_font_t over it is 296 bytes.
  hb_face_t* hb_face;

  /// Faces still alive, and whether the caller has asked for this font to go.
  ///
  /// Together these make destruction order irrelevant: whichever of the font
  /// and its last face is released second frees the font. A caller that
  /// destroys in the "wrong" order gets correct behaviour rather than a
  /// dangling FT_Face, and neither a leak nor a double free is reachable.
  /// This is the same bargain hb_face_t makes, and the same one ZtypesetLibrary
  /// makes with its fonts above -- so no handle in this ABI has an ordering
  /// rule, and none of them needs prose to say so.
  size_t live_faces;
  bool destroy_requested;

  /// The `fvar` axis table, fetched once at creation, or NULL for a static
  /// font. Kept rather than queried per call because FT_Get_MM_Var allocates
  /// and copies every time, and asking "how many axes?" should not.
  FT_MM_Var* mm;

  /// Current design coordinate per axis, `mm->num_axis` long, in FreeType's
  /// 16.16 and in HarfBuzz's float -- the same numbers twice, because the two
  /// upstreams take them in different types and nothing should have to
  /// convert on a path that runs per face.
  ///
  /// One allocation holds both, `coords` first; both are NULL when `mm` is.
  ///
  /// FreeType has these too, but ztypeset keeps its own copy because it is the
  /// only writer: every value that reaches FreeType was range-checked here
  /// first, so the two cannot diverge the way they would if FreeType were
  /// left to clamp. It also means reading one axis back costs no allocation.
  FT_Fixed* coords;
  float* design;

  /// Every live face of this font, intrusively linked through
  /// ZtypesetFace::next.
  ///
  /// A font-level setting has to reach face-level objects: variation
  /// coordinates live on the shared FT_Face, but HarfBuzz keeps its own copy
  /// on each hb_font_t, and each face's scaled metrics have to be recomputed
  /// once MVAR has moved the ascender. Without this list the font would have
  /// no way to find them, and the two halves of the pipeline would describe
  /// different instances.
  ZtypesetFace* faces;
};

struct ZtypesetFace {
  ZtypesetFont* font;

  /// Next face of the same font; see ZtypesetFont::faces. Order within the list
  /// is not meaningful, so creation pushes at the head.
  ZtypesetFace* next;

  /// This face's own scaled state. FreeType keeps the glyph SLOT on the
  /// FT_Face and the metrics on the FT_Size, so a face is exactly "the font
  /// at one size" and nothing else.
  FT_Size ft_size;

  /// Bumped when the face is created and on every size change.
  ///
  /// A shaper records the generation it shaped against so ztypesetShaperExtents
  /// can refuse a face that is not the one the run was shaped with, or the
  /// same face at a different size. An integer rather than a pointer, because
  /// comparing a pointer to a destroyed face is exactly the bug this is here
  /// to prevent -- and a recycled address would compare equal.
  uint64_t generation;

  /// Over the font's shared hb_face. The default source of shaping metrics.
  hb_font_t* hb_font;

  /// Built over the font's FT_Face on first use of
  /// ZtypesetShapeParams::use_freetype_metrics, so a host that never asks pays
  /// nothing.
  ///
  /// This is hb_ft_font_create, NOT hb_ft_font_set_funcs: the latter builds an
  /// FT_Face of its own from its own FT_Library, which would defeat the whole
  /// point of asking FreeType for metrics -- they would come from a different
  /// face than the one being rasterised.
  ///
  /// Its glyph callbacks load through the shared FT_Face, so this face's size
  /// must be active before HarfBuzz is handed it.
  hb_font_t* hb_ft_font;

  /// 26.6 fixed point -- exactly the value handed to FreeType, so nothing is
  /// re-derived through a float round trip. Never 0: a face is created with a
  /// size.
  int32_t pixel_width;
  int32_t pixel_height;

  /// Applied to every glyph this face loads; see ztypesetFaceSetSyntheticBold
  /// and ztypesetFaceSetSyntheticOblique.
  /// Synthetic bold as a fraction of the em, and synthetic oblique as a
  /// shear factor; zero for off. Floats rather than flags because neither
  /// upstream has one strength -- FreeType's ftsynth.c and HarfBuzz's
  /// hb_font_set_synthetic_bold both take an amount, and the reference
  /// amounts are only ZTYPESET_SYNTHETIC_*_DEFAULT.
  float synthetic_bold;
  float synthetic_oblique;

  /// The pen traced round every glyph this face draws; radius 0 for none.
  /// See ztypesetFaceSetStroke.
  ZtypesetStroke stroke;

  /// FreeType's stroker, built on the first glyph that needs one and kept
  /// for the face's life. It carries no per-glyph state between calls --
  /// FT_Stroker_Set rewinds it -- so one per face is exactly the reuse
  /// FreeType intends, and a per-glyph FT_Stroker_New would allocate its
  /// internal borders again for every character on the screen.
  FT_Stroker stroker;

  /// The stroked outline of the glyph loaded last, owned by this face.
  ///
  /// FreeType's stroker exports into an FT_Outline the caller sizes, and an
  /// FT_Outline carries no capacity of its own -- so the capacity is kept
  /// here and the outline is only reallocated when a glyph needs more than
  /// the largest one so far. Valid when `stroked_points` is non-zero.
  FT_Outline stroked;
  FT_UInt stroked_points;
  FT_UInt stroked_contours;

  /// The caller's 2x2, applied after the two above. The identity for a face
  /// that has none -- stored rather than flagged, because "has one" is
  /// exactly "is not the identity" and a flag could disagree with the matrix
  /// beside it.
  ZtypesetMatrix transform;

  /// The last rasterised glyph, COPIED out of the font's shared glyph slot.
  ///
  /// Borrowing the slot would tie every face of a font together: one slot per
  /// FT_Face means loading a glyph through a sibling face frees the pixels
  /// this one just handed out. Copying costs a memcpy of a few hundred bytes
  /// against a rasterisation that measures in microseconds, and buys a
  /// lifetime rule that mentions only the face the caller used.
  ZtypesetArray bitmap;
};

struct ZtypesetShaper {
  hb_buffer_t* buffer;
  /// ZtypesetGlyph, converted out of HarfBuzz's parallel info/position arrays.
  ZtypesetArray glyphs;
  /// hb_feature_t, converted from the caller's ZtypesetFeature array. Kept on
  /// the shaper so a per-frame shape with features allocates nothing either.
  ZtypesetArray features;
  ZtypesetDirection direction;
  /// Which metrics source the last shape used, so extents can be taken from
  /// the same one. Deliberately NOT a pointer to the face: a borrowed handle
  /// stored across calls is a dangling pointer waiting for someone to destroy
  /// the face, so ztypesetShaperExtents takes the face as a parameter instead.
  bool used_freetype_metrics;
  /// The face generation this run was shaped against; see ZtypesetFace.
  uint64_t face_generation;
  bool shaped;
};

struct ZtypesetParagraph {
  /// The caller's code units, copied. `length` counts UNITS, not bytes; the
  /// allocation is `length * ztypesetEncodingUnitSize(encoding)` bytes.
  ///
  /// SheenBidi's SBAlgorithm stores the SBCodepointSequence it was given BY
  /// VALUE, and that struct holds a bare pointer to the caller's buffer; the
  /// SBParagraph retained below copies it again. Nothing on the paths ztypeset
  /// uses dereferences it after creation -- line reordering works from the
  /// resolved types and levels alone -- but that is a property of the pinned
  /// version, not a contract, and the documented promise is that a paragraph
  /// borrows nothing. One allocation buys that outright, and it is also what
  /// lets ztypesetLineCreate reject a range that would split a character.
  ///
  /// Typed as char* because it is a byte buffer whose interpretation is
  /// `encoding`'s; it is cast to uint16_t* or uint32_t* at the two seams that
  /// read characters out of it, both of which go through ztypeset_core.c's text
  /// helpers.
  char* text;
  size_t length;
  ZtypesetEncoding encoding;
  uint8_t base_level;

  /// Retained for the paragraph's lifetime so lines can be created from it
  /// later. Rules L1 and L2 are defined PER LINE, so a line's reordering
  /// cannot be derived from the paragraph's runs -- it has to come from
  /// SBParagraphCreateLine over the line's own range.
  ///
  /// The SBAlgorithm is not held separately: SBParagraph retains it, so
  /// releasing ztypeset's reference after creation is correct.
  ///
  /// NULL for an empty paragraph, which never reaches SheenBidi at all.
  SBParagraphRef sb_paragraph;

  /// Which of the three segmentation passes this paragraph ran: an OR of
  /// ZtypesetSegmentation, as the caller gave it.
  uint32_t segmentation;

  /// The one allocation the break arrays live in, and the three views of it.
  ///
  /// One byte per CODE UNIT per pass ASKED FOR, in the order lines,
  /// graphemes, words -- so a paragraph that wanted only line breaks holds
  /// one array, not three with two unused. Each of the three pointers is
  /// NULL when its pass was not run, which is what the accessors return.
  ///
  /// Computed at creation rather than on demand, so every accessor stays
  /// infallible AND a built paragraph stays immutable: filling an array in on
  /// first access would make a const accessor a writer, and the documented
  /// promise that a paragraph is readable from several threads would go with
  /// it.
  uint8_t* breaks;
  uint8_t* line_breaks;
  uint8_t* grapheme_breaks;
  uint8_t* word_breaks;

  /// The whole paragraph laid out as a single line. Correct when the text
  /// fits on one; see ZtypesetLine for when it does not.
  ZtypesetArray visual_runs;
  /// Script runs are a property of the text, not of where it wraps, so a line
  /// reuses these rather than recomputing them.
  ZtypesetArray script_runs;
  ZtypesetArray shaping_runs;
};

struct ZtypesetLine {
  /// Byte range within the paragraph, as given.
  size_t offset;
  size_t length;

  /// Owned outright. A line copies what it needs at creation, so it holds no
  /// reference to the paragraph it came from and outliving it is legal.
  ZtypesetArray visual_runs;
  ZtypesetArray shaping_runs;
};

#endif  // ZTYPESET_INTERNAL_H_
