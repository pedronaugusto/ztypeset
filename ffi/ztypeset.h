//===----------------------------------------------------------------------===//
// ztypeset -- a C ABI over FreeType, HarfBuzz, SheenBidi and libunibreak.
//
// This header is the contract between the C implementation and the Zig wrapper
// in ../src. Unlike a binding over a C++ library, it is not here because Zig
// cannot call the upstreams -- all four expose C APIs, and build.zig installs
// their headers so a C or C++ host can use them directly, which for anything
// beyond ztypeset's scope is the better choice.
//
// It is here for one reason: FreeType's FT_FaceRec and FT_GlyphSlotRec are
// large, deeply nested, and partly conditional on FreeType's own configuration
// macros. Hand-mirroring those in Zig would put silent memory corruption one
// re-vendor away. Stopping them at a C boundary means the C compiler checks
// every upstream layout change for free, and only small, flat, ztypeset-owned
// structs cross into Zig.
//
// Ownership, uniformly:
//   *Create    allocates through the installed allocator; caller owns the
//              handle and must pass it to the matching *Destroy.
//   *Destroy   accepts NULL, and takes each non-NULL handle EXACTLY ONCE.
//              All six, without exception; see below, because the exception
//              looks like it is there and is not.
//   accessors  return pointers that borrow from the handle and die with it,
//              or sooner where noted.
//
// ORDERING: there is none. A ZtypesetLibrary, the fonts made from it and the
// faces made from those may be destroyed in any order. Whichever of a pair is
// released second frees what they share, so no caller can produce a dangling
// handle by destroying in the "wrong" order, and neither a leak nor a double
// free is reachable. What a released handle will not do is take new work: every
// entry point that takes a ZtypesetLibrary, and ztypesetFaceCreate on a
// ZtypesetFont, answers ZTYPESET_RESULT_INVALID_ARGUMENT once its handle has
// been passed to *Destroy -- an error rather than undefined behaviour. The
// accessors on the fonts and faces that outlive it keep working, because those
// handles are still alive.
//
// DESTROYING TWICE is a different question from destroying in the wrong
// order, and the paragraph above answers only the second. Passing one handle
// to its *Destroy twice is undefined behaviour for every one of the six.
//
// ztypesetLibraryDestroy and ztypesetFontDestroy open with a flag test
// that reads like a repeat guard -- `if (library == NULL ||
// library->destroy_requested) return;` -- and it is not one. The flag
// exists so that whichever of a
// library and its fonts is released SECOND performs the teardown; by the time
// a caller could repeat the call, the handle it names has already been freed
// by that teardown, so the flag test is itself the use-after-free. Measured,
// not reasoned: a second ztypesetLibraryDestroy on a released library segfaults
// on that line, at ffi/ztypeset_face.c:69. It reads like a guarantee, which is
// the only reason it is written out here.
//
// There is no runtime check, and that is a decision rather than an omission. A
// poison word written into the handle before the free could be read back on the
// second call to diagnose it -- but reading it IS the use-after-free, so the
// check would be undefined behaviour reporting undefined behaviour, and
// ztypeset's own sanitiser build (CI runs Debug with -Dsanitize_c=true) would
// be right to flag the diagnostic itself. A registry of live handles would
// avoid the freed read at the price of process-wide mutable state on the
// drawing path, which is the one thing the thread-safety rules above exist to
// keep out. FreeType leaves this to the caller for the same reason; HarfBuzz
// escapes it only by reference-counting every object, which is a different
// ownership model rather than a check bolted onto this one.
//
// What IS checked: every *Destroy accepts NULL, swept by tests/null_sweep.c
// over every entry point, and ci/measurements.sh --check requires each
// *Destroy in this header to carry the "exactly once" rule in its own
// documentation -- because the version of this comment that claimed two
// handles were exempt was written from reading the source, and was wrong.
//
// This is deliberately NOT what FreeType does. FT_Done_Library destroys every
// face still registered with it, so a library freed while its fonts are alive
// would leave them reading a freed FT_Face: a use-after-free whose symptom is
// arbitrary, and which no error code could ever report. The alternative was a
// rule that only a comment can state and only a caller can keep. Each handle
// counts what is still alive instead, which costs one size_t and a bool per
// handle and is checked by the suite rather than read.
//
// Threading: see "Thread safety" below. Read it -- FT_Face is not thread-safe
// and ztypeset does not pretend otherwise.
//===----------------------------------------------------------------------===//

#ifndef ZTYPESET_H_
#define ZTYPESET_H_

#include <stddef.h>
#include <stdint.h>

/* Exported deliberately and narrowly.
 *
 * A shared ztypeset is built with -fvisibility=hidden, so the ~10 000 FreeType,
 * HarfBuzz and SheenBidi symbols linked into it stay internal. Without that,
 * loading libztypeset.so alongside a system libfreetype -- anything that pulls
    in
 * pango, cairo or fontconfig does -- lets the two interpose on each other, and
 * one library's FT_Face ends up inside the other's functions.
 *
 * A consumer of an MSVC DLL must define ZTYPESET_SHARED before including this
 * header, so the declarations become dllimport. Everywhere else nothing is
 * needed. */
#if defined(_MSC_VER) && defined(ZTYPESET_SHARED)
#ifdef ZTYPESET_BUILD
#define ZTYPESET_API __declspec(dllexport)
#else
#define ZTYPESET_API __declspec(dllimport)
#endif
#elif defined(ZTYPESET_SHARED) && (defined(__GNUC__) || defined(__clang__))
#define ZTYPESET_API __attribute__((visibility("default")))
#else
#define ZTYPESET_API
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
// * A ZtypesetLibrary, every ZtypesetFont made from it, and every ZtypesetFace
// made from those, belong to ONE thread. The faces of a font share its FT_Face
// and its single glyph slot, so they are not independent even though they are
// separate handles. Use one ZtypesetLibrary per thread rather than sharing one
// behind a lock; FreeType's own documentation recommends the same. *
// ztypesetFaceRenderGlyph returns pixels the FACE owns, copied out of that
// shared slot. They are valid until the next ztypesetFaceRenderGlyph on the
// same face, and nothing else invalidates them -- not a call on a sibling face,
// not shaping, not measuring. Copying is what buys that; see the note on
// ZtypesetGlyphBitmap. * A ZtypesetShaper holds scratch for one shaping call at
// a time. Give each thread its own; they are cheap. * ZtypesetParagraph does
// not touch FreeType at all. Once created it is immutable and may be read from
// several threads.
//
// The allocator installed by ztypesetSetAllocator is process-wide (HarfBuzz's
// seam is compile-time, so it cannot be otherwise) and must therefore be
// thread-safe if ztypeset is used from more than one thread.
//
// ztypesetSetAllocator and ztypesetRegisterAllocator are SETUP, not operations.
// They mutate a process-wide registry without synchronisation and must be
// called before any other thread is using ztypeset -- once, at start-up, the
// way a host installs its allocator. That restriction is theirs alone:
// everything ztypeset keeps process-wide and writes AFTER start-up -- the face
// generation counter and SheenBidi's one-time allocator install -- is atomic,
// so the per-library rule above is the only one the drawing path imposes.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Version
//===----------------------------------------------------------------------===//

#define ZTYPESET_VERSION_MAJOR 0
#define ZTYPESET_VERSION_MINOR 2
#define ZTYPESET_VERSION_PATCH 1

/// Version of the ztypeset binding, packed as (major<<16)|(minor<<8)|patch.
/// Compare against the ZTYPESET_VERSION_* macros to detect a header/library
/// skew.
///
/// What a bump of each position promises -- and what an ABI change is
/// required to do -- is in CHANGELOG.md, which is also where the three homes
/// of this number are named. ci/measurements.sh --check gates them.
ZTYPESET_API uint32_t ztypesetVersion(void);

/// Versions of the vendored upstreams, same packing. These report what was
/// actually compiled in, not what UPSTREAM.md claims.
ZTYPESET_API uint32_t ztypesetFreetypeVersion(void);
ZTYPESET_API uint32_t ztypesetHarfbuzzVersion(void);
ZTYPESET_API uint32_t ztypesetSheenbidiVersion(void);
ZTYPESET_API uint32_t ztypesetUnibreakVersion(void);

/// Packs four characters into an OpenType tag, big-endian as the specs write
/// them: ZTYPESET_TAG('l','i','g','a').
#define ZTYPESET_TAG(a, b, c, d)                                       \
  ((uint32_t)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) |       \
              ((uint32_t)(c) << 8) | (uint32_t)(d)))

//===----------------------------------------------------------------------===//
// Results
//===----------------------------------------------------------------------===//

typedef enum ZtypesetResult {
  ZTYPESET_RESULT_OK = 0,
  /// The allocator returned NULL, or an upstream reported an allocation
  /// failure of its own.
  ZTYPESET_RESULT_OUT_OF_MEMORY = 1,
  /// A NULL handle, a zero-length buffer, an out-of-range index, or a
  /// non-finite scalar.
  ZTYPESET_RESULT_INVALID_ARGUMENT = 2,
  /// The text was not well-formed in the ZtypesetEncoding it was passed with.
  /// Checked by ztypeset before any of it reaches HarfBuzz or SheenBidi.
  ZTYPESET_RESULT_INVALID_TEXT = 3,
  /// FreeType refused the bytes: not a font, truncated, or structurally
  /// broken.
  ZTYPESET_RESULT_BAD_FONT = 4,
  /// A recognisable font in a format this build does not compile support for
  /// -- WOFF, WOFF2, Type 1, bitmap-only formats. See UPSTREAM.md.
  ZTYPESET_RESULT_UNSUPPORTED = 5,
  /// The glyph index is not present in the face.
  ZTYPESET_RESULT_GLYPH_NOT_FOUND = 6,
  /// FreeType loaded the glyph but could not rasterise it.
  ZTYPESET_RESULT_RENDER_FAILED = 7,
  /// HarfBuzz could not shape the run.
  ZTYPESET_RESULT_SHAPE_FAILED = 8,
  /// SheenBidi could not analyse the paragraph.
  ZTYPESET_RESULT_BIDI_FAILED = 9,
  /// A caller-provided output buffer was too small.
  ZTYPESET_RESULT_BUFFER_TOO_SMALL = 10,
} ZtypesetResult;

/// Static, never-NULL description of a result code. Borrowed; do not free.
ZTYPESET_API const char* ztypesetResultName(ZtypesetResult result);

/// The last message an upstream produced ON THIS THREAD, or "" if there was
/// none.
///
/// ztypeset's result enum is flat by design, which loses detail FreeType has
/// and ztypeset does not: "unknown file format" and "invalid table" both arrive
/// as ZTYPESET_RESULT_BAD_FONT. This returns FreeType's own string for the most
/// recent failure so a log line can say which.
///
/// The storage is thread-local, so it must be read on the thread that got the
/// error -- a logger running elsewhere sees "". Borrowed, overwritten by the
/// next failing call, and for diagnostics only: never branch on it.
ZTYPESET_API const char* ztypesetLastErrorDetail(void);

//===----------------------------------------------------------------------===//
// Text
//
// Every entry point that takes text takes an encoding with it, and every
// offset and length that describes that text -- a run's, a line's, a glyph's
// cluster, an index into a break array -- is counted in the CODE UNITS of
// that encoding. Bytes for UTF-8, uint16_t for UTF-16, uint32_t for UTF-32.
//
// All three are here because all three upstreams take all three natively:
// SheenBidi's SBStringEncoding, libunibreak's utf8/utf16/utf32 entry point
// per algorithm, and hb_buffer_add_utf8/utf16/utf32. Transcoding on the way
// in would cost a copy, an allocation and a second set of offsets to map
// back, and would be paid by every host whose strings are UTF-16 -- which is
// every Windows, Java, C# and JavaScript host. ztypeset transcodes nothing.
//
// Text is validated in its declared encoding before any of it reaches an
// upstream, and malformed text is ZTYPESET_RESULT_INVALID_TEXT rather than
// U+FFFD: substituting a replacement character is a decision about a host's
// data that a text engine is not in a position to make.
//===----------------------------------------------------------------------===//

typedef enum ZtypesetEncoding {
  /// `const char*`. One to four bytes per character.
  ZTYPESET_ENCODING_UTF8 = 0,
  /// `const uint16_t*`, native byte order, a surrogate PAIR per character
  /// above U+FFFF. An unpaired surrogate is not well-formed.
  ZTYPESET_ENCODING_UTF16 = 1,
  /// `const uint32_t*`, native byte order, one unit per character. A unit
  /// above U+10FFFF or inside the surrogate range is not well-formed.
  ZTYPESET_ENCODING_UTF32 = 2,
} ZtypesetEncoding;

