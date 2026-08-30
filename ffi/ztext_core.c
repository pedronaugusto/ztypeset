//===----------------------------------------------------------------------===//
// ztext -- allocation, versions, results, and the small shared helpers.
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>

#include <SheenBidi/SBAllocator.h>

#include "ztext_internal.h"

//===----------------------------------------------------------------------===//
// The allocator registry
//
// A block must be freed through the allocator that made it. FreeType,
// HarfBuzz and SheenBidi all free with a bare pointer and none of them
// remembers where a block came from, so the only place that knowledge can
// live is ztext.
//
// It could be a rule in a comment. It was, and the comment was not true:
// HarfBuzz's seam is compile-time and therefore process-wide, so an
// hb_face_t allocated under one installed allocator was destroyed under
// whichever one happened to be installed later -- while the FreeType memory
// of the same font went back to the right one. One handle, two heaps, and
// nothing that could tell you.
//
// So ztext does not ask. Every allocator ever installed is kept in a
// registry, each block records the INDEX of the one that made it, and every
// free and every grow is routed back to that entry rather than to whatever
// is installed at the time. The rule is not enforced by discipline; it is
// not expressible any other way.
//
// What that costs: one ZtextAllocator per DISTINCT allocator ever installed
// (installing the same one twice reuses its entry), allocated with malloc
// and never freed, because it has to outlive the last block it issued. That
// is the only allocation ztext makes outside the installed allocator, it is
// bounded by how many allocators the host installs, and it is at most a few
// dozen bytes each. Nothing else escapes the seam.
//
// What it buys: swapping the process-wide allocator with live handles is
// safe rather than undefined, ztextResetAllocator has a precondition a host
// can actually meet, and the upstreams' process-lifetime caches -- HarfBuzz's
// language intern table is the one that grows -- survive a swap instead of
// being reallocated onto a heap that never issued them.
//===----------------------------------------------------------------------===//

