//===----------------------------------------------------------------------===//
// ztypeset -- allocation, versions, results, and the small shared helpers.
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>

#include <SheenBidi/SBAllocator.h>

#include <stdatomic.h>

#include "ztypeset_internal.h"

//===----------------------------------------------------------------------===//
// The allocator registry
//
// A block must be freed through the allocator that made it. FreeType,
// HarfBuzz and SheenBidi all free with a bare pointer and none of them
// remembers where a block came from, so the only place that knowledge can
// live is ztypeset.
//
// It could be a rule in a comment. It was, and the comment was not true:
// HarfBuzz's seam is compile-time and therefore process-wide, so an
// hb_face_t allocated under one installed allocator was destroyed under
// whichever one happened to be installed later -- while the FreeType memory
// of the same font went back to the right one. One handle, two heaps, and
// nothing that could tell you.
//
// So ztypeset does not ask. Every allocator ever installed is kept in a
// registry, each block records the INDEX of the one that made it, and every
// free and every grow is routed back to that entry rather than to whatever
// is installed at the time. The rule is not enforced by discipline; it is
// not expressible any other way.
//
// What that costs: one ZtypesetAllocator per DISTINCT allocator ever installed
// (installing the same one twice reuses its entry), allocated with malloc
// and never freed, because it has to outlive the last block it issued. That
// is the only allocation ztypeset makes outside the installed allocator, it is
// bounded by how many allocators the host installs, and it is at most a few
// dozen bytes each. Nothing else escapes the seam.
//
// What it buys: swapping the process-wide allocator with live handles is
// safe rather than undefined, ztypesetResetAllocator has a precondition a host
// can actually meet, and the upstreams' process-lifetime caches -- HarfBuzz's
// language intern table is the one that grows -- survive a swap instead of
// being reallocated onto a heap that never issued them.
//===----------------------------------------------------------------------===//

