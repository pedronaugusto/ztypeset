//===----------------------------------------------------------------------===//
// ztext -- implementation-private declarations shared by the ffi/*.c units.
//
// Not installed and not part of the ABI. Nothing here may appear in ztext.h.
//===----------------------------------------------------------------------===//

#ifndef ZTEXT_INTERNAL_H_
#define ZTEXT_INTERNAL_H_

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

#include <hb.h>
#include <hb-ot.h>
#include <hb-ft.h>

#include <SheenBidi/SheenBidi.h>

#include <graphemebreak.h>
#include <linebreak.h>
#include <wordbreak.h>

#include "ztext.h"

//===----------------------------------------------------------------------===//
// Allocation
//
// Every allocation ztext makes -- its own, FreeType's, HarfBuzz's and
// SheenBidi's -- lands here. All three free without a size, so a
// header ahead of each block records what was asked for; that is what lets
// ZtextAllocator::deallocate be handed a size and alignment, which in turn is
// what lets a Zig host use its std.mem.Allocator directly.
//===----------------------------------------------------------------------===//

/// Alignment ztext uses when a caller asks for "whatever malloc would give".
/// FreeType, HarfBuzz and SheenBidi all allocate with malloc semantics and
/// never request more.
#define ZTEXT_DEFAULT_ALIGN (_Alignof(max_align_t))

/// Through the process-wide allocator.
void* ztextAlloc(size_t size, size_t alignment);
void* ztextRealloc(void* block, size_t new_size, size_t alignment);
void ztextFree(void* block);

/// Through a specific allocator, for memory that belongs to one object rather
/// than to the process. A block must be freed through the same allocator it
/// was allocated from -- the header records its size and alignment, not which
/// allocator produced it.
void* ztextAllocWith(const ZtextAllocator* allocator, size_t size,
                     size_t alignment);
void* ztextReallocWith(const ZtextAllocator* allocator, void* block,
                       size_t new_size, size_t alignment);
void ztextFreeWith(const ZtextAllocator* allocator, void* block);

/// Zero-initialising allocate, with the multiplication checked for overflow.
void* ztextCalloc(size_t count, size_t size);

/// Convenience for the common case: one zeroed object of a type.
#define ZTEXT_NEW(T) ((T*)ztextCalloc(1, sizeof(T)))

/// Wires `library`'s FreeType memory record to ztext, capturing the allocator
/// installed right now into `library->allocator`.
///
/// The record must outlive the FT_Library built from it, which is why it lives
/// inside the library rather than on a caller's stack.
void ztextInitFtMemory(ZtextLibrary* library);

/// Points SheenBidi's global default allocator at ztext's. Idempotent.
ZtextResult ztextInstallSheenbidiAllocator(void);

//===----------------------------------------------------------------------===//
// Error detail
//===----------------------------------------------------------------------===//

/// A process-unique, never-reused value, for detecting that a handle a caller
/// passed is not the one an earlier call was made against.
uint64_t ztextNextGeneration(void);

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
void ztextFaceActivate(const ZtextFace* face);

/// Pushes the font's current variation coordinates onto `face`'s HarfBuzz
/// fonts, both the OpenType one and the FreeType-backed one if it has been
/// built. A no-op for a font with no axes.
///
/// Called from three places, which is why it is shared rather than inlined:
/// when the axes move, when a face is created after they moved, and when the
/// FreeType-backed font is built lazily on the first shape that asks for it.
/// Miss any one of them and that face shapes the default instance while
/// FreeType rasterises the chosen one.
void ztextFaceApplyVariations(ZtextFace* face);

/// Records an upstream's own description of a failure, for
/// ztextLastErrorDetail. Never affects control flow.
void ztextSetErrorDetail(const char* detail);

/// Maps an FT_Error onto a ZtextResult, recording FreeType's error string as
/// the detail on the way through.
ZtextResult ztextFromFtError(FT_Error error);

//===----------------------------------------------------------------------===//
// UTF-8
//
// HarfBuzz substitutes U+FFFD for malformed input and SheenBidi has its own
// recovery; both are reasonable for a text editor and wrong for an engine
// reading a localisation table, where malformed bytes mean the table is
// corrupt and should say so. ztext validates first and refuses.
//===----------------------------------------------------------------------===//

