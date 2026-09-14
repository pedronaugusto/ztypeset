# What the tests cover, and what they found

The README says how to run the suite. This page says what is in it.

## The Zig suite

The tests that touch a face, a shaper or a paragraph install
`std.testing.allocator`, so any allocation ztypeset or an upstream fails to
return fails the test; the rest check tags, versions and the ABI and allocate
nothing.

- **Golden shaping** against the committed fonts: ligature substitution and
  kerning in Latin, cursive joining and mark attachment in Arabic,
  right-to-left without joining in Hebrew — with features switched off to prove
  the difference is real.
- **Cluster maps**: code-unit offsets in range, never pointing inside a
  character, monotone in visual order, and a ligature carrying the cluster of
  the first character it swallowed.
- **Bidi**: base-level resolution both ways, visual runs that tile the
  paragraph exactly once with no byte dropped or doubled, and script runs that
  are contiguous and complete.
- **Wrapping, end to end**: break opportunities from ztypeset, the width
  decision from the test, reordering per line from ztypeset, shaping with
  context from ztypeset — asserting the lines tile the paragraph exactly once.
  Removing the one line that marks a paragraph's end as a break makes it fail,
  which is how that bug was found rather than shipped.
- **A caret moves by grapheme**: `e` + combining acute is one grapheme in two
  characters, and two people joined by U+200D are one grapheme in eleven bytes.
  Both are pinned, in both directions, including that walking off either end
  stays put.
- **Per-line reordering**: a line whose break falls just after whitespace
  between two right-to-left words must move that whitespace to the other end,
  by rule L1. The test asserts the paragraph and the line genuinely disagree,
  which is the only way to show the line is not derived from the paragraph —
  and an implementation that clipped the paragraph's runs was written on
  purpose to watch it fail.
- **Fractional pixel sizes** are honoured rather than rounded: advances at
  18.25, 18.5 and 18.75 px are strictly increasing, and FreeType's whole-pixel
  grid-fitting of face metrics is pinned separately so it does not read as a
  bug.
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
  ztypeset relies on instead of duplicating.
- **The externs match the header**, checked by reflection over every public
  declaration rather than a hand-maintained list — see [abi.md](abi.md).
- **Faces of one font share the parse but not the size**: interleaved renders
  and metrics reads through two faces must each answer for their own size,
  which is what the `FT_Activate_Size` discipline buys. Removing one call was
  enough to fail it.
- **A bitmap survives everything but its own face's next render** — a sibling
  face's render, a measure, a shape with FreeType metrics — and two faces hold
  two independent, byte-identical bitmaps of the same glyph.
- **Subpixel offsets are real, not decoration**: a whole-pixel offset moves the
  bitmap by exactly one pixel with byte-identical coverage, a fractional one
  changes the antialiasing without moving the pixel grid, and SDF mode ignores
  the offset entirely.
- **Outline decomposition closes every contour it opens**, visits at least one
  curve on a round letter, lands in the same coordinate space `glyphExtents`
  reports, and propagates a callback's own failure rather than reporting
  success or a different error.
- **Synthetic bold widens the advance and the ink together**, and rasterisation
  agrees with `glyphExtents` because both load through the same path. Synthetic
  oblique moves the ink without moving the advance, and reaches outline
  decomposition too, not just render and extents.
- **Synthetic bold reaches a shaped run's advances**, on both metric sources,
  by the fraction of the em the caller asked for — the half FreeType cannot
  reach, because a shaped advance never passes through glyph loading. Setting
  either style ages a run shaped before it, so extents cannot mix two weights,
  and a strength that is not a finite number is refused.
- **Vertical face metrics report which they are**: synthesised from ascender
  and descender for every committed font, with the flag saying so rather than a
  caller having to guess.
- **A whole-font hinting golden**: every glyph of the Arabic, Latin and Hebrew
  fixtures rendered with `light` hinting, reduced to a digest, a total ink
  figure and the count of glyphs FreeType's rasteriser declines. The first
  attempt was a pair of single-glyph rasters, one of them a glyph only shaping
  can reach, and the mutation harness reported it as a hole in the suite: both
  rasters are byte-identical with the autohinter's GSUB coverage on and off.
  What that option moves is a scattered minority of each font, so only a sweep
  can see it.

