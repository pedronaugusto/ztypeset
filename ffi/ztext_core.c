//===----------------------------------------------------------------------===//
// ztext -- allocation, versions, results, and the small shared helpers.
//===----------------------------------------------------------------------===//

#include <stdlib.h>

#include <SheenBidi/SBAllocator.h>

#include "ztext_internal.h"

//===----------------------------------------------------------------------===//
// The installed allocator
//===----------------------------------------------------------------------===//

static void* defaultAllocate(void* user, size_t size, size_t alignment) {
  (void)user;
  (void)alignment;
  // malloc guarantees ZTEXT_DEFAULT_ALIGN and no more, which is why
  // ztextAllocWith refuses anything stricter before reaching any allocator.
  return malloc(size);
}

static void* defaultReallocate(void* user, void* block, size_t old_size,
                               size_t new_size, size_t alignment) {
  (void)user;
  (void)old_size;
  (void)alignment;
  return realloc(block, new_size);
}

static void defaultDeallocate(void* user, void* block, size_t size,
                              size_t alignment) {
  (void)user;
  (void)size;
  (void)alignment;
  free(block);
}

static ZtextAllocator g_allocator = {
    defaultAllocate,
    defaultReallocate,
    defaultDeallocate,
    NULL,
};

ZtextResult ztextSetAllocator(const ZtextAllocator* alloc) {
  if (alloc == NULL) {
    g_allocator.allocate = defaultAllocate;
    g_allocator.reallocate = defaultReallocate;
    g_allocator.deallocate = defaultDeallocate;
    g_allocator.user = NULL;
    return ZTEXT_RESULT_OK;
  }
  // Refuse a partial allocator without disturbing the working one: a host that
  // mis-fills the struct should keep running on malloc, not lose its heap.
  if (alloc->allocate == NULL || alloc->deallocate == NULL) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }
  g_allocator = *alloc;
  return ZTEXT_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Block header
//
// FreeType, HarfBuzz and SheenBidi all free with a bare pointer. Recording the
// allocation ahead of the payload is what lets ztext hand a size and alignment
// back to the host -- see the allocator section of ztext.h for why that is
// worth the sixteen bytes.
//===----------------------------------------------------------------------===//

typedef struct ZtextBlockHeader {
  /// Total bytes obtained from the host, prefix included.
  size_t total_size;
  /// Alignment the host was asked for, which is at least the payload's.
  size_t backing_alignment;
} ZtextBlockHeader;

static size_t backingAlignment(size_t alignment) {
  return alignment < _Alignof(ZtextBlockHeader) ? _Alignof(ZtextBlockHeader)
                                                : alignment;
}

/// Bytes reserved before the payload: enough for the header, rounded up so the
/// payload keeps the alignment that was asked for.
///
/// Computed identically at allocate and free time. That works out because the
/// rounded-up minimum is already a multiple of every alignment at or below it,
/// so rounding to max(requested, alignof(header)) lands on the same value as
/// rounding to `requested` -- which is why only the backing alignment needs
/// storing.
static size_t prefixSize(size_t alignment) {
  size_t minimum = sizeof(ZtextBlockHeader);
  if (minimum < _Alignof(ZtextBlockHeader)) {
    minimum = _Alignof(ZtextBlockHeader);
  }
  return (minimum + alignment - 1u) & ~(alignment - 1u);
}

static ZtextBlockHeader* headerOf(void* payload) {
  return (ZtextBlockHeader*)((unsigned char*)payload -
                            sizeof(ZtextBlockHeader));
}

void* ztextAllocWith(const ZtextAllocator* allocator, size_t size,
                     size_t alignment) {
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) return NULL;
  // Nothing in ztext, FreeType, HarfBuzz or SheenBidi asks for more than
  // malloc's guarantee, and a host allocator is only ever promised that much.
  // Refusing here means an over-aligned request can never be served
  // under-aligned by the default allocator -- it fails visibly instead.
  if (alignment > ZTEXT_DEFAULT_ALIGN) return NULL;
  if (size == 0u) size = 1u;  // A distinct, freeable pointer, never NULL.

  const size_t backing = backingAlignment(alignment);
  const size_t prefix = prefixSize(backing);
  if (size > SIZE_MAX - prefix) return NULL;
  const size_t total = prefix + size;

  unsigned char* base =
      (unsigned char*)allocator->allocate(allocator->user, total, backing);
  if (base == NULL) return NULL;

  unsigned char* payload = base + prefix;
  ZtextBlockHeader* header = headerOf(payload);
  header->total_size = total;
  header->backing_alignment = backing;
  return payload;
}