static void* defaultAllocate(void* user, size_t size, size_t alignment) {
  (void)user;
  (void)alignment;
  // malloc guarantees ZTEXT_DEFAULT_ALIGN and no more, so
  // ztextAllocFrom refuses anything stricter before reaching any allocator.
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

/// Registry entry 0, which exists before anything runs so that an allocation
/// made before the first ztextSetAllocator still has an allocator to name.
static ZtextAllocator g_default_entry = {
    defaultAllocate,
    defaultReallocate,
    defaultDeallocate,
    NULL,
};

/// The registry: an array of pointers to entries, never of entries by value.
/// The ARRAY moves when it grows; the ENTRIES must not, because FreeType
/// holds one for the life of an FT_Library and a block header names one by
/// index for the life of the block.
static ZtextAllocator* g_bootstrap[1] = {&g_default_entry};
static ZtextAllocator** g_registry = g_bootstrap;
static size_t g_registry_count = 1u;
static size_t g_registry_capacity = 1u;

/// Which entry ztextAlloc hands new blocks to. Never invalid: it is only ever
/// set to an index the registry already holds.
static ZtextAllocatorId g_installed = ZTEXT_ALLOCATOR_DEFAULT;

static bool sameAllocator(const ZtextAllocator* a, const ZtextAllocator* b) {
  // Field by field rather than memcmp: ZtextAllocator has no padding on any
  // ABI ztext builds for, but comparing padding bytes that were never written
  // would be undefined behaviour if one ever appeared.
  return a->allocate == b->allocate && a->reallocate == b->reallocate &&
         a->deallocate == b->deallocate && a->user == b->user;
}

/// Index of `alloc` in the registry, adding it if it is not already there.
/// Returns ZTEXT_ALLOCATOR_NONE if the registry itself could not grow.
static ZtextAllocatorId registerAllocator(const ZtextAllocator* alloc) {
  for (size_t i = 0u; i < g_registry_count; i++) {
    if (sameAllocator(g_registry[i], alloc)) return (ZtextAllocatorId)i;
  }
  // The registry outlives every allocator it describes, so it cannot be
  // allocated through one of them. See the section header.
  ZtextAllocator* entry = (ZtextAllocator*)malloc(sizeof(ZtextAllocator));
  if (entry == NULL) return ZTEXT_ALLOCATOR_NONE;
  *entry = *alloc;

  if (g_registry_count == g_registry_capacity) {
    if (g_registry_capacity > SIZE_MAX / (2u * sizeof(ZtextAllocator*))) {
      free(entry);
      return ZTEXT_ALLOCATOR_NONE;
    }
    const size_t capacity = g_registry_capacity * 2u;
    ZtextAllocator** grown =
        (ZtextAllocator**)malloc(capacity * sizeof(ZtextAllocator*));
    if (grown == NULL) {
      free(entry);
      return ZTEXT_ALLOCATOR_NONE;
    }
    memcpy(grown, g_registry, g_registry_count * sizeof(ZtextAllocator*));
    // The bootstrap array is static storage, so only a grown one is freed.
    if (g_registry != g_bootstrap) free(g_registry);
    g_registry = grown;
    g_registry_capacity = capacity;
  }

  g_registry[g_registry_count] = entry;
  return (ZtextAllocatorId)(g_registry_count++);
}

ZtextAllocatorId ztextInstalledAllocator(void) { return g_installed; }

ZtextResult ztextSetAllocator(const ZtextAllocator* alloc) {
  if (alloc == NULL) {
    g_installed = ZTEXT_ALLOCATOR_DEFAULT;
    return ZTEXT_RESULT_OK;
  }
  // Refuse a partial allocator without disturbing the working one: a host that
  // mis-fills the struct should keep running on what it had, not lose its heap.
  if (alloc->allocate == NULL || alloc->deallocate == NULL) {
    return ZTEXT_RESULT_INVALID_ARGUMENT;
  }
  // The upstreams' process-lifetime caches, populated HERE -- before the swap,
  // so they are charged to whatever was installed before this call and never
  // to the allocator arriving now. They are never freed, so an allocator that
  // issued one can never balance, and "call ztextWarmup first" was a rule a
  // host had to know and could only discover by not following it. Idempotent,
  // and free after the first time.
  ztextWarmup();
  const ZtextAllocatorId id = registerAllocator(alloc);
  // Same bargain: a registry that could not grow leaves the installed
  // allocator exactly as it was.
  if (id == ZTEXT_ALLOCATOR_NONE) return ZTEXT_RESULT_OUT_OF_MEMORY;
  g_installed = id;
  return ZTEXT_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Block header
//
// FreeType, HarfBuzz and SheenBidi all free with a bare pointer. Recording the
// allocation ahead of the payload is what lets ztext hand a size and alignment
// back to the host -- see the allocator section of ztext.h for why that is
// worth the sixteen bytes -- and it is where the allocator index lives, which
// is what makes the routing above possible.
//===----------------------------------------------------------------------===//

typedef struct ZtextBlockHeader {
  /// Total bytes obtained from the host, prefix included.
  size_t total_size;
  /// Registry index of the allocator that issued this block.
  ZtextAllocatorId allocator;
  /// Alignment the host was asked for, which is at least the payload's. A
  /// power of two, never above ZTEXT_DEFAULT_ALIGN, so 32 bits are ample and
  /// the header stays sixteen bytes on a 64-bit target: the allocator index
  /// costs no memory at all.
  uint32_t backing_alignment;
} ZtextBlockHeader;

/// A header that cannot be one. Both fields have a small, known range, so a
/// prefix that was overrun, freed twice or never written by ztext at all
/// usually fails one of them -- for free, on every deallocation.
///
/// It is a corruption DETECTOR, not a checksum: sixteen bytes leave no room
/// for a magic number, so a garbage header whose two fields happen to be in
/// range still passes. See README.
static bool headerIsPlausible(const ZtextBlockHeader* header) {
  if (header->allocator >= g_registry_count) return false;
  const uint32_t alignment = header->backing_alignment;
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) return false;
  if (alignment > (uint32_t)ZTEXT_DEFAULT_ALIGN) return false;
  return header->total_size > alignment;
}

/// A block reached an allocator that did not make it, or its header is not a
/// header. Neither is recoverable: the host is about to be handed a pointer
/// its heap never issued, and the outcomes are a corrupted free list or
/// silent double ownership.
///
/// So the block is NOT freed -- leaking one block is recoverable, freeing it
/// through the wrong heap is not -- the reason is named on stderr, and the
/// process stops. `_Exit` rather than `abort` so the exit code is the same on
/// every platform and no crash reporter, dialog or atexit handler runs on top
/// of a heap whose state is already in question.
///
/// Reaching this is a defect in ztext, not in a host: since every free is
/// ROUTED through the block's own allocator, a host cannot cause it by
/// swapping allocators. What it catches is ztext allocating a block from one
/// place and releasing it from another -- the mutation ci/check-guards.sh
/// plants to prove this is live.
static void ztextAllocatorFatal(const char* why, const void* block,
                                unsigned long recorded, unsigned long asked) {
  fprintf(stderr,
          "ztext: FATAL: %s (block %p, recorded allocator %lu, released "
          "through allocator %lu). A ztext block is freed through the "
          "allocator that made it; see ztextSetAllocator in ztext.h. The "
          "block was NOT freed and the process is stopping before the wrong "
          "heap is corrupted.\n",
          why, block, recorded, asked);
  fflush(stderr);
  _Exit(ZTEXT_EXIT_ALLOCATOR_MISMATCH);
}

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
/// rounding to `requested` -- so only the backing alignment needs
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

/// Payload bytes of a live block -- what the caller asked for, rounded up to
/// at least one. Used by the SheenBidi seam to find the tail a grow added.
static size_t ztextBlockSize(void* payload) {
  const ZtextBlockHeader* header = headerOf(payload);
  return header->total_size - prefixSize(header->backing_alignment);
}

/// The header of a live block, with both checks already made: the header has
/// to be plausible, and if the caller named an allocator it has to be the one
/// on record.
static ZtextBlockHeader* checkedHeaderOf(void* block, ZtextAllocatorId asked) {
  ZtextBlockHeader* header = headerOf(block);
  if (!headerIsPlausible(header)) {
    ztextAllocatorFatal("the block prefix is not a ztext allocation header",
                        block, (unsigned long)header->allocator,
                        (unsigned long)asked);
  }
  if (asked != ZTEXT_ALLOCATOR_ANY && header->allocator != asked) {
    ztextAllocatorFatal("a block was released through the wrong allocator",
                        block, (unsigned long)header->allocator,
                        (unsigned long)asked);
  }
  return header;
}

void* ztextAllocFrom(ZtextAllocatorId id, size_t size, size_t alignment) {
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) return NULL;
  // Nothing in ztext, FreeType, HarfBuzz or SheenBidi asks for more than
  // malloc's guarantee, and a host allocator is only ever promised that much.
  // Refusing here means an over-aligned request can never be served
  // under-aligned by the default allocator -- it fails visibly instead.
  if (alignment > ZTEXT_DEFAULT_ALIGN) return NULL;
  if (size == 0u) size = 1u;  // A distinct, freeable pointer, never NULL.

  const ZtextAllocator* allocator = g_registry[id];
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
  header->allocator = id;
  header->backing_alignment = (uint32_t)backing;
  return payload;
}