Those digests are absolute raster values, first measured on x86_64 Windows
against the pinned FreeType. FreeType's rasteriser and autohinter are
integer-only, so the same numbers are expected everywhere, and the hosted suite
has measured exactly that: Linux, macOS and Windows on both ABIs all hold the
same digests. The configurations CI only compile-checks remain unmeasured. A
hosted arm that disagrees is telling you to re-measure the golden, not that the
option regressed.

## The C boundary

`zig build test-c` adds four things no Zig test can reach, because they need
failure injected below the C boundary:

- Every byte comes back, counted by a plain-C allocator.
- Injected allocation-failure points walked across the whole pipeline: every
  one must give a typed error, leave the caller's out-parameter NULL, and free
  what it had already taken. Zero leaks, zero crashes.
- FreeType's allocation really is per-library, proven by swapping the
  process-wide allocator mid-life and watching where the traffic goes.
- Warm shapes allocate nothing, which is the claim in
  [measurements.md](measurements.md).

`tests/null_sweep.c` calls every entry point with nothing — NULL handles, with
the out-parameter checked for being left alone, then real handles with a NULL
out-parameter, which is what a host produces when an allocation failed two
lines up. `ci/api-surface.sh --sweep` fails if the header ever grows an entry
point the sweep does not name, because a sweep that silently covers less as the
surface grows is worse than none: it reads like coverage.

`ci/api-surface.sh --gaps` holds the other half. Every entry point has up to
six homes — the header, an implementation, a Zig extern, a Zig wrapper, a test,
the README — and the script fails if one of them is empty without a written
reason. Not every hole is a defect: `ztypesetFaceFont` has no Zig wrapper
because the Zig `Face` already carries its `Font` as a field, and wrapping the
accessor too would be a second way to ask one question. That is a decision, so
it is declared in the script. Anything not declared is a failure, which is the
difference between a report and a gate.

## The mutation harness

`ci/check-guards.sh` applies deliberate bugs, one at a time, to a copy of the
tree, and asserts a named test catches each. The README carries the table of
what it breaks; `ci/measurements.sh --check` diffs that table against the
harness's own case names, so the table is the list rather than a copy of it.

A mutation the suite survives is reported as a hole in the suite; one whose
anchor no longer applies is reported too, so the script rots loudly rather than
quietly passing. Writing it caught three expectation strings naming the wrong
test — precisely the failure mode the hand-run version had and could not
detect.

`--anchors` asks only the second of those two questions: does every case still
quote a piece of the tree, exactly once? It mutates nothing and builds nothing,
so it answers in seconds rather than minutes, and it is the question a refactor
breaks. Reaching a stranded anchor the slow way costs a full sweep, which is
long enough that nobody runs it before pushing, which is how a case comes to be
stranded in the first place.

Three verdicts beyond PASS and HOLE are worth knowing about.

**TRUNCATED.** When a test fails with a long trace, `zig build` replaces the
tail of its own output with `unable to read results of configure phase`, and a
later failure's diagnostics never appear, so a caught mutation can read as a
hole. The script names that state rather than calling it a wrong failure, and
the fix for a case that lands there is to expect the test that fails first.

**DID NOT COMPILE.** The mutation broke the build rather than a test — it
deleted the last use of a variable or the last call to a function, and
`-Werror` stopped the build before anything ran. Such a case tested nothing,
and the fix is the mutation rather than the expectation. Two guards are
compile-time by design — the ABI cross-check and the FreeType options file —
and for those the expected string names the compile error, so they are caught
before this verdict is reached.

**TIMED OUT.** A mutation is a deliberate bug and a bug is not obliged to
terminate. Each case's command is run with its output going to a file rather
than down a pipe the harness has to drain, and with a deadline of twenty times
the baseline build measured at the start of the same sweep. A case that reaches
the deadline is killed, reported, and stops the sweep: the command is dead but
a build it started may not be, so no later verdict would be judged on a tree
nothing else is writing to. The working copy is kept and named for the same
reason.

