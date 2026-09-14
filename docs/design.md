# Design notes

The decisions behind ztypeset's shape, and the reasoning the README states
only as a conclusion.

## The pipeline stops at runs

Correct text goes order → itemise → shape → rasterise. ztypeset does all four
and then stops. It has no atlas, no justification, no caret model and no rich
text, because those belong to a layout engine that knows about your UI.

What that means concretely: `Paragraph` gives you shaping runs — visual runs
intersected with script runs, each uniform in direction and script, in visual
order — and you call `shapeRun` once per run, handing it the paragraph the run
came from. Nothing here shapes a paragraph for you.

Note where the line falls. ztypeset says where a break is permitted; the host
decides where one happens, because that needs a width and a width is not a
property of text. Same for the caret: ztypeset says where the grapheme
boundaries are, the host owns the caret.

Segmentation is inside the line rather than outside it, and that is a
deliberate choice rather than an inherited one. ztypeset already owns UAX #9,
and `Line` takes a code-unit range a host would otherwise have no way to find.
FreeType, HarfBuzz and SheenBidi implement none of UAX #14 or #29 between them,
so ztypeset vendors libunibreak and provides break opportunities.

## Why there is a C layer at all

Not because Zig cannot call the upstreams. All four expose C APIs and Zig calls
C natively; `build.zig` installs their headers precisely so you can.

It exists because FreeType's `FT_FaceRec` and `FT_GlyphSlotRec` are large,
deeply nested, and partly conditional on FreeType's own configuration macros.
Hand-mirroring those as Zig `extern struct`s would put silent memory corruption
one re-vendor away. Stopping them at a C boundary means the C compiler checks
every upstream layout change for free, and only small, flat, ztypeset-owned
structs ever cross into Zig.

So `ffi/ztypeset.h` is scoped to that job. It is not an attempt to re-present
HarfBuzz to the world under a different name.

## Font and Face are separate

`Font` is one parsed font image; `Face` is that font at one size. Both
upstreams split the same way (`hb_face_t`/`hb_font_t`, `FT_Face`/`FT_Size`).
Collapsing them into one handle would make four sizes four full parses.

The measured cost of each arrangement is in [measurements.md](measurements.md).
What the split cannot save is structural rather than measured: `FT_Size`
carries the interpreter state and every scaled metric, so it is per size
whatever you do, while `hb_font_t` is a thin scaling wrapper over an
`hb_face_t` that owns the parse and lazily loads the OpenType tables. Sharing
the `Font` is therefore nearly all of the HarfBuzz saving and only part of the
FreeType one.

The memory is not the main reason. The split is what puts a size-independent
question — the family name, the glyph count, whether a character is covered —
on something that does not have a size, and it is where the variable-font axes
live.

Its one real cost is the glyph slot: FreeType keeps exactly one per `FT_Face`,
so faces of a font share it. Rather than widen the bitmap-borrow rule from
"this face" to "any face of this font", `renderGlyph` copies the glyph out of
the slot into a buffer the face owns. That is not defensive: `ft_smooth_render`
frees the slot's previous buffer before allocating the next
(`src/smooth/ftsmooth.c:589`), and `FT_Load_Glyph` frees it too, so a borrowed
pointer really is freed by the next load through any sibling. The copy is one
memcpy of a few hundred bytes against a rasterisation measured in microseconds,
the buffer is reused so steady state still allocates nothing, and the rule got
narrower rather than wider. It also lets ztypeset promise a tightly packed,
top-down bitmap, so a consumer cannot render upside down by ignoring FreeType's
signed pitch.

No handle has a destruction order: whichever of a pair is released second frees
what they share. That covers the library too, which is the one place it is not
free — `FT_Done_Library` destroys every face still registered with it, so a
library released first would leave its fonts reading freed memory. ztypeset
counts live fonts and defers the library's own teardown instead, for one
`size_t` and one `bool` per handle. The alternative was a rule that only a
comment could state and only a caller could keep, whose violation was a
use-after-free rather than an error code.

## Determinism

Shaping the same string with the same build produces the same glyphs on every
machine. Three things had to be done for that to be true.

**The process locale is ignored, on both paths that could read it.**
HarfBuzz's `guess_segment_properties` fills an unset language from
`setlocale(LC_CTYPE, NULL)`, so out of the box two machines shape differently.
ztypeset seeds the buffer so it cannot, and applies language-specific features
only when you pass a language explicitly. The second path is not shaping at
all: the autohinter shapes each script's blue-zone strings through HarfBuzz and
does let it guess, so hinting could read the locale where shaping cannot. It
does not, because `build.zig` passes `-DHB_NO_SETLOCALE` and `hb_setlocale` is
then the literal `"C"` — which was already true from the absence of
`HAVE_NEWLOCALE`, and is now a define rather than an absence.

**`FREETYPE_PROPERTIES` is ignored.** `FT_Init_FreeType` reads that environment
variable and lets it change the TrueType interpreter version and the
autohinter. ztypeset does not call the function that does it.