void* ztextCalloc(size_t count, size_t size) {
  if (count != 0u && size > SIZE_MAX / count) return NULL;
  const size_t total = count * size;
  void* block = ztextAlloc(total, ZTEXT_DEFAULT_ALIGN);
  if (block != NULL) memset(block, 0, total == 0u ? 1u : total);
  return block;
}

void ztextFreeFrom(ZtextAllocatorId id, void* block) {
  if (block == NULL) return;
  const ZtextBlockHeader* header = checkedHeaderOf(block, id);
  const ZtextAllocator* allocator = g_registry[header->allocator];
  const size_t total = header->total_size;
  const size_t backing = header->backing_alignment;
  unsigned char* base = (unsigned char*)block - prefixSize(backing);
  allocator->deallocate(allocator->user, base, total, backing);
}

void* ztextReallocFrom(ZtextAllocatorId id, void* block, size_t new_size,
                       size_t alignment) {
  if (block == NULL) {
    return ztextAllocFrom(id == ZTEXT_ALLOCATOR_ANY ? g_installed : id,
                          new_size, alignment);
  }
  if (new_size == 0u) new_size = 1u;

  const ZtextBlockHeader* header = checkedHeaderOf(block, id);
  // The block keeps its own allocator across a grow. Handing a grown block to
  // a different heap than the one that issued it is the same defect as
  // freeing it there, one step later.
  const ZtextAllocatorId owner = header->allocator;
  const ZtextAllocator* allocator = g_registry[owner];
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
      moved_header->allocator = owner;
      moved_header->backing_alignment = (uint32_t)backing;
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

  // Allocated from the block's OWN allocator, with the alignment the block
  // actually has rather than the one the caller happened to pass this time.
  void* fresh = ztextAllocFrom(owner, new_size, backing);
  if (fresh == NULL) return NULL;
  const size_t old_payload = old_total - prefix;
  memcpy(fresh, block, old_payload < new_size ? old_payload : new_size);
  ztextFreeFrom(owner, block);
  return fresh;
}