Most cases are a rebuild of the library and the suite; two are a rebuild plus
an install, a translate and a link. The ones under *Versioning and licences*
are neither — their guard is `ci/measurements.sh --check`, which reads sources
and runs in seconds, because what they break is a statement in a document
rather than a behaviour in the library. The set is minutes rather than seconds
either way, which is why it is a step of its own rather than part of every
push. HarfBuzz is a large, template-heavy C++ library and dominates a cold
build; `--quick` exists because of that.

## What the tests found

Two bugs worth naming, because both were invisible from reading the code.

**Shaping silently lost all OpenType layout when a host installed its own
allocator.** The full account is in [allocators.md](allocators.md): a declined
in-place resize was reported as an allocation failure, and HarfBuzz abandons
optional work on one. The symptom was Arabic rendered as unjoined nominal
letters, correct-looking and completely wrong, only on hosts with a custom
allocator.

**Every SDF glyph was rejected as malformed.** FreeType's smooth rasteriser
reports `num_grays = 256` and its SDF renderer reports `255`, for the same
8-bit output. A validity check that trusted one number rejected the other
renderer entirely.

Both are in [UPSTREAM.md](../UPSTREAM.md) with file and line.

An adversarial review pass afterwards found three more of the same character —
wrong, plausible, and silent.

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
keep, each found by building something rather than reading something.

**`dep.artifact("harfbuzz")` panicked for every consumer.** All four artifacts
were registered behind `if (b.pkg_hash.len == 0)`, and `Dependency.artifact`
resolves by scanning install steps — so the README's "use the upstreams
directly" example could never have worked, while every in-repo test passed.
`tests/consumer/` now builds ztypeset the way a dependant does, on every CI
host.

**FreeType's per-library allocator was not per-library.** The claim appeared in
five places; the shims ignored their `FT_Memory` and used the global. Swapping
allocators mid-life then handed a library's FreeType blocks to an allocator
that never issued them — an abort under Zig's DebugAllocator, heap corruption
under a pool. Both halves are fixed, and the Zig wrapper cannot re-break it:
`setAllocator` copies the allocator into a slot ztypeset owns and never frees,
so there is no single mutable global for a later install to overwrite.

**Seven headers promised an API that was not there.** Four HarfBuzz headers —
`hb-gpu.h`, `hb-raster.h`, `hb-subset-depend.h`, `hb-vector.h` — were installed
with no compiled translation unit behind them, which is a compile for the
consumer and a link error afterwards; `hb-subset-depend.h` could not even be
compiled, since it opens by demanding a header from a library ztypeset does not
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

Then the README was corrected to `shapeRun` — the run shaped in the context of
the whole text — and the same example in `src/ztypeset.zig`'s module doc went
on shaping `text[run.offset..][0..run.length]`, which is precisely the defect
that correction claimed to have fixed. Two homes for one example, and nothing
compiled either. Now there is one: `examples/quickstart.zig` is a program the
build runs, both documents quote its marked regions, and a test fails if either
has drifted.

**The glyph structs did not say what they were.** A `ZtypesetGlyph` carried
positions and nothing about the text around it, so a host wrapping a paragraph
had to re-shape every line after breaking it — the flags HarfBuzz computes to
make that unnecessary stopped at ztypeset's boundary. And a
`ZtypesetGlyphBitmap` described its pixels without saying how to read them: A8
coverage and an SDF are both one byte per pixel, so a caller that remembered the
wrong `ZtypesetRenderMode` got no error, just a washed-out picture. Both were
the same defect — a value whose meaning lived somewhere other than the value.
`ZtypesetGlyph` now carries `flags`, an OR of `ZtypesetGlyphFlag` mirroring
HarfBuzz's own with a static assertion per flag; `ZtypesetGlyphBitmap` carries
`format`, written even for a glyph with no ink so it is meaningful before the
NULL check.

**A version bump reached three of its four homes.** README.md's status line sat
at v0.1 through the whole of 0.2's development, while the header, the manifest
and the changelog were gated and agreed. Three homes gated and a fourth in
prose beside them is worse than gating none, because a green row for "the
version homes agree" reads as though it covered every one of them.

A fifth home was found and removed rather than gated: a test whose subject was
`Version.format` asserted the literals `"0.1.0"` and `"2.14.3"`, so the first
bump of ztypeset or of FreeType would have turned a formatting test red for a
reason that had nothing to do with formatting. It now formats a synthetic
version it owns, and compares the real one against its own fields.