void* ztextCalloc(size_t count, size_t size) {
  if (count != 0u && size > SIZE_MAX / count) return NULL;
  const size_t total = count * size;
  void* block = ztextAlloc(total, ZTEXT_DEFAULT_ALIGN);
  if (block != NULL) memset(block, 0, total == 0u ? 1u : total);
  return block;
}

void ztextFreeWith(const ZtextAllocator* allocator, void* block) {
  if (block == NULL) return;
  const ZtextBlockHeader* header = headerOf(block);
  const size_t total = header->total_size;
  const size_t backing = header->backing_alignment;
  unsigned char* base = (unsigned char*)block - prefixSize(backing);
  allocator->deallocate(allocator->user, base, total, backing);
}

void* ztextReallocWith(const ZtextAllocator* allocator, void* block,
                       size_t new_size, size_t alignment) {
  if (block == NULL) return ztextAllocWith(allocator, new_size, alignment);
  if (new_size == 0u) new_size = 1u;

  const ZtextBlockHeader* header = headerOf(block);
  const size_t old_total = header->total_size;
  const size_t backing = header->backing_alignment;
  const size_t prefix = prefixSize(backing);
  if (new_size > SIZE_MAX - prefix) return NULL;
  const size_t new_total = prefix + new_size;

  unsigned char* base = (unsigned char*)block - prefix;

  if (allocator->reallocate != NULL) {
    unsigned char* moved = (unsigned char*)allocator->reallocate(
        allocator->user, base, old_total, new_total, backing);
    if (moved != NULL) {
      unsigned char* payload = moved + prefix;
      ZtextBlockHeader* moved_header = headerOf(payload);
      moved_header->total_size = new_total;
      moved_header->backing_alignment = backing;
      return payload;
    }
    // NULL means the host declined, NOT that the process is out of memory,
    // and the two are indistinguishable from here. Zig's std.mem.Allocator is
    // the motivating case: its `remap` only ever resizes in place and returns
    // null the moment a block would have to move, which for a growing buffer
    // is most of the time.
    //
    // So a decline falls through to allocate-copy-free rather than being
    // reported upwards. Getting this wrong is worse than it sounds: HarfBuzz
    // treats a failed allocation as a reason to abandon an optional
    // accelerator and carry on, so the visible symptom is not a crash or an
    // error but correct-looking text shaped WITHOUT its OpenType layout --
    // nominal glyphs, no joining, no ligatures. Silent degradation, only on
    // hosts that supply their own allocator.
    //
    // The original block is untouched by a declined reallocate, so it is
    // still ours to copy out of and free.
  }

  // Allocated with the alignment the block actually has, not the one the
  // caller happened to pass this time. The two agree for every call ztext
  // makes today; using the recorded one means they cannot disagree later.
  void* fresh = ztextAllocWith(allocator, new_size, backing);
  if (fresh == NULL) return NULL;
  const size_t old_payload = old_total - prefix;
  memcpy(fresh, block, old_payload < new_size ? old_payload : new_size);
  ztextFreeWith(allocator, block);
  return fresh;
}

//===----------------------------------------------------------------------===//
// The same, through the process-wide allocator.
//
// Everything that is not owned by a specific ZtextLibrary goes through here:
// shaper and paragraph handles, and every allocation HarfBuzz and SheenBidi
// make, because neither of those can be told to use anything narrower.
//===----------------------------------------------------------------------===//

void* ztextAlloc(size_t size, size_t alignment) {
  return ztextAllocWith(&g_allocator, size, alignment);
}

void* ztextRealloc(void* block, size_t new_size, size_t alignment) {
  return ztextReallocWith(&g_allocator, block, new_size, alignment);
}

void ztextFree(void* block) { ztextFreeWith(&g_allocator, block); }

//===----------------------------------------------------------------------===//
// FreeType's seam
//
// Per FT_Library, which is the one upstream here whose allocation is not
// forced to be process-wide.
//===----------------------------------------------------------------------===//

/// FreeType hands every allocation call the FT_Memory it was built with, and
/// ztext points that at the owning library -- which is what makes this
/// per-library rather than a second route to the global.
static const ZtextAllocator* memoryAllocator(FT_Memory memory) {
  const ZtextLibrary* library = (const ZtextLibrary*)memory->user;
  return &library->allocator;
}