//===----------------------------------------------------------------------===//
// The same, for memory that belongs to the process rather than to one object.
//
// Shaper and paragraph handles, and every allocation HarfBuzz and SheenBidi
// make, because neither of those can be told to use anything narrower. A new
// block comes from whatever is installed now; an existing one is grown and
// freed through the entry it recorded, whatever is installed now.
//===----------------------------------------------------------------------===//

void* ztextAlloc(size_t size, size_t alignment) {
  return ztextAllocFrom(g_installed, size, alignment);
}

void* ztextRealloc(void* block, size_t new_size, size_t alignment) {
  return ztextReallocFrom(ZTEXT_ALLOCATOR_ANY, block, new_size, alignment);
}

void ztextFree(void* block) { ztextFreeFrom(ZTEXT_ALLOCATOR_ANY, block); }

ZtextAllocatorId ztextAllocatorOf(void* block) {
  return checkedHeaderOf(block, ZTEXT_ALLOCATOR_ANY)->allocator;
}

//===----------------------------------------------------------------------===//
// FreeType's seam
//
// Per FT_Library. FreeType hands every allocation call the FT_Memory it was
// built with, and ztext points that at the owning library, so a library's
// FreeType memory names the library's allocator entry rather than whatever is
// installed at the time.
//===----------------------------------------------------------------------===//

static ZtextAllocatorId memoryAllocator(FT_Memory memory) {
  return ((const ZtextLibrary*)memory->user)->allocator;
}

static void* ftAlloc(FT_Memory memory, long size) {
  if (size <= 0) return NULL;
  return ztextAllocFrom(memoryAllocator(memory), (size_t)size,
                        ZTEXT_DEFAULT_ALIGN);
}

static void ftFree(FT_Memory memory, void* block) {
  ztextFreeFrom(memoryAllocator(memory), block);
}

static void* ftRealloc(FT_Memory memory, long cur_size, long new_size,
                       void* block) {
  // cur_size is FreeType's idea of the old size; the block header is
  // authoritative, so it is deliberately ignored rather than trusted.
  (void)cur_size;
  const ZtextAllocatorId id = memoryAllocator(memory);
  if (new_size <= 0) {
    ztextFreeFrom(id, block);
    return NULL;
  }
  return ztextReallocFrom(id, block, (size_t)new_size, ZTEXT_DEFAULT_ALIGN);
}

