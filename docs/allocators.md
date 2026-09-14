# Allocators

The full account of the allocator seam. The README carries the rules a caller
has to follow; this page carries the reasoning and the failures behind them.

## Three seams, one of them global

Three of the four upstreams allocate, all three allow it to be redirected, and
all three do it differently. libunibreak allocates nothing at all — its tables
are static and its results go into a buffer the caller provides — so there is
no fourth seam.

| | Seam | Scope |
|---|---|---|
| FreeType | `FT_MemoryRec` per `FT_Library` | per `Library` |
| SheenBidi | a global default allocator object | process-wide |
| HarfBuzz | four macros resolved at compile time | process-wide |

HarfBuzz is the binding constraint, so `setAllocator` is process-wide. That is
surfaced rather than hidden behind a per-object parameter that could not be
honoured.

FreeType is the exception, and it is genuinely per-library: ztypeset builds it
with `FT_New_Library` rather than `FT_Init_FreeType`, records the installed
allocator in the `Library` at creation, and points FreeType's `FT_Memory` at
that record. So a `Library` and its faces keep allocating and freeing through
the allocator they were born with even if the process-wide one is replaced
underneath them. A test in `tests/c_smoke.c` proves it by swapping the global
mid-life and watching which allocator the FreeType traffic goes to — the claim
was false once, in a way nothing noticed, because every test installed one
allocator and kept it.

## Free without a size

All three seams free without a size — `FT_Free_Func`, `hb_free_impl` and
`SBAllocatorDeallocateBlockFunc` each receive only a pointer. Rather than push
that asymmetry onto every host, ztypeset records the size and alignment in a
header ahead of each block and hands them back on `deallocate`. A host with a
sized allocator — Zig's `std.mem.Allocator`, a pool, an arena with accounting —
therefore needs no bookkeeping of its own. It costs two pointer-sized words per
allocation, sixteen bytes on a 64-bit target, and it is why the Zig bridge is
three straight calls into `std.mem.Allocator` rather than a shadow table.

## Every block is freed through the allocator that made it

That sentence used to be a rule in a comment, and the rule was not kept.
HarfBuzz's seam is compile-time and therefore process-wide, so an `hb_face_t`
allocated under one installed allocator was destroyed under whichever one was
installed later — while the FreeType memory of the same font went back to the
right one. One handle, two heaps, and nothing that could tell you.

So ztypeset does not rely on the rule being kept. Every allocator ever
installed gets an entry in a small registry, the block header records the index
of the one that issued the block, and every free and every grow is routed back
to that entry rather than to whatever is installed at the time. The index
shares the sixteen bytes the size and alignment already occupied, so it costs
no memory at all.

What that buys:

- Swapping the process-wide allocator with live handles is safe, not undefined.
  `resetAllocator` has a precondition a host can actually meet.
- The upstreams' process-lifetime caches — HarfBuzz's language intern table is
  the one that grows — are reallocated through the allocator that made them,
  not through whichever came next.
- A ztypeset-internal mistake, a block allocated in one place and released by
  naming another, stops the process with both allocators named on stderr and
  exit code `ZTYPESET_EXIT_ALLOCATOR_MISMATCH` (70). The block is not freed:
  leaking one block is recoverable, handing a pointer to a heap that never
  issued it is not. `ci/check-guards.sh` plants that mistake to prove the check
  is live.

What it costs: one `ZtypesetAllocator` per distinct allocator ever installed
(installing the same one twice reuses its entry), allocated with `malloc` and
never freed, because it has to outlive the last block it issued. That is the
only allocation ztypeset makes outside the installed allocator.

## What the header check does and does not catch

The header check is a detector, not a checksum: sixteen bytes leave no room for
a magic number, so a prefix that was overrun into garbage whose two fields
happen to be in range still passes. What it does catch, for free on every
deallocation, is an allocator index past the end of the registry and an
alignment that is not a power of two at or below `max_align_t`'s.

## A declined reallocate is not an allocation failure

`reallocate` is optional, and returning null from it means "I decline", not
"out of memory" — ztypeset falls back to allocate-copy-free either way.

That distinction is load-bearing, and it was found the hard way. Zig's
`std.mem.Allocator` only resizes in place and returns null the moment a block
must move. ztypeset reported that upward as an allocation failure, and HarfBuzz
treats a failed allocation as a reason to abandon optional work and carry on.
The symptom was not a crash or an error: it was Arabic rendered as unjoined
nominal letters, correct-looking and completely wrong, only on hosts with a
custom allocator.

## Warming

`setAllocator` calls `warmup` before it installs anything, so a host never has
to know to. It populates the process-global caches the upstreams never free
before exit, and a tracking allocator installed afterwards therefore sees a
balanced heap. Two caches are out of its reach because both need a real face:
shape one throwaway run before installing if you audit and use either.