static void* ftAlloc(FT_Memory memory, long size) {
  if (size <= 0) return NULL;
  return ztextAllocWith(memoryAllocator(memory), (size_t)size,
                        ZTEXT_DEFAULT_ALIGN);
}

static void ftFree(FT_Memory memory, void* block) {
  ztextFreeWith(memoryAllocator(memory), block);
}

static void* ftRealloc(FT_Memory memory, long cur_size, long new_size,
                       void* block) {
  // cur_size is FreeType's idea of the old size; the block header is
  // authoritative, so it is deliberately ignored rather than trusted.
  (void)cur_size;
  const ZtextAllocator* allocator = memoryAllocator(memory);
  if (new_size <= 0) {
    ztextFreeWith(allocator, block);
    return NULL;
  }
  return ztextReallocWith(allocator, block, (size_t)new_size,
                          ZTEXT_DEFAULT_ALIGN);
}

void ztextInitFtMemory(ZtextLibrary* library) {
  // Captured by value, now, so a later ztextSetAllocator cannot redirect the
  // memory this library has already handed to FreeType.
  library->allocator = g_allocator;
  library->memory.user = library;
  library->memory.alloc = ftAlloc;
  library->memory.free = ftFree;
  library->memory.realloc = ftRealloc;
}

//===----------------------------------------------------------------------===//
// HarfBuzz's seam
//
// build.zig defines hb_malloc_impl and friends to these names, which is what
// makes HarfBuzz define HB_CUSTOM_MALLOC for itself and declare them extern
// "C". Compile-time, and therefore process-wide -- see ztext.h.
//===----------------------------------------------------------------------===//

void* ztext_hb_malloc(size_t size);
void* ztext_hb_calloc(size_t count, size_t size);
void* ztext_hb_realloc(void* block, size_t size);
void ztext_hb_free(void* block);

void* ztext_hb_malloc(size_t size) {
  return ztextAlloc(size, ZTEXT_DEFAULT_ALIGN);
}

void* ztext_hb_calloc(size_t count, size_t size) {
  return ztextCalloc(count, size);
}

void* ztext_hb_realloc(void* block, size_t size) {
  return ztextRealloc(block, size, ZTEXT_DEFAULT_ALIGN);
}

void ztext_hb_free(void* block) { ztextFree(block); }

//===----------------------------------------------------------------------===//
// SheenBidi's seam
//===----------------------------------------------------------------------===//

static void* sbAllocateBlock(SBUInteger size, void* info) {
  (void)info;
  return ztextAlloc((size_t)size, ZTEXT_DEFAULT_ALIGN);
}

static void* sbReallocateBlock(void* pointer, SBUInteger new_size, void* info) {
  (void)info;
  return ztextRealloc(pointer, (size_t)new_size, ZTEXT_DEFAULT_ALIGN);
}

static void sbDeallocateBlock(void* pointer, void* info) {
  (void)info;
  ztextFree(pointer);
}

/// Created once and kept for the life of the process.
///
/// SBAllocatorSetDefault stores the pointer without retaining it, so ztext has
/// to hold the reference. One instance is enough for any number of
/// ztextSetAllocator calls, because the three functions above dispatch through
/// whichever allocator is installed at the time rather than capturing one.
static SBAllocatorRef g_sb_allocator = NULL;

