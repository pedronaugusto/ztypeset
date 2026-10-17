# ztypeset

[![CI](https://github.com/pedronaugusto/ztypeset/actions/workflows/ci.yml/badge.svg)](https://github.com/pedronaugusto/ztypeset/actions/workflows/ci.yml)

Text shaping and glyph rasterisation for Zig, over vendored FreeType,
HarfBuzz, SheenBidi and libunibreak. It orders a paragraph, itemises it into
runs, finds the break and grapheme boundaries, shapes each run and rasterises
the glyphs; drawing them is the host's.

Status: **v0.2**.

Beside this file: [CHANGELOG.md](CHANGELOG.md) for what changed and what a
version bump means, [UPSTREAM.md](UPSTREAM.md) for the pinned upstream commits
and how to re-vendor them, [LICENSES.md](LICENSES.md) for which licence reaches
your binary, [CONTRIBUTING.md](CONTRIBUTING.md) for where each rule is written
down, [SECURITY.md](SECURITY.md) for the threat model, and
[docs/](docs/) for the design, allocator, ABI, measurement and testing notes.

## Contents

[Usage](#usage) · [Install](#install) · [API](#api) · [Allocators](#allocators) ·
[Design](#design) · [Build options](#build-options) · [Platforms](#platforms) ·
[Testing](#testing) · [Scope](#scope) · [Licence](#licence) ·
[Contributing](#contributing)

## Usage

```zig
const ztypeset = @import("ztypeset");

// Copied, not borrowed: ztypeset keeps its own copy for as long as any handle
// can reach it. Installing also warms the caches the upstreams keep for the
// life of the process, so this allocator only ever sees ztypeset's working set.
try ztypeset.setAllocator(gpa_state.allocator());
defer ztypeset.resetAllocator();

const library = try ztypeset.Library.init();
defer library.deinit();

// The bytes are BORROWED and must outlive the font. Handles have no
// destruction order: whichever of a pair goes second frees what they share.
const font = try library.createFont(font_bytes, 0);
defer font.deinit();

// A face is the font at one size. Make one per size you draw; a second size
// costs a size, not another parse.
const face = try font.face(0, 16);
defer face.deinit();

const shaper = try ztypeset.Shaper.init();
defer shaper.deinit();

const paragraph = try ztypeset.Paragraph.init(text, .{});
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

Pick the face per run from `run.script` when you carry more than one; ztypeset
has no fallback chain of its own.

Both code blocks on this page are quoted from
[`examples/quickstart.zig`](examples/quickstart.zig), which `zig build test`
compiles and runs; a test fails if either stops matching it.

If the text wraps, ask where it may break, decide with your own width, and
iterate a `Line` per visual line:

```zig
const breaks = paragraph.lineBreaks(); // one entry per code unit
var start: usize = 0;
while (start < text.len) {
    // Furthest permitted break that still fits. ztypeset says where a break is
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

`Line` is not a convenience wrapper. UAX #9 applies rules L1 and L2 per line,
so `paragraph.shapingRuns()` is the right answer only when the paragraph *is*
one line. And for a caret, `paragraph.nextGrapheme(offset)` — not the next
character, which lands between a letter and its accent.

## Install

```sh
zig fetch --save git+https://github.com/pedronaugusto/ztypeset
```

Then add the dependency and link the module:

```zig
const ztypeset_dep = b.dependency("ztypeset", .{ .target = target, .optimize = optimize });
exe.root_module.addImport("ztypeset", ztypeset_dep.module("ztypeset"));
```

There is nothing else to install. FreeType 2.14.3, HarfBuzz 14.4.0, SheenBidi
3.0.0 and libunibreak 7.0.0 live under `libs/` and are compiled by this
package; nothing is fetched at build time and no system library is linked.
[UPSTREAM.md](UPSTREAM.md) carries the commits and the re-vendor procedure, and
`ci/verify-vendor.sh` proves `libs/` is still a pristine copy of them.

FreeType, HarfBuzz and SheenBidi are also exposed as artifacts with their own
headers installed, so a C or C++ part of your program can use them directly —
linked alongside ztypeset itself, which the two lines above already bring in:

```zig
exe.root_module.linkLibrary(ztypeset_dep.artifact("harfbuzz"));
```

Order matters there. An upstream artifact is a piece of *this* build rather
than a self-contained copy: `freetype` has undefined `hb_*` symbols, because
ztypeset builds it with `FT_CONFIG_OPTION_USE_HARFBUZZ` and the autohinter asks
HarfBuzz for coverage and clusters, and `harfbuzz` has undefined
`ztypeset_hb_*` symbols, because its allocator seam is compile-time and
ztypeset points it at its own. The ztypeset library defines the shims and links
all four upstreams, so it closes both — and a host that imports no Zig module
takes `artifact("ztypeset")` beside whichever upstream it reaches for.

What is installed is exactly what was compiled. ztypeset builds a reduced
configuration of each upstream — FreeType without the modules it does not
register, and `libharfbuzz` without the four separate libraries upstream builds
beside it (`-subset`, `-raster`, `-vector`, `-gpu`, each with its own external
dependency) — so the headers for those are not installed either.
`ci/header-link.sh` compiles every installed header and makes the linker
resolve every entry point it declares, on both Windows ABIs.

## API

ztypeset is not a one-to-one binding of any of the four upstreams. Its own
surface covers ordering, itemisation, segmentation, shaping and rasterisation
and stops there; everything else those libraries expose — HarfBuzz's draw,
paint, colour, maths and OpenType layout queries, FreeType's glyph objects and
colour layers — is reached through their own headers, installed beside
ztypeset's. Of the outline formats FreeType can read, this build compiles
TrueType and CFF/CFF2 only. Nothing in the repository enforces that division:
it is the state of the surface at 0.2.1, not a checked property.

The handles. None has a destruction order — whichever of a pair is released
second frees what they share, so `deinit` is safe in any sequence:

| | |
|---|---|
| `Library` | One FreeType library, from `init`. One thread. |
| `Font` | One parsed font image, from `library.createFont(bytes, index)`. The bytes are borrowed and must outlive it. |
| `Face` | That font at one pixel size, from `font.face(index, size)`. |
| `Shaper` | One reused HarfBuzz buffer. One call at a time. |
| `Paragraph` | Text, validated and copied, with its bidi, itemisation and segmentation results. |
| `Line` | One code-unit range of a paragraph, reordered on its own terms. |

The calls:

| | |
|---|---|
| Text | `view` — a slice or a pointer to an array of `u8`, `u16` or `u32`, with the `length` in code units |
| Font | `familyName` `styleName` `glyphCount` `unitsPerEm` `countFaces` `glyphIndex` `coveredPrefix` `variantGlyphIndex` |
| Character maps | `charmapCount` `charmap` `activeCharmap` `selectCharmap` `selectCharmapEncoding` |
| Variable fonts | `axisCount` `axis` `setVariations` `namedInstanceCount` `namedInstanceCoords` `namedInstanceNameLen` `namedInstanceName` `setNamedInstance` |
| Face | `setPixelSize` `metrics` `metric` `metricWithFallback` `tag` |
| Shaping | `shapeRun` `shape` `shapeRange` `glyphs` `direction` `extents` `glyphExtents` `glyphHas` |
| Ordering | `baseLevel` `baseDirection` `levels` `visualRuns` `scriptRuns` `shapingRuns` `runDirection` `line` `offset` |
| Segmentation | `lineBreaks` `graphemeBreaks` `wordBreaks` `nextGrapheme` `previousGrapheme` `segmentation` `segmentationHas` |
| Rasterisation | `renderGlyph` `setSdfSpread` `bitmapRows` `bitmapChannels` `decomposeOutline` |
| Styles | `setSyntheticBold` `setSyntheticOblique` `setStroke` `stroke` `outline` `setTransform` `transform` `rotation` `scaling` `shear` |
| Memory | `setAllocator` `resetAllocator` `warmup` |
| Errors | `check` `resultName` `lastDetail` |
| Versions | `version` `freetypeVersion` `harfbuzzVersion` `sheenbidiVersion` `unibreakVersion` |

The parts of that a reader would otherwise get wrong:

- UTF-8, UTF-16 and UTF-32 are taken natively by all four upstreams, so
  ztypeset transcodes nothing and cluster maps come back in code units of
  whichever encoding you passed. String literals are pointers to arrays, which
  is why `view` accepts both those and slices.
- `variantGlyphIndex` is a base character plus a variation selector — cmap
  format 14, behind U+FE0E/U+FE0F and the Ideographic Variation Sequences — and
  is nonzero exactly when this font draws that exact pair.
- The selected character map governs `glyphIndex` and `coveredPrefix` and never
  reaches shaping, because HarfBuzz reads the same tables itself. FreeType
  selects a Unicode map when it opens a font; an icon font whose glyphs live
  only in a (3, 0) MS Symbol map has none to select.
- `setVariations` and `setNamedInstance` move FreeType and HarfBuzz together,
  so metrics and rasterisation cannot describe different instances. The
  two-call instance-name pattern is upstream's, kept rather than hidden so a
  caller allocates once and exactly.
- `metric` reads any of the 28 `hb_ot_metrics_tag_t` values FreeType does not
  scale onto an `FT_Size` — x-height, cap-height, strikeout, the caret slope,
  the sub/superscript boxes — honouring the USE_TYPO_METRICS bit and applying
  variations. `metrics` is the scaled face metrics, with a flag saying whether
  the column-direction ones are real (from a `vhea`/`vmtx`) or synthesised.
- `shape` and `shapeRange` exist for text that is not a paragraph; a shape
  takes direction, script, language, OpenType features (`Feature`, and
  `feature_global` for the whole run), cluster level, and a choice of metrics
  source — HarfBuzz's tables or FreeType's.
- `shapingRuns` is `visualRuns` intersected with `scriptRuns`: spans uniform in
  direction *and* script, in the order they are drawn. `line` reorders any
  code-unit range on its own terms and reports its `offset` and `length` back.
- `setPixelSize` takes a fractional size, quantised to FreeType's own 1/64 px.
- `renderGlyph` produces A8 coverage, FreeType's native SDF, or `.lcd` and
  `.lcd_v` subpixel coverage — three samples per pixel, in FreeType's Harmony
  mode, since `FT_CONFIG_OPTION_SUBPIXEL_RENDERING` is off and no filter has to
  be chosen. It takes a hinting mode and a fractional offset in 26.6, ignored
  in SDF mode because a field is baked once and sampled anywhere later. SDF
  costs about 1100 times an A8 glyph; bake it.
- `width`, `height`, `left` and `top` are in pixels in every format and `pitch`
  is bytes per pixel row, so `pitch * height` is the buffer. `bitmapRows` hands
  back one slice per row honouring the sign of the pitch.
- The synthetic styles are amounts rather than switches, with
  `synthetic_bold_default` and `synthetic_oblique_default` the reference
  weights FreeType and HarfBuzz themselves use and nothing clamped above them.
  HarfBuzz is told the same two numbers, so a shaped run's advances widen with
  the ink instead of overlapping it.
- `setStroke` moves no advance, and `setTransform` maps the glyph image and no
  advance — neither the face's nor a shaped run's — because a shaped advance
  comes from HarfBuzz, which has no matrix to be told about. Lay the run out in
  text space, then map pen positions and glyph images together.
- All three of those run at glyph loading, so `glyphExtents`, `renderGlyph` and
  `decomposeOutline` see one glyph.
- `lastDetail` returns this thread's most recent error detail, which is where
  FreeType's own error name survives ztypeset's flat result code.
- The four upstream version calls report what was linked rather than what was
  pinned, which is the distinction the downstream-consumer test exists to hold.
  `Version.format` prints any of them.

## Allocators

`setAllocator` installs a `std.mem.Allocator` for ztypeset and the upstreams;
`resetAllocator` takes it out again. The allocator is copied, not borrowed, and
ztypeset keeps its copy for as long as any handle can reach it.

Three of the four upstreams allocate, and all three redirect differently.
libunibreak allocates nothing at all — its tables are static and its results go
into your buffer — so there is no fourth seam.

| | Seam | Scope |
|---|---|---|
| FreeType | `FT_MemoryRec` per `FT_Library` | per `Library` |
| SheenBidi | a global default allocator object | process-wide |
| HarfBuzz | four macros resolved at compile time | process-wide |

HarfBuzz is the binding constraint, so `setAllocator` is process-wide; that is
surfaced rather than hidden behind a per-object parameter that could not be
honoured. FreeType is the exception and is genuinely per-library: a `Library`
records the installed allocator when it is created and keeps allocating and
freeing through it even if the process-wide one is replaced underneath.

What the injection covers, and what it cannot:

- **Every block is freed through the allocator that made it.** A block header
  carries its size, its alignment and the index of the allocator that issued
  it, so a free or a grow is routed back to that allocator and not to whatever
  is installed at the time. Swapping the process-wide allocator with live
  handles is therefore defined, and so is `resetAllocator`.
- **A sized allocator needs no bookkeeping of its own.** All three seams free
  without a size; the header hands the size and alignment back. Two
  pointer-sized words per allocation, sixteen bytes on a 64-bit target.
- **A mismatch stops the process.** A block released by naming the wrong
  allocator exits with `ZTYPESET_EXIT_ALLOCATOR_MISMATCH` (70), both allocators
  named on stderr, and the block is not freed.
- **One `malloc` per distinct allocator ever installed** falls outside yours
  and is never freed: the registry entry has to outlive the last block it
  issued. Installing the same allocator twice reuses its entry.
- **Returning null from `reallocate` means "I decline", not "out of memory".**
  ztypeset falls back to allocate-copy-free either way. Zig's
  `std.mem.Allocator` declines every move, so the distinction is load-bearing.
- **Installing is start-up, not an operation.** `setAllocator` mutates a
  process-wide registry with no synchronisation of its own; call it once before
  any other thread is using ztypeset. A thread-safe allocator does not cover
  the install itself.
- **The upstreams' process-lifetime caches are not yours to free.**
  `setAllocator` calls `warmup` first, so a tracking allocator installed after
  it sees a balanced heap. Two caches need a real face and stay out of its
  reach: shape one throwaway run before installing if you audit and use either.

The full account, and the two bugs this design was written against, is in
[docs/allocators.md](docs/allocators.md).

## Design

The decisions a caller has to know about. The reasoning is in
[docs/design.md](docs/design.md) and [docs/shaping.md](docs/shaping.md).

- **The pipeline stops at runs.** Text goes order → itemise → shape →
  rasterise; ztypeset does all four and nothing after. Where a break *happens*
  needs a width, which is not a property of text, so that stays yours.
- **`shapeRun` takes the paragraph, not the text.** Offsets are only meaningful
  against the buffer they were computed from, and a slice passed with offsets
  computed against the whole produces text that is almost right. The paragraph
  also carries the direction and the script, and its text was validated when it
  was created, so it is not walked again per run.
- **A wrapped paragraph is reordered per line.** UAX #9 resolves embedding
  levels over the paragraph but applies rules L1 and L2 per line. The
  difference is trailing whitespace between two right-to-left words, which the
  paragraph puts in the middle and the line puts at the end.
- **Bidi mirroring is already done and needs no API.** Rule L4 is applied by
  HarfBuzz at shaping time, before OpenType features run, and a test pins that.
- **Segmentation is a choice.** The three passes are the largest thing a
  paragraph holds — on a 4300-character paragraph, 56.6% of the time to build
  it, and more memory than the copied text and the embedding levels together.
- **Text is validated at the boundary.** HarfBuzz substitutes U+FFFD for
  malformed input and SheenBidi has its own recovery: reasonable for a text
  editor, wrong for an engine reading a localisation table, so ztypeset returns
  `InvalidText`. That is not a claim that hostile fonts are safe to load — the
  suite sweeps truncated prefixes and byte mutations and finds no crash, but
  that is a sample, not a proof.
- **Shaping is deterministic across machines.** The process locale reaches
  neither shaping nor hinting, and `FREETYPE_PROPERTIES`, `HB_SHAPER_LIST`,
  `HB_FONT_FUNCS` and `HB_FACE_LOADER` are all ignored. Two of those changed
  what ztypeset rendered before they were closed off.
- **One thread per `Library`.** A library, its fonts and their faces belong to
  one thread; the faces of a font share its `FT_Face` and its one glyph slot. A
  `Shaper` handles one call at a time. A `Paragraph` is immutable once built
  and readable from several threads.
- **`renderGlyph` returns pixels the face owns**, valid until the next
  `renderGlyph` on the same face and disturbed by nothing else — not shaping,
  not measuring, not a sibling face. The bitmap is tightly packed and top-down,
  so a consumer cannot render upside down by ignoring FreeType's signed pitch.
- **`Font` and `Face` are separate.** A font is one parse; a face is that font
  at one size. Four sizes over one shared font cost about 56% less than four
  collapsed handles, and the split is what puts the size-independent questions
  on something with no size.
- **Upstream type layouts stop at a C boundary, and drift is a build failure.**
  `_Static_assert`s fail the build if a re-vendor changes the shape of
  something ztypeset depends on, and three guards check the Zig externs against
  the header, against the compiled library, and from C with no Zig in the
  picture. See [docs/abi.md](docs/abi.md).

## Build options

Two, and they are the whole set. Each is declared once in `build.zig` and
mirrored into a Zig `options` module, so the wrapper cannot disagree with how
the C it links was compiled.

| option | default | what it changes |
|---|---|---|
| `-Dshared` | `false` | builds the C library as a shared object, with `-fvisibility=hidden` |
| `-Dsanitize_c` | `false` | compiles ztypeset's own C with Zig's undefined-behaviour sanitizer |

A third option added to `build.zig` and not to that table fails
`ci/measurements.sh --check`: an option a caller cannot find is an option that
does not exist for them.

The sanitizer is opt-in rather than tied to `optimize` because a library that
turns it on by default forces its runtime into every consumer's link.

## Platforms

| | Suite executed by CI | Compile-checked by CI |
|---|---|---|
| Linux | x86_64 (glibc) | + aarch64, musl (both arches) |
| macOS | aarch64 | + x86_64 |
| Windows | x86_64, both gnu and MSVC ABI | + aarch64 (gnu only) |

Every job in that matrix passed on run
[`34779589536`](https://github.com/pedronaugusto/ztypeset/actions/runs/34779589536).

## Testing

```sh
zig build test              # the whole suite
zig build test-c            # the C boundary alone
ci/run.sh                   # the matrix the workflow runs, on this host
ci/run.sh --quick           # native Debug only, for the inner loop
ci/run.sh --full            # + the mutation harness
ci/check-guards.sh          # break each guard on purpose; minutes, not seconds
ci/check-guards.sh --anchors  # does every case still apply? seconds
ci/measurements.sh          # every number this file states, recomputed
ci/measurements.sh --check  # ... and compared against what it says
ci/header-link.sh           # every installed header compiles, is reachable, links
ci/verify-vendor.sh         # diff libs/ against pinned upstream (needs network)
ci/install-hooks.sh         # run ci/run.sh automatically before every push
```

**148 tests**, executed twice: the second pass runs the same binary with
HarfBuzz's three environment variables set to values that change what it does,
and every assertion has to hold unchanged.
`zig build test` therefore reports **296/296 passed**.
Tests that touch a face, a shaper or a paragraph install
`std.testing.allocator`, so any allocation ztypeset or an upstream fails to
return fails the test.

`zig build test-c` injects failure below the C boundary, which no Zig test can:
**220 injected allocation-failure points** across the pipeline, each of which
must give a typed error, leave the caller's out-parameter NULL and free what it
had already taken; a plain-C allocator that counts every byte back; and proof
that FreeType's allocation really is per-library.
It also holds that **500 warm shapes allocate nothing**.

`tests/null_sweep.c` calls every one of the 88 entry points with nothing — NULL
handles, with the out-parameter checked for being left alone, then real handles
with a NULL out-parameter, which is what a host produces when an allocation
failed two lines up.

What the suite covers case by case, and the defects it found, are in
[docs/testing.md](docs/testing.md).

### The mutation harness

A passing test says nothing about whether it can fail. `ci/check-guards.sh`
applies **96** deliberate bugs, one at a time, to a copy of the tree, and
asserts a named test catches each. The right column below is the harness's own
case names, in its own order, and `ci/measurements.sh --check` diffs the whole
table against them.

| section | what is broken, one at a time |
|---|---|
| ABI cross-check | enum: a MIDDLE enumerator renumbered; enum: the tag narrowed on the Zig side; struct: two same-sized fields swapped; struct: a field added to the header only; function: a by-value parameter widened; function: a parameter dropped; function: exported by the header, undeclared in c.zig |
| Bidi | a line derived from the paragraph, skipping rule L1; script pieces emitted forwards inside an RTL run; the end of a paragraph left as no break at all |
| Segmentation | every segmentation pass run whatever was asked for; an unnamed segmentation bit accepted and ignored; the word array laid over the line array |
| Faces and fonts | a face loads without activating its own FT_Size; a covered prefix that splits a base from its marks; a covered prefix that breaks at a format character; a bitmap that does not say which format it is; a pixel size rounded to whole pixels; a font released without telling the library that owns it; a glyph buffer charged to whatever is installed |
| Hinting | the autohinter's coverage taken from the cmap alone; the language the autohinter interns, left cold |
| FreeType build switches | a FreeType switch the options file says is off |
| Subpixel rasterisation | an LCD bitmap's width reported in samples; an LCD_V bitmap's height reported in sub-rows; an LCD_V pitch that counts one of its three sub-rows; a subpixel mode rendered as plain greyscale; three bytes per pixel counted as one |
| Process-wide state | the generation counter made non-atomic; the one-time install flag made non-atomic |
| The internal contracts | the decoder reading the unit at the end; the 26.6 domain written as a range test |
| The ABI handshake | a layout field measuring the wrong type; a probed field the library never writes; two probed fields of one type expecting one marker; a probed field expecting a marker of zero |
| The stroker | the pen traced before the styles it should follow; the pen traced after the matrix instead of before it; a pen that reaches the pixels but not the measurements; an export buffer that is never grown after the first glyph; the inside and outside borders swapped; every stroke style exporting both of the stroker's contours; a pen naming a cap this build does not have, accepted; a radius too large for the fixed point it is converted to |
| The face transform | the caller's matrix applied before the font's own styles; the two off-diagonal terms of the matrix swapped; the advance transformed too, the way FT_Set_Transform does; a face whose matrix starts at the memset's zero; a matrix that is not made of numbers, taken at face value |
| Character maps | every map reported as the first one; whichever map is selected reported as the first; an encoding this font has no map for, accepted; a charmap index past the end quietly clamped |
| Synthetic styles | a style applied to the ink and not to the shaping; emboldening asked for in place, so the advance never moves; the lazily built font never told what style it was born into; a restyled face that still passes for the one a run was shaped against; a strength quantised back to the one weight upstream ships; a strength that is not a number, taken at face value |
| Encodings | SheenBidi told the text is UTF-8 whatever it is; libunibreak given UTF-16 through its UTF-8 entry point; HarfBuzz handed UTF-16 as UTF-8; a UTF-16 surrogate pair treated as two characters |
| Shaping | a run shaped without the text around it; the optional glyph flags never asked for; extents taken from a face the run was not shaped against; a rejected shape leaves the previous run queryable; a paragraph run shaped left to right whatever its level; a run's direction and the caller's, both accepted; a hand-built run trusted about its own bounds |
| Allocator | a declined reallocate reported as out of memory; a block freed through whatever is installed now; a library-owned block released by the wrong allocator; the process-lifetime caches left unwarmed; an allocator slot per install rather than per allocator; SheenBidi handed memory ztypeset did not write |
| Documentation | a documented example edited away from the program |
| Reproducibility | the environment allowed to reach HarfBuzz |
| OpenType metrics | a metric tag nobody vetted, forwarded to HarfBuzz |
| Variable fonts | named instance coordinates that are not the font's; an instance name reported one byte longer than it is |
| Variation sequences | a variation selector ignored, the base answered; the fixture's cmap records left unsorted |
| Versioning and licences | a version bump that reached three of its four homes; the changelog heading the gate reads by shape; a gated number restated in another document; a directory build.zig compiles, dropped from the package; a licence text changed under the page that summarises it; a licence row deleted rather than rechecked; a licence row that no longer matches the build; a C artifact left out of the sanitizer; a build option README cannot name; a test allocator taken out only on the happy path; a guard case run unbounded again |
| Installed headers | an installed header no compiled code stands behind; a declared entry point with no implementation |

The left column is not a summary of that script's sections; it **is** them. A
mutation the suite survives is reported as a hole in the suite, and one whose
anchor no longer applies is reported too, so the script rots loudly rather than
quietly passing. The verdicts beyond pass and hole — TRUNCATED, DID NOT
COMPILE, TIMED OUT — and what each one means are in
[docs/testing.md](docs/testing.md).

### Continuous integration

CI runs the whole suite on Linux, macOS and Windows, in four optimize modes,
plus the standalone C test, a downstream-consumer build and the Windows MSVC
ABI; it cross-compiles eight targets, verifies the vendored trees against their
upstreams, and runs the mutation harness. See
[`.github/workflows/ci.yml`](.github/workflows/ci.yml). `ci/run.sh` mirrors
those steps locally, and `ci/check-mirror.sh` fails if the two rosters stop
building the same option combinations.

`tests/consumer/` builds ztypeset the way a dependant does, which is a
genuinely different code path from building it in place: artifact registration
and installed-header spelling are invisible to the in-repo suite.

## Scope

- No font fallback chain — pick the face per run from `run.script` yourself.
- No colour or bitmap glyphs. CBDT and sbix strikes are PNG, which would mean
  vendoring libpng and zlib; COLRv1 is a paint graph a renderer owns; COLRv0
  layer enumeration is compiled in but unexposed, for want of an OFL colour
  font small enough to commit as a fixture.
- Only TrueType and CFF/CFF2 outlines. FreeType's other drivers — Type 1,
  BDF, PCF, WinFNT and the rest — are not compiled.
- Vertical text is plumbed rather than supported. `ttb` and `btt` reach
  HarfBuzz and come back with vertical advances, but no committed font has a
  `vmtx`, so only the synthesised metric path is tested.
- No reusable `Paragraph` scratch. SheenBidi allocates per paragraph internally
  regardless, so a reusable handle would save ztypeset's copy and nothing else.

## Licence

ztypeset's own code is MIT — see [LICENSE](LICENSE).

The vendored upstreams are **FreeType** under the FreeType License (elected
explicitly over the GPLv2 alternative), **HarfBuzz** under "Old MIT",
**SheenBidi** under Apache-2.0 and **libunibreak** under zlib. All four permit
static linking into a closed commercial product; none requires source
disclosure, royalties or on-screen attribution.

If you ship a binary containing ztypeset, [LICENSES.md](LICENSES.md) is the
file to read: what has to appear in your third-party notices, which of
FreeType's four internal sub-licences actually reaches a binary, and what the
FTL's incompatibility with GPLv2 does and does not mean for you. It pins the
licence texts it summarises by digest, and `ci/measurements.sh --check`
recomputes them.

The test fonts under `tests/fonts/` are Noto, under the SIL Open Font License
1.1. They are not part of the library and are not compiled into it.

## Contributing

Issues and pull requests are welcome. Three things to know first:

- **`libs/` is vendored verbatim and must not be edited.** Changes there are
  lost at the next re-vendor, and `ci/verify-vendor.sh` will fail. If upstream
  needs fixing, fix it upstream; if ztypeset needs to work around upstream, do
  it in `ffi/` and record it in [UPSTREAM.md](UPSTREAM.md).
- **Run `ci/run.sh` before pushing** — or `ci/install-hooks.sh` once, and it
  runs itself.
- **A failing golden test is information, not an obstacle.** Read the diff
  before updating the numbers: a changed advance is a changed layout for every
  consumer.

New source files are added to the explicit lists in `build.zig` deliberately;
there are no globs, so nothing starts compiling by accident.