**HarfBuzz's three are ignored too.** It reads `HB_SHAPER_LIST`,
`HB_FONT_FUNCS` and `HB_FACE_LOADER`, and two of them changed what ztypeset
rendered: with `HB_SHAPER_LIST=fallback` five golden tests failed — standard
ligatures stopped applying, and moving a variation axis stopped moving the
shaped result. `HB_FONT_FUNCS=ft` leaked 216 bytes, because hb-ft's font funcs
open an `FT_Face` from an `FT_Library` ztypeset neither owns nor frees. The
build passes `-DHB_NO_GETENV`, which makes `getenv` return null inside
HarfBuzz.

All three are recorded with file and line in [UPSTREAM.md](../UPSTREAM.md), and
all three have a test — the suite and the C smoke test each run a second time
with the variables set to hostile values.

## Thread safety

FreeType's `FT_Library` and `FT_Face` are not internally synchronised, and that
propagates:

- A `Library`, every `Font` from it and every `Face` from those belong to one
  thread. The faces of a font share its `FT_Face` and its one glyph slot, so
  they are not independent even though they are separate handles. Use one
  library per thread rather than sharing behind a lock — that is FreeType's own
  advice, and a lock would serialise rasterisation across the process.
- `renderGlyph` returns pixels the face owns, copied out of that shared slot,
  valid until the next `renderGlyph` on the same face. Nothing else disturbs
  them — not shaping, not measuring, not a sibling face.
- A `Shaper` handles one call at a time; give each thread its own.
- `Paragraph` touches FreeType not at all. Once built it is immutable and
  readable from several threads.
- The installed allocator is process-wide and must be thread-safe if ztypeset
  is used from more than one thread.
- Installing one is start-up, not an operation. `setAllocator` mutates a
  process-wide registry with no synchronisation of its own, so call it once
  before any other thread is using ztypeset. A thread-safe allocator does not
  cover the install itself. That restriction is the allocator's alone:
  everything else ztypeset keeps process-wide and writes after start-up is
  atomic, so the per-thread rules above are all the drawing path imposes.

## Validation at the boundary

Two of the four upstreams are continuously fuzzed upstream and two are not, and
the two that are not parse the text rather than the font.
[SECURITY.md](../SECURITY.md) carries that table, with what was checked and
when; it is not repeated here, because a fact written in two documents is a
fact that can go stale in one of them while the other still reads as current.

What ztypeset does about it:

- **Not adding holes of its own** — null and size checks at every entry point,
  overflow-checked arithmetic in the allocator and the array helper, and
  refusing a font larger than HarfBuzz's `unsigned int` blob length rather than
  truncating it.
- **Validating the text before either library sees it**, in whichever of the
  three encodings it arrived in. HarfBuzz substitutes U+FFFD for malformed
  input and SheenBidi has its own recovery. Reasonable for a text editor, wrong
  for an engine reading a localisation table, where malformed input means the
  table is corrupt. ztypeset returns `InvalidText`. Text enters once: a
  `Paragraph` validates and copies it, and `shapeRun` therefore does not walk
  it again per run.
- **Turning failures into typed errors**, and separating "this format is not
  compiled in" from "these bytes are broken" — the first tells you to re-cook,
  the second to go looking for corruption.

This does not make hostile fonts safe to load. The suite sweeps truncated
prefixes and byte mutations and finds no crash, but it is a sample, not a
proof. If you load fonts from untrusted sources, put a signature around them.

## Build hygiene

- Source lists are explicit, never globs — a re-vendor cannot silently change
  what compiles. HarfBuzz's own `harfbuzz.cc` amalgam is deliberately not used
  for the same reason.
- FreeType's configuration lives in `ffi/`, not `libs/`, so the vendored trees
  stay byte-identical to upstream and `ci/verify-vendor.sh` can prove it.
- No `-fno-access-control`. Zig's C sanitizer is opt-in rather than tied to
  `optimize`, and ztypeset's own Debug runs ask for it. A library that turns it
  on by default forces its runtime into every consumer's link — a consumer who
  writes `b.dependency("ztypeset", .{})` and forgets to forward `optimize`
  otherwise gets `undefined symbol: __ubsan_handle_*`, which names nothing they
  can act on.
- Only headers whose implementation is actually compiled are installed. A
  header for a module ztypeset leaves out would compile for a consumer and then
  fail at link; `FT_Stroker_New` was the one that made the point.
- A shared build compiles with `-fvisibility=hidden`, so the upstream symbols
  statically inside it stay internal instead of being exported alongside
  ztypeset's own. What remains beyond ztypeset's entry points is FreeType's
  public API, which upstream marks `visibility("default")` itself; if you load
  a shared ztypeset beside a system FreeType, link statically or add a version
  script. `ci/measurements.sh` prints the exact counts rather than this page
  quoting one: a macOS `.dylib`, a Linux `.so` and a Windows `.dll` do not
  export the same set.
- `-fno-exceptions`/`-fno-rtti` are off under the MSVC ABI, where disabling
  them through Clang flags is a known source of header errors.