//===----------------------------------------------------------------------===//
// Allocator seam
//
// Three of the four upstreams allocate; all three allow it to be redirected,
// and all three do it differently. (libunibreak allocates nothing, so it has
// no seam and needs none.)
//
//   FreeType   an FT_MemoryRec per FT_Library. ztypeset captures the
//              installed allocator when a ZtypesetLibrary is created, so
//              FreeType memory is genuinely per-library rather than global.
//   SheenBidi  a global default allocator object (its creation functions take
//              no allocator argument).
//   HarfBuzz   compile-time only: four macros resolved when the library is
//              built. Necessarily process-wide.
//
// HarfBuzz is the binding constraint, so ztypesetSetAllocator is process-wide.
// That is surfaced here rather than hidden behind a per-object parameter that
// could not be honoured.
//
// All three also free without a size -- FT_Free_Func, hb_free_impl and
// SBAllocatorDeallocateBlockFunc each receive only a pointer. Rather than push
// that asymmetry onto every host, ztypeset records the size and alignment in a
// header ahead of each block and hands them back on deallocate. A host with a
// sized allocator (Zig's std.mem.Allocator, a pool, an arena with accounting)
// therefore needs no bookkeeping of its own.
//===----------------------------------------------------------------------===//

typedef struct ZtypesetAllocator {
  /// Must return a block of at least `size` bytes aligned to `alignment`
  /// (always a power of two), or NULL on failure. `size` is never 0.
  void* (*allocate)(void* user, size_t size, size_t alignment);

  /// Optional; may be NULL. Provided so a host that can grow a block in place
  /// is not forced through a copy.
  ///
  /// Return NULL to decline -- because the block cannot grow in place, or
  /// because there is no memory. ztypeset does not distinguish the two: it
  /// falls back to allocate, copy, deallocate either way, and only then reports
  /// failure. `block` must be left valid and untouched when declining, which is
  /// what makes that fallback safe.
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
} ZtypesetAllocator;

/// Exit code ztypeset uses when it stops the process because a block reached
/// the wrong allocator; see ztypesetSetAllocator. Fixed so a test harness can
/// assert on it rather than on a platform's abort convention.
#define ZTYPESET_EXIT_ALLOCATOR_MISMATCH 70

/// Installs a process-wide allocator for all subsequent ztypeset allocation.
///
/// `alloc` is copied by value; the caller need not keep the struct alive, but
/// `user` must outlive every handle allocated through it. Passing NULL
/// restores malloc/free.
///
/// Calls ztypesetWarmup() first, so the upstreams' process-lifetime caches are
/// charged to whatever was installed BEFORE this call rather than to the
/// allocator arriving now. Those caches are never freed, so an allocator that
/// paid for one can never report a balanced heap -- and the host would have
/// had to know to warm up first to avoid it. See ztypesetWarmup for the two
/// caches this cannot reach, which need a face and so belong to the host.
///
/// EVERY BLOCK IS FREED THROUGH THE ALLOCATOR THAT MADE IT, BY CONSTRUCTION
/// RATHER THAN BY DISCIPLINE. Every allocator ever installed keeps an entry in
/// a small registry; each block's header records the INDEX of the entry that
/// issued it, and every deallocate and reallocate is routed back to that entry
/// rather than to whatever happens to be installed at the time.
///
/// So swapping the allocator with live handles is safe, not undefined: an
/// hb_face_t built under one allocator is destroyed through that one even if
/// the process-wide allocator changed in between, and the FreeType memory of
/// the same font -- which has always been per-library -- agrees with it. That
/// was not true before: one handle could span two heaps, with nothing that
/// could tell you.
///
/// WHICH allocator makes a block is the other half of the rule, and it has
/// two cases because the upstreams offer two kinds of seam:
///
///   * Everything ztypeset and FreeType allocate FOR A HANDLE comes from the
///     allocator that issued the handle. A font's struct, its axis arrays, a
///     face's FT_Size, and the buffer a face keeps its rendered glyph in are
///     all their library's memory -- whatever is installed at the moment they
///     are first created or grown. FreeType's seam takes an FT_Memory per
///     library, and ztypeset's own takes the allocator read back from the
///     handle's own block header, so neither depends on the current global.
///   * Everything HARFBUZZ allocates comes from whatever is installed when it
///     asks. Its seam is a compile-time pair of functions with no context
///     argument, so there is nothing for per-handle routing to hang on: an
///     hb_font_t built after a swap belongs to the new allocator, and is
///     freed back to it. SheenBidi's seam is the same shape.
///
/// A ZtypesetShaper, ZtypesetParagraph and ZtypesetLine own no library, so the
/// first rule places their memory too -- with the allocator that issued the
/// handle simply being the one installed at the time.
///
/// The consequence worth stating: a host that gives each of its subsystems an
/// allocator gets a font's FreeType and ztypeset memory attributed to the
/// subsystem that made the LIBRARY, and its HarfBuzz memory attributed to
/// whichever subsystem happened to make the face.
///
/// Installing the same allocator twice reuses its entry. A distinct one costs a
/// single ZtypesetAllocator, allocated with malloc and never freed, because it
/// must outlive the last block it issued. That is the only allocation ztypeset
/// makes outside the installed allocator.
///
/// If ztypeset itself ever names the wrong owner for a block -- an internal
/// mistake, not something a host can cause -- it does NOT free the block
/// (leaking one block is recoverable; handing a pointer to a heap that never
/// issued it is not), writes a line naming both allocators to stderr, and exits
/// with ZTYPESET_EXIT_ALLOCATOR_MISMATCH. There is no way to continue: the
/// deallocate callback has no error channel, because FreeType's, HarfBuzz's and
/// SheenBidi's have none.
///
/// The header check is a detector, not a checksum: sixteen bytes leave no room
/// for a magic number, so a prefix overrun into values that happen to be in
/// range still passes. What it catches on every deallocation is an index past
/// the end of the registry and an alignment that is not a power of two at or
/// below max_align_t's.
///
/// Returns ZTYPESET_RESULT_INVALID_ARGUMENT if `allocate` or `deallocate` is
/// NULL, in which case the previously installed allocator is left untouched.
/// Returns ZTYPESET_RESULT_OUT_OF_MEMORY if the registry entry cannot be
/// allocated, in which case the previously installed allocator is likewise left
/// untouched.
ZTYPESET_API ZtypesetResult ztypesetSetAllocator(const ZtypesetAllocator*
                                                 alloc);

/// Populates the process-global caches the upstreams never free before exit,
/// so a host can install a tracking allocator afterwards and still see a
/// balanced heap.
///
/// Optional for correctness, and the ONLY route to a balanced heap for a host
/// that audits one. Two of the upstreams keep caches for the life of the
/// process: HarfBuzz builds several singletons on first use -- the shaper
/// list, the Unicode database, the font-functions, and an intern table with
/// one entry per distinct language tag ever passed -- and SheenBidi creates
/// its allocator object once and keeps it. One of those language entries is
/// the default tag, interned the first time a glyph is HINTED rather than
/// shaped: the autohinter takes its glyph coverage from GSUB, and the buffer
/// it does that with asks HarfBuzz for the default language. That tag is a
/// compile-time constant here, not the machine's locale.
///
/// None of the HarfBuzz ones are freed at exit in this build. Upstream frees
/// them from an atexit handler only where HAVE_ATEXIT is defined; ztypeset does
/// not define it and passes -DHB_NO_ATEXIT to make that a decision rather than
/// an omission, so hb_atexit expands to nothing. An atexit handler calling a
/// host's allocator after the host has torn it down is worse than a bounded,
/// documented cache.
///
/// Without this call, those allocations are attributed to whichever allocator
/// happened to be installed when something first touched them -- which is
/// usually the host's tracking one, and shows up as an unbalanced heap that no
/// amount of correct ztypeset use will fix. All of them are bounded and small;
/// see UPSTREAM.md.
///
/// Two such caches are out of its reach, because both are built from a real
/// face that warm-up has no way to obtain: HarfBuzz's FreeType font-functions
/// singleton, created by the first shape with `use_freetype_metrics` (about
/// 200 bytes), and one intern-table entry per DISTINCT language tag ever
/// passed (tens of bytes each). A host that uses neither allocates neither; a
/// host that audits and uses either can warm them by shaping one throwaway run
/// before installing its allocator.
///
/// ztypesetSetAllocator calls this before it installs anything, so a host that
/// audits its heap does not have to know to. Call it directly only to warm the
/// caches earlier than that -- or to warm the two above that need a face, by
/// shaping a throwaway run. Safe to call more than once, and safe never to
/// call.
ZTYPESET_API void ztypesetWarmup(void);

//===----------------------------------------------------------------------===//
// Library, fonts and faces
//
// Three handles, because there are three lifetimes:
//
//   ZtypesetLibrary   FreeType's modules and the allocator they were built
//                     with.
//   ZtypesetFont      one font image, parsed once. Its only mutable state
//                     is its variation axes, which are per-font because
//                     FreeType keeps them on the FT_Face; see "Variable
//                     fonts".
//   ZtypesetFace      that font at one size. Cheap; make one per size you
//                     draw.
//
// The split is not decoration. A ZtypesetFace used to carry its own FT_Face and
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
typedef struct ZtypesetLibrary ZtypesetLibrary;

/// One parsed font image, shared by every face made from it.
///
/// Immutable apart from its variation axes, which live here rather than on a
/// face because FreeType keeps them on the shared FT_Face.
typedef struct ZtypesetFont ZtypesetFont;

/// One font at one pixel size. Everything that depends on the size, and
/// nothing that does not.
typedef struct ZtypesetFace ZtypesetFace;

ZTYPESET_API ZtypesetResult ztypesetLibraryCreate(ZtypesetLibrary** out);

/// Releases the caller's claim on the library. Its memory goes when the last
/// font made from it does, so this may be called in any order against them.
///
/// Call it exactly once. The flag this sets is read by the release path, not
/// by a second call to this function -- by then the handle is freed.
ZTYPESET_API void ztypesetLibraryDestroy(ZtypesetLibrary* library);

/// Creates a font from an image already in memory.
///
/// There is deliberately no path-based entry point. A host feeding fonts out
/// of an asset pack has the bytes, not a path, and a file API would have to
/// carry FreeType's stream layer and its own error surface for no one's
/// benefit.
///
/// `data` is BORROWED, not copied: FreeType and HarfBuzz both read tables out
/// of it lazily for as long as the font is alive. The buffer must outlive the
/// ZtypesetFont and every face made from it, and must not move or be written to
/// in the meantime. This is FreeType's contract for FT_New_Memory_Face and
/// ztypeset passes it through rather than hiding a copy the caller did not ask
/// for.
///
/// `face_index` selects a face inside a collection (.ttc); use 0 otherwise.
/// ztypesetLibraryCountFaces says how many there are.
ZTYPESET_API ZtypesetResult ztypesetFontCreateFromMemory(ZtypesetLibrary*
                                                         library,
                                                const void* data, size_t size,
                                                uint32_t face_index,
                                                ZtypesetFont** out);

/// Releases the caller's claim on the font.
///
/// The font's memory goes when its last face does, so this may be called
/// before or after ztypesetFaceDestroy with the same result. Faces created from
/// it stay fully usable; only ztypesetFaceCreate stops working.
///
/// Call it exactly once, for the same reason ztypesetLibraryDestroy must be.
ZTYPESET_API void ztypesetFontDestroy(ZtypesetFont* font);

/// Borrowed family and style names, "" when the font does not name itself.
/// Valid while the font is alive.
ZTYPESET_API const char* ztypesetFontFamilyName(const ZtypesetFont* font);
ZTYPESET_API const char* ztypesetFontStyleName(const ZtypesetFont* font);

/// Glyph index for a character in the font's SELECTED character map, or 0
/// (.notdef) if it has none. Shaping does its own mapping; this is for callers
/// checking coverage before choosing a fallback font.
///
/// The argument is a Unicode scalar for as long as a Unicode charmap is
/// selected, which is the default and what almost every font wants. Select a
/// non-Unicode one -- an icon font's MS Symbol map, say -- and the argument is
/// a code in THAT encoding instead; see ztypesetFontSelectCharmap.
ZTYPESET_API uint32_t ztypesetFontGlyphIndex(const ZtypesetFont* font,
                                       uint32_t codepoint);