ZtextResult ztextInstallSheenbidiAllocator(void) {
  if (g_sb_allocator != NULL) return ZTEXT_RESULT_OK;

  SBAllocatorProtocol protocol;
  memset(&protocol, 0, sizeof(protocol));
  protocol.allocateBlock = sbAllocateBlock;
  protocol.reallocateBlock = sbReallocateBlock;
  protocol.deallocateBlock = sbDeallocateBlock;
  // allocateScratch and resetScratch stay NULL: SheenBidi then serves scratch
  // out of allocateBlock, which keeps every byte visible to the host's
  // accounting. A host wanting a thread-local scratch pool can still have one
  // by making its own allocator do that.

  g_sb_allocator = SBAllocatorCreate(&protocol, NULL);
  if (g_sb_allocator == NULL) return ZTEXT_RESULT_OUT_OF_MEMORY;
  SBAllocatorSetDefault(g_sb_allocator);
  return ZTEXT_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Versions
//===----------------------------------------------------------------------===//

static uint32_t pack(uint32_t major, uint32_t minor, uint32_t patch) {
  return (major << 16) | (minor << 8) | patch;
}

uint32_t ztextVersion(void) {
  return pack(ZTEXT_VERSION_MAJOR, ZTEXT_VERSION_MINOR, ZTEXT_VERSION_PATCH);
}

uint32_t ztextFreetypeVersion(void) {
  return pack(FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
}

uint32_t ztextHarfbuzzVersion(void) {
  return pack(HB_VERSION_MAJOR, HB_VERSION_MINOR, HB_VERSION_MICRO);
}

uint32_t ztextSheenbidiVersion(void) {
  return pack(SHEENBIDI_VERSION_MAJOR, SHEENBIDI_VERSION_MINOR,
              SHEENBIDI_VERSION_PATCH);
}

uint32_t ztextUnibreakVersion(void) {
  // libunibreak packs its version as one hex integer -- 0x0700 for 7.0 -- so
  // unlike the other three there are no separate major/minor macros to read.
  return pack((unsigned)((UNIBREAK_VERSION >> 8) & 0xFFu),
              (unsigned)(UNIBREAK_VERSION & 0xFFu), 0u);
}

//===----------------------------------------------------------------------===//
// Results
//===----------------------------------------------------------------------===//

const char* ztextResultName(ZtextResult result) {
  switch (result) {
    case ZTEXT_RESULT_OK:
      return "ok";
    case ZTEXT_RESULT_OUT_OF_MEMORY:
      return "out of memory";
    case ZTEXT_RESULT_INVALID_ARGUMENT:
      return "invalid argument";
    case ZTEXT_RESULT_INVALID_UTF8:
      return "invalid UTF-8";
    case ZTEXT_RESULT_BAD_FONT:
      return "bad font";
    case ZTEXT_RESULT_UNSUPPORTED:
      return "unsupported font format";
    case ZTEXT_RESULT_GLYPH_NOT_FOUND:
      return "glyph not found";
    case ZTEXT_RESULT_RENDER_FAILED:
      return "render failed";
    case ZTEXT_RESULT_SHAPE_FAILED:
      return "shape failed";
    case ZTEXT_RESULT_BIDI_FAILED:
      return "bidi failed";
    case ZTEXT_RESULT_BUFFER_TOO_SMALL:
      return "buffer too small";
  }
  // Not reachable through the enum, but a caller can cast anything into it.
  return "unknown result";
}

//===----------------------------------------------------------------------===//
// Generations
//===----------------------------------------------------------------------===//

uint64_t ztextNextGeneration(void) {
  // Not atomic, and does not need to be: a ZtextLibrary and its faces belong
  // to one thread, so two threads creating faces are creating them in
  // different libraries. Even a torn increment only ever produces a value that
  // fails to match, which is the safe direction.
  static uint64_t counter = 0u;
  return ++counter;
}

//===----------------------------------------------------------------------===//
// Error detail
//===----------------------------------------------------------------------===//

#define ZTEXT_DETAIL_MAX 160

static _Thread_local char g_detail[ZTEXT_DETAIL_MAX] = {0};

void ztextSetErrorDetail(const char* detail) {
  if (detail == NULL) {
    g_detail[0] = '\0';
    return;
  }
  size_t i = 0;
  while (i + 1u < ZTEXT_DETAIL_MAX && detail[i] != '\0') {
    g_detail[i] = detail[i];
    i++;
  }
  g_detail[i] = '\0';
}

const char* ztextLastErrorDetail(void) { return g_detail; }

ZtextResult ztextFromFtError(FT_Error error) {
  if (error == FT_Err_Ok) return ZTEXT_RESULT_OK;

  const char* string = FT_Error_String(error);
  ztextSetErrorDetail(string != NULL ? string : "FreeType error");

  switch (error) {
    case FT_Err_Out_Of_Memory:
      return ZTEXT_RESULT_OUT_OF_MEMORY;
    case FT_Err_Invalid_Argument:
    case FT_Err_Invalid_Face_Handle:
    case FT_Err_Invalid_Size_Handle:
    case FT_Err_Invalid_Library_Handle:
      return ZTEXT_RESULT_INVALID_ARGUMENT;
    case FT_Err_Invalid_Glyph_Index:
      return ZTEXT_RESULT_GLYPH_NOT_FOUND;
    // FreeType reports "unknown file format" both for a format this build has
    // no driver for and for bytes that are not a font at all, which are worth
    // telling apart. ztext_face.c sniffs the known-but-not-compiled formats
    // before FreeType sees them and reports ZTEXT_RESULT_UNSUPPORTED itself, so
    // anything reaching here really is unrecognisable.
    case FT_Err_Unknown_File_Format:
      return ZTEXT_RESULT_BAD_FONT;
    case FT_Err_Missing_Module:
      return ZTEXT_RESULT_UNSUPPORTED;
    default:
      return ZTEXT_RESULT_BAD_FONT;
  }
}

//===----------------------------------------------------------------------===//
// UTF-8
//===----------------------------------------------------------------------===//

bool ztextIsValidUtf8(const char* text, size_t length) {
  const unsigned char* p = (const unsigned char*)text;
  size_t i = 0;

  while (i < length) {
    const unsigned char lead = p[i];

    if (lead < 0x80u) {
      i += 1u;
      continue;
    }

    size_t extra;
    uint32_t code;
    // Lower bound per length, so an overlong encoding is rejected rather than
    // decoded: C0/C1 never appear, E0 80.. is not U+0000, and so on.
    uint32_t lowest;

    if ((lead & 0xE0u) == 0xC0u) {
      extra = 1u;
      code = lead & 0x1Fu;
      lowest = 0x80u;
    } else if ((lead & 0xF0u) == 0xE0u) {
      extra = 2u;
      code = lead & 0x0Fu;
      lowest = 0x800u;
    } else if ((lead & 0xF8u) == 0xF0u) {
      extra = 3u;
      code = lead & 0x07u;
      lowest = 0x10000u;
    } else {
      return false;  // A continuation byte or 0xF8..0xFF as a lead.
    }

    // Truncated sequence at the end of the buffer. `i < length` holds, so
    // `length - i - 1` is the number of bytes available after the lead.
    if (extra > length - i - 1u) return false;

    for (size_t k = 1u; k <= extra; k++) {
      const unsigned char continuation = p[i + k];
      if ((continuation & 0xC0u) != 0x80u) return false;
      code = (code << 6) | (continuation & 0x3Fu);
    }

    if (code < lowest) return false;                        // Overlong.
    if (code > 0x10FFFFu) return false;                     // Out of range.
    if (code >= 0xD800u && code <= 0xDFFFu) return false;   // Surrogate half.

    i += extra + 1u;
  }

  return true;
}

size_t ztextDecodeUtf8(const char* text, size_t length, uint32_t* out) {
  const unsigned char* p = (const unsigned char*)text;
  const unsigned char lead = p[0];

  if (lead < 0x80u) {
    *out = lead;
    return 1u;
  }

  size_t extra;
  uint32_t code;
  if ((lead & 0xE0u) == 0xC0u) {
    extra = 1u;
    code = lead & 0x1Fu;
  } else if ((lead & 0xF0u) == 0xE0u) {
    extra = 2u;
    code = lead & 0x0Fu;
  } else {
    extra = 3u;
    code = lead & 0x07u;
  }

  // The caller validated the buffer, so the continuations are known to be
  // there. The bound is kept anyway: this decodes from the same pointer the
  // validator walked, and a future caller that forgets is a read past the end.
  if (extra > length - 1u) {
    *out = 0xFFFDu;
    return length;
  }

  for (size_t k = 1u; k <= extra; k++) code = (code << 6) | (p[k] & 0x3Fu);
  *out = code;
  return extra + 1u;
}

//===----------------------------------------------------------------------===//
// Growable array
//===----------------------------------------------------------------------===//

bool ztextArrayReserve(ZtextArray* array, size_t count, size_t element_size) {
  if (count <= array->capacity) return true;

  size_t capacity = array->capacity == 0u ? 16u : array->capacity;
  while (capacity < count) {
    if (capacity > SIZE_MAX / 2u) return false;
    capacity *= 2u;
  }
  if (capacity > SIZE_MAX / element_size) return false;

  void* grown = ztextRealloc(array->data, capacity * element_size,
                             ZTEXT_DEFAULT_ALIGN);
  if (grown == NULL) return false;

  array->data = grown;
  array->capacity = capacity;
  return true;
}

void ztextArrayFree(ZtextArray* array, size_t element_size) {
  (void)element_size;
  ztextFree(array->data);
  array->data = NULL;
  array->count = 0u;
  array->capacity = 0u;
}