/// True if `text[0..length]` is well-formed UTF-8: no overlong encodings, no
/// surrogate halves, nothing above U+10FFFF, no truncated sequence at the end.
bool ztextIsValidUtf8(const char* text, size_t length);

/// Decodes one scalar from `text`, which must already be VALID UTF-8, and
/// returns how many bytes it took. Never returns 0, so a loop over it always
/// terminates.
size_t ztextDecodeUtf8(const char* text, size_t length, uint32_t* out);

//===----------------------------------------------------------------------===//
// Growable array
//
// Used by the shaper for its converted glyph array and by the paragraph for
// its run lists. Deliberately tiny: capacity in elements, doubling growth,
// and no shrink -- a shaper that reaches steady state should stop allocating.
//===----------------------------------------------------------------------===//

typedef struct ZtextArray {
  void* data;
  size_t count;
  size_t capacity;
} ZtextArray;

/// Ensures room for `count` elements of `element_size`, growing if needed.
/// Leaves the array untouched and returns false on allocation failure.
bool ztextArrayReserve(ZtextArray* array, size_t count, size_t element_size);

void ztextArrayFree(ZtextArray* array, size_t element_size);

//===----------------------------------------------------------------------===//
// Handles
//
// Defined here as real structs so every accessor is statically typed and no
// cast from void* crosses an entry point.
//===----------------------------------------------------------------------===//

struct ZtextLibrary {
  /// FreeType holds a pointer to this, so it lives inside the library rather
  /// than on the stack of whoever created it. Its `user` points back at the
  /// library, which is how the allocation shims find `allocator` below.
  struct FT_MemoryRec_ memory;

  /// The allocator installed when this library was created, captured by value.
  ///
  /// This is what makes FreeType's memory genuinely per-library rather than
  /// process-wide: FT_New_Library takes an FT_Memory, and every allocation
  /// FreeType makes for this library -- faces, glyph slots, hinting state --
  /// is routed back through this copy, even if the process-wide allocator is
  /// replaced afterwards. HarfBuzz cannot do this; its seam is compile-time.
  ZtextAllocator allocator;

  FT_Library ft;
};

struct ZtextFont {
  ZtextLibrary* library;

  /// One FT_Face, shared by every face of this font. Each ZtextFace owns an
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
  /// This is the same bargain hb_face_t makes, and it is why ZtextFont has no
  /// documented ordering rule while ZtextLibrary still does.
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
  /// FreeType has these too, but ztext keeps its own copy because it is the
  /// only writer: every value that reaches FreeType was range-checked here
  /// first, so the two cannot diverge the way they would if FreeType were
  /// left to clamp. It also means reading one axis back costs no allocation.
  FT_Fixed* coords;
  float* design;

  /// Every live face of this font, intrusively linked through ZtextFace::next.
  ///
  /// A font-level setting has to reach face-level objects: variation
  /// coordinates live on the shared FT_Face, but HarfBuzz keeps its own copy
  /// on each hb_font_t, and each face's scaled metrics have to be recomputed
  /// once MVAR has moved the ascender. Without this list the font would have
  /// no way to find them, and the two halves of the pipeline would describe
  /// different instances.
  ZtextFace* faces;
};

struct ZtextFace {
  ZtextFont* font;

  /// Next face of the same font; see ZtextFont::faces. Order within the list
  /// is not meaningful, so creation pushes at the head.
  ZtextFace* next;

  /// This face's own scaled state. FreeType keeps the glyph SLOT on the
  /// FT_Face and the metrics on the FT_Size, so a face is exactly "the font
  /// at one size" and nothing else.
  FT_Size ft_size;

  /// Bumped when the face is created and on every size change.
  ///
  /// A shaper records the generation it shaped against so ztextShaperExtents
  /// can refuse a face that is not the one the run was shaped with, or the
  /// same face at a different size. An integer rather than a pointer, because
  /// comparing a pointer to a destroyed face is exactly the bug this is here
  /// to prevent -- and a recycled address would compare equal.
  uint64_t generation;