static void* defaultAllocate(void* user, size_t size, size_t alignment) {
  (void)user;
  (void)alignment;
  // malloc guarantees ZTYPESET_DEFAULT_ALIGN and no more, so
  // ztypesetAllocFrom refuses anything stricter before reaching any allocator.
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
/// made before the first ztypesetSetAllocator still has an allocator to name.
static ZtypesetAllocator g_default_entry = {
    defaultAllocate,
    defaultReallocate,
    defaultDeallocate,
    NULL,
};

/// The registry: an array of pointers to entries, never of entries by value.
/// The ARRAY moves when it grows; the ENTRIES must not, because FreeType
/// holds one for the life of an FT_Library and a block header names one by
/// index for the life of the block.
static ZtypesetAllocator* g_bootstrap[1] = {&g_default_entry};
static ZtypesetAllocator** g_registry = g_bootstrap;
static size_t g_registry_count = 1u;
static size_t g_registry_capacity = 1u;

/// Which entry ztypesetAlloc hands new blocks to. Never invalid: it is only
/// ever set to an index the registry already holds.
static ZtypesetAllocatorId g_installed = ZTYPESET_ALLOCATOR_DEFAULT;

static bool sameAllocator(const ZtypesetAllocator* a,
                          const ZtypesetAllocator* b) {
  // Field by field rather than memcmp: ZtypesetAllocator has no padding on any
  // ABI ztypeset builds for, but comparing padding bytes that were never
  // written would be undefined behaviour if one ever appeared.
  return a->allocate == b->allocate && a->reallocate == b->reallocate &&
         a->deallocate == b->deallocate && a->user == b->user;
}

/// Index of `alloc` in the registry, adding it if it is not already there.
/// Returns ZTYPESET_ALLOCATOR_NONE if the registry itself could not grow.
static ZtypesetAllocatorId registerAllocator(const ZtypesetAllocator* alloc) {
  for (size_t i = 0u; i < g_registry_count; i++) {
    if (sameAllocator(g_registry[i], alloc)) return (ZtypesetAllocatorId)i;
  }
  // The registry outlives every allocator it describes, so it cannot be
  // allocated through one of them. See the section header.
  ZtypesetAllocator* entry =
      (ZtypesetAllocator*)malloc(sizeof(ZtypesetAllocator));
  if (entry == NULL) return ZTYPESET_ALLOCATOR_NONE;
  *entry = *alloc;

  if (g_registry_count == g_registry_capacity) {
    if (g_registry_capacity > SIZE_MAX / (2u * sizeof(ZtypesetAllocator*))) {
      free(entry);
      return ZTYPESET_ALLOCATOR_NONE;
    }
    const size_t capacity = g_registry_capacity * 2u;
    ZtypesetAllocator** grown =
        (ZtypesetAllocator**)malloc(capacity * sizeof(ZtypesetAllocator*));
    if (grown == NULL) {
      free(entry);
      return ZTYPESET_ALLOCATOR_NONE;
    }
    memcpy(grown, g_registry, g_registry_count * sizeof(ZtypesetAllocator*));
    // The bootstrap array is static storage, so only a grown one is freed.
    if (g_registry != g_bootstrap) free(g_registry);
    g_registry = grown;
    g_registry_capacity = capacity;
  }

  g_registry[g_registry_count] = entry;
  return (ZtypesetAllocatorId)(g_registry_count++);
}

ZtypesetAllocatorId ztypesetInstalledAllocator(void) { return g_installed; }

ZtypesetResult ztypesetSetAllocator(const ZtypesetAllocator* alloc) {
  if (alloc == NULL) {
    g_installed = ZTYPESET_ALLOCATOR_DEFAULT;
    return ZTYPESET_RESULT_OK;
  }
  // Refuse a partial allocator without disturbing the working one: a host that
  // mis-fills the struct should keep running on what it had, not lose its heap.
  if (alloc->allocate == NULL || alloc->deallocate == NULL) {
    return ZTYPESET_RESULT_INVALID_ARGUMENT;
  }
  // The upstreams' process-lifetime caches, populated HERE -- before the swap,
  // so they are charged to whatever was installed before this call and never
  // to the allocator arriving now. They are never freed, so an allocator that
  // issued one can never balance, and "call ztypesetWarmup first" was a rule a
  // host had to know and could only discover by not following it. Idempotent,
  // and free after the first time.
  ztypesetWarmup();
  const ZtypesetAllocatorId id = registerAllocator(alloc);
  // Same bargain: a registry that could not grow leaves the installed
  // allocator exactly as it was.
  if (id == ZTYPESET_ALLOCATOR_NONE) return ZTYPESET_RESULT_OUT_OF_MEMORY;
  g_installed = id;
  return ZTYPESET_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Block header
//
// FreeType, HarfBuzz and SheenBidi all free with a bare pointer. Recording the
// allocation ahead of the payload is what lets ztypeset hand a size and
// alignment back to the host -- see the allocator section of ztypeset.h for why
// that is worth the sixteen bytes -- and it is where the allocator index lives,
// which is what makes the routing above possible.
//===----------------------------------------------------------------------===//

typedef struct ZtypesetBlockHeader {
  /// Total bytes obtained from the host, prefix included.
  size_t total_size;
  /// Registry index of the allocator that issued this block.
  ZtypesetAllocatorId allocator;
  /// Alignment the host was asked for, which is at least the payload's. A
  /// power of two, never above ZTYPESET_DEFAULT_ALIGN, so 32 bits are ample and
  /// the header stays sixteen bytes on a 64-bit target: the allocator index
  /// costs no memory at all.
  uint32_t backing_alignment;
} ZtypesetBlockHeader;

/// A header that cannot be one. Both fields have a small, known range, so a
/// prefix that was overrun, freed twice or never written by ztypeset at all
/// usually fails one of them -- for free, on every deallocation.
///
/// It is a corruption DETECTOR, not a checksum: sixteen bytes leave no room
/// for a magic number, so a garbage header whose two fields happen to be in
/// range still passes. See README.
static bool headerIsPlausible(const ZtypesetBlockHeader* header) {
  if (header->allocator >= g_registry_count) return false;
  const uint32_t alignment = header->backing_alignment;
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) return false;
  if (alignment > (uint32_t)ZTYPESET_DEFAULT_ALIGN) return false;
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
/// Reaching this is a defect in ztypeset, not in a host: since every free is
/// ROUTED through the block's own allocator, a host cannot cause it by
/// swapping allocators. What it catches is ztypeset allocating a block from one
/// place and releasing it from another -- the mutation ci/check-guards.sh
/// plants to prove this is live.
static void ztypesetAllocatorFatal(const char* why, const void* block,
                                unsigned long recorded, unsigned long asked) {
  fprintf(stderr,
          "ztypeset: FATAL: %s (block %p, recorded allocator %lu, released "
          "through allocator %lu). A ztypeset block is freed through the "
          "allocator that made it; see ztypesetSetAllocator in ztypeset.h. The "
          "block was NOT freed and the process is stopping before the wrong "
          "heap is corrupted.\n",
          why, block, recorded, asked);
  fflush(stderr);
  _Exit(ZTYPESET_EXIT_ALLOCATOR_MISMATCH);
}

static size_t backingAlignment(size_t alignment) {
  return alignment < _Alignof(ZtypesetBlockHeader) ?
                              _Alignof(ZtypesetBlockHeader)
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
  size_t minimum = sizeof(ZtypesetBlockHeader);
  if (minimum < _Alignof(ZtypesetBlockHeader)) {
    minimum = _Alignof(ZtypesetBlockHeader);
  }
  return (minimum + alignment - 1u) & ~(alignment - 1u);
}

static ZtypesetBlockHeader* headerOf(void* payload) {
  return (ZtypesetBlockHeader*)((unsigned char*)payload -
                            sizeof(ZtypesetBlockHeader));
}

/// Payload bytes of a live block -- what the caller asked for, rounded up to
/// at least one. Used by the SheenBidi seam to find the tail a grow added.
static size_t ztypesetBlockSize(void* payload) {
  const ZtypesetBlockHeader* header = headerOf(payload);
  return header->total_size - prefixSize(header->backing_alignment);
}

/// The header of a live block, with both checks already made: the header has
/// to be plausible, and if the caller named an allocator it has to be the one
/// on record.
static ZtypesetBlockHeader* checkedHeaderOf(void* block,
                                            ZtypesetAllocatorId asked) {
  ZtypesetBlockHeader* header = headerOf(block);
  if (!headerIsPlausible(header)) {
    ztypesetAllocatorFatal("the block prefix is not a ztypeset "
                           "allocation header",
                        block, (unsigned long)header->allocator,
                        (unsigned long)asked);
  }
  if (asked != ZTYPESET_ALLOCATOR_ANY && header->allocator != asked) {
    ztypesetAllocatorFatal("a block was released through the wrong allocator",
                        block, (unsigned long)header->allocator,
                        (unsigned long)asked);
  }
  return header;
}

void* ztypesetAllocFrom(ZtypesetAllocatorId id, size_t size, size_t alignment) {
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) return NULL;
  // Nothing in ztypeset, FreeType, HarfBuzz or SheenBidi asks for more than
  // malloc's guarantee, and a host allocator is only ever promised that much.
  // Refusing here means an over-aligned request can never be served
  // under-aligned by the default allocator -- it fails visibly instead.
  if (alignment > ZTYPESET_DEFAULT_ALIGN) return NULL;
  if (size == 0u) size = 1u;  // A distinct, freeable pointer, never NULL.

  const ZtypesetAllocator* allocator = g_registry[id];
  const size_t backing = backingAlignment(alignment);
  const size_t prefix = prefixSize(backing);
  if (size > SIZE_MAX - prefix) return NULL;
  const size_t total = prefix + size;

  unsigned char* base =
      (unsigned char*)allocator->allocate(allocator->user, total, backing);
  if (base == NULL) return NULL;

  unsigned char* payload = base + prefix;
  ZtypesetBlockHeader* header = headerOf(payload);
  header->total_size = total;
  header->allocator = id;
  header->backing_alignment = (uint32_t)backing;
  return payload;
}

void* ztypesetCalloc(size_t count, size_t size) {
  if (count != 0u && size > SIZE_MAX / count) return NULL;
  const size_t total = count * size;
  void* block = ztypesetAlloc(total, ZTYPESET_DEFAULT_ALIGN);
  if (block != NULL) memset(block, 0, total == 0u ? 1u : total);
  return block;
}

void ztypesetFreeFrom(ZtypesetAllocatorId id, void* block) {
  if (block == NULL) return;
  const ZtypesetBlockHeader* header = checkedHeaderOf(block, id);
  const ZtypesetAllocator* allocator = g_registry[header->allocator];
  const size_t total = header->total_size;
  const size_t backing = header->backing_alignment;
  unsigned char* base = (unsigned char*)block - prefixSize(backing);
  allocator->deallocate(allocator->user, base, total, backing);
}

void* ztypesetReallocFrom(ZtypesetAllocatorId id, void* block, size_t new_size,
                       size_t alignment) {
  if (block == NULL) {
    return ztypesetAllocFrom(id == ZTYPESET_ALLOCATOR_ANY ? g_installed : id,
                          new_size, alignment);
  }
  if (new_size == 0u) new_size = 1u;

  const ZtypesetBlockHeader* header = checkedHeaderOf(block, id);
  // The block keeps its own allocator across a grow. Handing a grown block to
  // a different heap than the one that issued it is the same defect as
  // freeing it there, one step later.
  const ZtypesetAllocatorId owner = header->allocator;
  const ZtypesetAllocator* allocator = g_registry[owner];
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
      ZtypesetBlockHeader* moved_header = headerOf(payload);
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
  void* fresh = ztypesetAllocFrom(owner, new_size, backing);
  if (fresh == NULL) return NULL;
  const size_t old_payload = old_total - prefix;
  memcpy(fresh, block, old_payload < new_size ? old_payload : new_size);
  ztypesetFreeFrom(owner, block);
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

void* ztypesetAlloc(size_t size, size_t alignment) {
  return ztypesetAllocFrom(g_installed, size, alignment);
}

void* ztypesetRealloc(void* block, size_t new_size, size_t alignment) {
  return ztypesetReallocFrom(ZTYPESET_ALLOCATOR_ANY, block, new_size,
                             alignment);
}

void ztypesetFree(void* block) { ztypesetFreeFrom(ZTYPESET_ALLOCATOR_ANY,
                  block); }

ZtypesetAllocatorId ztypesetAllocatorOf(void* block) {
  return checkedHeaderOf(block, ZTYPESET_ALLOCATOR_ANY)->allocator;
}

//===----------------------------------------------------------------------===//
// FreeType's seam
//
// Per FT_Library. FreeType hands every allocation call the FT_Memory it was
// built with, and ztypeset points that at the owning library, so a library's
// FreeType memory names the library's allocator entry rather than whatever is
// installed at the time.
//===----------------------------------------------------------------------===//

static ZtypesetAllocatorId memoryAllocator(FT_Memory memory) {
  return ((const ZtypesetLibrary*)memory->user)->allocator;
}

static void* ftAlloc(FT_Memory memory, long size) {
  if (size <= 0) return NULL;
  return ztypesetAllocFrom(memoryAllocator(memory), (size_t)size,
                        ZTYPESET_DEFAULT_ALIGN);
}

static void ftFree(FT_Memory memory, void* block) {
  ztypesetFreeFrom(memoryAllocator(memory), block);
}

static void* ftRealloc(FT_Memory memory, long cur_size, long new_size,
                       void* block) {
  // cur_size is FreeType's idea of the old size; the block header is
  // authoritative, so it is deliberately ignored rather than trusted.
  (void)cur_size;
  const ZtypesetAllocatorId id = memoryAllocator(memory);
  if (new_size <= 0) {
    ztypesetFreeFrom(id, block);
    return NULL;
  }
  return ztypesetReallocFrom(id, block, (size_t)new_size,
                             ZTYPESET_DEFAULT_ALIGN);
}

void ztypesetInitFtMemory(ZtypesetLibrary* library) {
  // Recorded now, so a later ztypesetSetAllocator cannot redirect the memory
  // this library has already handed to FreeType -- and, because the registry
  // entry outlives the allocator, the library stays able to free its own blocks
  // even after the host has moved on.
  library->allocator = ztypesetInstalledAllocator();
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
// "C". Compile-time, and therefore process-wide -- see ztypeset.h.
//===----------------------------------------------------------------------===//

void* ztypeset_hb_malloc(size_t size);
void* ztypeset_hb_calloc(size_t count, size_t size);
void* ztypeset_hb_realloc(void* block, size_t size);
void ztypeset_hb_free(void* block);

void* ztypeset_hb_malloc(size_t size) {
  return ztypesetAlloc(size, ZTYPESET_DEFAULT_ALIGN);
}

void* ztypeset_hb_calloc(size_t count, size_t size) {
  return ztypesetCalloc(count, size);
}

void* ztypeset_hb_realloc(void* block, size_t size) {
  return ztypesetRealloc(block, size, ZTYPESET_DEFAULT_ALIGN);
}

void ztypeset_hb_free(void* block) { ztypesetFree(block); }

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
/// ztypeset may not patch a vendored upstream (see UPSTREAM.md), and there is
/// no route to that failure path that does not go through this function, so the
/// containment belongs here: SheenBidi never sees a byte ztypeset has not
/// written. tests/c_smoke.c's poisoning arm holds it -- remove the memset and
/// that arm dies every run rather than one run in fifty.
static void* sbAllocateBlock(SBUInteger size, void* info) {
  (void)info;
  void* block = ztypesetAlloc((size_t)size, ZTYPESET_DEFAULT_ALIGN);
  if (block != NULL) memset(block, 0, (size_t)size);
  return block;
}

/// The grown tail gets the same treatment, for the same reason: a block that
/// SheenBidi has already used is fully initialised as far as it wrote, and
/// everything past that is fresh memory it may read before writing.
static void* sbReallocateBlock(void* pointer, SBUInteger new_size, void* info) {
  (void)info;
  const size_t old_payload = pointer == NULL ? 0u : ztypesetBlockSize(pointer);
  void* block = ztypesetRealloc(pointer, (size_t)new_size,
                                ZTYPESET_DEFAULT_ALIGN);
  if (block != NULL && (size_t)new_size > old_payload) {
    memset((unsigned char*)block + old_payload, 0,
           (size_t)new_size - old_payload);
  }
  return block;
}

static void sbDeallocateBlock(void* pointer, void* info) {
  (void)info;
  ztypesetFree(pointer);
}

/// Created once and kept for the life of the process.
///
/// SBAllocatorSetDefault stores the pointer without retaining it, so ztypeset
/// has to hold the reference. One instance is enough for any number of
/// ztypesetSetAllocator calls, because the three functions above dispatch
/// through whichever allocator is installed at the time rather than capturing
/// one.
static SBAllocatorRef g_sb_allocator = NULL;

/// What makes "once" true when several threads ask at the same instant.
///
///   0 -- nobody has tried, or a try ran out of memory
///   1 -- one thread is inside SBAllocatorCreate
///   2 -- installed; g_sb_allocator is safe to read
///
/// g_sb_allocator is written before the state moves to 2 and never again, so
/// the release/acquire pair on the state is what publishes it. A plain
/// check-then-set stood here, and the paragraph and shaping paths both reach
/// it -- on threads the header explicitly invites, since a ZtypesetLibrary per
/// thread is what it asks for.
static _Atomic int g_sb_install_state = 0;

ZtypesetResult ztypesetInstallSheenbidiAllocator(void) {
  if (atomic_load_explicit(&g_sb_install_state, memory_order_acquire) == 2) {
    return ZTYPESET_RESULT_OK;
  }

  int idle = 0;
  if (!atomic_compare_exchange_strong_explicit(&g_sb_install_state, &idle, 1,
                                               memory_order_acq_rel,
                                               memory_order_acquire)) {
    // Another thread got there first. Wait for it rather than installing a
    // second allocator: the hazard is not the duplicate object, it is
    // SBAllocatorSetDefault -- a process-wide store SheenBidi reads without
    // synchronisation, from calls already in flight. Two of those racing is
    // what a check-then-set allows.
    //
    // The wait is bounded by one SBAllocatorCreate, which is one small
    // allocation, and every caller here is about to do far more work than
    // that. It happens at most once per process.
    while (atomic_load_explicit(&g_sb_install_state, memory_order_acquire) ==
           1) {
    }
    return atomic_load_explicit(&g_sb_install_state, memory_order_acquire) == 2
               ? ZTYPESET_RESULT_OK
               : ZTYPESET_RESULT_OUT_OF_MEMORY;
  }

  SBAllocatorProtocol protocol;
  memset(&protocol, 0, sizeof(protocol));
  protocol.allocateBlock = sbAllocateBlock;
  protocol.reallocateBlock = sbReallocateBlock;
  protocol.deallocateBlock = sbDeallocateBlock;
  // allocateScratch and resetScratch stay NULL: SheenBidi then serves scratch
  // out of allocateBlock, which keeps every byte visible to the host's
  // accounting. A host wanting a thread-local scratch pool can still have one
  // by making its own allocator do that.

  SBAllocatorRef made = SBAllocatorCreate(&protocol, NULL);
  if (made == NULL) {
    // Back to 0, so a later call may try again: an allocation that failed
    // once under pressure is not a permanent property of the process, and a
    // state machine that latched on failure would turn one bad moment into a
    // library that can never lay out bidirectional text again.
    atomic_store_explicit(&g_sb_install_state, 0, memory_order_release);
    return ZTYPESET_RESULT_OUT_OF_MEMORY;
  }

  g_sb_allocator = made;
  SBAllocatorSetDefault(made);
  atomic_store_explicit(&g_sb_install_state, 2, memory_order_release);
  return ZTYPESET_RESULT_OK;
}

//===----------------------------------------------------------------------===//
// Versions
//===----------------------------------------------------------------------===//

static uint32_t pack(uint32_t major, uint32_t minor, uint32_t patch) {
  return (major << 16) | (minor << 8) | patch;
}

uint32_t ztypesetVersion(void) {
  return pack(ZTYPESET_VERSION_MAJOR, ZTYPESET_VERSION_MINOR,
              ZTYPESET_VERSION_PATCH);
}

uint32_t ztypesetFreetypeVersion(void) {
  return pack(FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
}

uint32_t ztypesetHarfbuzzVersion(void) {
  return pack(HB_VERSION_MAJOR, HB_VERSION_MINOR, HB_VERSION_MICRO);
}

uint32_t ztypesetSheenbidiVersion(void) {
  return pack(SHEENBIDI_VERSION_MAJOR, SHEENBIDI_VERSION_MINOR,
              SHEENBIDI_VERSION_PATCH);
}

uint32_t ztypesetUnibreakVersion(void) {
  // libunibreak packs its version as one hex integer -- 0x0700 for 7.0 -- so
  // unlike the other three there are no separate major/minor macros to read.
  return pack((unsigned)((UNIBREAK_VERSION >> 8) & 0xFFu),
              (unsigned)(UNIBREAK_VERSION & 0xFFu), 0u);
}

//===----------------------------------------------------------------------===//
// Results
//===----------------------------------------------------------------------===//

const char* ztypesetResultName(ZtypesetResult result) {
  switch (result) {
    case ZTYPESET_RESULT_OK:
      return "ok";
    case ZTYPESET_RESULT_OUT_OF_MEMORY:
      return "out of memory";
    case ZTYPESET_RESULT_INVALID_ARGUMENT:
      return "invalid argument";
    case ZTYPESET_RESULT_INVALID_TEXT:
      return "text not well-formed in its encoding";
    case ZTYPESET_RESULT_BAD_FONT:
      return "bad font";
    case ZTYPESET_RESULT_UNSUPPORTED:
      return "unsupported font format";
    case ZTYPESET_RESULT_GLYPH_NOT_FOUND:
      return "glyph not found";
    case ZTYPESET_RESULT_RENDER_FAILED:
      return "render failed";
    case ZTYPESET_RESULT_SHAPE_FAILED:
      return "shape failed";
    case ZTYPESET_RESULT_BIDI_FAILED:
      return "bidi failed";
    case ZTYPESET_RESULT_BUFFER_TOO_SMALL:
      return "buffer too small";
  }
  // Not reachable through the enum, but a caller can cast anything into it.
  return "unknown result";
}

//===----------------------------------------------------------------------===//
// Generations
//===----------------------------------------------------------------------===//

int32_t ztypesetToFixed266(float pixels) {
  if (!(pixels > 0.0f) || !(pixels <= 16384.0f)) return 0;
  return (int32_t)(pixels * 64.0f + 0.5f);
}

uint64_t ztypesetNextGeneration(void) {
  // Process-wide, and ztypeset.h tells callers to use one ZtypesetLibrary PER
  // THREAD -- so two threads bumping this at the same instant is the usage
  // the header asks for, not an abuse of it.
  //
  // What stood here argued that a torn increment "only ever produces a value
  // that fails to match, which is the safe direction". The arithmetic is
  // right: a lost update leaves the counter at old+1 twice, so a newly issued
  // generation still exceeds every generation already issued and a stale
  // shaper still refuses. The argument is about the wrong thing. A plain
  // read-modify-write on an object two threads reach is a DATA RACE, and a
  // data race is undefined behaviour in C11 whatever the machine would have
  // done -- the compiler may assume it cannot happen, and may keep the
  // counter in a register across a call whose whole body it can see.
  //
  // Relaxed is the entire requirement. Nothing is published through this
  // counter; a reader only ever asks "is this the value I recorded?", so it
  // has to be unique, not ordered against anything else.
  static _Atomic uint64_t counter = 0u;
  return atomic_fetch_add_explicit(&counter, 1u, memory_order_relaxed) + 1u;
}

//===----------------------------------------------------------------------===//
// Error detail
//===----------------------------------------------------------------------===//

#define ZTYPESET_DETAIL_MAX 160

static _Thread_local char g_detail[ZTYPESET_DETAIL_MAX] = {0};

void ztypesetSetErrorDetail(const char* detail) {
  if (detail == NULL) {
    g_detail[0] = '\0';
    return;
  }
  size_t i = 0;
  while (i + 1u < ZTYPESET_DETAIL_MAX && detail[i] != '\0') {
    g_detail[i] = detail[i];
    i++;
  }
  g_detail[i] = '\0';
}

const char* ztypesetLastErrorDetail(void) { return g_detail; }

ZtypesetResult ztypesetFromFtError(FT_Error error) {
  if (error == FT_Err_Ok) return ZTYPESET_RESULT_OK;

  const char* string = FT_Error_String(error);
  ztypesetSetErrorDetail(string != NULL ? string : "FreeType error");

  switch (error) {
    case FT_Err_Out_Of_Memory:
      return ZTYPESET_RESULT_OUT_OF_MEMORY;
    case FT_Err_Invalid_Argument:
    case FT_Err_Invalid_Face_Handle:
    case FT_Err_Invalid_Size_Handle:
    case FT_Err_Invalid_Library_Handle:
      return ZTYPESET_RESULT_INVALID_ARGUMENT;
    case FT_Err_Invalid_Glyph_Index:
      return ZTYPESET_RESULT_GLYPH_NOT_FOUND;
    // FreeType reports "unknown file format" both for a format this build has
    // no driver for and for bytes that are not a font at all, which are worth
    // telling apart. ztypeset_face.c sniffs the known-but-not-compiled formats
    // before FreeType sees them and reports ZTYPESET_RESULT_UNSUPPORTED itself,
    // so anything reaching here really is unrecognisable.
    case FT_Err_Unknown_File_Format:
      return ZTYPESET_RESULT_BAD_FONT;
    case FT_Err_Missing_Module:
      return ZTYPESET_RESULT_UNSUPPORTED;
    default:
      return ZTYPESET_RESULT_BAD_FONT;
  }
}

//===----------------------------------------------------------------------===//
// Text
//===----------------------------------------------------------------------===//

size_t ztypesetEncodingUnitSize(ZtypesetEncoding encoding) {
  switch (encoding) {
    case ZTYPESET_ENCODING_UTF8:
      return 1u;
    case ZTYPESET_ENCODING_UTF16:
      return 2u;
    case ZTYPESET_ENCODING_UTF32:
      return 4u;
    default:
      // Not an encoding this build names. Reported as a size of zero rather
      // than defaulted to one, so a caller compiled against a newer header gets
      // ZTYPESET_RESULT_INVALID_ARGUMENT instead of its text read as UTF-8.
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

bool ztypesetTextIsWellFormed(const void* text, size_t length,
                           ZtypesetEncoding encoding) {
  // A zero length is well-formed in every encoding, including with a NULL
  // pointer: an empty label is not a malformed one.
  if (length == 0u) return true;
  if (text == NULL) return false;
  switch (encoding) {
    case ZTYPESET_ENCODING_UTF8:
      return isValidUtf8((const char*)text, length);
    case ZTYPESET_ENCODING_UTF16:
      return isValidUtf16((const uint16_t*)text, length);
    case ZTYPESET_ENCODING_UTF32:
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
  //
  // `length - 1u` cannot underflow here: ztypesetTextDecode refuses an index at
  // or past the end, so this is never entered with a length of zero. That
  // was not true before -- and an underflow turns this bound into one that
  // can never fire, which is worse than not having it.
  if (extra > length - 1u) {
    *out = 0xFFFDu;
    return length;
  }

  for (size_t k = 1u; k <= extra; k++) code = (code << 6) | (p[k] & 0x3Fu);
  *out = code;
  return extra + 1u;
}

size_t ztypesetTextDecode(const void* text, size_t length,
                       ZtypesetEncoding encoding, size_t index, uint32_t* out) {
  // The one bound, for all three encodings, before the switch.
  //
  // Every branch below reads the unit at `index` before it can know anything
  // about the character there, so the check cannot live inside the switch;
  // and it cannot live in the callers, because "no caller passes the end" is
  // a property of today's callers rather than of this function. The UTF-8
  // path was the sharp edge: it computed `length - index`, which underflows
  // to SIZE_MAX at the end, so its own continuation bound could never fire
  // and it read the lead byte plus up to three more past the buffer.
  //
  // U+FFFD and a step of ONE, never zero: the contract this function is
  // written around is that a loop over it terminates.
  if (index >= length) {
    *out = 0xFFFDu;
    return 1u;
  }

  switch (encoding) {
    case ZTYPESET_ENCODING_UTF16: {
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
    case ZTYPESET_ENCODING_UTF32:
      *out = ((const uint32_t*)text)[index];
      return 1u;
    case ZTYPESET_ENCODING_UTF8:
    default:
      return decodeUtf8((const char*)text + index, length - index, out);
  }
}

bool ztypesetTextSplitsCharacter(const void* text, size_t length,
                              ZtypesetEncoding encoding, size_t index) {
  if (index >= length) return false;
  switch (encoding) {
    case ZTYPESET_ENCODING_UTF16: {
      // Validated text has no unpaired low surrogate, so a low surrogate is
      // always the second half of a pair -- and a boundary there is inside a
      // character.
      const uint16_t unit = ((const uint16_t*)text)[index];
      return unit >= 0xDC00u && unit <= 0xDFFFu;
    }
    case ZTYPESET_ENCODING_UTF32:
      // One unit, one character: no index can be inside one.
      return false;
    case ZTYPESET_ENCODING_UTF8:
    default:
      return (((const unsigned char*)text)[index] & 0xC0u) == 0x80u;
  }
}

//===----------------------------------------------------------------------===//
// Growable array
//===----------------------------------------------------------------------===//

bool ztypesetArrayReserve(ZtypesetAllocatorId owner, ZtypesetArray* array,
                          size_t count,
                       size_t element_size) {
  if (count <= array->capacity) return true;

  size_t capacity = array->capacity == 0u ? 16u : array->capacity;
  while (capacity < count) {
    if (capacity > SIZE_MAX / 2u) return false;
    capacity *= 2u;
  }
  if (capacity > SIZE_MAX / element_size) return false;

  void* grown = ztypesetReallocFrom(owner, array->data, capacity * element_size,
                                 ZTYPESET_DEFAULT_ALIGN);
  if (grown == NULL) return false;

  array->data = grown;
  array->capacity = capacity;
  return true;
}

void ztypesetArrayFree(ZtypesetAllocatorId owner, ZtypesetArray* array,
                    size_t element_size) {
  (void)element_size;
  ztypesetFreeFrom(owner, array->data);
  array->data = NULL;
  array->count = 0u;
  array->capacity = 0u;
}