void ztextInitFtMemory(ZtextLibrary* library) {
  // Recorded now, so a later ztextSetAllocator cannot redirect the memory this
  // library has already handed to FreeType -- and, because the registry entry
  // outlives the allocator, the library stays able to free its own blocks even
  // after the host has moved on.
  library->allocator = ztextInstalledAllocator();
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

/// Zeroed, and that is load-bearing rather than defensive.
///
/// SheenBidi 3.0.0's object model reads a field it has not written, on its own
/// allocation-failure path: Core/Object.c ObjectCreate hands out a raw block,
/// API/SBParagraph.c AllocateParagraph fills in `fixedLevels` and nothing
/// else, and when ResolveParagraph then fails CreateParagraph calls
/// ObjectRelease -- whose finalizer reads `paragraph->_algorithm` and releases
/// whatever happened to be in those eight bytes.
///
/// Measured on this tree, not inferred: with malloc's leftovers the C smoke
/// test segfaulted 11 times in 600 runs, always at the same injection budget
/// -- the one point where the paragraph object is allocated and the resolve
/// after it is refused. Filling every SheenBidi block with 0xCD made that
/// 60 out of 60; zeroing made it 0 out of 400.
///
/// ztext may not patch a vendored upstream (see UPSTREAM.md), and there is no
/// route to that failure path that does not go through this function, so the
/// containment belongs here: SheenBidi never sees a byte ztext has not
/// written. tests/c_smoke.c's poisoning arm holds it -- remove the memset and
/// that arm dies every run rather than one run in fifty.
static void* sbAllocateBlock(SBUInteger size, void* info) {
  (void)info;
  void* block = ztextAlloc((size_t)size, ZTEXT_DEFAULT_ALIGN);
  if (block != NULL) memset(block, 0, (size_t)size);
  return block;
}

/// The grown tail gets the same treatment, for the same reason: a block that
/// SheenBidi has already used is fully initialised as far as it wrote, and
/// everything past that is fresh memory it may read before writing.
static void* sbReallocateBlock(void* pointer, SBUInteger new_size, void* info) {
  (void)info;
  const size_t old_payload = pointer == NULL ? 0u : ztextBlockSize(pointer);
  void* block = ztextRealloc(pointer, (size_t)new_size, ZTEXT_DEFAULT_ALIGN);
  if (block != NULL && (size_t)new_size > old_payload) {
    memset((unsigned char*)block + old_payload, 0,
           (size_t)new_size - old_payload);
  }
  return block;
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
    case ZTEXT_RESULT_INVALID_TEXT:
      return "text not well-formed in its encoding";
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

int32_t ztextToFixed266(float pixels) {
  if (!(pixels > 0.0f) || !(pixels <= 16384.0f)) return 0;
  return (int32_t)(pixels * 64.0f + 0.5f);
}

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
// Text
//===----------------------------------------------------------------------===//

size_t ztextEncodingUnitSize(ZtextEncoding encoding) {
  switch (encoding) {
    case ZTEXT_ENCODING_UTF8:
      return 1u;
    case ZTEXT_ENCODING_UTF16:
      return 2u;
    case ZTEXT_ENCODING_UTF32:
      return 4u;
    default:
      // Not an encoding this build names. Reported as a size of zero rather
      // than defaulted to one, so a caller compiled against a newer header
      // gets ZTEXT_RESULT_INVALID_ARGUMENT instead of its text read as UTF-8.
      return 0u;
  }
}

static bool isValidUtf8(const char* text, size_t length) {
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

/// A high surrogate must be followed by a low one, and a low one may not
/// stand alone. Nothing else in UTF-16 can be malformed: every other unit is
/// a character.
static bool isValidUtf16(const uint16_t* text, size_t length) {
  for (size_t i = 0u; i < length; i++) {
    const uint16_t unit = text[i];
    if (unit < 0xD800u || unit > 0xDFFFu) continue;
    if (unit >= 0xDC00u) return false;   // A low surrogate with no high one.
    if (i + 1u >= length) return false;  // A high surrogate at the end.
    const uint16_t low = text[i + 1u];
    if (low < 0xDC00u || low > 0xDFFFu) return false;
    i++;
  }
  return true;
}

/// One unit, one character -- so the only malformations are values that are
/// not scalars at all.
static bool isValidUtf32(const uint32_t* text, size_t length) {
  for (size_t i = 0u; i < length; i++) {
    const uint32_t code = text[i];
    if (code > 0x10FFFFu) return false;
    if (code >= 0xD800u && code <= 0xDFFFu) return false;
  }
  return true;
}

bool ztextTextIsWellFormed(const void* text, size_t length,
                           ZtextEncoding encoding) {
  // A zero length is well-formed in every encoding, including with a NULL
  // pointer: an empty label is not a malformed one.
  if (length == 0u) return true;
  if (text == NULL) return false;
  switch (encoding) {
    case ZTEXT_ENCODING_UTF8:
      return isValidUtf8((const char*)text, length);
    case ZTEXT_ENCODING_UTF16:
      return isValidUtf16((const uint16_t*)text, length);
    case ZTEXT_ENCODING_UTF32:
      return isValidUtf32((const uint32_t*)text, length);
    default:
      return false;
  }
}

static size_t decodeUtf8(const char* text, size_t length, uint32_t* out) {
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

size_t ztextTextDecode(const void* text, size_t length,
                       ZtextEncoding encoding, size_t index, uint32_t* out) {
  switch (encoding) {
    case ZTEXT_ENCODING_UTF16: {
      const uint16_t* units = (const uint16_t*)text;
      const uint16_t lead = units[index];
      if (lead >= 0xD800u && lead <= 0xDBFFu && index + 1u < length) {
        const uint32_t high = (uint32_t)(lead - 0xD800u);
        const uint32_t low = (uint32_t)(units[index + 1u] - 0xDC00u);
        *out = 0x10000u + ((high << 10) | low);
        return 2u;
      }
      *out = lead;
      return 1u;
    }
    case ZTEXT_ENCODING_UTF32:
      *out = ((const uint32_t*)text)[index];
      return 1u;
    case ZTEXT_ENCODING_UTF8:
    default:
      return decodeUtf8((const char*)text + index, length - index, out);
  }
}

bool ztextTextSplitsCharacter(const void* text, size_t length,
                              ZtextEncoding encoding, size_t index) {
  if (index >= length) return false;
  switch (encoding) {
    case ZTEXT_ENCODING_UTF16: {
      // Validated text has no unpaired low surrogate, so a low surrogate is
      // always the second half of a pair -- and a boundary there is inside a
      // character.
      const uint16_t unit = ((const uint16_t*)text)[index];
      return unit >= 0xDC00u && unit <= 0xDFFFu;
    }
    case ZTEXT_ENCODING_UTF32:
      // One unit, one character: no index can be inside one.
      return false;
    case ZTEXT_ENCODING_UTF8:
    default:
      return (((const unsigned char*)text)[index] & 0xC0u) == 0x80u;
  }
}

//===----------------------------------------------------------------------===//
// Growable array
//===----------------------------------------------------------------------===//

bool ztextArrayReserve(ZtextAllocatorId owner, ZtextArray* array, size_t count,
                       size_t element_size) {
  if (count <= array->capacity) return true;

  size_t capacity = array->capacity == 0u ? 16u : array->capacity;
  while (capacity < count) {
    if (capacity > SIZE_MAX / 2u) return false;
    capacity *= 2u;
  }
  if (capacity > SIZE_MAX / element_size) return false;

  void* grown = ztextReallocFrom(owner, array->data, capacity * element_size,
                                 ZTEXT_DEFAULT_ALIGN);
  if (grown == NULL) return false;

  array->data = grown;
  array->capacity = capacity;
  return true;
}

void ztextArrayFree(ZtextAllocatorId owner, ZtextArray* array,
                    size_t element_size) {
  (void)element_size;
  ztextFreeFrom(owner, array->data);
  array->data = NULL;
  array->count = 0u;
  array->capacity = 0u;
}