  /// Over the font's shared hb_face. The default source of shaping metrics.
  hb_font_t* hb_font;

  /// Built over the font's FT_Face on first use of
  /// ZtextShapeParams::use_freetype_metrics, so a host that never asks pays
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

  /// The last rasterised glyph, COPIED out of the font's shared glyph slot.
  ///
  /// Borrowing the slot would tie every face of a font together: one slot per
  /// FT_Face means loading a glyph through a sibling face frees the pixels
  /// this one just handed out. Copying costs a memcpy of a few hundred bytes
  /// against a rasterisation that measures in microseconds, and buys a
  /// lifetime rule that mentions only the face the caller used.
  ZtextArray bitmap;
};

struct ZtextShaper {
  hb_buffer_t* buffer;
  /// ZtextGlyph, converted out of HarfBuzz's parallel info/position arrays.
  ZtextArray glyphs;
  /// hb_feature_t, converted from the caller's ZtextFeature array. Kept on the
  /// shaper so a per-frame shape with features allocates nothing either.
  ZtextArray features;
  ZtextDirection direction;
  /// Which metrics source the last shape used, so extents can be taken from
  /// the same one. Deliberately NOT a pointer to the face: a borrowed handle
  /// stored across calls is a dangling pointer waiting for someone to destroy
  /// the face, so ztextShaperExtents takes the face as a parameter instead.
  bool used_freetype_metrics;
  /// The face generation this run was shaped against; see ZtextFace.
  uint64_t face_generation;
  bool shaped;
};

struct ZtextParagraph {
  /// The caller's bytes, copied.
  ///
  /// SheenBidi's SBAlgorithm stores the SBCodepointSequence it was given BY
  /// VALUE, and that struct holds a bare pointer to the caller's buffer; the
  /// SBParagraph retained below copies it again. Nothing on the paths ztext
  /// uses dereferences it after creation -- line reordering works from the
  /// resolved types and levels alone -- but that is a property of the pinned
  /// version, not a contract, and the documented promise is that a paragraph
  /// borrows nothing. One allocation buys that outright, and it is also what
  /// lets ztextLineCreate reject a byte range that would split a character.
  char* text;
  size_t length;
  uint8_t base_level;

  /// Retained for the paragraph's lifetime so lines can be created from it
  /// later. Rules L1 and L2 are defined PER LINE, so a line's reordering
  /// cannot be derived from the paragraph's runs -- it has to come from
  /// SBParagraphCreateLine over the line's own range.
  ///
  /// The SBAlgorithm is not held separately: SBParagraph retains it, so
  /// releasing ztext's reference after creation is correct.
  ///
  /// NULL for an empty paragraph, which never reaches SheenBidi at all.
  SBParagraphRef sb_paragraph;

  /// Where a line MAY break (UAX #14), where a grapheme cluster ends and
  /// where a word ends (UAX #29), one byte each per byte of `length`.
  ///
  /// Computed at creation rather than on demand, so every accessor stays
  /// infallible like the rest of the paragraph's. Three bytes per byte of
  /// text, against a handle that already holds a copy of the text and its
  /// levels; a paragraph is small by construction.
  ///
  /// One allocation, sliced three ways: the three arrays are always the same
  /// length and always live and die together.
  uint8_t* breaks;

  /// The whole paragraph laid out as a single line. Correct when the text
  /// fits on one; see ZtextLine for when it does not.
  ZtextArray visual_runs;
  /// Script runs are a property of the text, not of where it wraps, so a line
  /// reuses these rather than recomputing them.
  ZtextArray script_runs;
  ZtextArray shaping_runs;
};

struct ZtextLine {
  /// Byte range within the paragraph, as given.
  size_t offset;
  size_t length;

  /// Owned outright. A line copies what it needs at creation, so it holds no
  /// reference to the paragraph it came from and outliving it is legal.
  ZtextArray visual_runs;
  ZtextArray shaping_runs;
};

#endif  // ZTEXT_INTERNAL_H_