//===----------------------------------------------------------------------===//
// Character maps
//
// A font may carry several, and which one is selected decides what
// ztypesetFontGlyphIndex and ztypesetFontCoveredPrefix answer. FreeType selects
// a Unicode one when it opens the font, so a caller who never touches this gets
// Unicode -- but an icon font whose glyphs live only in a (3, 0) MS Symbol map
// has no Unicode map to select, and without a way to say so its glyphs are
// reachable by index alone.
//
// Shaping is NOT affected: HarfBuzz does its own Unicode mapping from the same
// tables and never consults FreeType's selection. That is a property of the two
// upstreams rather than a choice ztypeset made, and it is why this is a font
// operation rather than a face one.
//===----------------------------------------------------------------------===//

/// FreeType's reading of a (platform, encoding) pair, as a four-character tag.
/// These are FT_Encoding's own values, republished so a consumer need not
/// include FreeType's headers to name one.
#define ZTYPESET_CHARMAP_NONE 0u
#define ZTYPESET_CHARMAP_MS_SYMBOL ZTYPESET_TAG('s', 'y', 'm', 'b')
#define ZTYPESET_CHARMAP_UNICODE ZTYPESET_TAG('u', 'n', 'i', 'c')
#define ZTYPESET_CHARMAP_SJIS ZTYPESET_TAG('s', 'j', 'i', 's')
#define ZTYPESET_CHARMAP_PRC ZTYPESET_TAG('g', 'b', ' ', ' ')
#define ZTYPESET_CHARMAP_BIG5 ZTYPESET_TAG('b', 'i', 'g', '5')
#define ZTYPESET_CHARMAP_WANSUNG ZTYPESET_TAG('w', 'a', 'n', 's')
#define ZTYPESET_CHARMAP_JOHAB ZTYPESET_TAG('j', 'o', 'h', 'a')
#define ZTYPESET_CHARMAP_ADOBE_STANDARD ZTYPESET_TAG('A', 'D', 'O', 'B')
#define ZTYPESET_CHARMAP_ADOBE_EXPERT ZTYPESET_TAG('A', 'D', 'B', 'E')
#define ZTYPESET_CHARMAP_ADOBE_CUSTOM ZTYPESET_TAG('A', 'D', 'B', 'C')
#define ZTYPESET_CHARMAP_ADOBE_LATIN_1 ZTYPESET_TAG('l', 'a', 't', '1')
#define ZTYPESET_CHARMAP_OLD_LATIN_2 ZTYPESET_TAG('l', 'a', 't', '2')
#define ZTYPESET_CHARMAP_APPLE_ROMAN ZTYPESET_TAG('a', 'r', 'm', 'n')

/// The index no charmap has, answered by ztypesetFontActiveCharmap for a font
/// with none selected -- which is a state FreeType allows and a font with no
/// character map at all is in.
#define ZTYPESET_CHARMAP_INDEX_NONE 0xFFFFFFFFu

typedef struct ZtypesetCharmap {
  /// The pair exactly as the font's `cmap` records it, unfiltered: (3, 1) is
  /// Windows Unicode BMP, (3, 0) is Windows Symbol, (0, x) is Apple Unicode.
  uint16_t platform_id;
  uint16_t encoding_id;
  /// What FreeType makes of that pair: one of ZTYPESET_CHARMAP_* above, or
  /// ZTYPESET_CHARMAP_NONE for a pair it has no name for. Two records can share
  /// one encoding, and two encodings can name the same subtable, so this is
  /// the value to select by and the pair above is the value to display.
  uint32_t encoding;
} ZtypesetCharmap;

/// How many character maps this font declares, and what the one at `index`
/// is. `index` must be below the count; anything else is
/// ZTYPESET_RESULT_INVALID_ARGUMENT, and `out` is zeroed either way.
ZTYPESET_API uint32_t ztypesetFontCharmapCount(const ZtypesetFont* font);
ZTYPESET_API ZtypesetResult ztypesetFontCharmap(const ZtypesetFont* font,
                                                uint32_t index,
                                       ZtypesetCharmap* out);

/// Which one is selected, as an index into the same list, or
/// ZTYPESET_CHARMAP_INDEX_NONE when none is.
ZTYPESET_API uint32_t ztypesetFontActiveCharmap(const ZtypesetFont* font);

/// Selects one, by index or by encoding.
///
/// By ENCODING is the form to reach for: a font's records are in its own
/// order, so an index is only meaningful next to the list it came from, while
/// ZTYPESET_CHARMAP_MS_SYMBOL means the same thing in every font. Selecting an
/// encoding no charmap of this font carries is
/// ZTYPESET_RESULT_INVALID_ARGUMENT, so "this font has a symbol map" is a
/// question this answers rather than one the caller has to walk the list for.
///
/// The selection belongs to the FONT, so every face built from it sees it, and
/// it changes nothing about a run already shaped -- shaping never went through
/// here.
ZTYPESET_API ZtypesetResult ztypesetFontSelectCharmap(ZtypesetFont* font,
                                                      uint32_t index);
ZTYPESET_API ZtypesetResult ztypesetFontSelectCharmapEncoding(ZtypesetFont*
                                                              font,
                                                     uint32_t encoding);

/// Glyph index for a base character followed by a VARIATION SELECTOR, or 0 if
/// the font names no glyph for that exact pair.
///
/// This is cmap subtable format 14, the mechanism behind U+FE0E/U+FE0F (text
/// and emoji presentation) and the Ideographic Variation Sequences that
/// distinguish two written forms of the same CJK character. Without it a
/// coverage check has to decide with the base character alone and will report
/// coverage the font does not actually have for the sequence.
///
/// Format 14 has two kinds of entry and this collapses them on purpose. A
/// sequence the font gives a glyph of its own returns that glyph; a sequence
/// the font records as its DEFAULT -- "draw this pair with the base
/// character's own glyph", stored with no glyph of its own -- returns the base
/// character's glyph, which FreeType looks up through the Unicode cmap for you
/// (libs/freetype/src/sfnt/ttcmap.c, tt_cmap14_char_var_index).
///
/// So the answer is nonzero exactly when this font draws this exact sequence,
/// and 0 for all three ways it does not: no format-14 subtable, no record for
/// that selector, and a selector whose tables do not list that base
/// character.
ZTYPESET_API uint32_t ztypesetFontVariantGlyphIndex(const ZtypesetFont* font,
                                              uint32_t codepoint,
                                              uint32_t variation_selector);

/// Number of glyphs, and design units per em. The latter is 0 for a font with
/// no scalable outlines.
ZTYPESET_API uint32_t ztypesetFontGlyphCount(const ZtypesetFont* font);
ZTYPESET_API uint32_t ztypesetFontUnitsPerEm(const ZtypesetFont* font);

/// How many leading code units of `text` this font can draw, for a host
/// walking its own fallback list.
///
/// ztypeset does not own that list, because which font to fall back to is a
/// policy question -- a UI's answer differs from a document reader's, and both
/// differ from a game's. What ztypeset owns is the part that is not a policy
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
/// Rejects malformed text with ZTYPESET_RESULT_INVALID_TEXT, like everything
/// else that takes text. `length` and `*out` are in `encoding`'s code units.
ZTYPESET_API ZtypesetResult ztypesetFontCoveredPrefix(const ZtypesetFont* font,
                                             const void* text, size_t length,
                                             ZtypesetEncoding encoding,
                                             size_t* out);

// Variable fonts.
//
// A variable font carries an `fvar` table naming a few axes -- weight, width,
// optical size -- and one set of outlines interpolated across the range each
// axis declares. Choosing a coordinate on every axis picks one instance out of
// that continuum, which is what the four functions below do.
//
// The setting belongs to the FONT, not to a face, and that is FreeType's
// arrangement rather than a choice ztypeset made: FT_Set_Var_Design_Coordinates
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
// make unreachable. ztypesetFontSetVariations updates FreeType and the HarfBuzz
// font of every live face in one call, so the two cannot come apart.
//
// Values are DESIGN coordinates -- the numbers `fvar` itself names, 400 for a
// regular weight and 700 for a bold one -- not the normalised -1..1 the
// OpenType internals work in. A value outside an axis's range is refused
// rather than clamped, the same bargain ztypesetLibrarySetSdfSpread makes, so a
// caller asking for a weight the font does not have finds out.

/// One variable axis, in design units.
typedef struct ZtypesetVariationAxis {
  /// Four-character tag: 'wght', 'wdth', 'slnt', 'opsz', 'ital'. Build one
  /// with ZTYPESET_TAG.
  uint32_t tag;
  float min_value;
  float default_value;
  float max_value;
} ZtypesetVariationAxis;

/// One axis set to one value.
typedef struct ZtypesetVariation {
  uint32_t tag;
  float value;
} ZtypesetVariation;

/// Number of variable axes, or 0 for a static font -- which is not an error,
/// just the answer.
ZTYPESET_API uint32_t ztypesetFontAxisCount(const ZtypesetFont* font);

/// Describes axis `index`, which must be below ztypesetFontAxisCount.
///
/// The axes are in the font's own order, and that order is what
/// ztypesetFontVariation indexes too.
ZTYPESET_API ZtypesetResult ztypesetFontAxis(const ZtypesetFont* font,
                                             uint32_t index,
                                    ZtypesetVariationAxis* out);

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
///     FreeType clamps such a value, ztypeset refuses it, exactly as with the
///     SDF spread;
///   * a font with no axes at all.
///
/// The whole request is checked before any of it is applied, so a rejection
/// never leaves half the axes moved.
///
/// This invalidates every run already measured against a face of this font:
/// HVAR moves the advances and MVAR can move the ascender, so a run measured
/// before the change is not one that can be laid out after it.
ZTYPESET_API ZtypesetResult ztypesetFontSetVariations(ZtypesetFont* font,
                                             const ZtypesetVariation* values,
                                             size_t count);

/// Current design value of axis `index`, which starts at the axis default.
ZTYPESET_API ZtypesetResult ztypesetFontVariation(const ZtypesetFont* font,
                                                  uint32_t index,
                                         float* out);

/// Number of NAMED INSTANCES the font declares -- the entries a font's own
/// designers named, "Condensed Light" and the rest, each one a point in the
/// axis space. 0 for a font with no `fvar`, and 0 for a variable font that
/// names none.
///
/// A variable font is a continuous space and a picker needs the points in it
/// that someone chose deliberately; deriving them from the axes is not
/// possible, because they are data rather than a rule.
ZTYPESET_API uint32_t ztypesetFontNamedInstanceCount(const ZtypesetFont* font);

/// Design coordinates of one named instance, one per axis, in the order
/// ztypesetFontAxis reports them.
///
/// `*count` is the capacity of `values` on the way in and the number written
/// on the way out. Pass `values = NULL` to ask only for the count, which is
/// always ztypesetFontAxisCount. A buffer too small is
/// ZTYPESET_RESULT_BUFFER_TOO_SMALL with `*count` set to what is needed, and
/// nothing written.
ZTYPESET_API ZtypesetResult ztypesetFontNamedInstanceCoords(const ZtypesetFont*
                                                            font,
                                                   uint32_t index,
                                                   float* values,
                                                   size_t* count);

/// The instance's subfamily name, as UTF-8, NUL-terminated.
///
/// `*size` is the capacity of `buffer` in bytes on the way in and the length
/// written EXCLUDING the NUL on the way out. Pass `buffer = NULL` to ask for
/// the length first; `*size` then comes back as the length a buffer must hold
/// beyond its NUL. Too small a buffer is ZTYPESET_RESULT_BUFFER_TOO_SMALL with
/// `*size` set to what is needed, and nothing written.
///
/// The name comes from the font's `name` table through HarfBuzz, which
/// decodes the platform encoding -- usually UTF-16BE -- so a caller never
/// meets one.
///
/// ZTYPESET_RESULT_UNSUPPORTED when the lookup yields nothing, which covers two
/// cases HarfBuzz does not distinguish: an instance whose `name` id the table
/// does not carry, and one whose name is the empty string. Both are
/// malformed fonts, and neither gives a picker anything to show.
ZTYPESET_API ZtypesetResult ztypesetFontNamedInstanceName(const ZtypesetFont*
                                                          font,
                                                 uint32_t index, char* buffer,
                                                 size_t* size);

/// Moves every axis to the named instance's coordinates, in one step.
///
/// Equivalent to reading the coordinates and passing all of them to
/// ztypesetFontSetVariations, and subject to the same rule: it invalidates
/// every face of this font for measurement, because a run measured before the
/// change is not one that can be laid out after it.
ZTYPESET_API ZtypesetResult ztypesetFontSetNamedInstance(ZtypesetFont* font,
                                                uint32_t index);

