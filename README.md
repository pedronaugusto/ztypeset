# ztext

[![CI](https://github.com/pedronaugusto/ztext/actions/workflows/ci.yml/badge.svg)](https://github.com/pedronaugusto/ztext/actions/workflows/ci.yml)

Text shaping and glyph rasterisation for Zig: [FreeType](https://freetype.org),
[HarfBuzz](https://harfbuzz.github.io),
[SheenBidi](https://github.com/Tehreer/SheenBidi) and
[libunibreak](https://github.com/adah1972/libunibreak) vendored, pinned and
wired together, with no renderer, no atlas and no layout engine attached.

- Fonts load **from memory**. There is no path-based API at all — a host
  feeding fonts out of an asset pack has bytes, not paths — and a font parsed
  once serves every size you draw it at.
- Shape a run of **UTF-8, UTF-16 or UTF-32** → glyph ids, advances, offsets,
  and a cluster map in **code-unit offsets** of whichever encoding you passed.
  All three upstreams take all three natively, so ztext transcodes nothing.
- Bidi paragraph → ordered visual runs, plus script itemisation, so mixed text
  can be split into runs a shaper can actually take — and **per line**, because
  UAX #9 applies rules L1 and L2 over a line and not over a paragraph.
- **Where a line may break** (UAX #14), and where graphemes and words end
  (UAX #29) — so a host can actually wrap, move a caret, and select a word.
- Rasterise → A8 coverage or FreeType's native SDF, both **measured** below.
- Host allocator injection across every upstream that allocates. (One does
  not: libunibreak's tables are static and its results go into your buffer.)
- Drift between the C header and the Zig externs is a **build failure**, not a
  memory-corruption bug — checked by reflection over every declaration, and
  the check itself is [broken on purpose in CI](#do-the-guards-actually-fail)
  to prove it can fail.

Status: **v0.1**. See [Scope](#scope) for what is deliberately absent.

## Usage

```zig
const ztext = @import("ztext");

// Copied, not borrowed: ztext keeps its own copy for as long as any handle
// can reach it. Installing also warms the caches the upstreams keep for the
// life of the process, so this allocator only ever sees ztext's working set.
try ztext.setAllocator(gpa_state.allocator());
defer ztext.resetAllocator();

const library = try ztext.Library.init();
defer library.deinit();

// The bytes are BORROWED and must outlive the font. Handles have no
// destruction order: whichever of a pair goes second frees what they share.
const font = try library.createFont(font_bytes, 0);
defer font.deinit();

// A face is the font at one size. Make one per size you draw; a second size
// costs a size, not another parse.
const face = try font.face(0, 16);
defer face.deinit();

const shaper = try ztext.Shaper.init();
defer shaper.deinit();

const paragraph = try ztext.Paragraph.init(text, .{});
defer paragraph.deinit();

// shapingRuns, not visualRuns: one visual run can span several scripts, and
// HarfBuzz shapes one script at a time.
for (paragraph.shapingRuns()) |run| {
    // shapeRun, not shape: the PARAGRAPH owns the text, so a run can only be
    // applied to the text it came from, HarfBuzz sees the characters either
    // side of the run, direction and script come from the run itself, and
    // text a paragraph already validated is not validated again per run.
    const glyphs = try shaper.shapeRun(face, paragraph, run, .{});
    for (glyphs) |glyph| {
        const bitmap = try face.renderGlyph(glyph.glyph_id, .a8, .light, 0, 0);
        // ... into your atlas, before the next call on this face.
        _ = bitmap;
    }
}
```

Pick the face per run from `run.script` when you carry more than one; ztext has
no fallback chain of its own.

Both blocks on this page are quoted from
[`examples/quickstart.zig`](examples/quickstart.zig), which `zig build test`
compiles and runs; a test fails if they stop matching it. They were two copies
before that, and the two disagreed — see the defect list below.

If the text wraps, ask where it may break, decide with your own width, and
iterate a `Line` per visual line:

```zig
const breaks = paragraph.lineBreaks(); // one entry per code unit
var start: usize = 0;
while (start < text.len) {
    // Furthest permitted break that still fits. ztext says where a break is
    // ALLOWED; only you know how wide the box is.
    const end = chooseBreak(breaks, start, box_width);

    const line = try paragraph.line(start, end - start);
    defer line.deinit();
    for (line.shapingRuns()) |run| {
        // Shaped and rendered exactly as above, per line this time.
        const glyphs = try shaper.shapeRun(face, paragraph, run, .{});
        _ = glyphs;
    }
    start = end;
}
```

`Line` is not a convenience wrapper: UAX #9 applies rules L1 and L2 per line,
so `paragraph.shapingRuns()` is the right answer only when the paragraph *is*
one line. And for a caret, `paragraph.nextGrapheme(offset)` — not the next
character, which lands between a letter and its accent.

Add it as a dependency and link the module:

```zig
const ztext_dep = b.dependency("ztext", .{ .target = target, .optimize = optimize });
exe.root_module.addImport("ztext", ztext_dep.module("ztext"));
```

FreeType, HarfBuzz and SheenBidi are also exposed as artifacts with their own
headers installed, so a C or C++ part of your program can use them directly:

```zig
exe.root_module.linkLibrary(ztext_dep.artifact("harfbuzz"));
```

That is deliberate. For anything past ztext's scope, HarfBuzz's own API is
better documented and more capable than a façade over it could be.

What is installed is exactly what was compiled. ztext builds a reduced
configuration of each upstream — FreeType without the modules it does not
register, and `libharfbuzz` without the four separate libraries upstream builds
beside it (`-subset`, `-raster`, `-vector`, `-gpu`, each with its own external
dependency) — so the headers for those are not installed either.
`ci/header-link.sh` is what makes that sentence true rather than intended: it
compiles every installed header, checks each is reachable from a documented
root, and references every entry point any of them declares so the linker has
to resolve it against the libraries installed beside them. It runs on both
Windows ABIs.

## Design

### The pipeline stops at runs

Correct text goes **order → itemise → shape → rasterise**, and ztext does all
four — then stops. It has no atlas, no line breaking, no justification, no
caret model and no rich text, because those belong to a layout engine that
knows about your UI, and folding them in is what makes a text library
unreusable.

What that means concretely: `Paragraph` gives you **shaping runs** — visual
runs intersected with script runs, each uniform in direction and script, in
visual order — and you call `shapeRun` once per run, handing it the paragraph
the run came from. Nothing here shapes a paragraph for you.

`shapeRun` takes the paragraph rather than the text on purpose. A run is a pair
of offsets, and offsets are only meaningful against the buffer they were
computed from: passing a *slice* of that buffer together with offsets computed
against the whole is the single most common way to get this wrong, and it
produces text that is almost right. With the paragraph as the argument there is
no second buffer to disagree with. It also settles two other things — the run
carries the direction and the script, so `ShapeParams` must not also carry
them, and the paragraph's text was validated when it was created, so it is not
walked again for every run.

`shape` and `shapeRange` remain for text a host has *not* made a paragraph of:
one string, or one range of one string with the rest as context, with the
direction and script the host names.

Where a host has already decided the breaks, `Line` reorders one range of a
paragraph on its own terms. That is a correctness boundary, not a convenience:
UAX #9 resolves embedding levels over the paragraph but applies rules L1 and L2
per *line*, so a paragraph's run list is only the right answer when the
paragraph is one line. The difference shows up as trailing whitespace between
two right-to-left words, which the paragraph puts in the middle and the line
puts at the end — an indent on the wrong side, visible only when the text wraps
and only in a right-to-left locale.

The intersection is provided rather than left to the caller because it is the
one part of itemisation that is easy to get subtly wrong. Visual runs are in
visual order and script runs are in logical order, so inside a right-to-left
visual run the script pieces have to come out backwards; and a single visual
run can span three scripts, which HarfBuzz will happily shape as one and get
wrong. The raw `visualRuns()` and `scriptRuns()` are still exposed for callers
doing something else with them.

Two things this boundary implies, said plainly because they are cheaper to
learn here than during an integration:

- **Bidi mirroring is already done, and needs no API.** Rule L4 — the one that
  turns `(` into `)` inside a right-to-left run — is applied by HarfBuzz
  itself, at shaping time, before OpenType features run
  (`hb_ot_rotate_chars`). ztext neither duplicates it nor exposes a hook for
  it, and a test pins the behaviour so a re-vendor that changed it would fail
  rather than silently produce backwards brackets.
- **Segmentation is in scope, and that is a deliberate line.** It would be
  easy to call line breaking "the host's job", but that is arbitrary: ztext
  already owns UAX #9, and `Line` takes a code-unit range a host would
  otherwise have no way to find. FreeType, HarfBuzz and SheenBidi implement none of
  UAX #14 or #29 between them, so ztext vendors libunibreak and provides break
  *opportunities*. Deciding where to break needs a width, and that stays the
  host's.
- **Which passes run is the caller's choice, because they are the largest
  thing a paragraph holds.** `Paragraph.init(text, .{ .segmentation = ... })`
  takes an OR of `lines`, `graphemes` and `words`, and the default is all
  three. On a 4300-character paragraph the three passes are **56.6% of the
  time** to build it (265 µs against 115 µs) and **12 916 B of its 26 898 B** —
  more than the copied text and the embedding levels together. A single-line
  label that only wants bidi runs asks for none; a wrapping text box asks for
  `lines`. The accessor for a pass that was not run returns an empty slice,
  and `segmentation()` says which ran, so an empty array is never mistaken
  for a text with no boundaries.

### Why there is a C layer at all

Not because Zig cannot call the upstreams. All three expose C APIs and Zig
calls C natively; `build.zig` installs their headers precisely so you can.

It exists because FreeType's `FT_FaceRec` and `FT_GlyphSlotRec` are large,
deeply nested, and **partly conditional on FreeType's own configuration
macros**. Hand-mirroring those as Zig `extern struct`s would put silent memory
corruption one re-vendor away. Stopping them at a C boundary means the C
compiler checks every upstream layout change for free, and only small, flat,
ztext-owned structs ever cross into Zig.

So `ffi/ztext.h` is scoped to that job. It is not an attempt to re-present
HarfBuzz to the world under a different name.

### Allocator injection — three seams, one of them global

Three of the four upstreams allocate, all three allow it to be redirected, and
all three do it differently. (libunibreak allocates nothing at all, so there is
no fourth seam to describe.)

| | Seam | Scope |
|---|---|---|
| FreeType | `FT_MemoryRec` per `FT_Library` | **per `Library`** |
| SheenBidi | a global default allocator object | process-wide |
| HarfBuzz | four macros resolved at compile time | process-wide |

HarfBuzz is the binding constraint, so `setAllocator` is process-wide. That is
surfaced rather than hidden behind a per-object parameter that could not be
honoured.

FreeType is the exception, and it is genuinely per-library: ztext builds it
with `FT_New_Library` rather than `FT_Init_FreeType`, records the installed
allocator in the `Library` at creation, and points FreeType's `FT_Memory` at
that record. So a `Library` and its faces keep allocating and freeing through
the allocator they were born with even if the process-wide one is replaced
underneath them. A test in `tests/c_smoke.c` proves it by swapping the global
mid-life and watching which allocator the FreeType traffic goes to — the claim
was false once, in a way nothing noticed, because every test installed one
allocator and kept it.

All three also **free without a size** — `FT_Free_Func`, `hb_free_impl` and
`SBAllocatorDeallocateBlockFunc` each receive only a pointer. Rather than push
that asymmetry onto every host, ztext records the size and alignment in a
header ahead of each block and hands them back on `deallocate`. A host with a
sized allocator — Zig's `std.mem.Allocator`, a pool, an arena with accounting —
therefore needs no bookkeeping of its own. It costs two pointer-sized words per
allocation — sixteen bytes on a 64-bit target — and it is why the Zig bridge is
three straight calls into `std.mem.Allocator` rather than a shadow table.

### Every block is freed through the allocator that made it

That sentence used to be a rule in a comment, and the rule was not kept.
HarfBuzz's seam is compile-time and therefore process-wide, so an `hb_face_t`
allocated under one installed allocator was destroyed under whichever one was
installed later — while the FreeType memory of the *same font* went back to the
right one. One handle, two heaps, and nothing that could tell you.

So ztext does not rely on the rule being kept. Every allocator ever installed
gets an entry in a small registry, the block header records the **index** of
the one that issued the block, and every free and every grow is routed back to
that entry rather than to whatever is installed at the time. The index shares
the sixteen bytes the size and alignment already occupied, so it costs no
memory at all.

What that buys, concretely:

- Swapping the process-wide allocator with live handles is **safe**, not
  undefined. `resetAllocator` has a precondition a host can actually meet.
- The upstreams' process-lifetime caches — HarfBuzz's language intern table is
  the one that grows — are reallocated through the allocator that made them,
  not through whichever came next.
- A ztext-internal mistake, a block allocated in one place and released by
  naming another, **stops the process** with both allocators named on stderr
  and exit code `ZTEXT_EXIT_ALLOCATOR_MISMATCH` (70). The block is not freed:
  leaking one block is recoverable, handing a pointer to a heap that never
  issued it is not. `ci/check-guards.sh` plants that mistake to prove the check
  is live.

What it costs: one `ZtextAllocator` per **distinct** allocator ever installed
(installing the same one twice reuses its entry), allocated with `malloc` and
never freed, because it has to outlive the last block it issued. That is the
only allocation ztext makes outside the installed allocator.

The header check is a detector, not a checksum: sixteen bytes leave no room for
a magic number, so a prefix that was overrun into garbage whose two fields
happen to be in range still passes. What it does catch, for free on every
deallocation, is an allocator index past the end of the registry and an
alignment that is not a power of two at or below `max_align_t`'s.

`reallocate` is optional, and returning null from it means *"I decline"*, not
*"out of memory"* — ztext falls back to allocate-copy-free either way. That
distinction is load-bearing; see [what the tests found](#what-the-tests-found).

### Determinism

Shaping the same string with the same build must produce the same glyphs on
every machine. Two things had to be done for that to be true:

- **The process locale is ignored, on both paths that could read it.**
  HarfBuzz's `guess_segment_properties` fills an unset language from
  `setlocale(LC_CTYPE, NULL)`, so out of the box two machines shape
  differently. ztext seeds the buffer so it cannot, and applies
  language-specific features only when you pass a language explicitly. The
  second path is not shaping at all: the autohinter shapes each script's
  blue-zone strings through HarfBuzz and does let it guess, so hinting could
  read the locale where shaping cannot. It does not, because `build.zig`
  passes `-DHB_NO_SETLOCALE` and `hb_setlocale` is then the literal `"C"` —
  which was already true from the absence of `HAVE_NEWLOCALE`, and is now a
  define rather than an absence.
- **`FREETYPE_PROPERTIES` is ignored.** `FT_Init_FreeType` reads that
  environment variable and lets it change the TrueType interpreter version and
  the autohinter. ztext does not call the function that does it.
- **HarfBuzz's three are ignored too.** It reads `HB_SHAPER_LIST`,
  `HB_FONT_FUNCS` and `HB_FACE_LOADER`, and two of them changed what ztext
  rendered: with `HB_SHAPER_LIST=fallback` five golden tests failed — standard
  ligatures stopped applying, and moving a variation axis stopped moving the
  shaped result. `HB_FONT_FUNCS=ft` leaked 216 bytes, because hb-ft's font
  funcs open an `FT_Face` from an `FT_Library` ztext neither owns nor frees.
  The build passes `-DHB_NO_GETENV`, which makes `getenv` return null inside
  HarfBuzz.

All three are recorded with file and line in [UPSTREAM.md](UPSTREAM.md), and
all three have a test — the suite and the C smoke test each run a second time
with the variables set to hostile values.

### Thread safety, stated rather than implied

FreeType's `FT_Library` and `FT_Face` are not internally synchronised, and that
propagates:

- A `Library`, every `Font` from it and every `Face` from those belong to **one
  thread**. The faces of a font share its `FT_Face` and its one glyph slot, so
  they are not independent even though they are separate handles. Use one
  library per thread rather than sharing behind a lock — that is FreeType's own
  advice, and a lock would serialise rasterisation across the process.
- `renderGlyph` returns pixels the **face owns**, copied out of that shared
  slot, valid until the next `renderGlyph` on the same face. Nothing else
  disturbs them — not shaping, not measuring, not a sibling face.
- A `Shaper` handles one call at a time; give each thread its own.
- `Paragraph` touches FreeType not at all. Once built it is immutable and
  readable from several threads.
- The installed allocator is process-wide and must be thread-safe if ztext is
  used from more than one thread.

### Font and Face are separate, and why the bitmap is copied

`Font` is one parsed font image; `Face` is that font at one size. Both
upstreams split the same way (`hb_face_t`/`hb_font_t`, `FT_Face`/`FT_Size`).
Collapsing them into one handle is tempting and costs more than it looks: four
sizes would mean four full parses.

Both rows below come from `zig build bench -Doptimize=ReleaseFast
-Dsanitize_c=false`, which builds four sizes over one shared `Font` and then
four independent `Font`s at the same four sizes — the second being exactly the
handle a collapsed design gives you. Everything is shaped and rasterised with
before it is measured, on the same schedule in both arms, because a handle that
has never been used has not built its table accelerators, scaled state or glyph
buffers yet and measuring it would flatter the design.

Noto Sans, x86_64-windows-gnu:

| | Per additional size | Four sizes |
|---|---|---|
| One handle carrying its own size | 34 703 B | 138 459 B |
| Font shared, faces per size | **15 333 B** | **80 350 B** |

That is 56% off a size. The absolute bytes move with the platform, the
allocator and the build options; the ratio is the claim, and `ci/measurements.sh`
reprints both rows on whatever machine you run it. Blind spot: the bench counts
what ztext's allocator seam is asked for, so it cannot attribute a byte to
FreeType rather than HarfBuzz, and it does not see the host allocator's own
per-block overhead.

What the split cannot save is also worth stating, and it is structural rather
than measured: `FT_Size` carries the interpreter state and every scaled metric,
so it is per size whatever you do, while `hb_font_t` is a thin scaling wrapper
over an `hb_face_t` that owns the parse and lazily loads the OpenType tables.
Sharing the `Font` is therefore nearly all of the HarfBuzz saving and only part
of the FreeType one — which is why the number above is 56% and not 90%.

The memory is not the main reason, though. The split is what puts a
size-independent question — the family name, the glyph count, whether a
character is covered — on something that does not have a size, and it is where
variable-font axes will live.

Its one real cost is the glyph slot: FreeType keeps exactly one per `FT_Face`,
so faces of a font share it. Rather than widen the bitmap-borrow rule from
"this face" to "any face of this font", `renderGlyph` **copies** the glyph out
of the slot into a buffer the face owns. That is not defensive: `ft_smooth_render`
frees the slot's previous buffer before allocating the next
(`src/smooth/ftsmooth.c:589`), and `FT_Load_Glyph` frees it too, so a borrowed
pointer really is freed by the next load through any sibling. The copy is one
memcpy of a few hundred bytes against a rasterisation measured in microseconds,
the buffer is reused so steady state still allocates nothing, and the rule got
*narrower* rather than wider -- "this face", not "any face of this font". It
also lets ztext promise a tightly
packed, top-down bitmap, so a consumer cannot render upside down by ignoring
FreeType's signed pitch.

No handle has a **destruction order**: whichever of a pair is released second
frees what they share. That covers the library too, which is the one place it
is not free -- `FT_Done_Library` destroys every face still registered with it,
so a library released first would leave its fonts reading freed memory. ztext
counts live fonts and defers the library's own teardown instead, for one
`size_t` and one `bool` per handle. The alternative was a rule that only a
comment could state and only a caller could keep, whose violation was a
use-after-free rather than an error code.

### Validation at the boundary

Unlike a binding over an unmaintained parser, ztext is **not** compensating for
upstreams that check nothing. FreeType, HarfBuzz and SheenBidi are all
continuously fuzzed by OSS-Fuzz and are among the most attacked parsers in
software. ztext's job is narrower and it should be said plainly:

- **Not adding holes of its own** — null and size checks at every entry point,
  overflow-checked arithmetic in the allocator and the array helper, and
  refusing a font larger than HarfBuzz's `unsigned int` blob length rather than
  truncating it.
- **Validating the text before either library sees it**, in whichever of the
  three encodings it arrived in. HarfBuzz substitutes U+FFFD for malformed
  input and SheenBidi has its own recovery. Reasonable for a text editor,
  wrong for an engine reading a localisation table, where malformed input
  means the table is corrupt. ztext returns `InvalidText`. Text enters once:
  a `Paragraph` validates and copies it, and `shapeRun` therefore does not
  walk it again per run — which is a cost, not only a tidiness, and it is
  measured below.
- **Turning failures into typed errors**, and separating *"this format is not
  compiled in"* from *"these bytes are broken"* — the first tells you to
  re-cook, the second to go looking for corruption.

What this does **not** claim: that hostile fonts are safe to load. The suite
sweeps truncated prefixes and byte mutations and finds no crash, but it is a
sample, not a proof — the test says so in its own comments. If you load fonts
from untrusted sources, put a signature around them.

### Versions, and what a bump promises

[CHANGELOG.md](CHANGELOG.md) is the one home for ztext's release history and
for the rules its numbers follow: with the major at 0 the MINOR position is
the breaking one, an ABI change lands as one commit, and the
`ZtextAbiLayout` handshake grows with the struct that changed.

The version itself is written in three files -- `ffi/ztext.h`'s macros,
`build.zig.zon`, and that changelog's newest heading -- and
`ci/measurements.sh --check` fails if they disagree, including if one of the
three patterns stops matching. A policy nothing checks is a paragraph.

### The ABI guard, in both directions

Downwards, at the upstreams: `_Static_assert`s in `ffi/ztext_abi.c` fail the
**build** if a re-vendor changes the shape of something ztext depends on —
`hb_position_t`'s width, `FT_Bitmap::pitch` staying *signed* (a width check
alone would let an upstream change to `unsigned` through, and every bottom-up
glyph would then render as garbage), `SBLevel` being a byte. That is the payoff
for stopping upstream types at the C boundary.

Upwards, at Zig: the wrapper hand-writes `extern struct`s mirroring `ztext.h`,
and nothing in either compiler checks they still agree. **Three** guards close
that, each answering a question the other two cannot, and each validated by
breaking it on purpose rather than by passing:

- **The comptime cross-check** (`src/abi_check.zig`) `@cImport`s the real
  header — in a test only, so the shipped module stays translate-c-free — and
  compares it against `src/c.zig` declaration by declaration. There is no
  hand-written list of what to check: every public declaration is discovered by
  reflection, paired by naming convention, and compared; one that fits no
  category is a **compile error**, so the guard cannot quietly stop covering
  something. Structs are compared field by *name*, so two same-sized adjacent
  fields swapping places fails even though the sequence of offsets is
  unchanged. Functions are compared on arity and on the size and alignment of
  every parameter and the return value. Enumerators are compared one by one,
  which a check on the last enumerator alone cannot do — renumbering a *middle*
  one leaves the last unchanged. And the sweep runs in both directions: a
  function the header exports that `c.zig` never declared fails too.
- **`ztextAbiProbe()`** writes a distinct marker into every field of every
  shared struct so the Zig side reads each one back. This is the only guard
  that compares the declarations against the **compiled library** rather than
  against the header, and the two diverge exactly when a header is preprocessed
  with different macros than the library was built with — the axis a
  header-only comparison is blind to by construction. It is also the guard that
  needs no source at all: it would catch a prebuilt `libztext` from a different
  configuration.
- **`tests/c_smoke.c`** drives the whole API from C, with no Zig in the
  picture. That is what covers the cross-check's documented residue: translate-c
  renders every C pointer as `[*c]T`, so pointee types are compared only by
  size and alignment, and a `float*` declared as `*i32` passes. The C test also
  proves the allocator seam is genuinely in use, by asserting allocations
  balance.

`ztextAbiLayout()` remains part of the C API — a non-Zig host still wants a way
to self-check what it linked against — but it is no longer the primary guard
and is not grown by hand as the API grows.

The naming convention the cross-check relies on is load-bearing, not cosmetic:
translate-c flattens a C enum to an integer alias and loses which enumerators
belonged to it, so `ZTEXT_<TYPE>_<FIELD>` is the only thing that can pair them
back up. That is why the header's enumerators are named strictly, with no
readable-but-irregular exceptions.

One thing the cross-check does **not** need, and the reason is measured rather
than assumed: macro wiring. `ffi/ztext.h` includes only `<stddef.h>` and
`<stdint.h>` and is sensitive to exactly one macro, `ZTEXT_SHARED`, which
changes an attribute and no type; every FreeType and HarfBuzz configuration
macro reaches the implementation, never the installed header. A package whose
public header changes type widths with its build options would have to forward
those macros into the `@cImport`; ztext's does not.

### Build hygiene

- Source lists are explicit, never globs — a re-vendor cannot silently change
  what compiles. HarfBuzz's own `harfbuzz.cc` amalgam is deliberately **not**
  used for the same reason.
- FreeType's configuration lives in `ffi/`, not `libs/`, so the vendored trees
  stay byte-identical to upstream and `ci/verify-vendor.sh` can prove it.
- No `-fno-access-control`. Zig's C sanitizer is **opt-in** (`-Dsanitize_c=true`)
  rather than tied to `optimize`, and ztext's own Debug runs ask for it. A
  library that turns it on by default forces its runtime into every consumer's
  link — a consumer who writes `b.dependency("ztext", .{})` and forgets to
  forward `optimize` otherwise gets `undefined symbol: __ubsan_handle_*`, which
  names nothing they can act on.
- Only headers whose implementation is actually compiled are installed. A
  header for a module ztext leaves out would compile for a consumer and then
  fail at link; `FT_Stroker_New` was the one that made the point.
- A shared build compiles with `-fvisibility=hidden`, so the upstream symbols
  statically inside it stay internal instead of being exported alongside
  ztext's own. What remains beyond ztext's entry points is FreeType's public
  API, which upstream marks `visibility("default")` itself; if you load a
  shared ztext beside a system FreeType, link statically or add a version
  script. `ci/measurements.sh` prints the exact counts, and they are printed
  rather than quoted here on purpose: a macOS `.dylib`, a Linux `.so` and a
  Windows `.dll` do not export the same set, so any single number in this
  sentence would be wrong on two platforms out of three.
- Build options are declared once and mirrored into a Zig `options` module, so
  the wrapper cannot disagree with how the C was compiled.
- `-fno-exceptions`/`-fno-rtti` are off under the MSVC ABI, where disabling
  them through Clang flags is a known source of header errors.

## Measurements

Run them yourself — the harness is committed:

```sh
zig build bench -Doptimize=ReleaseFast -Dsanitize_c=false
```

Numbers below are from that harness on x86_64-windows-gnu with Noto Sans at
32 px, reporting CPU time, over four consecutive runs of an unchanged tree.
Absolute values will differ on your machine; the ratios will not move much.

Timings vary by 30-40% run to run on a loaded machine, so they are quoted to
two significant figures with the spread stated; the ratio is the stable part.
The byte counts were identical to the byte in all four runs, which is what you
would expect of an allocation total and is worth checking rather than assuming.

| | Cost | Read this as |
|---|---|---|
| Shape a 43-character run | **~2.5 µs** | 2.25-2.70 µs. Reusing one `Shaper`. A separate test proves 500 warm shapes allocate **nothing**. |
| The same run, 4300 characters around it, through `shape` | **~3.3 µs** | HarfBuzz decodes the run and at most five characters either side, so its cost does not move. The extra **~1.05 µs** is ztext validating the whole borrowed buffer — about 0.25 ns per code unit, paid on every call. |
| Analyse a 4300-character paragraph, all three segmentation passes | **~265 µs** | 26 898 B live. Bidi, itemisation, the copy of the text, UAX #14 and both of #29. |
| The same paragraph, no segmentation | **~115 µs** | 13 982 B live. One variable between the two arms: **the three passes are 56.6% of the time and 12 916 B of the memory**, which is why they are a choice and not a policy. |
| The same run, through `shapeRun` | **~2.25 µs** | Identical to the 43-character case: a paragraph's text was validated when the paragraph was created. Shaping an N-unit paragraph as R runs costs R walks of N through `shape` and none through `shapeRun`. |
| Rasterise one glyph, A8 | **~2.3 µs** | 2.16-2.50 µs. Uncached; `FT_Load_Glyph` every time, and one memcpy of the result. Atlas it anyway. |
| Rasterise one glyph, SDF | **~2.6 ms** | **~1100× the A8 cost** (1091-1216 across the four runs). |
| One font, first face | 34 349 B | The parse, plus everything the first size needs. |
| Each additional size | **15 333 B** | Against 34 703 B for a single collapsed handle; see [Font and Face](#font-and-face-are-separate-and-why-the-bitmap-is-copied). |

**On SDF specifically.** It is a bake-time tool and the ratio is not a typo.
Two hundred glyphs is a little over half a second, once, into an atlas —
fine. Generating SDF glyphs during a frame would exhaust a 16 ms budget at
**about six glyphs**. The API exposes it because a distance field is the right way
to draw text that scales, rotates or lives in world space; the number is here
so nobody finds the cost out the hard way.

FreeType's SDF output is verified rather than assumed: a test asserts the field
grows by the spread on every side, that a scanline across an `o` crosses the
128 mid-point exactly four times, and that it ramps rather than steps.

**On the optional glyph flags.** ztext asks HarfBuzz for `UNSAFE_TO_CONCAT` and
`SAFE_TO_INSERT_TATWEEL` on every shape, which upstream withholds by default
because computing them costs something. That cost was measured on this row
rather than argued about: three runs of the unchanged tree gave 2.35, 2.45 and
2.60 µs, and three runs with the flags asked for gave 2.30, 2.25 and 2.30 µs —
the second set inside the spread of the first, so the difference is below what
this harness resolves. The blind spot is the sentence itself: it is Latin, where
little joins and nothing elongates. A cursive script does more work for these
flags and is not on this row.

**On the memory rows.** Every handle is shaped and rendered with before being
measured: one that has never been used has not built its table accelerators,
scaled state or glyph buffers, and measuring the bare handle would flatter the
design. The split between the two rows, and what it does and does not buy, is
in [Font and Face](#font-and-face-are-separate-and-why-the-bitmap-is-copied).

## Testing

```sh
zig build test        # the whole suite
zig build test-c      # the C boundary alone
ci/check-guards.sh    # and prove the guards can fail -- see below
ci/measurements.sh          # every number this file claims, recomputed
ci/measurements.sh --check  # ... and compared against what it says
```

**122 tests**, executed twice. The second pass runs the same binary with
HarfBuzz's three environment variables — `HB_SHAPER_LIST`, `HB_FONT_FUNCS`,
`HB_FACE_LOADER` — set to values that change what it does, and every assertion
has to hold unchanged; that is what proves `-DHB_NO_GETENV` is doing its job
rather than being believed. So `zig build test` reports **244/244 passed**.

The tests that touch a face, a shaper or a paragraph install
`std.testing.allocator`, so any allocation ztext or an upstream fails to return
fails the test; the rest check tags, versions and the ABI and allocate nothing.

- **Golden shaping** against the committed fonts: ligature substitution and
  kerning in Latin, cursive joining and mark attachment in Arabic, right-to-left
  without joining in Hebrew — with features switched off to prove the
  difference is real.
- **Cluster maps**: code-unit offsets in range, never pointing inside a
  character, monotone in visual order, and a ligature carrying the cluster of
  the first character it swallowed.
- **Bidi**: base-level resolution both ways, visual runs that tile the
  paragraph exactly once with no byte dropped or doubled, and script runs that
  are contiguous and complete.
- **Wrapping, end to end**: break opportunities from ztext, the width decision
  from the test, reordering per line from ztext, shaping with context from
  ztext — asserting the lines tile the paragraph exactly once. Removing the
  one line that marks a paragraph's end as a break makes it fail, which is how
  that bug was found rather than shipped.
- **A caret moves by grapheme**: `e` + combining acute is one grapheme in two
  characters, and two people joined by U+200D are one grapheme in eleven
  bytes. Both are pinned, in both directions, including that walking off
  either end stays put.
- **Per-line reordering**: a line whose break falls just after whitespace
  between two right-to-left words must move that whitespace to the other end,
  by rule L1. The test asserts the paragraph and the line genuinely disagree,
  which is the only way to show the line is not derived from the paragraph —
  and an implementation that clipped the paragraph's runs was written on
  purpose to watch it fail.
- **Fractional pixel sizes** are honoured rather than rounded: advances at
  18.25, 18.5 and 18.75 px are strictly increasing, and FreeType's
  whole-pixel grid-fitting of *face* metrics is pinned separately so it does
  not read as a bug.
- **Hostile input**: malformed UTF-8 of eight shapes, an unpaired UTF-16
  surrogate, a UTF-32 unit that is not a scalar, non-font bytes, truncated
  prefixes, and byte mutations across the table directory — each of which must
  produce a typed error or behave, never crash.
- **A font FreeType accepts and HarfBuzz rejects** — a doctored sfnt version
  word — is refused, rather than loading into a face that rasterises perfectly
  and shapes everything to `.notdef`.
- **A rejected shape clears the previous run**, so a caller that logs the error
  and keeps drawing cannot redraw last frame's text.
- **Extents refuse a face the run was not shaped against**, or the same face
  resized since.
- **A library keeps its own allocator** when the process-wide one is replaced,
  and a face's glyph buffer is its library's however long after the face it is
  first drawn.
- **Shaping runs tile the paragraph exactly once**, split by both direction and
  script, with the script pieces reversed inside a right-to-left run.
- **Bracket mirroring stays HarfBuzz's job**: `(` shaped right-to-left must
  come back as the `)` glyph and vice versa, which pins the upstream behaviour
  ztext relies on instead of duplicating.
- **The externs match the header**, checked by reflection over every public
  declaration rather than a hand-maintained list — see
  [The ABI guard](#the-abi-guard-in-both-directions).
- **Faces of one font share the parse but not the size**: interleaved renders
  and metrics reads through two faces must each answer for their own size,
  which is what the `FT_Activate_Size` discipline buys. Removing one call was
  enough to fail it.
- **A bitmap survives everything but its own face's next render** — a sibling
  face's render, a measure, a shape with FreeType metrics — and two faces hold
  two independent, byte-identical bitmaps of the same glyph.
- **Subpixel offsets are real, not decoration**: a whole-pixel offset moves
  the bitmap by exactly one pixel with byte-identical coverage, a fractional
  one changes the antialiasing without moving the pixel grid, and SDF mode
  ignores the offset entirely.
- **Outline decomposition closes every contour it opens**, visits at least
  one curve on a round letter, lands in the same coordinate space
  `glyphExtents` reports, and propagates a callback's own failure rather than
  reporting success or a different error.
- **Synthetic bold widens the advance and the ink together**, and rasterisation
  agrees with `glyphExtents` because both load through the same path.
  **Synthetic oblique** moves the ink without moving the advance, and reaches
  outline decomposition too, not just render and extents.
- **Synthetic bold reaches a SHAPED run's advances**, on both metric sources,
  by the fraction of the em the caller asked for -- the half FreeType cannot
  reach, because a shaped advance never passes through glyph loading. Setting
  either style ages a run shaped before it, so extents cannot mix two weights,
  and a strength that is not a finite number is refused.
- **Vertical face metrics report which they are**: synthesised from ascender
  and descender for every committed font, with the flag saying so rather than
  a caller having to guess.

`zig build test-c` adds four things no Zig test can reach, because they need
failure injected *below* the C boundary:

- **Every byte comes back**, counted by a plain-C allocator.
- **220 injected allocation-failure points** walked across the whole pipeline:
  every one must give a typed error, leave the caller's out-parameter NULL, and
  free what it had already taken. Zero leaks, zero crashes.
- **FreeType's allocation really is per-library**, proven by swapping the
  process-wide allocator mid-life and watching where the traffic goes.
- **500 warm shapes allocate nothing**, which is the claim in Measurements.

And `tests/null_sweep.c` calls **every one of the 78 entry points with
nothing** — NULL handles, with the out-parameter checked for being left alone,
then real handles with a NULL out-parameter, which is what a host produces when
an allocation failed two lines up. `ci/api-surface.sh --sweep` fails if the
header ever grows an entry point the sweep does not name, because a sweep that
silently covers less as the surface grows is worse than none: it reads like
coverage.

`ci/api-surface.sh --gaps` holds the other half. Every entry point has up to
six homes -- the header, an implementation, a Zig extern, a Zig wrapper, a
test, this file -- and the script fails if one of them is empty without a
written reason. Not every hole is a defect: `ztextFaceFont` has no Zig wrapper
because the Zig `Face` already carries its `Font` as a field, and wrapping the
accessor too would be a second way to ask one question. That is a decision, so
it is declared in the script. Anything not declared is a failure, which is the
difference between a report and a gate.

### What the tests found

Two bugs worth naming, because both were invisible from reading the code:

**Shaping silently lost all OpenType layout when a host installed its own
allocator.** Zig's `std.mem.Allocator` only resizes in place and returns null
the moment a block must move. ztext reported that upward as an allocation
failure — and HarfBuzz treats a failed allocation as a reason to abandon
optional work and carry on. The symptom was not a crash or an error: it was
Arabic rendered as unjoined nominal letters, correct-looking and completely
wrong, only on hosts with a custom allocator. Now a decline falls back to
allocate-copy-free.

**Every SDF glyph was rejected as malformed.** FreeType's smooth rasteriser
reports `num_grays = 256` and its SDF renderer reports `255`, for the same
8-bit output. A validity check that trusted one number rejected the other
renderer entirely.

Both are in [UPSTREAM.md](UPSTREAM.md) with file and line.

An adversarial review pass afterwards found three more of the same character —
wrong, plausible, and silent:

**A font FreeType accepted and HarfBuzz rejected loaded successfully.**
`hb_face_create` answers a blob its sanitiser refuses with a perfectly normal
face object that simply has no tables. Every character then shapes to
`.notdef`, no error is reported anywhere, and FreeType goes on rasterising the
same font correctly — so it presents as a broken font rather than a failed
load. Now `hb_face_create_or_fail` plus a glyph-count check, with a test that
doctors an sfnt version word.

**A rejected shape left the previous run queryable.** The reset sat below the
argument checks, so bad UTF-8 returned an error while `glyphs()` still held the
last successful run — but an out-of-memory failure further down cleared it. Two
behaviours for one word. The reset moved to the top.

**Extents could be measured against the wrong face.** The signature took a face
but checked nothing, so measuring against another face, or the same face
resized since, mixed one font's ink bounds with another's advances and returned
`OK`. Faces now carry a generation, bumped on resize, and a mismatch is an
error.

And three that were not bugs in the library so much as promises it could not
keep, each found by building something rather than reading something:

**`dep.artifact("harfbuzz")` panicked for every consumer.** All four artifacts
were registered behind `if (b.pkg_hash.len == 0)`, and `Dependency.artifact`
resolves by scanning install steps — so the README's "use the upstreams
directly" example could never have worked, while every in-repo test passed.
`tests/consumer/` now builds ztext the way a dependant does, on every CI host.

**FreeType's per-library allocator was not per-library.** The claim appeared in
five places; the shims ignored their `FT_Memory` and used the global. Swapping
allocators mid-life then handed a library's FreeType blocks to an allocator
that never issued them — an abort under Zig's DebugAllocator, heap corruption
under a pool. Both halves are fixed, and the Zig wrapper cannot re-break it:
`setAllocator` copies the allocator into a slot ztext owns and never frees, so
there is no single mutable global for a later install to overwrite.

**Seven headers promised an API that was not there.** Four HarfBuzz headers —
`hb-gpu.h`, `hb-raster.h`, `hb-subset-depend.h`, `hb-vector.h` — were installed
with no compiled translation unit behind them, which is a compile for the
consumer and a link error afterwards; `hb-subset-depend.h` could not even be
compiled, since it opens by demanding a header from a library ztext does not
build. FreeType's `ftrender.h` included `ftglyph.h`, which was not installed at
all, and `freetype.h` declared three entry points — `FT_Get_FSType_Flags`,
`FT_Face_CheckTrueTypePatents`, `FT_Face_SetUnpatentedHinting` — whose sources
were not compiled. A comment in `build.zig` said the lists were restricted to
headers whose translation units were built. It was prose, and it was wrong in
seven places; `ci/header-link.sh` is the gate that replaced it.

**The quickstart itemised incorrectly and leaked, and the fix reached one of
its two copies.** It looped over `visualRuns()`, which hands HarfBuzz one run
spanning three scripts for `"Hello Ελληνικά мир"`, and it omitted `warmup()`,
so running it verbatim under a debug allocator reported leaks. `shapingRuns()`
exists because of the first. The second is gone at the root rather than by
remembering: `setAllocator` warms those caches itself, so there is no longer a
call to omit.

Then this page was corrected to `shapeRun` — the run shaped in the context
of the whole text — and the same example in `src/ztext.zig`'s module doc
went on shaping `text[run.offset..][0..run.length]`, which is precisely the
defect this paragraph claimed was fixed. Two homes for one example, and
nothing compiled either. Now there is one:
[`examples/quickstart.zig`](examples/quickstart.zig) is a program the build
runs, both documents quote its marked regions, and a test fails if either has
drifted.

**The glyph structs did not say what they were.** A `ZtextGlyph` carried
positions and nothing about the text around it, so a host wrapping a paragraph
had to re-shape every line after breaking it — the flags HarfBuzz computes to
make that unnecessary stopped at ztext's boundary. And a `ZtextGlyphBitmap`
described its pixels without saying how to read them: A8 coverage and an SDF
are both one byte per pixel, so a caller that remembered the wrong
`ZtextRenderMode` got no error, just a washed-out picture. Both were the same
defect — a value whose meaning lived somewhere other than the value.

`ZtextGlyph` now carries `flags`, an OR of `ZtextGlyphFlag` mirroring
HarfBuzz's own with a static assertion per flag; `ZtextGlyphBitmap` carries
`format`, written even for a glyph with no ink so it is meaningful before the
NULL check. Both structs changed size, so the ABI went 0.1 to 0.2 in one
change: the header's version macros, `build.zig.zon`, the `ZtextAbiLayout`
handshake, the `ztextAbiProbe` markers and the Zig mirror move together, and
`ci/measurements.sh --check` fails if the two version homes disagree.

The bump found a third home for the version while it was at it: a test whose
subject was `Version.format` asserted the literals `"0.1.0"` and `"2.14.3"`, so
the first bump of ztext or of FreeType would turn a formatting test red for a
reason that had nothing to do with formatting. It now formats a synthetic
version it owns, and compares the real one against its own fields.

**The same number, from two libraries, meaning two different things.** Adding
`Face.metric` put `post`'s underline beside the one `FaceMetrics` already
carried, and they disagreed: HarfBuzz reports `underlinePosition` as the font
stores it, the TOP edge of the stroke, while FreeType subtracts half the
thickness to convert the TrueType meaning to its own
(`libs/freetype/src/sfnt/sfobjs.c`). For Noto Sans that is -100 against -125
design units. Neither is wrong and a caller drawing a rectangle needs to know
which edge it was handed, so the difference is now stated in `ffi/ztext.h` and
pinned by a test that asserts the gap is exactly half the thickness. The first
draft of that test asserted the two would *agree*, which is what turned an
assumption into a measurement.

**A documented contract that the source contradicted.** The first draft of
`ztextFontVariantGlyphIndex` said 0 also meant "this font maps the pair by
default". FreeType does the opposite: for a sequence recorded as the default it
resolves the base character through the Unicode cmap and returns that glyph
(`tt_cmap14_char_var_index`, `libs/freetype/src/sfnt/ttcmap.c`). The corrected
contract is simpler and more useful — nonzero exactly when this font draws this
exact pair — and it was corrected by reading upstream, not by testing, because
no vendored font has a format-14 cmap to test against. `tests/fonts.zig` now
builds one: `withVariationSequences` rewrites a font's cmap with an extra
format-14 subtable naming sequences the test states outright, which is what
makes the two interesting answers reachable at all.

That fixture also produced a finding of its own. Appending the new `(0,5)`
encoding record at the end of the cmap left the records unsorted, and FreeType
picks a face's default charmap by walking them **backwards** and taking the
last Unicode one (`find_unicode_charmap`, `libs/freetype/src/base/ftobjs.c`).
The format-14 subtable became the default charmap, its `char_index` answers 0
for everything, and the font's ordinary lookups all returned `.notdef` while
the variation sequences worked perfectly. Sorted records fix it, and a mutation
case unsorts them again so the reason is a gate rather than a comment.

**A FreeType option that changed a HarfBuzz allocation nobody was watching.**
FreeType's autohinter can take its glyph coverage from GSUB instead of the
character map, and that matters here more than it does for most consumers:
`light` hinting is the autohinter and nothing else for a TrueType face (only
the CFF driver sets `HINTS_LIGHTLY`), and a glyph shaping produced — an Arabic
medial form, a ligature — is reachable from no character at all, so through the
cmap alone it is hinted against no script's blue zones. Turning the option on
changed the raster of every light-hinted glyph of all four test fonts, and of
every glyph of the variable font in every mode, since it carries no `fpgm` and
FreeType autohints it unconditionally.

The golden that holds it is a whole-font sweep -- every glyph of the Arabic,
Latin and Hebrew fixtures rendered with `light` hinting, reduced to a digest,
a total ink figure and the count of glyphs FreeType's rasteriser declines. The
first attempt was a pair of single-glyph rasters, one of them a glyph only
shaping can reach, and the mutation harness reported it as a hole in the suite:
both rasters are byte-identical with the option on and off. What the option
moves is a scattered minority of each font, so only a sweep can see it. Both
arms were measured; all three digests and all three ink totals differ.

Blind spot, stated: those digests are absolute raster values, measured on
x86_64 Windows against the pinned FreeType. FreeType's rasteriser and
autohinter are integer-only, so the same numbers are expected everywhere, but
that is read from the source rather than measured -- this package has never run
the suite on a hosted runner. A hosted arm that disagrees is telling you to
re-measure the golden, not that the option regressed.

It also broke the heap balance, which is the part worth writing down. The
coverage pass builds a HarfBuzz buffer and lets it guess its segment
properties, which interns the default language — a **fifth** process-lifetime
HarfBuzz singleton, on a path that did not exist in this package before, and
charged to whichever allocator was installed the first time a glyph was
*hinted* rather than shaped. `ztextWarmup()` now touches it, and the C smoke
test — the one place that counts every byte in and out — now hints with the
autohinter, because a byte count that never meets a path cannot say anything
about it.

### Continuous integration

CI runs the whole suite on **Linux, macOS and Windows**, in four optimize
modes, plus the standalone C test, a downstream-consumer build and the Windows
MSVC ABI — cross-compiles eight targets, verifies the vendored trees against
their upstreams, and runs the mutation harness below. See
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

`tests/consumer/` is worth singling out: it builds ztext the way a dependant
does, which is a genuinely different code path from building it in place. Every
artifact used to be registered behind `if (b.pkg_hash.len == 0)`, so
`dep.artifact("harfbuzz")` panicked for anyone who took ztext as a dependency
while every in-repo test passed. A promise nothing exercises is a promise that
breaks.

The same matrix runs locally:

```sh
ci/run.sh              # the full matrix
ci/run.sh --quick      # native Debug only, for the inner loop
ci/run.sh --full       # + the mutation harness below
ci/check-guards.sh     # break each guard on purpose; minutes, not seconds
ci/header-link.sh      # every installed header compiles, is reachable, links
ci/verify-vendor.sh    # diff libs/ against pinned upstream (needs network)
ci/install-hooks.sh    # run ci/run.sh automatically before every push
```

### Do the guards actually fail?

A passing test says nothing about whether it *can* fail. `ci/check-guards.sh`
applies **59** deliberate bugs, one at a time, to a copy of the tree, and
asserts a **named** test catches each:

| | |
|---|---|
| ABI | a *middle* enumerator renumbered, an enum tag narrowed, two same-sized struct fields swapped, a field added to the header only, a by-value parameter widened, a parameter dropped, a function the header exports that `c.zig` never declares |
| Bidi | a line reordered over the paragraph instead of over itself, script pieces emitted forwards inside a right-to-left run, a paragraph's end left as no break at all |
| Faces | a glyph loaded without activating the face's own `FT_Size`, a covered prefix that splits a base from its marks or breaks at a format character, a pixel size rounded to whole pixels, a bitmap that does not say which format its bytes are in |
| Metrics | a metric tag nobody vetted, forwarded to HarfBuzz as if it were one this build names |
| Variable fonts | named-instance coordinates that are not the font's, an instance name reported one byte longer than it is |
| Variation sequences | a variation selector ignored and the base character answered instead, and the test fixture's own cmap records left in the order they were appended |
| Encodings | SheenBidi told the text is UTF-8 whatever it was, libunibreak handed UTF-16 through its UTF-8 entry point, HarfBuzz the same, a UTF-16 surrogate pair treated as two characters |
| Segmentation | every pass run whatever was asked for, an unnamed segmentation bit accepted and ignored, the word array laid over the line array |
| Shaping | extents taken from the wrong face, a rejected shape that leaves the previous run queryable, the optional glyph flags never asked for, a paragraph run shaped left to right whatever its level, a direction the run and the caller both set, a hand-built run trusted about its own bounds |
| Allocator | a declined `reallocate` reported as out of memory, a block freed through whatever allocator is installed now, a library-owned block released by the wrong one, an allocator slot taken per install rather than per allocator, SheenBidi handed memory ztext did not write |
| Caches | the process-lifetime caches left unwarmed — planted where `ztextSetAllocator` warms them, since nothing warms up by hand any more |
| Handle lifetimes | a font released without telling the library that owns it, a face's glyph buffer charged to whatever allocator is installed when it is first drawn |
| Reproducibility | the environment allowed to reach HarfBuzz |
| Hinting | the autohinter's glyph coverage taken from the character map alone, and the language its coverage pass interns left cold |
| Documentation | a documented example edited away from the program it quotes |
| Installed headers | a header put back in the install list with nothing compiled behind it, an implementation removed from under a header that still declares it |
| Versioning | a bump that reached two of the version's three homes, and a changelog heading reworded out from under the gate that reads it |
| Licences | a licence text changed under the page that summarises it, the row deleted rather than rechecked, and a row whose answer the build configuration decides left behind when that configuration changed |

A mutation the suite survives is reported as a hole in the *suite*; one whose
anchor no longer applies is reported too, so the script rots loudly rather than
quietly passing. Writing it caught three expectation strings naming the wrong
test — which is precisely the failure mode the hand-run version had and could
not detect.

Each case is a rebuild, and the last two are a rebuild plus an install, a
translate and a link — minutes rather than seconds for the set, which is why
it is a step of its own rather than part of every push. HarfBuzz is a large,
template-heavy C++ library and dominates a cold build; `--quick` exists
because of that, not despite it.

### Platform coverage

| | Suite executed by CI | Compile-checked by CI |
|---|---|---|
| Linux | x86_64 (glibc) | + aarch64, musl (both arches) |
| macOS | aarch64 | + x86_64 |
| Windows | x86_64, both gnu and MSVC ABI | + aarch64 (gnu only) |

Compiling proves the sources and build graph are portable; only an executed
configuration proves behaviour, which is why the two are separate jobs. Two of
the eight cross targets duplicate configurations the executed jobs already
cover, so six are genuinely additional.

That table describes the matrix, not a promise: **the badge at the top of this
file is the authority on whether those runs have actually happened and passed.**

At the time of writing that badge is not green, and not red either — while the
repository is private, GitHub Actions does not run, so the badge will not
render. What has actually happened is this: the suite has been executed by hand
on **macOS/aarch64 only** — the whole suite across four optimize modes with the
C sanitiser on, the standalone C test, the null sweep, the downstream-consumer
build, all eight cross targets compiled, the mutation harness at eighteen for
eighteen, and the vendor check green. **The Windows MSVC configuration has
never been executed at all**, and neither has anything on Linux.

## Scope

Exposed today:

- Fonts from memory, with family/style names, glyph count, units per em, and a
  face count for TrueType collections
- Faces: one font at one pixel size, with scaled metrics
- Cmap lookup (`Font.glyphIndex`) for checking coverage before falling back,
  and `Font.variantGlyphIndex` for a base character plus a **variation
  selector** — cmap format 14, the mechanism behind U+FE0E/U+FE0F and the
  Ideographic Variation Sequences
- Shaping a run: direction, script, language, OpenType features, cluster level,
  and a choice of metrics source (HarfBuzz's tables or FreeType's)
- Cluster maps in code-unit offsets, shaped-run extents, per-glyph extents
- Bidi paragraphs: base level, per-code-unit levels, visual runs
- Bidi **lines**: any code-unit range of a paragraph reordered on its own
  terms,
  which is what rules L1 and L2 require of anything that wraps
- Script itemisation, and the two intersected into **shaping runs** — spans
  uniform in direction *and* script, in the order they are drawn
- Fractional pixel sizes, quantised to FreeType's own 1/64 px
- **Line break opportunities** (UAX #14), and grapheme and word boundaries
  (UAX #29), with `nextGrapheme`/`previousGrapheme` for caret movement
- **Variable-font axes**: what a font declares, and setting them on both
  FreeType and HarfBuzz at once so metrics and rasterisation cannot describe
  different instances
- **Named instances**: the points in that axis space the font's own designers
  named, with their coordinates and their subfamily names, and
  `setNamedInstance` to move every axis to one in a step — data a picker cannot
  derive from the axes, because they are choices rather than a rule
- **The OpenType metrics FreeType does not scale onto an `FT_Size`**:
  `Face.metric` reads any of the 28 `hb_ot_metrics_tag_t` values — x-height,
  cap-height, strikeout, the caret slope, the sub/superscript boxes — honouring
  the USE_TYPO_METRICS bit and applying variations, with
  `metricWithFallback` for HarfBuzz's own synthesised value when a font
  declares none
- **Run context**: shaping a range of a longer text, so joining stays correct
  where a host split a word between fonts
- Rasterisation: A8 coverage and SDF, three hinting modes, configurable
  spread. `light` is FreeType's autohinter and nothing else for a TrueType
  face, and the autohinter takes its glyph coverage from **GSUB** rather than
  from the character map, so a glyph that only shaping can produce is hinted
  against its own script's blue zones
- **Subpixel positioning**: `renderGlyph` takes a fractional offset in 26.6,
  so text laid out at a fractional x is not forced onto the pixel grid.
  Ignored in SDF mode: a distance field is baked once and sampled at any
  position later, so baking a sub-pixel shift in would be wasted, unrecoverable
  work.
- **Glyph outlines as paths**: `decomposeOutline` walks `FT_Outline_Decompose`
  through move-to/line-to/conic-to/cubic-to/close callbacks, points in 26.6,
  for a host that fills its own shapes rather than sampling a bitmap.
- **Synthetic bold and oblique**, for a family with no bold or italic face of
  its own. Both are amounts rather than switches -- a fraction of the em and a
  shear factor, with `synthetic_bold_default` and `synthetic_oblique_default`
  the reference weights FreeType and HarfBuzz themselves use, and nothing
  clamped above them. FreeType applies them at glyph loading, so
  `glyphExtents`, `renderGlyph` and `decomposeOutline` agree on one widened,
  sheared glyph; HarfBuzz is told the same two numbers, so a shaped run's
  advances widen with the ink instead of overlapping it.
- **Vertical face metrics**: column-direction analogues of `ascender`,
  `descender`, `line_height` and `max_advance`, plus a flag saying whether
  they are real (from a `vhea`/`vmtx`) or synthesised — see below.

**Vertical direction, exactly as far as it goes.** `ttb` and `btt` reach
HarfBuzz and come back with vertical advances instead of horizontal ones —
tested, including that the shaper reports back the direction it was given
rather than a normalisation of it. `FaceMetrics` now carries column spacing
too, but none of the committed fonts has a `vmtx`, so what the tests exercise
is the synthesised path — `has_vertical_metrics` reporting false, and a span
derived from `ascender` and `descender` — not real vertical metrics read from
a CJK face's own tables. Enough to say the plumbing works; not enough to call
vertical text supported.

Not exposed:

- A font fallback chain for glyphs a font does not cover
- **Colour and bitmap glyphs**, and the reason is worth stating because two of
  the three routes are not equally closed:
  - *CBDT and sbix strikes* are PNG. `FT_CONFIG_OPTION_USE_PNG` is off, and
    turning it on means vendoring **libpng and zlib** — two more upstreams with
    their own pins, licences and re-vendor procedure. That is a real decision,
    not an oversight, and it has not been worth making yet.
  - *COLRv1* stays out on design grounds. It is a paint graph with gradients,
    transforms and compositing modes — a renderer's job, and folding one into a
    glyph source is how a text library stops being reusable.
  - *COLRv0* layer enumeration is the one that is genuinely cheap:
    `TT_CONFIG_OPTION_COLOR_LAYERS` is already compiled in, `FT_Get_Color_Glyph_Layer`
    and `FT_Palette_Select` need no new dependency, and layers rasterise as
    ordinary A8 for the host to tint. It is absent for a duller reason: there
    is no OFL colour font small enough to commit as a fixture, and shipping an
    untested rasterisation path is exactly the thing that reaches a consumer as
    garbage pixels rather than as an error.
- Reusable `Paragraph` scratch. Less valuable than it looks: SheenBidi
  allocates per paragraph internally regardless, so a reusable handle would
  save ztext's copy and nothing else.

Deliberately out of scope: an atlas, a layout engine, justification, a caret
*model*, rich text. Those are a host's job, and keeping them out is what makes
this package reusable.

Note exactly where the line falls, because "no line breaking" would be the
easier claim and the wrong one. ztext says where a break is **permitted**; the
host decides
where one **happens**, because that needs a width and a width is not a property
of text. Same for the caret: ztext says where the grapheme boundaries are, the
host owns the caret.

## Licence

ztext's own code is MIT — see [LICENSE](LICENSE).

The vendored upstreams are **FreeType** under the FreeType License (elected
explicitly over the GPLv2 alternative), **HarfBuzz** under "Old MIT",
**SheenBidi** under Apache-2.0 and **libunibreak** under zlib. All four permit
static linking into a closed commercial product; none requires source
disclosure, royalties or on-screen attribution.

If you ship a binary containing ztext, [LICENSES.md](LICENSES.md) is the file
to read — it says exactly what has to appear in your third-party notices,
which of FreeType's four internal sub-licences actually reaches a binary, and
what the FTL's incompatibility with GPLv2 does and does not mean for you. It
pins the licence texts it summarises by digest, and `ci/measurements.sh
--check` recomputes them.

The test fonts under `tests/fonts/` are Noto, under the SIL Open Font License
1.1. They are not part of the library and are not compiled into it.

## Contributing

Issues and pull requests are welcome. Three things to know first:

- **`libs/` is vendored verbatim and must not be edited.** Changes there are
  lost at the next re-vendor, and `ci/verify-vendor.sh` will fail. If upstream
  needs fixing, fix it upstream; if ztext needs to work around upstream, do it
  in `ffi/` and record it in [UPSTREAM.md](UPSTREAM.md).
- **Run `ci/run.sh` before pushing** — or `ci/install-hooks.sh` once, and it
  runs itself.
- **A failing golden test is information, not an obstacle.** Read the diff
  before updating the numbers: a changed advance is a changed layout for every
  consumer.

New source files are added to the explicit lists in `build.zig` deliberately;
there are no globs, so nothing starts compiling by accident.