/// Creates a face: this font, at this size.
///
/// A face is never sizeless -- the size is part of what it is -- so there is
/// no state in which measuring or rendering has to be refused for want of one.
/// See ztypesetFaceSetPixelSize for what the size arguments accept.
///
/// Faces of one font share its FT_Face, and therefore its single glyph slot
/// and its one thread. They do not share a size, a HarfBuzz font, or a glyph
/// bitmap.
ZTYPESET_API ZtypesetResult ztypesetFaceCreate(ZtypesetFont* font, float width,
                                      float height, ZtypesetFace** out);

/// Destroys the face, which nothing else is waiting on: its memory is
/// released here rather than deferred. Call it exactly once.
ZTYPESET_API void ztypesetFaceDestroy(ZtypesetFace* face);

/// The font this face was made from, borrowed. Never NULL for a live face.
ZTYPESET_API ZtypesetFont* ztypesetFaceFont(const ZtypesetFace* face);

/// Changes this face's size in pixels. Passing 0 for one axis copies the
/// other.
///
/// A face already has a size from ztypesetFaceCreate; this is for a face that
/// follows a changing scale factor, and it invalidates any run measured
/// against the face.
///
/// Fractional sizes are real, not rounded away: 9 pt at a 150% scale factor is
/// 18.75 px, and a UI that rounds that to 19 drifts against every other
/// element on the same scaled layout. The value is quantised to 1/64 px, which
/// is FreeType's own resolution, and anything that would quantise to zero is
/// ZTYPESET_RESULT_INVALID_ARGUMENT rather than a face that renders nothing.
///
/// Non-finite values are refused. So is anything above 16384 px, which is a
/// caller error long before it is a FreeType one.
ZTYPESET_API ZtypesetResult ztypesetFaceSetPixelSize(ZtypesetFace* face,
                                                     float width,
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
typedef struct ZtypesetFaceMetrics {
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
  /// deciding a layout wants them in the same answer, and are on ZtypesetFont
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
} ZtypesetFaceMetrics;

ZTYPESET_API ZtypesetResult ztypesetFaceMetrics(const ZtypesetFace* face,
                                       ZtypesetFaceMetrics* out);

/// One metric from the font's own tables, named as OpenType names it.
///
/// ZtypesetFaceMetrics above is what a line of text needs and comes from
/// FreeType, which reads `hhea`. This is the rest of what OpenType defines,
/// read through HarfBuzz -- and the two do not always agree, which is the
/// point rather than an inconsistency:
///
/// * ZTYPESET_METRIC_HORIZONTAL_ASCENDER prefers OS/2's sTypoAscender when the
/// font sets the USE_TYPO_METRICS bit in fsSelection, and falls back to `hhea`
/// otherwise (libs/harfbuzz/src/hb-ot-metrics.cc). That is the rule modern text
/// stacks follow. ZtypesetFaceMetrics::ascender is `hhea` unconditionally,
/// because that is what FreeType scales onto the FT_Size. A font that sets the
/// bit and disagrees between the two tables will report two different ascenders
/// here, and both are correct answers to different questions. *
/// ZTYPESET_METRIC_UNDERLINE_OFFSET is `post`'s own number, which is the TOP
/// edge of the stroke. ZtypesetFaceMetrics::underline_position is the CENTRE:
/// FreeType subtracts half the thickness to convert the TrueType meaning to its
/// own (libs/freetype/src/sfnt/sfobjs.c). The two differ by half the underline
/// thickness in every TrueType font, and a caller drawing a rectangle wants to
/// know which edge it has. * Every value is in pixels at this face's current
/// size, positive upward, with the font's own sign conventions kept: a
/// descender is negative, and a strikeout offset is the distance ABOVE the
/// baseline. * Variations are applied. Moving an axis moves these.
///
/// The values are HarfBuzz's own tags, so a reader who knows
/// hb_ot_metrics_tag_t already knows this enum; ffi/ztypeset_abi.c asserts each
/// one equal to its HB_OT_METRICS_TAG_ counterpart, so the two cannot drift.
/// Every metric ztypeset names, written once.
///
/// This list expands three ways: into ZtypesetMetric below, into the check
/// ztypesetFaceMetric applies to the metric it is handed, and into the static
/// assertions in ffi/ztypeset_abi.c that tie each name to HarfBuzz's own
/// HB_OT_METRICS_TAG_ counterpart. A metric reaches all three or none of
/// them, so there is no second list to fall behind -- which is also why
/// ZtypesetAbiLayout reports metric_count from this list rather than a number
/// written down beside it.
#define ZTYPESET_METRIC_LIST(X)                                                \
  X(HORIZONTAL_ASCENDER, 'h', 'a', 's', 'c')                                  \
  X(HORIZONTAL_DESCENDER, 'h', 'd', 's', 'c')                                 \
  X(HORIZONTAL_LINE_GAP, 'h', 'l', 'g', 'p')                                  \
  X(HORIZONTAL_CLIPPING_ASCENT, 'h', 'c', 'l', 'a')                           \
  X(HORIZONTAL_CLIPPING_DESCENT, 'h', 'c', 'l', 'd')                          \
  X(VERTICAL_ASCENDER, 'v', 'a', 's', 'c')                                    \
  X(VERTICAL_DESCENDER, 'v', 'd', 's', 'c')                                   \
  X(VERTICAL_LINE_GAP, 'v', 'l', 'g', 'p')                                    \
  X(HORIZONTAL_CARET_RISE, 'h', 'c', 'r', 's')                                \
  X(HORIZONTAL_CARET_RUN, 'h', 'c', 'r', 'n')                                 \
  X(HORIZONTAL_CARET_OFFSET, 'h', 'c', 'o', 'f')                              \
  X(VERTICAL_CARET_RISE, 'v', 'c', 'r', 's')                                  \
  X(VERTICAL_CARET_RUN, 'v', 'c', 'r', 'n')                                   \
  X(VERTICAL_CARET_OFFSET, 'v', 'c', 'o', 'f')                                \
  X(X_HEIGHT, 'x', 'h', 'g', 't')                                             \
  X(CAP_HEIGHT, 'c', 'p', 'h', 't')                                           \
  X(SUBSCRIPT_EM_X_SIZE, 's', 'b', 'x', 's')                                  \
  X(SUBSCRIPT_EM_Y_SIZE, 's', 'b', 'y', 's')                                  \
  X(SUBSCRIPT_EM_X_OFFSET, 's', 'b', 'x', 'o')                                \
  X(SUBSCRIPT_EM_Y_OFFSET, 's', 'b', 'y', 'o')                                \
  X(SUPERSCRIPT_EM_X_SIZE, 's', 'p', 'x', 's')                                \
  X(SUPERSCRIPT_EM_Y_SIZE, 's', 'p', 'y', 's')                                \
  X(SUPERSCRIPT_EM_X_OFFSET, 's', 'p', 'x', 'o')                              \
  X(SUPERSCRIPT_EM_Y_OFFSET, 's', 'p', 'y', 'o')                              \
  X(STRIKEOUT_SIZE, 's', 't', 'r', 's')                                       \
  X(STRIKEOUT_OFFSET, 's', 't', 'r', 'o')                                     \
  X(UNDERLINE_SIZE, 'u', 'n', 'd', 's')                                       \
  X(UNDERLINE_OFFSET, 'u', 'n', 'd', 'o')

typedef enum ZtypesetMetric {
#define ZTYPESET_METRIC_ENUMERATOR(name, a, b, c, d) \
  ZTYPESET_METRIC_##name = ZTYPESET_TAG(a, b, c, d),
  ZTYPESET_METRIC_LIST(ZTYPESET_METRIC_ENUMERATOR)
#undef ZTYPESET_METRIC_ENUMERATOR
} ZtypesetMetric;

/// Reads one metric, and says whether the font declares it.
///
/// ZTYPESET_RESULT_UNSUPPORTED, with `*out` set to 0, when the font's tables do
/// not carry it -- which is the common case for x-height and cap-height in
/// older fonts, and for every vertical metric in a font with no `vhea`. That
/// is a real answer, not a failure: 0 on its own could not be told from a
/// font that declares a zero.
///
/// A `metric` this header does not name is ZTYPESET_RESULT_INVALID_ARGUMENT
/// rather than an unsupported metric, so a caller casting an integer in finds
/// out.
ZTYPESET_API ZtypesetResult ztypesetFaceMetric(const ZtypesetFace* face,
                                      ZtypesetMetric metric, float* out);

/// The same, with a value synthesised when the font declares none.
///
/// HarfBuzz's own fallbacks (hb_ot_metrics_get_position_with_fallback): an
/// absent x-height or cap-height is estimated from the ink of a
/// representative glyph, an absent strikeout or underline from the em, and so
/// on. Never ZTYPESET_RESULT_UNSUPPORTED -- there is always an answer, and the
/// price is that a caller cannot tell a designed value from an estimate. Use
/// ztypesetFaceMetric when that distinction matters, this when a number is
/// needed and any reasonable one will do.
ZTYPESET_API ZtypesetResult ztypesetFaceMetricWithFallback(const ZtypesetFace*
                                                           face,
                                                  ZtypesetMetric metric,
                                                  float* out);

//===----------------------------------------------------------------------===//
// Shaping
//===----------------------------------------------------------------------===//

/// Reusable shaping scratch: a HarfBuzz buffer and the glyph array converted
/// out of it.
///
/// A user interface shapes the same strings every frame, so the buffer is
/// owned by a handle the caller keeps rather than allocated per call. After
/// the first few calls a steady-state shape allocates nothing.
typedef struct ZtypesetShaper ZtypesetShaper;

typedef enum ZtypesetDirection {
  /// Let HarfBuzz infer it from the script. Prefer passing the direction from
  /// bidi analysis instead: inference is per-script and cannot know that this
  /// run sits inside an RTL paragraph.
  ZTYPESET_DIRECTION_AUTO = 0,
  ZTYPESET_DIRECTION_LTR = 1,
  ZTYPESET_DIRECTION_RTL = 2,
  ZTYPESET_DIRECTION_TTB = 3,
  ZTYPESET_DIRECTION_BTT = 4,
} ZtypesetDirection;

/// How finely clusters are allowed to be split. The default merges a base and
/// its combining marks into one cluster, which is what a caret and a selection
/// highlight want.
typedef enum ZtypesetClusterLevel {
  ZTYPESET_CLUSTER_LEVEL_MONOTONE_GRAPHEMES = 0,
  ZTYPESET_CLUSTER_LEVEL_MONOTONE_CHARACTERS = 1,
  ZTYPESET_CLUSTER_LEVEL_CHARACTERS = 2,
  /// Group by grapheme without forcing monotone order. Useful when you intend
  /// to reorder glyphs yourself and want the grouping without the constraint.
  ZTYPESET_CLUSTER_LEVEL_GRAPHEMES = 3,
} ZtypesetClusterLevel;

/// One OpenType feature setting. `start`/`end` are code-unit offsets into the
/// run the feature applies to; use 0 and ZTYPESET_FEATURE_GLOBAL for the whole
/// run.
typedef struct ZtypesetFeature {
  /// ZTYPESET_TAG('l','i','g','a') and friends.
  uint32_t tag;
  /// 0 disables, 1 enables, higher values select an alternate where the
  /// feature takes one.
  uint32_t value;
  uint32_t start;
  uint32_t end;
} ZtypesetFeature;

#define ZTYPESET_FEATURE_GLOBAL ((uint32_t)0xFFFFFFFFu)

typedef struct ZtypesetShapeParams {
  ZtypesetDirection direction;
  /// ISO 15924 script as a tag -- ZTYPESET_TAG('A','r','a','b'). 0 asks
  /// HarfBuzz to guess from the text.
  uint32_t script;
  /// BCP 47 language tag, NULL for none. Affects language-specific features
  /// such as Turkish dotless i.
  const char* language;
  const ZtypesetFeature* features;
  size_t feature_count;
  ZtypesetClusterLevel cluster_level;
  /// 0 (default) takes metrics from HarfBuzz's own OpenType table reader:
  /// advances scale linearly from design units, so layout does not shift when
  /// hinting changes, and the shaping font is immutable.
  ///
  /// 1 takes them from FreeType instead, computed from the same face that
  /// will rasterise, with hinting on -- so advances match a hinted raster at
  /// the cost of hinting-dependent layout. Pair it with ZTYPESET_HINTING_NORMAL
  /// when rendering; mixing hinted advances with an unhinted raster, or the
  /// reverse, is what makes text drift.
  ///
  /// The two sources are close but not identical: ztypeset's suite measures the
  /// gap over a 15-glyph run and asserts it stays under one pixel.
  int use_freetype_metrics;
} ZtypesetShapeParams;

/// What shaping learned about the text around one glyph, as a bit mask in
/// ZtypesetGlyph::flags.
///
/// Singular because each enumerator is ONE flag; the field holds any OR of
/// them. The values are HarfBuzz's own, and ztypeset_abi.c carries a static
/// assertion per flag tying each to its HB_GLYPH_FLAG_ counterpart -- so this
/// is a rename of upstream's contract, not a re-encoding of it, and a
/// re-vendor that renumbered a flag would fail the build rather than shift
/// every line break by one.
///
/// ztypeset always asks HarfBuzz to produce all three. Upstream leaves two of
/// them off by default because computing them costs something, but a flag
/// that is present on some builds and absent on others is worse than either
/// answer: a consumer cannot tell "not set" from "not computed", and the only
/// safe reading of that ambiguity is to assume the worst -- which throws away
/// the entire optimisation the flags exist for. The cost is measured in
/// README.md rather than assumed.
typedef enum ZtypesetGlyphFlag {
  /// Breaking the text at the start of this glyph's cluster may change the
  /// shaping of BOTH sides, so both would have to be re-shaped. Its ABSENCE
  /// is the useful half: a line broken only at unflagged clusters is
  /// identical to the same text shaped in one piece, so no re-shape is
  /// needed after line breaking.
  ZTYPESET_GLYPH_FLAG_UNSAFE_TO_BREAK = 0x00000001,
  /// Changing the text on one side of this glyph's cluster may change the
  /// shaping on the other. Absence alone does not make a concatenation safe:
  /// both pieces being joined have to be clear of it.
  ZTYPESET_GLYPH_FLAG_UNSAFE_TO_CONCAT = 0x00000002,
  /// U+0640 TATWEEL may be inserted before this cluster to elongate the line
  /// without disturbing shaping. Whether elongating THERE is typographically
  /// right is a decision this does not make.
  ZTYPESET_GLYPH_FLAG_SAFE_TO_INSERT_TATWEEL = 0x00000004,
  /// Every flag this version defines. A consumer that masks with this ignores
  /// bits a newer ztypeset may add, rather than mistaking one for another flag.
  ZTYPESET_GLYPH_FLAG_DEFINED = 0x00000007,
} ZtypesetGlyphFlag;

/// One positioned glyph. Advances and offsets are in pixels at the face's
/// current size, y-up.
typedef struct ZtypesetGlyph {
  uint32_t glyph_id;
  /// Code-unit offset into the text passed to ztypesetShaperShape, in that
  /// text's own encoding -- not a codepoint index. Several glyphs may share a
  /// cluster (one character decomposing) and several characters may share one
  /// (a ligature).
  uint32_t cluster;
  /// An OR of ZtypesetGlyphFlag, 0 when none of them applies. Always produced;
  /// see ZtypesetGlyphFlag for why "always" is part of the contract.
  uint32_t flags;
  float x_advance;
  float y_advance;
  float x_offset;
  float y_offset;
} ZtypesetGlyph;

ZTYPESET_API ZtypesetResult ztypesetShaperCreate(ZtypesetShaper** out);

/// Destroys the shaper and the buffers it grew. Call it exactly once.
ZTYPESET_API void ztypesetShaperDestroy(ZtypesetShaper* shaper);

/// Shapes one run of text with one face, one direction and one script.
///
/// This is a run shaper, not a paragraph shaper: it does not itemise. Split
/// text into runs with ztypesetParagraph* first, then call this once per run.
///
/// The text is validated in `encoding` before HarfBuzz sees it and rejected
/// with ZTYPESET_RESULT_INVALID_TEXT if it is malformed, rather than silently
/// substituting replacement characters.
///
/// Results replace whatever the shaper held before.
/// `text` is the whole paragraph and `[run_offset, run_offset + run_length)`
/// is the part to shape. Characters outside that range are NOT shaped, but
/// they ARE seen -- which is the difference between correct and nearly correct
/// at a run boundary.
///
/// This matters the moment a host splits a word, which
/// ztypesetFontCoveredPrefix invites it to do. Shaping the two halves of an
/// Arabic word separately gives the letter before the split a final form and
/// the letter after it an initial form, when both should be medial: five
/// letters of "marhaba" split in the middle come back with two wrong glyphs,
/// silently, in a run that otherwise looks fine. Handing over the surrounding
/// text costs nothing and removes the whole class.
///
/// To shape a standalone string, pass 0 and `length`.
///
/// The whole `text` is validated on every call, not just the run: it is
/// borrowed, and ztypeset has no way to know it is the same buffer it saw last
/// time. For runs that came out of a ZtypesetParagraph use
/// ztypesetShaperShapeRun, which walks nothing.
///
/// `length`, `run_offset`, `run_length` and every cluster value are in
/// `encoding`'s code units. Cluster values index `text` -- the whole buffer,
/// not the run -- so they index the same slice a ZtypesetShapingRun's offsets
/// do.
ZTYPESET_API ZtypesetResult ztypesetShaperShape(ZtypesetShaper* shaper,
                                                ZtypesetFace* face,
                                       const void* text, size_t length,
                                       ZtypesetEncoding encoding,
                                       size_t run_offset, size_t run_length,
                                       const ZtypesetShapeParams* params);

/// Number of glyphs from the last successful shape.
ZTYPESET_API size_t ztypesetShaperGlyphCount(const ZtypesetShaper* shaper);

/// Borrowed glyph array in visual order. Valid until the next shape on this
/// shaper, or its destruction.
///
/// NULL when nothing has been shaped -- and also when the last shape succeeded
/// but produced no glyphs, as shaping empty text does. Use
/// ztypesetShaperGlyphCount to tell those apart; it is the count that is
/// authoritative.
ZTYPESET_API const ZtypesetGlyph* ztypesetShaperGlyphs(const ZtypesetShaper*
                                                       shaper);

/// Direction actually used, which is what AUTO resolved to.
ZTYPESET_API ZtypesetDirection ztypesetShaperDirection(const ZtypesetShaper*
                                                       shaper);

typedef struct ZtypesetExtents {
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
} ZtypesetExtents;

/// Extents of the last successful shape.
///
/// `face` must be the face the run was shaped with, still at the size it was
/// shaped at. It is a parameter rather than something the shaper remembers on
/// your behalf, deliberately: a stored face pointer would dangle the moment
/// the face was destroyed, and the compiler would never mention it. Passing it
/// makes the dependency part of the call, and ztypeset checks it -- a different
/// face, or the same face resized since, is ZTYPESET_RESULT_INVALID_ARGUMENT
/// rather than a plausible mixture of one font's ink and another's advances.
///
/// Metrics come from whichever source the shape used, so extents and advances
/// cannot disagree about which font they describe.
///
/// Costs one glyph-extents query per glyph, so cache the result rather than
/// asking every frame.
ZTYPESET_API ZtypesetResult ztypesetShaperExtents(const ZtypesetShaper* shaper,
                                         ZtypesetFace* face,
    ZtypesetExtents* out);

//===----------------------------------------------------------------------===//
// Bidi and script itemisation
//
// The Unicode Bidirectional Algorithm decides the visual order of mixed
// left-to-right and right-to-left text. Shaping needs its output twice over:
// runs must be shaped in the direction the algorithm assigns, and mixed-script
// text must be split before shaping because HarfBuzz shapes one script at a
// time.
//
// ztypeset stops at runs. Line breaking, justification and where those runs end
// up on screen are a layout engine's job, not this package's.
//===----------------------------------------------------------------------===//

/// One analysed paragraph. Immutable once created, and independent of any
/// face, library or thread.
typedef struct ZtypesetParagraph ZtypesetParagraph;

typedef enum ZtypesetBaseDirection {
  /// Derive the base level from the first strong character, per rule P2/P3.
  ZTYPESET_BASE_DIRECTION_AUTO = 0,
  ZTYPESET_BASE_DIRECTION_LTR = 1,
  ZTYPESET_BASE_DIRECTION_RTL = 2,
} ZtypesetBaseDirection;

/// A maximal span of one embedding level, in VISUAL order: run 0 is leftmost
/// for an LTR base, rightmost for RTL. Offsets are code-unit offsets into the
/// paragraph text, in the paragraph's own encoding.
typedef struct ZtypesetVisualRun {
  uint32_t offset;
  uint32_t length;
  /// Even levels run left-to-right, odd right-to-left.
  uint8_t level;
} ZtypesetVisualRun;

/// A maximal span of one script, in LOGICAL order. Offsets are code-unit
/// offsets, in the paragraph's own encoding.
typedef struct ZtypesetScriptRun {
  uint32_t offset;
  uint32_t length;
  /// ISO 15924 as a tag, ready to hand to ZtypesetShapeParams::script.
  uint32_t script;
} ZtypesetScriptRun;

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
typedef struct ZtypesetShapingRun {
  uint32_t offset;
  uint32_t length;
  /// ISO 15924 as a tag, for ZtypesetShapeParams::script.
  uint32_t script;
  /// Even runs left-to-right, odd right-to-left.
  uint8_t level;
} ZtypesetShapingRun;

/// Which segmentation passes a paragraph runs. An OR of these, and a
/// parameter of ztypesetParagraphCreate.
///
/// Singular for the same reason as ZtypesetGlyphFlag: the parameter holds any
/// OR of these, not one of them.
///
/// Each pass costs one walk of libunibreak over the text and one byte per
/// code unit, kept for the paragraph's lifetime -- which for a long paragraph
/// is more memory than the text and the embedding levels together. A pass not
/// asked for is not run, allocates nothing, and its accessor answers NULL.
/// ZTYPESET_SEGMENTATION_ALL is what a caller that has not thought about it
/// should pass.
///
/// The choice is made here rather than on first access because a built
/// paragraph is immutable and readable from several threads; filling an array
/// in lazily would trade that away.
typedef enum ZtypesetSegmentation {
  ZTYPESET_SEGMENTATION_NONE = 0x00000000,
  /// UAX #14: where a line MAY break.
  ZTYPESET_SEGMENTATION_LINES = 0x00000001,
  /// UAX #29: where a grapheme cluster ends -- caret movement, backspace.
  ZTYPESET_SEGMENTATION_GRAPHEMES = 0x00000002,
  /// UAX #29: where a word ends -- double-click selection.
  ZTYPESET_SEGMENTATION_WORDS = 0x00000004,
  /// The OR of every pass above.
  ZTYPESET_SEGMENTATION_ALL = 0x00000007,
} ZtypesetSegmentation;

/// Analyses one paragraph of text.
///
/// `length` is in `encoding`'s code units, and so is every offset the
/// paragraph reports afterwards.
///
/// `segmentation` is an OR of ZtypesetSegmentation saying which break arrays to
/// build. A bit this build has no name for is ZTYPESET_RESULT_INVALID_ARGUMENT.
///
/// `text` is read during the call only; the paragraph copies what it needs and
/// does not borrow the buffer. That differs from ztypesetFontCreateFromMemory
/// on purpose -- a paragraph is small and copying it removes a lifetime the
/// caller would otherwise have to track.
///
/// Rejects malformed text with ZTYPESET_RESULT_INVALID_TEXT. Text containing a
/// paragraph separator is analysed as a single paragraph up to the first one;
/// split beforehand if that is not what you want.
ZTYPESET_API ZtypesetResult ztypesetParagraphCreate(const void* text,
                                                    size_t length,
                                           ZtypesetEncoding encoding,
                                           ZtypesetBaseDirection base,
                                           uint32_t segmentation,
                                           ZtypesetParagraph** out);

/// Destroys the paragraph, the copy of the text it took and the arrays it
/// built. Call it exactly once. Lines made from it may outlive it and are
/// destroyed separately.
ZTYPESET_API void ztypesetParagraphDestroy(ZtypesetParagraph* paragraph);

/// Length actually analysed, in the paragraph's own code units, which is at
/// most the `length` passed in -- less if the text contained a paragraph
/// separator.
ZTYPESET_API size_t ztypesetParagraphLength(const ZtypesetParagraph* paragraph);

/// The encoding this paragraph was created with, which is the unit every
/// offset and length it reports is counted in.
///
/// Reported rather than remembered by the caller: a run list outlives the call
/// that made it and is routinely passed on alone, and reading a UTF-16
/// paragraph's offsets as bytes indexes half a character.
ZTYPESET_API ZtypesetEncoding ztypesetParagraphEncoding(
    const ZtypesetParagraph* paragraph);

/// The segmentation passes this paragraph ran, as they were asked for.
///
/// Reported for the same reason as the encoding: a paragraph outlives the
/// call that made it, and an empty break array is otherwise indistinguishable
/// from a pass that was never run.
ZTYPESET_API uint32_t ztypesetParagraphSegmentation(const ZtypesetParagraph*
                                                    paragraph);

/// Resolved base embedding level: even for LTR, odd for RTL.
ZTYPESET_API uint8_t ztypesetParagraphBaseLevel(const ZtypesetParagraph*
                                                paragraph);

/// Borrowed per-code-unit embedding levels, one entry per unit of
/// ztypesetParagraphLength. Every unit of a multi-unit character carries the
/// same level as its first.
///
/// These are the levels UAX #9 resolves over the PARAGRAPH, before rule L1
/// resets trailing whitespace for a particular line. Where the two differ, a
/// line's own visual runs are the authority -- see ztypesetLineCreate.
///
/// Valid until the paragraph is destroyed.
ZTYPESET_API const uint8_t* ztypesetParagraphLevels(
    const ZtypesetParagraph* paragraph);

//===----------------------------------------------------------------------===//
// Segmentation
//
// Where a line may break, where a grapheme cluster ends, where a word ends.
//
// These are here for the same reason bidi is: they are pure functions of the
// text and the Unicode character database, not decisions about layout. ztypeset
// already owns UAX #9; owning that and not UAX #14 and #29 would be an
// arbitrary line, and it would leave ztypesetLineCreate -- which takes a unit
// range -- with no way for a caller to find one.
//
// The division of labour is the same as everywhere else here. ztypeset says
// where a break is PERMITTED; the host decides where one HAPPENS, because that
// needs a width, and a width is not a property of text. Concretely: walk the
// allowed positions, measure with ztypesetShaperExtents until the next one
// would not fit, and hand the range you chose to ztypesetLineCreate.
//
// From libunibreak, which allocates nothing and keeps no global state -- so
// unlike the other three upstreams there is no allocator seam and no
// initialisation to get wrong.
//===----------------------------------------------------------------------===//

/// The three values a break entry can hold.
///
/// Macros rather than an enum, and that is deliberate: these live in arrays of
/// one byte per code unit, and C does not let you fix an enum's width. An
/// int-sized enum beside a uint8_t array would be a type that cannot hold its
/// own values -- the kind of near-miss this package spends its guards on.
///
/// No boundary here. Also what a code unit inside a multi-unit character
/// reports, since a break there is never a break.
#define ZTYPESET_BREAK_NONE 0u
/// A boundary is permitted here.
#define ZTYPESET_BREAK_ALLOWED 1u
/// A boundary is REQUIRED here. Line breaks only -- a paragraph's last code
/// unit is always mandatory, and so is a U+2028 LINE SEPARATOR within it.
#define ZTYPESET_BREAK_MANDATORY 2u

/// Borrowed, one ZTYPESET_BREAK_* value per code unit of
/// ztypesetParagraphLength, describing the boundary AFTER that unit. NULL for
/// an empty paragraph, and NULL if ZTYPESET_SEGMENTATION_LINES was not asked
/// for.
///
/// So a line may run from `start` to `i + 1` whenever `line_breaks[i]` is not
/// ZTYPESET_BREAK_NONE. Valid until the paragraph is destroyed.
///
/// Language tailoring is not exposed: these are the untailored rules. That
/// matters mainly for strict Japanese and Korean line breaking, and adding it
/// later is a parameter rather than a redesign.
ZTYPESET_API const uint8_t* ztypesetParagraphLineBreaks(
    const ZtypesetParagraph* paragraph);

/// Grapheme cluster boundaries -- what a caret moves by and what backspace
/// deletes. Never ZTYPESET_BREAK_MANDATORY. NULL unless
/// ZTYPESET_SEGMENTATION_GRAPHEMES was asked for.
///
/// This is emphatically not the same as a character: a base plus its combining
/// marks is one grapheme, and so is a regional-indicator pair or an emoji
/// joined with U+200D. Moving a caret by character puts it inside one.
ZTYPESET_API const uint8_t* ztypesetParagraphGraphemeBreaks(
    const ZtypesetParagraph* paragraph);

/// Word boundaries -- double-click selection, and word-wise caret movement.
/// Never ZTYPESET_BREAK_MANDATORY. NULL unless ZTYPESET_SEGMENTATION_WORDS was
/// asked for.
ZTYPESET_API const uint8_t* ztypesetParagraphWordBreaks(
    const ZtypesetParagraph* paragraph);

/// The next and previous grapheme boundary from `offset`, for moving a caret.
///
/// `ztypesetParagraphNextGrapheme(p, length)` is `length`, and
/// `ztypesetParagraphPreviousGrapheme(p, 0)` is 0, so a caret walked off either
/// end stays put rather than wrapping or going out of range. An `offset` that
/// is not itself a boundary is snapped outward to one. Without
/// ZTYPESET_SEGMENTATION_GRAPHEMES there are no boundaries at all, and both
/// return `offset` unchanged.
///
/// Written as functions rather than left to the caller because the loop is
/// three lines and everyone writes it slightly differently -- usually by
/// stepping a character at a time, which is the bug these exist to prevent.
ZTYPESET_API size_t ztypesetParagraphNextGrapheme(const ZtypesetParagraph*
                                                  paragraph,
                                            size_t offset);
ZTYPESET_API size_t ztypesetParagraphPreviousGrapheme(const ZtypesetParagraph*
                                                      paragraph,
                                                size_t offset);

/// The paragraph laid out as ONE line.
///
/// Correct whenever the text fits on one, which is most labels, most buttons
/// and every single-line field -- and wrong the moment it wraps. Rules L1 and
/// L2 of UAX #9 are defined over a line, not a paragraph, so where the text
/// breaks changes the answer. Use ztypesetLineCreate for anything that wraps;
/// see the note there for what actually differs.
ZTYPESET_API size_t ztypesetParagraphVisualRunCount(const ZtypesetParagraph*
                                                    paragraph);
ZTYPESET_API const ZtypesetVisualRun* ztypesetParagraphVisualRuns(
    const ZtypesetParagraph* paragraph);

/// Script runs are a property of the text, so unlike the runs above they do
/// not change when it wraps. A line reuses these.
ZTYPESET_API size_t ztypesetParagraphScriptRunCount(const ZtypesetParagraph*
                                                    paragraph);
ZTYPESET_API const ZtypesetScriptRun* ztypesetParagraphScriptRuns(
    const ZtypesetParagraph* paragraph);

/// Visual runs intersected with script runs: what to actually shape, for the
/// paragraph laid out as one line. Same caveat as ztypesetParagraphVisualRuns.
ZTYPESET_API size_t ztypesetParagraphShapingRunCount(const ZtypesetParagraph*
                                                     paragraph);
ZTYPESET_API const ZtypesetShapingRun* ztypesetParagraphShapingRuns(
    const ZtypesetParagraph* paragraph);

/// Shapes one run of a paragraph, with the paragraph as its own context.
///
/// Declared here rather than beside ztypesetShaperShape because it needs both
/// halves: it is the call that joins a paragraph's itemisation to the shaper.
///
/// Prefer it over ztypesetShaperShape for anything a ZtypesetParagraph or a
/// ZtypesetLine produced, for three reasons, in the order they will bite:
///
///  1. The text cannot be the wrong text. `run`'s offsets are the paragraph's
///     own, and the paragraph is where the text comes from -- so the classic
///     failure of passing a SLICE together with offsets computed against the
///     whole cannot be expressed here.
///  2. `params->direction` and `params->script` must be AUTO and 0: the run
///     carries both, and a second source for one fact means a silent loser.
///     A run's level decides LTR against RTL. For vertical text, which no
///     run list describes, call ztypesetShaperShape.
///  3. The text is NOT revalidated. ztypesetParagraphCreate validated it and
///     copied it, so it cannot have changed since -- while ztypesetShaperShape
///     borrows a buffer it has never seen and must walk all of it, on every
///     call. Iterating an N-unit paragraph's R runs through ztypesetShaperShape
///     therefore costs R walks of N; this costs none. Measured in README.md.
///
/// A ZtypesetLine's shaping runs index the same paragraph text, so they are
/// passed here with the paragraph they came from.
///
/// Everything else -- features, cluster level, language, the FreeType-metrics
/// switch -- still comes from `params`.
ZTYPESET_API ZtypesetResult ztypesetShaperShapeRun(ZtypesetShaper* shaper,
                                                   ZtypesetFace* face,
                                          const ZtypesetParagraph* paragraph,
                                          const ZtypesetShapingRun* run,
                                          const ZtypesetShapeParams* params);

//===----------------------------------------------------------------------===//
// Lines
//
// One code-unit range of a paragraph, reordered as its own line.
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
// ztypeset does not decide where the breaks go: see the README on UAX #14. It
// takes the ranges a host has already chosen and reorders each correctly.
//===----------------------------------------------------------------------===//

typedef struct ZtypesetLine ZtypesetLine;

/// Reorders `paragraph`'s units `[offset, offset + length)` as one line.
///
/// Offsets are code-unit offsets into the paragraph, in the paragraph's own
/// encoding, and the runs come back with paragraph-relative offsets too -- not
/// line-relative -- so they index the same buffer the caller already has.
///
/// A zero-length line is legal and has no runs. A range that ends past
/// ztypesetParagraphLength, or that starts or ends in the middle of a
/// character, is ZTYPESET_RESULT_INVALID_ARGUMENT rather than a silent
/// half-character.
///
/// The line copies what it needs, so it holds no reference to the paragraph
/// and may outlive it.
ZTYPESET_API ZtypesetResult ztypesetLineCreate(const ZtypesetParagraph*
                                               paragraph,
                                      size_t offset, size_t length,
                                      ZtypesetLine** out);

/// Destroys the line. Call it exactly once.
ZTYPESET_API void ztypesetLineDestroy(ZtypesetLine* line);

ZTYPESET_API size_t ztypesetLineOffset(const ZtypesetLine* line);
ZTYPESET_API size_t ztypesetLineLength(const ZtypesetLine* line);

/// Runs of one embedding level, in visual order, with L1 and L2 applied over
/// this line's range.
ZTYPESET_API size_t ztypesetLineVisualRunCount(const ZtypesetLine* line);
ZTYPESET_API const ZtypesetVisualRun* ztypesetLineVisualRuns(const
                                                             ZtypesetLine*
                                                             line);

/// This line's visual runs intersected with the paragraph's script runs: what
/// to actually shape.
ZTYPESET_API size_t ztypesetLineShapingRunCount(const ZtypesetLine* line);
ZTYPESET_API const ZtypesetShapingRun* ztypesetLineShapingRuns(const
                                                               ZtypesetLine*
                                                               line);

//===----------------------------------------------------------------------===//
// Rasterisation
//===----------------------------------------------------------------------===//

typedef enum ZtypesetRenderMode {
  /// 8-bit coverage, one byte per pixel, 0 = uncovered.
  ZTYPESET_RENDER_MODE_A8 = 0,
  /// FreeType's native signed distance field, 8-bit, 128 at the outline and
  /// rising inside. See README for what it costs and what it is good for --
  /// measured, not assumed.
  ZTYPESET_RENDER_MODE_SDF = 1,
  /// Subpixel coverage for an LCD whose stripes run horizontally: three bytes
  /// per pixel, one per stripe, in the panel's own left-to-right order.
  ///
  /// This build renders it in FreeType's HARMONY mode -- three coverage
  /// samples a third of a pixel apart rather than one sample through a filter
  /// -- because FT_CONFIG_OPTION_SUBPIXEL_RENDERING is off, which is
  /// upstream's default. Harmony needs no filter to be chosen and produces no
  /// colour fringing of its own; the alternative is a COMPILE-time FreeType
  /// option and not a runtime one, so it is not a choice ztypeset could offer
  /// both of. See ffi/ztypeset_ftoption.h.
  ///
  /// Which stripe order a panel has is the consumer's to know: ztypeset hands
  /// back the three samples in geometric order and does not reverse them for
  /// a BGR panel, because it cannot know.
  ZTYPESET_RENDER_MODE_LCD = 2,
  /// The same, for a panel whose stripes run vertically.
  ZTYPESET_RENDER_MODE_LCD_V = 3,
} ZtypesetRenderMode;

typedef enum ZtypesetHinting {
  /// The face's own hinting where it has any, FreeType's autohinter where it
  /// does not.
  ZTYPESET_HINTING_NORMAL = 0,
  /// Vertical hinting only -- crisp baselines, horizontal metrics left alone.
  /// The usual choice when advances come from unhinted shaping.
  ZTYPESET_HINTING_LIGHT = 1,
  /// No hinting. Required for SDF, which wants unhinted outlines.
  ZTYPESET_HINTING_NONE = 2,
} ZtypesetHinting;

/// A 2x2 linear map applied to every glyph this face draws.
///
/// x' = xx*x + xy*y and y' = yx*x + yy*y, with y UP, which is FreeType's
/// convention and the one every coordinate ztypeset reports uses. The identity
/// is { 1, 0, 0, 1 }, and a face is created with it.
///
/// There is deliberately no translation here. A glyph is shifted by
/// ztypesetFaceRenderGlyph's offset_x and offset_y, which is where sub-pixel
/// positioning already lives; a second way to say the same thing is a second
/// place for it to be said differently.
typedef struct ZtypesetMatrix {
  float xx;
  float xy;
  float yx;
  float yy;
} ZtypesetMatrix;

/// Sets this face's transform, or clears it when `matrix` is NULL. Reading it
/// back gives the identity for a face that has none.
///
/// This is FreeType's FT_Set_Transform, applied where ztypeset can compose it
/// correctly: AFTER any synthetic bold or oblique, so emboldening stays
/// isotropic in the font's own space instead of being stretched by whatever
/// the caller is mapping into. Hinting still happens in the untransformed
/// space, as it does upstream, because the outline is loaded and hinted before
/// this runs.
///
/// It reaches the glyph IMAGE and nothing else, and that is a decision rather
/// than an omission. ztypesetFaceGlyphExtents, ztypesetFaceRenderGlyph and
/// ztypesetFaceDecomposeOutline agree on one transformed glyph; every
/// ADVANCE -- this face's, and every advance a shaped run reports -- stays
/// in the text's
/// own space. FreeType's FT_Set_Transform does transform the advance, but a
/// shaped run's advances come from HarfBuzz, which has no matrix to be told
/// about, so transforming one and not the other would make the two disagree
/// by exactly the caller's matrix. The model this leaves is the one every
/// shaping stack uses: lay the run out in text space, then map the whole run
/// -- pen positions and glyph images together -- with the same matrix.
///
/// Nothing is validated beyond being finite. A singular matrix produces an
/// empty glyph, which is what a collapsed transform means, and a mirrored one
/// is a legitimate thing to ask for.
ZTYPESET_API ZtypesetResult ztypesetFaceSetTransform(ZtypesetFace* face,
                                            const ZtypesetMatrix* matrix);
ZTYPESET_API ZtypesetResult ztypesetFaceTransform(const ZtypesetFace* face,
                                         ZtypesetMatrix* out);

/// FreeType's own reference emboldening: 0x0AAA/65536 of the em, which is
/// what FT_GlyphSlot_Embolden applies. HarfBuzz documents 0.01 to 0.05 as the
/// useful range for the same quantity, and this sits inside it.
#define ZTYPESET_SYNTHETIC_BOLD_DEFAULT 0.041656494f
/// FreeType's own reference slant: 0x0366A/65536, a shear of about 12
/// degrees, which is what FT_GlyphSlot_Oblique applies.
#define ZTYPESET_SYNTHETIC_OBLIQUE_DEFAULT 0.212554932f

/// Fakes a bold or an italic on a face that has neither of its own, the way a
/// production stack does.
///
/// `strength` is a fraction of the EM, not a pixel count, so it holds across
/// sizes: 0 is off, ZTYPESET_SYNTHETIC_BOLD_DEFAULT is what FreeType and
/// HarfBuzz both use, and a negative value thins instead of thickens. `slant`
/// is a shear factor -- the tangent of the angle -- with
/// ZTYPESET_SYNTHETIC_OBLIQUE_DEFAULT the reference italic. Neither is clamped:
/// a display face at twice the reference weight is a legitimate thing to ask
/// for, and only the caller knows what its text is for.
///
/// Both reach every reader of this face. FreeType applies them at glyph
/// LOADING, so ztypesetFaceGlyphExtents, ztypesetFaceRenderGlyph and
/// ztypesetFaceDecomposeOutline agree on one widened, sheared glyph; and
/// HarfBuzz is told the same two numbers, so a SHAPED run's advances widen
/// by the same fraction of the em rather than staying at the unstyled font's
/// widths. A shaped run laid out with unwidened advances overlaps its own
/// ink, one glyph at a time, and gets worse the bolder it is.
///
/// Emboldening widens the glyph and its advance together; a shear leaves the
/// advance alone, because slanting does not change how far the pen moves.
///
/// Applies from the next call on: already-rendered bitmaps and already-taken
/// extents are unaffected, and the face's generation moves, so extents taken
/// for a run shaped before the change are refused rather than mixed.
///
/// A strength that is not a finite number is ZTYPESET_RESULT_INVALID_ARGUMENT.
ZTYPESET_API ZtypesetResult ztypesetFaceSetSyntheticBold(ZtypesetFace* face,
                                                float strength);
ZTYPESET_API ZtypesetResult ztypesetFaceSetSyntheticOblique(ZtypesetFace* face,
                                                   float slant);

/// How the two ends of an open path are finished. FreeType's
/// FT_Stroker_LineCap, restated so a consumer switches on ztypeset's own enum.
///
/// A glyph contour is closed, so this is only reached where a contour is
/// left open -- which FreeType's stroker does at a path it cannot close.
typedef enum ZtypesetLineCap {
  /// Stop dead at the end point.
  ZTYPESET_LINE_CAP_BUTT = 0,
  /// A half-disc of the pen's radius.
  ZTYPESET_LINE_CAP_ROUND = 1,
  /// A half-square of the pen's radius.
  ZTYPESET_LINE_CAP_SQUARE = 2,
} ZtypesetLineCap;

/// How two segments meet at a corner. FreeType's FT_Stroker_LineJoin.
typedef enum ZtypesetLineJoin {
  /// An arc of the pen's radius. Never spikes, at any angle.
  ZTYPESET_LINE_JOIN_ROUND = 0,
  /// Cut straight across the corner.
  ZTYPESET_LINE_JOIN_BEVEL = 1,
  /// A miter that falls back to a bevel past `miter_limit` -- the join XPS
  /// and PostScript specify, and FreeType's FT_STROKER_LINEJOIN_MITER.
  ZTYPESET_LINE_JOIN_MITER = 2,
  /// A miter TRIMMED at `miter_limit` rather than dropped, which is what SVG
  /// and PDF specify. FreeType's FT_STROKER_LINEJOIN_MITER_FIXED.
  ZTYPESET_LINE_JOIN_MITER_FIXED = 3,
} ZtypesetLineJoin;

/// Which of the three shapes a pen traced round a glyph is kept.
///
/// The stroker walks each contour and produces two of them, the contour
/// pushed OUT by the radius and the contour pushed IN by it, wound against
/// each other. Keeping both gives the band between them; keeping either one
/// alone gives a solid shape. All three are named for what they ARE rather
/// than for which of FreeType's borders they came from, because what a caller
/// chooses between is three pictures.
///
/// The measurements quoted below are Noto Sans `H` at 128 px with a radius
/// of 4, whose stems are several times the pen.
typedef enum ZtypesetStrokeStyle {
  /// The BAND the pen sweeps along the glyph's contour: 2R wide, centred on
  /// the outline, hollow in the middle. A genuinely outlined letter, in one
  /// pass. FreeType's FT_Glyph_Stroke.
  ///
  /// Measured: the ink box is the glyph's own grown by the radius on every
  /// side, and it inks 4255 pixels against the grown shape's 4762 -- the
  /// difference being the hole.
  ///
  /// The hole closes when a stem is thinner than the pen, because the inward
  /// contour then turns itself inside out. At 32 px with the same radius --
  /// stems of about 3 px against a pen of 4 -- this band and
  /// ZTYPESET_STROKE_STYLE_GROWN measure identical, to the pixel.
  ZTYPESET_STROKE_STYLE_BAND = 0,
  /// The glyph GROWN by the radius, solid: the outward contour and everything
  /// inside it. FT_Glyph_StrokeBorder's outside border.
  ///
  /// This is the bottom layer of the two-pass outlined text every game UI
  /// draws -- render it in the outline's colour, then the unstroked glyph on
  /// top -- and it is what `ztypeset.outline(radius)` selects.
  ///
  /// Measured: ink box [-0.891, 24.578] against the glyph's [3.109, 20.578]
  /// at 32 px, exactly the radius on each side.
  ZTYPESET_STROKE_STYLE_GROWN = 1,
  /// The glyph SHRUNK by the radius, solid: the inward contour and everything
  /// inside it. FT_Glyph_StrokeBorder's inside border.
  ///
  /// Measured: the ink box comes in by exactly the radius on each side, and
  /// it inks less than the unstroked glyph. A stem thinner than the pen is a
  /// stem this contour cannot stay inside of, and FreeType does not clip it
  /// -- it self-intersects, and the box comes out WIDER than the glyph rather
  /// than narrower. That is the shape upstream produces, reported as it is.
  ZTYPESET_STROKE_STYLE_SHRUNK = 2,
} ZtypesetStrokeStyle;

/// A pen traced around every glyph this face draws.
typedef struct ZtypesetStroke {
  /// HALF the pen's width, in PIXELS at this face's current size. 0 or less
  /// turns stroking off, and is what a face is created with.
  ///
  /// Pixels, where ztypesetFaceSetSyntheticBold takes a fraction of the em, and
  /// the difference is the point rather than an inconsistency: synthetic bold
  /// fakes a WEIGHT, which is a property of the design and has to hold across
  /// sizes, while a stroke is an ornament drawn for a display -- a one-pixel
  /// outline is legible at 12px and at 72px, and an em fraction would make it
  /// invisible at one and a slab at the other. It is also FreeType's unit for
  /// FT_Stroker_Set. A caller who does want it to scale multiplies by the
  /// ppem it asked for.
  float radius;

  /// How far a miter join may run past the corner before
  /// ZTYPESET_LINE_JOIN_MITER gives up and bevels, or
  /// ZTYPESET_LINE_JOIN_MITER_FIXED trims -- as a multiple of `radius`. Ignored
  /// by the other two joins. 0 or less means FreeType's own default of 4, which
  /// is also SVG's and PostScript's.
  float miter_limit;

  ZtypesetLineCap cap;
  ZtypesetLineJoin join;
  ZtypesetStrokeStyle style;
} ZtypesetStroke;

/// Sets this face's stroke, or clears it when `stroke` is NULL or its radius
/// is 0. Reading it back gives a zero radius for a face that has none.
///
/// This is FreeType's stroker (FT_Stroker_ParseOutline and its exports) run
/// at glyph LOADING, so ztypesetFaceGlyphExtents, ztypesetFaceRenderGlyph and
/// ztypesetFaceDecomposeOutline all agree on one stroked glyph -- the outline
/// that is measured is the outline that is drawn. Composition is fixed and
/// stated once: synthetic bold and oblique first, because they are part of
/// the font's design; then the pen; then the caller's matrix, which maps the
/// finished shape. A pen applied before emboldening would be widened by the
/// emboldening, and one applied after the matrix would be a pen in device
/// space -- both are legitimate effects, and neither is what "outline this
/// text" means.
///
/// No ADVANCE moves, and no advance should: a stroked glyph is wider than its
/// ink box by the radius on each side, and the run it belongs to is still
/// laid out on the font's own advances. That is FreeType's behaviour for
/// FT_Glyph_Stroke too. It also means a face's stroke does not change what a
/// SHAPED run reports, so setting one does not stale an existing measurement.
///
/// Hinting happens before this, on the unstroked outline, as it does for the
/// matrix. A radius that is not a finite number, or a `style`, `cap` or
/// `join` this build does not name, is ZTYPESET_RESULT_INVALID_ARGUMENT.
ZTYPESET_API ZtypesetResult ztypesetFaceSetStroke(ZtypesetFace* face,
                                         const ZtypesetStroke* stroke);
ZTYPESET_API ZtypesetResult ztypesetFaceStroke(const ZtypesetFace* face,
                                               ZtypesetStroke* out);

/// Callbacks for ztypesetFaceDecomposeOutline, one per outline command. Points
/// are in 26.6 fixed point, at this face's current size. Modelled on
/// ZtypesetAllocator: `user` is passed back unmodified, and a callback other
/// than ZTYPESET_RESULT_OK aborts decomposition and becomes the result
/// ztypesetFaceDecomposeOutline returns.
typedef struct ZtypesetOutlineFuncs {
  ZtypesetResult (*move_to)(void* user, int32_t x, int32_t y);
  ZtypesetResult (*line_to)(void* user, int32_t x, int32_t y);
  ZtypesetResult (*conic_to)(void* user, int32_t control_x, int32_t control_y,
                          int32_t x, int32_t y);
  ZtypesetResult (*cubic_to)(void* user, int32_t control1_x, int32_t control1_y,
                          int32_t control2_x, int32_t control2_y, int32_t x,
                          int32_t y);
  /// Emitted once a contour is complete, before the next move_to and after
  /// the last -- FT_Outline_Decompose itself has no such event, only an
  /// implicit line/conic/cubic back to the contour's start.
  ZtypesetResult (*close)(void* user);
  void* user;
} ZtypesetOutlineFuncs;

/// How to read ZtypesetGlyphBitmap::pixels.
///
/// The format travels WITH the pixels rather than being remembered by the
/// caller from the ZtypesetRenderMode it passed. A8 coverage and an SDF are
/// both one byte per pixel, so a field sampled as coverage does not fail -- it
/// produces a picture, a washed-out wrong one, which is the failure mode this
/// package exists to refuse. Two independent enums rather than one shared with
/// ZtypesetRenderMode, because what was asked for and what came back are
/// different facts: a mode may one day be satisfied by more than one format, or
/// by falling back to another.
///
/// Forward compatibility: switch on this and REJECT a value this header does
/// not name. New formats will be added. `pitch` is BYTES per row and
/// `pitch * height` is the buffer size in every format, present or future, so
/// a consumer that only copies pixels into an atlas never has to understand
/// them.
typedef enum ZtypesetBitmapFormat {
  /// One byte per pixel: coverage, 0 for no ink and 255 for solid.
  ZTYPESET_BITMAP_FORMAT_A8 = 0,
  /// One byte per pixel: distance to the outline, biased so 128 is ON the
  /// outline and larger values are inside. The ramp's half-width in pixels is
  /// the library's SDF spread -- see ztypesetLibrarySetSdfSpread.
  ZTYPESET_BITMAP_FORMAT_SDF = 1,
  /// Three bytes per pixel, side by side: the pixel at (x, y) is the three
  /// bytes at `pixels[y * pitch + 3 * x]`.
  ZTYPESET_BITMAP_FORMAT_LCD = 2,
  /// Three bytes per pixel, one above the other: the pixel at (x, y) is
  /// `pixels[y * pitch + k * width + x]` for k of 0, 1, 2. That is FreeType's
  /// own layout -- three sub-rows per pixel row -- restated rather than
  /// repacked, so no consumer pays for a shuffle it may not want.
  ZTYPESET_BITMAP_FORMAT_LCD_V = 3,
} ZtypesetBitmapFormat;

/// Bytes per pixel in `format`: 1 for A8 and SDF, 3 for both LCD formats.
///
/// A function rather than a table in this comment, because a consumer that
/// switches on the format has a default branch this build's newest value would
/// fall through. 0 for a value this build does not name.
ZTYPESET_API uint32_t ztypesetBitmapFormatChannels(ZtypesetBitmapFormat format);

typedef struct ZtypesetGlyphBitmap {
  /// Owned by the FACE, and valid until the next ztypesetFaceRenderGlyph on it.
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
  /// How to read `pixels`. Written on every successful render, INCLUDING a
  /// glyph with no ink -- so it is meaningful before the NULL check, and a
  /// consumer that batches renders never sees an unset one.
  ///
  /// First after the pointer on purpose: it has to be read before the pixels
  /// it describes are interpreted.
  ZtypesetBitmapFormat format;
  /// The glyph's size in PIXELS, in every format -- not in bytes and not in
  /// FreeType's rows, both of which are three times this for one of the LCD
  /// formats. `left` and `top` are in pixels too, so the three agree.
  uint32_t width;
  uint32_t height;
  /// Bytes per PIXEL ROW, so `pitch * height` is the buffer's size in every
  /// format. Always positive and always tightly packed, because the copy
  /// above normalises FreeType's bottom-up bitmaps on the way out -- so a
  /// consumer cannot render upside down by ignoring a sign.
  int32_t pitch;
  /// Pen-relative position of the bitmap's top-left corner, in pixels, y-up.
  int32_t left;
  int32_t top;
  /// The glyph's own advance at this size, in pixels. For laying out a single
  /// glyph; a shaped run's advances come from shaping and may differ.
  float x_advance;
} ZtypesetGlyphBitmap;

/// Loads and rasterises one glyph by index.
///
/// SDF mode forces unhinted loading regardless of `hinting`, because a hinted
/// outline produces a distance field that does not match the shape at other
/// sizes -- which is the only reason to want one.
///
/// The two LCD modes hint against the subpixel grid they are about to be
/// sampled on (FT_LOAD_TARGET_LCD and FT_LOAD_TARGET_LCD_V) when `hinting` is
/// normal. Light hinting is its own target, unrelated to the render mode, and
/// is honoured as asked in every mode.
///
/// `offset_x`/`offset_y` place the glyph at a fractional pixel offset, in
/// 26.6 fixed point -- the unit FreeType uses and shaping advances already
/// come back in -- so a host laying out text at fractional x is not forced to
/// snap every glyph to the pixel grid. 0, 0 renders identically to a build
/// without this parameter. Ignored in SDF mode: the field is meant to be
/// sampled at any sub-pixel position later, so baking one in here would be
/// wasted, unrecoverable work -- apply the offset where the field is sampled
/// instead, the same way scale and rotation already are.
ZTYPESET_API ZtypesetResult ztypesetFaceRenderGlyph(ZtypesetFace* face,
                                                    uint32_t glyph_id,
                                           ZtypesetRenderMode mode,
                                           ZtypesetHinting hinting,
                                           int32_t offset_x, int32_t offset_y,
                                           ZtypesetGlyphBitmap* out);

/// Metrics for one glyph without rasterising it.
///
/// It loads the glyph, which invalidates any ZtypesetGlyphBitmap previously
/// returned for this face -- so size an atlas entry with this BEFORE
/// rasterising, not while holding pixels you still intend to read.
ZTYPESET_API ZtypesetResult ztypesetFaceGlyphExtents(ZtypesetFace* face,
                                                     uint32_t glyph_id,
                                            ZtypesetHinting hinting,
                                            ZtypesetExtents* out);

/// Walks one glyph's outline through `funcs`, for a host that fills its own
/// shapes -- an offline SDF baker, a path-effect renderer -- rather than
/// sampling a bitmap. A wrapper over FT_Outline_Decompose; see
/// ZtypesetOutlineFuncs for the callback shape.
///
/// Subject to this face's synthetic bold and oblique settings, the same as
/// ztypesetFaceRenderGlyph and ztypesetFaceGlyphExtents.
ZTYPESET_API ZtypesetResult ztypesetFaceDecomposeOutline(ZtypesetFace* face,
                                                uint32_t glyph_id,
                                                ZtypesetHinting hinting,
                                                const ZtypesetOutlineFuncs*
    funcs);

/// Half-width of the distance field ramp, in pixels, for
/// ZTYPESET_RENDER_MODE_SDF.
///
/// Accepts 2..32, which is the range FreeType supports; anything else is
/// ZTYPESET_RESULT_INVALID_ARGUMENT rather than a silent clamp. The default is
/// 8. Applies to every face made from this library.
ZTYPESET_API ZtypesetResult ztypesetLibrarySetSdfSpread(ZtypesetLibrary*
                                                        library,
                                               uint32_t spread);

/// Number of faces inside a font image, which is 1 for a plain TTF or OTF and
/// more for a TrueType collection (.ttc).
///
/// Call it before ztypesetFontCreateFromMemory if you intend to iterate a
/// collection; the index that function takes must be below this.
ZTYPESET_API ZtypesetResult ztypesetLibraryCountFaces(ZtypesetLibrary* library,
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
// ztypeset's own Zig wrapper additionally compares its externs against THIS
// HEADER at comptime (src/abi_check.zig), by reflection over every public
// declaration, which is a stronger check on the axis it covers: it sees
// function arity and per-parameter sizes, and every enumerator by name rather
// than only the last. What it cannot see is a header preprocessed differently
// from the library it is linked against -- and that is exactly what
// ztypesetAbiProbe covers, so both exist.
//
// A consumer without a comptime view of this header has only what is below,
// so it is a public API rather than a test fixture. It is not
// grown field-by-field as the API grows; treat it as a self-check on the types
// most likely to be mirrored, not as an exhaustive manifest.
//
// The upstream types are guarded differently and more strongly: static
// assertions in ztypeset_abi.c fail the BUILD if a re-vendor changes the shape
// of anything ztypeset casts to or from. That is the whole reason the upstream
// structs stop at this boundary.
//===----------------------------------------------------------------------===//

typedef struct ZtypesetAbiLayout {
  /// sizeof(ZtypesetAbiLayout). Read this first: if it disagrees with the
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
  uint32_t glyph_offset_flags;

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

  uint32_t charmap_size;
  uint32_t charmap_align;
  uint32_t matrix_size;
  uint32_t matrix_align;
  uint32_t stroke_size;
  uint32_t stroke_align;

  uint32_t visual_run_size;
  uint32_t visual_run_align;
  uint32_t script_run_size;
  uint32_t script_run_align;

  uint32_t glyph_bitmap_size;
  uint32_t glyph_bitmap_align;
  uint32_t glyph_bitmap_offset_pixels;
  uint32_t glyph_bitmap_offset_format;
  uint32_t glyph_bitmap_offset_pitch;
  uint32_t glyph_bitmap_offset_x_advance;

  /// Number of enumerators in ZtypesetResult, so a consumer can assert its own
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
  uint32_t bitmap_format_size;
  uint32_t bitmap_format_last;
  uint32_t line_cap_size;
  uint32_t line_cap_last;
  uint32_t line_join_size;
  uint32_t line_join_last;
  uint32_t stroke_style_size;
  uint32_t stroke_style_last;
  uint32_t encoding_size;
  uint32_t encoding_last;
  /// ZtypesetSegmentation is a bit mask, so `segmentation_last` is
  /// ZTYPESET_SEGMENTATION_ALL -- the OR of every pass -- for the same reason
  /// as `glyph_flag_last` below.
  uint32_t segmentation_size;
  uint32_t segmentation_last;
  /// ZtypesetGlyphFlag is a bit mask, so `glyph_flag_last` is
  /// ZTYPESET_GLYPH_FLAG_DEFINED -- the OR of every flag -- rather than the
  /// highest single flag. A consumer masking with the value it reads here
  /// therefore keeps exactly the bits this build can produce.
  uint32_t glyph_flag_size;
  uint32_t glyph_flag_last;
  /// ZtypesetMetric's enumerators are OpenType TAGS, not an ordinal sequence,
  /// so a "last value" would say nothing about the range. The COUNT is what a
  /// consumer can act on: it says how many metrics this build names, and a
  /// consumer that iterates its own list can tell that ztypeset knows more.
  uint32_t metric_size;
  uint32_t metric_count;
} ZtypesetAbiLayout;

/// Fills `out` with the layout the library was compiled with. Never fails.
ZTYPESET_API void ztypesetAbiLayout(ZtypesetAbiLayout* out);

/// Every plain-data type ztypeset hands across the boundary, in one struct.
///
/// `ztypesetAbiProbe` fills each field with a distinct value derived from its
/// position, and the Zig side asserts each field reads back the value meant
/// for it. Sizes and alignments alone cannot catch two same-typed fields
/// swapping places -- `ascender` and `descender` are both floats, and
/// transposing them changes no size, no alignment and no offset the other
/// check looks at, while turning every line of text upside down.
///
/// This catches that, and catches a field changing type, with one function
/// instead of an offset table that would have to grow a field at a time.
typedef struct ZtypesetAbiProbe {
  ZtypesetAllocator allocator;
  ZtypesetFeature feature;
  ZtypesetShapeParams shape_params;
  ZtypesetGlyph glyph;
  ZtypesetFaceMetrics face_metrics;
  ZtypesetExtents extents;
  ZtypesetVisualRun visual_run;
  ZtypesetScriptRun script_run;
  ZtypesetShapingRun shaping_run;
  ZtypesetGlyphBitmap glyph_bitmap;
  ZtypesetCharmap charmap;
  ZtypesetVariationAxis variation_axis;
  ZtypesetVariation variation;
  ZtypesetMatrix matrix;
  ZtypesetStroke stroke;
  ZtypesetOutlineFuncs outline_funcs;
} ZtypesetAbiProbe;

/// Fills every field of every plain-data type with a distinct marker. Never
/// fails.
ZTYPESET_API void ztypesetAbiProbe(ZtypesetAbiProbe* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ZTYPESET_H_
