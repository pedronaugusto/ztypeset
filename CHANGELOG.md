# Changelog

Every version of ztypeset, newest first, and the rules the numbers follow.

**No dates.** Git holds when each change landed, precisely and without anyone
typing it; a date written here would be a second home for that, and a second
home is the one that goes quietly wrong. What this file records is what
changed and what it costs a consumer.

## How ztypeset is versioned

`ffi/ztypeset.h`'s `ZTYPESET_VERSION_MAJOR`, `ZTYPESET_VERSION_MINOR` and
`ZTYPESET_VERSION_PATCH` are the one home for ztypeset's version. `build.zig.zon`
and the newest heading in this file mirror them, and `ci/measurements.sh
--check` fails if the three disagree — so a bump that reaches two of the three
is a red gate, not a discovery someone makes later.

The major is 0, so the **minor is the breaking position**:

| bump | means |
|---|---|
| **minor** | a consumer compiled against the previous header must be recompiled. The layout of a shared struct changed, an enum gained or renumbered an enumerator or changed tag size, an entry point's signature changed, or one was added or removed. |
| **patch** | behaviour, fixes, tests, documentation, or a re-vendor that moves no declaration in `ffi/ztypeset.h`. |

A **new** entry point or a **new** enumerator is a minor bump too, and
deliberately: `ztypesetAbiLayout` reports each enum's last value and
`ci/api-surface.sh` counts the entry points, so a consumer that checks the
handshake can see the difference — and code that switches exhaustively on an
enum is entitled to be told rather than to fall through a default.

Three rules bind every ABI change, and each is gated rather than written down
and hoped for:

1. **It lands as one commit.** The header, `build.zig.zon`, this file, the
   `ZtypesetAbiLayout` handshake, the `ztypesetAbiProbe` markers and the Zig mirror
   in `src/c.zig` move together. `src/abi_check.zig` fails the build if the
   mirror lags the header; the layout and probe tests fail if the handshake
   lags either.
2. **The handshake grows with the struct.** A field added to a shared struct
   without its offset appearing in `ZtypesetAbiLayout` is a field no consumer can
   check for, which is the whole failure mode the handshake exists to prevent.
3. **A version is not a promise about the library you linked.** Compare
   `ztypesetVersion()` against the `ZTYPESET_VERSION_*` macros at startup, and read
   `ztypesetAbiLayout` if the two builds could ever differ — the macros describe
   the header you compiled with, the function describes the library you got.

The vendored upstreams have their own versions, recorded in `UPSTREAM.md` with
`src/pins.zig` as their one home and `ci/verify-vendor.sh` as their gate. They
move independently of this number.

## 0.2.1

### Fixed

- **The upstream artifacts are documented as pieces of this build, not
  standalone libraries.** `dep.artifact("freetype")` has always carried
  undefined `hb_*` symbols — ztypeset builds FreeType with
  `FT_CONFIG_OPTION_USE_HARFBUZZ`, so the autohinter asks HarfBuzz for coverage
  and clusters — and `dep.artifact("harfbuzz")` has always carried undefined
  `ztypeset_hb_*` symbols, because HarfBuzz's allocator seam is compile-time
  and ztypeset points it at its own. Both are closed by the ztypeset library
  and by nothing else, and no build-graph edge can say so: HarfBuzz already
  links FreeType, so the reverse edge is a cycle. Nothing said it where a
  consumer would read it, so the first news of it was a link error naming
  HarfBuzz from an artifact the consumer had not asked for. The install site in
  `build.zig` and the README's artifact section now state the pairing; what is
  installed and what links are unchanged.
- **The README says that installing an allocator is start-up, not an
  operation.** Its thread-safety list mirrored every other clause of
  `ffi/ztypeset.h`'s block but stopped at "process-wide and must be
  thread-safe", which reads as sufficient and is not: `setAllocator` mutates a
  process-wide registry with no synchronisation of its own, and a thread-safe
  host allocator does not cover the install. A consumer working from the README
  could install one after its workers were running. The Zig doc comment on
  `setAllocator` carries the same restriction now; the rationale had been
  written on a private helper, where no consumer reads it.
- **A mutation the suite survived: the NaN boundary of the 26.6 domain.**
  `ztypesetToFixed266` refuses a NaN by writing its domain as two negations,
  and a NaN is the ONLY input that separates that spelling from the range test
  it reads like — for every ordered value, endpoints included, the two decide
  identically. `tests/c_internal.c` pinned the NaN and asserted the value
  returned, which cannot see the difference: converting a NaN to an integer is
  undefined, and on AArch64 it yields 0, the refusal value itself. The test
  now also asserts the invalid-operation flag is clear when the call returns,
  so a NaN that reached the conversion is caught by what the conversion did
  rather than by what it happened to return — in an unoptimised build, where
  the refusal is still a branch, which is the build the mutation harness runs.
- **The C smoke test's fault reporter could not name a bus error.** It
  installed handlers for SIGSEGV, SIGILL, SIGFPE and SIGABRT, and a wild
  pointer is not always SIGSEGV: an address no mapping can hold — `0xCD..CD`
  from the poisoned injection arm — is SIGBUS on arm64. So the one fault the
  reporter was written for arrived as a bare "terminated with signal BUS",
  naming no phase, which is the outcome it exists to prevent.

## 0.2.0

### Renamed

- The package: **ztext is now ztypeset**, and every name follows — the C
  prefix (`ztypeset*` / `Ztypeset*` / `ZTYPESET_*`), the headers
  (`ffi/ztypeset.h`), the Zig module and the repository. Nothing keeps the old
  spellings: the library was never published under them, so there is no
  consumer to break and no compatibility alias to carry.

### Added

- `ZtypesetGlyph.flags`, an OR of the new `ZtypesetGlyphFlag`: `UNSAFE_TO_BREAK`,
  `UNSAFE_TO_CONCAT`, `SAFE_TO_INSERT_TATWEEL` and `DEFINED`. These are
  HarfBuzz's own values, republished under ztypeset's names with a static
  assertion per flag. A host that wraps a paragraph can now break lines
  without re-shaping them.
- All three flags are produced on **every** shape. HarfBuzz withholds two of
  them by default because computing them costs something; ztypeset asks anyway,
  because a flag that is computed on some builds and not others cannot be told
  apart from a flag that is unset. The cost was measured and is below what the
  bench resolves — see README.md.
- `ZtypesetGlyphBitmap.format`, a `ZtypesetBitmapFormat` (`A8` or `SDF`), written on
  every successful render including a glyph with no ink. A8 coverage and an
  SDF are both one byte per pixel, so a consumer that remembered the wrong
  `ZtypesetRenderMode` previously got a washed-out picture rather than an error.
- Zig: `ztypeset.GlyphFlag`, `ztypeset.BitmapFormat` and `ztypeset.glyphHas`.
- `ztypesetFaceMetric` and `ztypesetFaceMetricWithFallback`, reading any of the 28
  OpenType metrics through HarfBuzz: x-height, cap-height, strikeout, the
  caret slope, the sub/superscript boxes and the rest. The new `ZtypesetMetric`
  carries HarfBuzz's own tag VALUES, so the mapping is the identity and
  `ffi/ztypeset_abi.c` asserts each against its `HB_OT_METRICS_TAG_`
  counterpart. `ZtypesetFaceMetrics` remains FreeType's view, which is `hhea`;
  these honour the USE_TYPO_METRICS bit and apply MVAR, and the two answer
  different questions rather than disagreeing.
- `ztypesetFontNamedInstanceCount`, `...Coords`, `...Name` and
  `ztypesetFontSetNamedInstance`: the points in a variable font's axis space that
  its designers named. Nothing can derive them from the axes — they are data,
  not a rule — and the name is decoded out of the font's `name` table, so a
  caller never meets UTF-16BE.
- `ztypesetFontVariantGlyphIndex`: a base character plus a variation selector,
  through cmap format 14. Nonzero exactly when the font draws that exact pair.
- The autohinter takes its glyph coverage from **GSUB**, not from the
  character map alone (`FT_CONFIG_OPTION_USE_HARFBUZZ`). A glyph that only
  shaping produces — an Arabic contextual form, a ligature, an Indic conjunct
  — is reachable from no character, so without this it was hinted against no
  script's blue zones; `light` hinting is the autohinter and nothing else for
  a TrueType face. This changes the pixels of every light-hinted glyph. No new
  dependency: HarfBuzz was already linked, and FreeType declares what it needs
  itself rather than including a HarfBuzz header. Held by a whole-font golden
  — every glyph of three fixtures at `light`, as a digest, an ink total and a
  refusal count — because single-glyph rasters were measured not to move.
- `ztypesetWarmup` touches a fifth process-lifetime HarfBuzz singleton: the
  default language, interned by the buffer the autohinter's coverage pass
  guesses the properties of. It is reached by hinting, not by shaping, so a
  host that audits its heap and never called it would have seen two permanent
  allocations appear at the first hinted glyph.
- Zig: `ztypeset.Metric`, `Face.metric`, `Face.metricWithFallback`,
  `Font.variantGlyphIndex`, `Font.namedInstanceCount`,
  `Font.namedInstanceCoords`, `Font.namedInstanceName`,
  `Font.namedInstanceNameLen` and `Font.setNamedInstance`.

### Changed — ABI

Measured on x86_64-windows, both the gnu and the MSVC ABI:

| type | 0.1.0 | 0.2.0 | |
|---|---|---|---|
| `ZtypesetGlyph` | 24 B | 28 B | `flags` at offset 8, after `cluster` |
| `ZtypesetGlyphBitmap` | 32 B | 40 B | `format` at offset 8, immediately after `pixels` — it has to be read before they are interpreted. `width`, `height` and `pitch` are unchanged in size and moved meaning: pixels and bytes-per-pixel-row, which are what they already were for A8 and SDF |
| `ZtypesetCharmap` | — | 8 B | new: `platform_id`, `encoding_id`, `encoding` |
| `ZtypesetMatrix` | — | 16 B | new: `xx`, `xy`, `yx`, `yy` |
| `ZtypesetAbiLayout` | 192 B | 288 B | `charmap_size`, `charmap_align`, `matrix_size`, `matrix_align`, `stroke_size`, `stroke_align`, `line_cap_size`, `line_cap_last`, `line_join_size`, `line_join_last`, `stroke_style_size`, `stroke_style_last`, `glyph_offset_flags`, `glyph_bitmap_offset_format`, `bitmap_format_size`, `bitmap_format_last`, `encoding_size`, `encoding_last`, `segmentation_size`, `segmentation_last`, `glyph_flag_size`, `glyph_flag_last`, `metric_size`, `metric_count` |

`ZtypesetMetric` reports a **count** rather than a last value, alone among the
enums in the handshake: its enumerators are four-character OpenType tags, so
"the highest one" is an accident of spelling and says nothing about the range.
The count is generated from `ZTYPESET_METRIC_LIST`, which is also what the enum
itself is generated from, so it cannot be a number someone forgot to raise.

`ztypesetAbiProbe` writes a marker into each new field, so a consumer's mirror is
checked against what the library does rather than against what the header
says.

### Added

- **A paragraph runs the segmentation passes it is asked for.**
  `ztypesetParagraphCreate` takes a `segmentation` mask of `ZtypesetSegmentation`
  (`LINES`, `GRAPHEMES`, `WORDS`, `ALL`), and `Paragraph.init` takes it in an
  options struct that defaults to all three. It used to run all three always
  and allocate three bytes per code unit for their arrays, with no way to
  decline: measured on a 4300-character paragraph, that is 56.6% of the time
  to build one (265 µs against 115 µs) and 12 916 B of its 26 898 B — more
  than the copied text and the embedding levels together. The accessor for a
  pass that was not run answers NULL (an empty slice in Zig) and
  `ztypesetParagraphSegmentation` reports what ran, so an absent array is never
  mistaken for a text with no boundaries. A bit this build has no name for is
  `ZTYPESET_RESULT_INVALID_ARGUMENT`, as an unknown encoding is.
- **Subpixel rasterisation.** `ZTYPESET_RENDER_MODE_LCD` and
  `ZTYPESET_RENDER_MODE_LCD_V`, with `ZTYPESET_BITMAP_FORMAT_LCD` and
  `ZTYPESET_BITMAP_FORMAT_LCD_V` to read the result, and
  `ztypesetBitmapFormatChannels` for the bytes per pixel. Zig: `.lcd`, `.lcd_v`
  and `ztypeset.bitmapChannels`.

  FreeType renders these in **Harmony** mode in this build -- three coverage
  samples a third of a pixel apart -- because
  `FT_CONFIG_OPTION_SUBPIXEL_RENDERING` is off, upstream's default. That is a
  compile-time choice in FreeType, not a runtime one, so `FT_Library_SetLcdFilter`
  is deliberately not exposed: in this configuration it returns
  `FT_Err_Unimplemented_Feature`, and an entry point that can only fail is
  worse than none.

  `ZtypesetGlyphBitmap.width` and `height` are in **pixels** in every format, and
  `pitch` is bytes per pixel row, so `pitch * height` is the buffer's size
  everywhere. FreeType counts an LCD bitmap's width in samples and an LCD_V
  bitmap's height in sub-rows; passing either number through would have made
  `width` mean one thing for two formats and another for a third. Normal
  hinting now targets the grid the glyph will be sampled on
  (`FT_LOAD_TARGET_LCD`/`_LCD_V`); light hinting is its own target and is
  unchanged.
- **`ci/run.sh` runs what CI runs.** The local roster and the hosted one were
  two lists of the same thing, and they had diverged in two places. On a
  Windows host `ci/run.sh` executed exactly one of the four MSVC arms CI does
  -- the header gate -- so every other difference between the two Windows ABIs
  was untested on the only machine that could test it, Zig defaulting the
  Windows ABI to gnu. It now runs the suite, the 200-run crash loop and the
  downstream consumer on `native-native-msvc` as well. It also runs
  `ci/check-guards.sh --anchors`, which CI gained at the same time.

- **The version has a fourth home, and it had been wrong for a release.**
  `ci/measurements.sh --check` compared `ffi/ztypeset.h`, `build.zig.zon` and this
  file. README.md's status line said **v0.1** through the whole of 0.2's
  development and nothing looked at it -- three homes gated and a fourth in
  prose beside them, which is worse than gating none: a green row reading
  "the version homes agree" is taken to mean all of them. It is compared as a
  `major.minor` prefix now, because a patch release is not a status change.

- **README has a table of contents, and it cannot drift.** At 1200 lines it
  had none. A table of contents is a copy of a list already in the file, so
  `ci/measurements.sh --check` diffs it against the file's own `##` headings.
  Beside it, pointers to the four documents that sit next to this one.

- **CONTRIBUTING.md and SECURITY.md exist.** CONTRIBUTING.md is a map rather
  than a second copy of the rules -- they stay in README, and it says where
  each one is. SECURITY.md is new content: how to report privately, the
  threat model, and the OSS-Fuzz asymmetry stated plainly (FreeType and
  HarfBuzz are continuously fuzzed; SheenBidi and libunibreak are not, and
  both parse the same untrusted text).

- **libunibreak's three initialisers are called.** `init_linebreak`,
  `init_wordbreak` and `init_graphemebreak` are what its headers ask a caller
  to run before `set_*breaks_*`, and ztypeset had never run them. In 7.0.0 all
  three are empty, which is precisely why the omission was invisible -- and
  why it is fixed rather than documented: upstream may give them a body in any
  release, and a version that started interning a table there would charge
  that allocation to whichever host allocator happened to be installed at the
  first paragraph. That is the defect the five HarfBuzz warm-up calls exist to
  prevent, arriving silently on a routine re-vendor. They go in `ztypesetWarmup`,
  which is where a process pays its one-time costs on purpose.

- **26.6 fixed point converts to pixels in one place.** `(float)x / 64.0f` was
  written out at **26** sites across three translation units, four of them per
  glyph in the shaping loop. `ztypesetFrom266` is the one home, multiplying by an
  exactly representable reciprocal rather than dividing -- the same value for
  every finite input, and four multiplies instead of four divides per glyph in
  an unoptimised build. `ci/measurements.sh --check` greps for the divisor.

- **ztypeset's own C compiles with `-Wall -Wextra -Werror`.** It never had, so
  ztypeset's C was the only code here whose warnings nobody had to read. The flag
  list is separate from the one `libs/` is built with and always will be:
  turning ztypeset's standards into build failures for four upstream projects
  would mean patching them locally, which the vendor rules forbid. The C test
  translation units get the same treatment. Clean at zero warnings on
  x86_64-windows-gnu the first time it was run -- and not on
  x86_64-windows-msvc, where it named a real portability fault in the test
  drivers the same day. See the `fopen` entry below.

- **Three more cases that tested nothing, found by the verdict that names
  them.** The first full sweep after **DID NOT COMPILE** was added reported it
  three times. Two were the same `-Werror` mechanism as before -- a mutation
  that returned a constant left a local unused -- and are now written to narrow
  a range and to drop a bound, so the code still reads the value and then gets
  it wrong. Widening the cap bound by three was tried first and reported NOT
  CAUGHT: the test probes with 99, so a mutation has to accept 99 to be seen.
  A mutation that is too small to reach the fixture tests nothing either. The third was not that at all: turning
  `FT_CONFIG_OPTION_USE_HARFBUZZ` off is caught by an `#error` in
  `ffi/ztypeset_abi.c`, so the build cannot be produced, and the case had simply
  been expecting a weaker guard than the one that holds. It now expects the
  compile error, which is what the ABI cross-check cases have always done.

- **Nine modules compile C; the sanitizer setting reaches all nine, and now
  has to.** The three C test executables were built without
  `sanitize_c` while every library had it, so one link could hold an
  unsanitized library and a default-sanitized test object -- and a trap raised
  in the test half reads as a crash in the library half. It was one of four
  candidate causes on the table while the intermittent segfault was being
  chased, and eliminating it took a run of its own. `ci/measurements.sh
  --check` now matches every identifier handed a C source file against every
  identifier handed the build's sanitizer setting, and names any module in the
  first set and not the second. A guard case takes the line off one of them.

- **`-Dshared` was a build option nobody could find.** `build.zig` declares
  two options; README documented one of them by name and referred to the other
  only as "a shared build", so a caller wanting a `.dll` or a `.so` had to read
  `build.zig` to learn what to type. README now carries the whole set as a
  table -- flag, default, and what it changes -- and `ci/measurements.sh
  --check` extracts every `b.option` name from `build.zig` and fails if one of
  them is not named in README.md. A guard case renames the option on the
  command line only, which is the shape of the bug that has no other detector:
  the flag becomes inert, `ci/run.sh` still passes it, and every test still
  passes -- with the sanitizer silently off.

- **A process-wide allocator installed for a test and taken out only if the
  test passed.** Two tests in `src/integration_test.zig` install a ztypeset
  allocator backed by a `DebugAllocator` living in the test's own frame, and
  called `ztypeset.resetAllocator()` as the last statement of the body. An
  assertion above it returns early, so a FAILING run of either left ztypeset
  allocating and freeing through a frame that had ended -- for every test after
  it, in the same process. It is the defect the C smoke test was already fixed
  for, in the language the wrapper is written in.

  It does not announce itself, and the way it surfaced says why that matters:
  the mutation harness ran for 55 minutes on a single case with no output and
  no verdict, and every process in the chain was idle. What the mutation broke
  was the assertion in one of these two tests. The reset was never reached, and
  the next allocation went through the dead frame.

  Both now scope the install to a block and `defer ztypeset.resetAllocator()`,
  which is the only construct that covers every path out.

  Held by a gate and a guard case, because neither the suite nor the sweep can
  see this on its own -- the damage is in the FAILING run of a test, which is
  not a state a passing suite reaches. `ci/measurements.sh --check` reads every
  `test` block in the file and requires the deferred reset in any that installs
  an allocator (measured: 3 install one, 3 defer the reset); the guard case *a
  test allocator taken out only on the happy path* deletes one of the defers
  and requires the gate to say so.

- **The mutation applier read source with the locale's codec.** `open(path)`
  decodes using whatever the platform's locale says, so on a Windows console --
  cp1252 -- reading a file that holds any non-ASCII text raised
  `UnicodeDecodeError` and the case was reported **NO ANCHOR**. That verdict
  means "the code this case names has moved, update the case", so the harness
  was telling a maintainer to edit a case that was correct. Measured on
  `src/integration_test.zig`, whose fixtures carry Hebrew and Arabic.

  The same two calls wrote in text mode, which translates every newline to the
  platform's: applying a one-line mutation on Windows rewrote all 724 lines of
  `ffi/ztypeset_raster.c` as CRLF. The build did not care, and anyone diffing the
  kept working copy to see what a case actually changed did.

  Both now pass `encoding="utf-8", newline=""` on the read and the write, which
  is the only pair that makes the harness behave the same on every host it
  runs on -- and it runs on four.

- **Six guard cases proved nothing, and now say so when they do.** Four
  applied a mutation that could not compile: each deleted the last use of a
  variable or the last call to a function, and `-Werror` stopped the build on
  the unused one before any test ran. The verdict read "expected to see: <a
  real test name>", which is true and useless. They now break the value a
  function returns or the flag a branch reads, so the code still compiles and
  then misbehaves -- which is what a mutation has to do to test anything.

  The other two expected a test that is not the first to fail. `zig build`
  replaces the tail of its own output once a failure carries a long trace --
  measured: one failing test is enough, not two, which is what the harness and
  the README both said -- so the named test's line was never printed and the
  case read as TRUNCATED. Each now names the test that fails first.

  Both states are now verdicts of their own rather than a generic wrong
  failure: **DID NOT COMPILE** says the mutation broke the build, and points
  at the mutation rather than the expectation.

- **A guard case can no longer hang the sweep.** `ci/check-guards.sh` ran
  each case's command straight into a command substitution, unbounded: the
  output went down a pipe the harness only drained at the end, and nothing put
  a limit on how long a case could take. A mutation is a deliberate bug and a
  bug is not obliged to terminate, so a case that hung took the sweep with it
  -- measured here as a run stalled for nearly an hour on one case, with no
  verdict, no further output, and nothing in the harness able to say so.

  Both halves are fixed in `run_guarded`, now the only place a case's command
  is run: the output goes to a file, so there is no pipe for a build and its
  reader to deadlock across, and the command is given a deadline of twenty
  times the baseline build measured at the start of that same sweep -- a bound
  that scales with the machine instead of expiring on a slow one. A case that
  reaches it is killed and reported **TIMED OUT**, and the sweep stops rather
  than continuing: the command is dead but a build it started may not be, so
  the working copy is kept and named instead of deleted under a live process.

  The gate: `ci/measurements.sh --check` holds the guard command to one call
  site, inside `run_guarded`; a second site would be unbounded again and would
  read exactly like the first. Guard case: "a guard case run unbounded again",
  which is the one case that mutates the file it lives in.

- **The guard table is the harness's case names, not a paraphrase of them.**
  README's table of what `ci/check-guards.sh` breaks had a hand-written
  sentence per section, and a gate that compared only the section names --
  so the sentences could and did fall behind: two sections listed five cases
  each while the harness ran seven. The right column is now generated from
  the case names, in the harness's order, and the gate diffs the whole row.
  A renamed case, a new case, a deleted row and a reordered table are all
  caught; the rows that were behind are correct by construction rather than
  by proof-reading.

- **And README.md was doing it to itself.** The check above looked at the
  other documents; README carried two second copies of its own. Its
  quick-reference list said the anchor pass proves "all 90" cases still apply
  while the sentence the script reads said 92 -- the stale copy was the one a
  reader meets first. A measurement table restated the warm-shape count that
  the testing section states and the script checks. Neither carries a number
  now. The check reads README for a SECOND LINE holding a value it
  recomputes, counting lines rather than matches, because `294/294 passed` is
  one number written once.

  What it cannot do is written beside it: a copy that has already gone stale
  holds the old value, and the check searches for the current one. Nothing
  detects that. What prevents it is there being one place to write the
  number, which is the rule this enforces rather than the symptom it finds.

- **A number the documentation states has one home, and a script says so.**
  `ci/measurements.sh --check` recomputes every number README.md states, and
  nothing stopped another document from writing one down as well. Two had:
  SECURITY.md carried the entry-point count, and CONTRIBUTING.md carried an
  approximate count of the mutation cases -- in the paragraph telling a
  contributor that a number needs the line that recomputes it. Neither is a
  number now; both point at what does the counting. The check searches the
  other prose documents for the values it recomputes and fails on a second
  copy, and `ci/check-guards.sh` plants one to prove it fails. CHANGELOG.md is
  deliberately not searched: a released entry states what was true at that
  release, and editing it later would turn the history into a second, lying
  copy of the present.

- **The threat model has one home too.** README.md restated SECURITY.md's
  OSS-Fuzz table -- the two project names, the fuzzer count and the date, in
  both files. README now states the consequence and points at the document
  whose subject it is.

- **The package ships the files its own build graph reads.** `build.zig`
  compiles `examples/quickstart.zig` -- twice, once as a program and once as
  the bytes the documentation is diffed against -- and `examples` was not in
  `build.zig.zon`'s `paths`. A consumer fetching ztypeset received a build graph
  naming a directory its package did not contain. CONTRIBUTING.md and
  SECURITY.md were absent the same way, having been added without the list
  being revisited.

  Nothing in the repository could have noticed: every file is present in a
  checkout, and `tests/consumer` resolves ztypeset by local path rather than by
  fetch, so `paths` is invisible to the one test written to stand where a
  consumer stands. `ci/measurements.sh --check` now compares the repository's
  top-level entries against `paths`, with a short exclusion list beside it --
  `.git`, `.zig-cache`, `zig-out`, `.gitignore` -- where adding an entry is a
  visible edit rather than a silent omission.

- **Both source-hygiene gates now cover the files they were about.**
  `zig fmt --check` read `src`, `tests/fonts.zig` and `build.zig` and not
  `tests/consumer` -- the one Zig file a newcomer is most likely to open,
  since it is where the dependency-consumer path is written down, and the
  only one nothing formatted. `ci/check-columns.sh` read `ffi/` and not
  `tests/`, which is the rest of ztypeset's own C, built with the same warnings
  and held to the same standard; twelve lines there had drifted past eighty
  columns while ffi/ could not hold one. Both are wrapped, and both
  directories are checked.

  The same script also claimed that counting bytes per line "is what keeps
  ffi/ ASCII". It never did: a short line of UTF-8 passes a byte count. ASCII
  is the property that makes a byte count a column count, so it is now
  checked on its own -- which found the one non-ASCII byte in ztypeset's C, an
  em dash in the bench banner.

- **Two mutation cases expected a count the gate had stopped printing.**
  Adding README.md as the fourth version home changed the failure line from
  "the three version homes disagree" to "the four", and the two cases that
  plant a version mismatch still quoted the old wording -- so both would have
  reported a caught mutation as a hole in the suite. A verdict string that
  repeats a number is a copy of that number; they match on the part of the
  sentence that says what went wrong and nothing on how many.

- **The C tests read a font in one place, and the msvc ABI builds again.**
  `-Wall -Wextra -Werror` on ztypeset's own C is correct on the gnu ABI and was
  a build failure on the msvc one: Microsoft's UCRT marks ISO C's `fopen`
  deprecated in favour of Annex K's `fopen_s`, which neither glibc nor musl
  implements. The three test drivers had each written out their own
  read-a-file-into-memory routine -- and they were three qualities of it, two
  leaking the `FILE*` on a short read and checking neither `fseek` nor
  `ftell`. `tests/ztypeset_test_io.h` is the one home; the deprecation is
  suppressed around that single call rather than by defining
  `_CRT_SECURE_NO_WARNINGS`, so every other deprecation the CRT reports still
  fails the build. `ci/measurements.sh --check` refuses any `fopen` in a test
  translation unit.

- **Every handle goes to its `*Destroy` exactly once, and the header no
  longer suggests two of them are exempt.** `ztypesetLibraryDestroy` and
  `ztypesetFontDestroy` open with `if (h == NULL || h->destroy_requested)
  return;`, which reads like a repeat guard. It is not one. The flag exists so
  that whichever of a library and its fonts is released SECOND performs the
  teardown -- and by the time a caller could repeat the call, that teardown
  has already freed the handle, so the flag test is itself the use-after-free.
  A test written to pin the supposed tolerance segfaulted on that line.

  All six `*Destroy` entry points now document the same rule, in their own
  documentation rather than only in the ownership preamble, and
  `ci/measurements.sh --check` fails if one of them stops saying it. Each Zig
  `deinit` says it too, with the reason a copyable handle cannot be protected
  from it.

  No runtime check is possible and the header says why: a poison word read
  back on the second call would be the use-after-free doing the reporting, and
  ztypeset's own sanitiser build would be right to flag it. What is checked stays
  checked -- every `*Destroy` accepts NULL, swept over all 88 entry points.

- **The downstream consumer links every artifact ztypeset installs.** It linked
  four of five and had never linked libunibreak. `dependency.artifact(name)`
  panics on a name the dependency does not register, and no in-repo test goes
  through that path -- which is the entire reason `tests/consumer` exists, so
  the one hole in it was the one thing it could not see. `ci/measurements.sh
  --check` now compares build.zig's installed artifacts against the names the
  consumer passes. The artifact is `unibreak` rather than `libunibreak`
  because Zig adds the `lib` prefix itself; the pin is the project name, and
  the two namespaces are documented where they meet.

- **The mutation harness can be asked the cheap question, and it stopped
  keeping its own section list in two places.** `ci/check-guards.sh --anchors`
  checks that all 90 cases still quote the tree exactly once, mutating nothing
  and building nothing: fifteen seconds against the minutes a full sweep
  costs. A refactor strands an anchor, and reaching that verdict the slow way
  is long enough that nobody runs it before pushing -- which is how a case
  comes to be stranded. It runs in the static job, on all three hosts.

  The harness also gained a third verdict, **TRUNCATED**. When two tests fail
  at once `zig build` replaces the tail of its own output with `unable to read
  results of configure phase`, so the second failure's diagnostics never
  appear and a CAUGHT mutation reads as a hole -- which happened twice while
  the ABI probe cases were being written. The state is now named rather than
  reported as a wrong failure.

  And the README table listing the harness's sections was a hand-kept mirror:
  four sections had been added and none of them reached it. Its left column is
  now the section name itself, and `ci/measurements.sh --check` fails if
  either list holds a name the other does not. The section holding the five
  licence cases is now called "Versioning and licences", which is what it is.

- **The documentation says what is true, and two more of its claims are
  gated.** Five separate statements had drifted from the tree, and they had
  drifted the same way: each was a fact written out in prose, beside a file
  that already held it.

  * Six places said ztypeset vendors **three** upstreams. It has vendored four
    since libunibreak arrived. `ffi/ztypeset.h`'s banner line is the one that
    matters -- it is the first line a consumer reads, and it is a LIST rather
    than a count -- so `ci/measurements.sh --check` now requires every name in
    `src/pins.zig` to appear in it.
  * The README said all of FreeType, HarfBuzz and SheenBidi are continuously
    fuzzed by OSS-Fuzz. FreeType and HarfBuzz are; **SheenBidi and libunibreak
    have no OSS-Fuzz project at all**, and both parse the same untrusted text.
    Stated with its source and the date it was checked.
  * The platform section carried a hand-written list of which suites had been
    run, on which machine. It was true when written and had no way to stay
    true. The badge is the authority; the paragraph now says so and nothing
    more. It also said two of the eight cross targets duplicate an executed
    configuration -- there are three, and they are named.
  * **Thirty public functions of the Zig wrapper were named nowhere in the
    README.** They are now, and `ci/api-surface.sh` fails if a public function
    is named nowhere in it -- the same shape as the entry-point table, with
    the same declared-exception list.
  * `ffi/ztypeset_shape.c` said deleting the warm-up call leaks "4 blocks and 500
    bytes". Measured after the HarfBuzz re-vendor it is 6 and 550. The count is
    HarfBuzz's and moves with its version, nothing recomputes it, so the
    comment no longer asserts one; the guard case matches on the invariant it
    always should have.

  `ci/api-surface.sh` also stopped printing a DECLARED gap in red. A decision
  and an oversight looked identical, and the one declared gap in the table was
  read as an unfilled column by someone doing what the colour told them.

- **FreeType's build switches are what `ffi/ztypeset_ftoption.h` says they are.**
  That file is a page of prose about macros, and one paragraph of it was
  false: undefining `FT_CONFIG_OPTION_MAC_FONTS` was said to drop
  `src/base/ftrfork.c`'s resource-fork guessing heuristics with it. It does
  not. `ftbase.c` includes `ftrfork.c`, and the heuristics -- a table of
  guesses over attacker-visible bytes -- sit under
  `FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK` alone; `MAC_FONTS` gates only
  `FT_Raccess_Guess`'s outer entry point. The heuristics were in every binary
  ztypeset has ever produced, with a comment saying they were not.

  They are now undefined, which selects `ftrfork.c`'s other branch: a stub
  that reports the format unsupported. Nothing ztypeset exposes could reach
  either branch -- faces come from memory, and there is no path-based entry
  point -- so this removes parser surface rather than behaviour.

  The structural fix is the second half. Each of the six switches that file
  turns off or on is now an `#error` in `ffi/ztypeset_abi.c`, so a claim about a
  macro and the macro cannot part again, and `ci/check-guards.sh` deletes the
  `#undef` to prove the refusal fires. A claim about a macro is exactly the
  kind a build can check, and this one went unchecked for the life of the
  package.

- **The two pieces of process-wide state written after start-up are atomic.**
  The face generation counter was a plain `++` on a shared `static`, and
  SheenBidi's one-time allocator install was a plain check-then-set. `ztypeset.h`
  asks callers to use one `ZtypesetLibrary` per THREAD, which makes both of them
  concurrent by the header's own design.

  The generation counter's old comment argued that a torn increment "only ever
  produces a value that fails to match, which is the safe direction", and the
  arithmetic in it holds -- a lost update leaves the counter at old+1 twice,
  so a newly issued generation still exceeds every generation already issued
  and a stale shaper still refuses. It is an argument about the wrong thing. A
  plain read-modify-write on an object two threads reach is a data race, and a
  data race is undefined behaviour in C11 whatever the machine would have
  done. It is now a relaxed `atomic_fetch_add`: nothing is published through
  the counter, so it needs to be unique, not ordered.

  The install is now a three-state handshake. The hazard there is not the
  duplicate allocator object but `SBAllocatorSetDefault` -- a process-wide
  store SheenBidi reads without synchronisation, from calls already in flight
  -- so a loser waits for the winner rather than installing a second one, and
  a failed allocation returns the state to "nobody has tried" so a later call
  may succeed.

  `ztypeset.h`'s "Thread safety" section now states, once, that
  `ztypesetSetAllocator` and `ztypesetRegisterAllocator` are start-up operations:
  that restriction is theirs alone, and it is the only one left outside the
  per-library rule.

- **The internal contracts have a test that can reach them.**
  `ztypesetTextDecode` read the code unit at `index` before comparing `index` to
  `length`, so an index at the end read one past the buffer -- and in the
  UTF-8 case up to four past it, because `length - index` underflows to
  SIZE_MAX there and the continuation bound could never fire. The bound is now
  one comparison before the switch, covering all three encodings, answering
  `index >= length` with U+FFFD and a step of one rather than leaving it
  undefined.

  The structural cause was not the missing comparison. `ffi/ztypeset_internal.h`
  declares helpers `ztypeset.h` never exposes, and nothing could reach them: the
  Zig suite enters through the public header and the two C tests link the
  installed library, which exports only `ZTYPESET_API`. An internal precondition
  was checkable in exactly one way -- by reading every caller and finding none
  that violates it -- and "no caller does that today" is not a property a
  header can promise about tomorrow. `tests/c_internal.c` is the caller that
  can: it compiles the `ffi/*.c` units into itself rather than linking
  libztypeset, so it runs on the shared and MSVC arms too.

- **The ABI handshake now proves itself.** `ZtypesetAbiProbe` gained
  `ZtypesetCharmap`, `ZtypesetVariationAxis`, `ZtypesetVariation`, `ZtypesetMatrix`,
  `ZtypesetStroke` and `ZtypesetOutlineFuncs`, and `ZtypesetAbiLayout` gained
  `stroke_size`/`stroke_align` and the three stroke enums' sizes and last
  enumerators.

  Both structs said something they did not do. `ztypesetAbiProbe` documents
  itself as "every plain-data type ztypeset hands across the boundary, in one
  struct" and covered ten of sixteen. `ZtypesetAbiLayout` was checked against a
  hand-written list of expectations, and four of its fields -- `charmap_size`,
  `charmap_align`, `matrix_size`, `matrix_align` -- were checked by nothing at
  all, having been added to the struct without being added to the list.

  Both are now checked by construction rather than by a list. Every
  `ZtypesetAbiLayout` field's expectation is derived from its own NAME
  (`<type>_size`, `<type>_align`, `<type>_offset_<field>`, `<enum>_last`,
  `<enum>_count`), so a field added without a rule is a compile error and a
  field added with one is checked the moment it exists. Every extern struct in
  `src/c.zig` must appear in `ZtypesetAbiProbe`, checked at comptime. And every
  probed field must carry a marker the library actually wrote, with no two
  fields of one type sharing one -- which is the property that makes a
  transposition detectable at all.
- **A pen traced round every glyph.** `ztypesetFaceSetStroke(face, const
  ZtypesetStroke*)` and `ztypesetFaceStroke`, with `ZtypesetStroke` carrying a `radius`
  in pixels, a `miter_limit`, and a `ZtypesetLineCap`, `ZtypesetLineJoin` and
  `ZtypesetStrokeStyle` -- the glyph `GROWN` by the radius, the glyph `SHRUNK`
  by it, or the hollow `BAND` the pen sweeps between them, each named for the
  picture it produces rather than for the FreeType border it comes from. NULL or a zero radius clears it, and a face is created
  with none. Zig: `Face.setStroke`, `Face.stroke`, `ztypeset.Stroke`,
  `ztypeset.stroke_none` and `ztypeset.outline(radius)`.

  Outlined text was not reachable at all: FreeType's stroker lives in
  `ftstroke.c`, which this build did not compile, and `ftstroke.h` was
  therefore not installed either. Both now are, and `ci/header-link.sh` is
  what proves the header and its implementation arrived together.

  Three decisions in it. The radius is in PIXELS where synthetic bold's
  strength is a fraction of the em, because a weight is a property of the
  design and has to hold across sizes while a pen is an ornament drawn for a
  display -- and pixels are also FreeType's own unit for `FT_Stroker_Set`.
  Composition is fixed at design, then ornament, then map: synthetic bold and
  oblique, then the pen, then the face's matrix, so the pen traces the glyph
  the font describes and is not widened by the emboldening or turned into a
  device-space pen by the matrix. And no advance moves, as in FreeType's own
  `FT_Glyph_Stroke`: a stroked glyph is wider than its ink box by the radius
  on each side, and the run is still laid out on the font's advances -- which
  also means setting a pen stales no shaped measurement.

  A cap, join or style this build does not name is
  `ZTYPESET_RESULT_INVALID_ARGUMENT` rather than a silent fallback, and so is a
  radius too large for the 26.6 fixed point it is converted to, which would
  otherwise read as "no pen".
- **A 2x2 transform per face.** `ztypesetFaceSetTransform(face, const ZtypesetMatrix*)`
  and `ztypesetFaceTransform`, with `ZtypesetMatrix` holding `xx`, `xy`, `yx`, `yy`
  as floats, y up. NULL clears it, and a face is created with the identity.
  Zig: `Face.setTransform`, `Face.transform`, `ztypeset.Matrix`,
  `ztypeset.matrix_identity`, and `ztypeset.rotation`, `ztypeset.scaling`,
  `ztypeset.shear` as builders.

  FreeType's `FT_Set_Transform` had no way through the API, so a rotated or
  mirrored glyph was not reachable at all. Two things about this one are
  decisions rather than transcription. It is applied AFTER any synthetic bold
  or oblique, so emboldening stays isotropic in the font's own space instead of
  being stretched by the caller's map -- upstream has no opinion, because
  upstream applies its transform at load and its synthesis afterwards. And it
  reaches the glyph IMAGE and no advance, where `FT_Set_Transform` transforms
  the advance too: a shaped run's advances come from HarfBuzz, which has no
  matrix to be told about, so transforming FreeType's and not HarfBuzz's would
  make the two disagree by exactly the caller's matrix. There is also no
  translation term, because `ztypesetFaceRenderGlyph`'s `offset_x`/`offset_y`
  already is one.
- **Which character map a font uses is now a choice.** `ztypesetFontCharmapCount`,
  `ztypesetFontCharmap`, `ztypesetFontActiveCharmap`, `ztypesetFontSelectCharmap` and
  `ztypesetFontSelectCharmapEncoding`, with `ZtypesetCharmap` carrying the
  `(platform_id, encoding_id)` pair the font records and FreeType's reading of
  it as a four-character tag (`ZTYPESET_CHARMAP_UNICODE`,
  `ZTYPESET_CHARMAP_MS_SYMBOL` and the rest of `FT_Encoding`, republished). Zig:
  `Font.charmapCount`, `charmap`, `activeCharmap`, `selectCharmap`,
  `selectCharmapEncoding`, `ztypeset.Charmap` and the tag constants.

  FreeType selects a Unicode map when it opens a font, which is what every
  previous version got and could not change. An icon font whose glyphs live
  only in a (3, 0) MS Symbol map has no Unicode map to select, so its glyphs
  were reachable by index alone. The selection governs `ztypesetFontGlyphIndex`
  and `ztypesetFontCoveredPrefix`; it does not reach shaping, because HarfBuzz
  maps characters through its own reader of the same tables and never consults
  FreeType's selection. Selecting an encoding the font does not carry is
  `ZTYPESET_RESULT_INVALID_ARGUMENT`, which is how "does this font have a symbol
  map" is asked.
- **`ztypesetShaperShapeRun` / `Shaper.shapeRun(face, paragraph, run, params)`** —
  shape one run of a paragraph, with the paragraph as its own text. It removes
  three things at once: a run applied to the wrong buffer cannot be expressed,
  the direction and script come from the run (and a `params` that also sets
  them is refused rather than quietly losing), and the text is not validated
  again — `ztypesetShaperShape` walks the whole borrowed buffer on every call,
  so iterating an N-unit paragraph's R runs through it cost R walks of N.
  Measured at ~1.05 µs a call for a 4300-unit paragraph; see README.md.

### Changed

- **Synthetic bold and oblique are amounts, not switches, and they reach
  shaping.** `ztypesetFaceSetSyntheticBold(face, float strength)` and
  `ztypesetFaceSetSyntheticOblique(face, float slant)` replace the `int enabled`
  pair; `ZTYPESET_SYNTHETIC_BOLD_DEFAULT` (0.041656494, FreeType's own
  `0x0AAA/65536`) and `ZTYPESET_SYNTHETIC_OBLIQUE_DEFAULT` (0.212554932, its
  `0x0366A/65536`) are the reference weights, exported to Zig as
  `ztypeset.synthetic_bold_default` and `ztypeset.synthetic_oblique_default`.
  Nothing is clamped, and a strength that is not finite is
  `ZTYPESET_RESULT_INVALID_ARGUMENT`.

  The strength used to be two `#define`s no caller could reach, and it used to
  apply at FreeType's glyph loading only — the header said so, which made a
  documented limitation of the defect: a SHAPED run's advances come from
  HarfBuzz and never pass through glyph loading, so bold text was laid out at
  the unstyled font's widths and overlapped its own ink, one glyph at a time.
  Both HarfBuzz fonts a face owns are now told the same two numbers
  (`hb_font_set_synthetic_bold` with `in_place = false`, and
  `hb_font_set_synthetic_slant`), including the FreeType-backed one, which is
  built lazily and can come into existence long after a style was set.
  HarfBuzz's advance arithmetic is FreeType's — `round(|scale| * embolden)`
  against `ppem * 0x0AAA / 1024` — so one fraction of the em drives both.
  Setting either style moves the face's generation, so extents for a run
  shaped before the change are refused rather than mixed.
- **`Paragraph.init` takes an options struct**: `init(text, .{})` where it
  used to be `init(text, .auto)`, with `.base` and `.segmentation` in it.
- **`ztypesetParagraphNextGrapheme` and `ztypesetParagraphPreviousGrapheme` return
  `offset` unchanged** when the paragraph has no grapheme pass, rather than
  jumping to the end of the text and to 0 respectively.
- **`Shaper.shapeRun` no longer takes the text**, and the range form of the old
  one is now `Shaper.shapeRange(face, text, offset, length, params)` — same
  call as before, named for what it does. `shapeRun` is for runs a `Paragraph`
  or a `Line` produced; `shapeRange` is for a range of a buffer ztypeset has not
  seen.
- **Text may be UTF-8, UTF-16 or UTF-32.** Every entry point that takes text
  takes a `ZtypesetEncoding` with it, and every offset and length beside it — a
  run's, a line's, a glyph's cluster, an index into a break array — is counted
  in that encoding's code units. All three upstreams take all three natively
  (SheenBidi's `SBStringEncoding`, libunibreak's `set_*_utf8/utf16/utf32`,
  `hb_buffer_add_utf8/utf16/utf32`), so ztypeset transcodes nothing and a host
  whose strings are UTF-16 pays no copy, no allocation and no offset mapping.
  Three entry points are renamed rather than duplicated, because an adapter
  taking UTF-8 alone would be a second way to say the same thing:
  `ztypesetShaperShapeUtf8` → `ztypesetShaperShape`, `ztypesetParagraphCreateUtf8` →
  `ztypesetParagraphCreate`, and `ztypesetFontCoveredPrefix` gains the same
  parameter. `ztypesetParagraphEncoding` reports what a paragraph was built from,
  because a run list is routinely passed on without its text.
- **`ZTYPESET_RESULT_INVALID_UTF8` is `ZTYPESET_RESULT_INVALID_TEXT`** (same value,
  3), and the Zig error `InvalidUtf8` is `InvalidText`: it now means "not
  well-formed in the encoding it was passed in", which for UTF-16 is an
  unpaired surrogate and for UTF-32 is a unit that is not a scalar value.
- **Zig callers never name an encoding.** `Paragraph.init`, `Shaper.shape`,
  `Shaper.shapeRun` and `Font.coveredPrefix` take the text itself, and the
  encoding is read off the slice's ELEMENT type — `u8`, `u16` or `u32`. The
  one mistake the C ABI cannot prevent, text in one encoding declared as
  another, does not compile here. The cost is that `&.{ 0xFF, 0xFE }` no
  longer infers a `[]const u8`: write `&[_]u8{ ... }`.
- **`ztypeset.setAllocator` takes a `std.mem.Allocator` by value.** It took a
  `*const std.mem.Allocator` whose pointee the caller had to keep alive for
  longer than a caller can compute: the C side copies the bridge — `user`
  included — into every `Library`, and a library's FreeType memory outlives
  the install that made it. ztypeset now copies the allocator into a slot of its
  own, one per distinct allocator ever installed, allocated with `malloc` and
  never freed — the same bargain `ffi/ztypeset_core.c` already makes for its
  registry entries. Installing the same allocator twice reuses its slot, which
  is gated.
- **`ztypesetSetAllocator` warms the process-lifetime caches before it installs.**
  HarfBuzz's singletons are never freed in this build, so whichever allocator
  paid for one can never report a balanced heap; the fix was a documented rule
  saying to call `ztypesetWarmup` first, which a host could only discover by not
  following it. The warm-up now happens inside `ztypesetSetAllocator`, before the
  swap, so the caches are charged to whatever was installed before the call.
  `ztypesetWarmup` stays public for the two caches that need a real face and so
  cannot be reached from inside ztypeset. No test warms up by hand any more,
  which is what makes the mutation that deletes the internal call meaningful.
- **No handle has a destruction order.** A `ZtypesetLibrary` may be destroyed
  before the fonts and faces made from it: whichever of a pair is released
  second frees what they share. Previously a library destroyed first left every
  font holding a freed `FT_Face` — `FT_Done_Library` destroys the faces still
  registered with it — and the rule against it lived only in a comment. The
  cost is one `size_t` and one `bool` per library. Building on a released
  handle is now `ZTYPESET_RESULT_INVALID_ARGUMENT` rather than undefined:
  `ztypesetFontCreateFromMemory`, `ztypesetLibraryCountFaces` and
  `ztypesetLibrarySetSdfSpread` all refuse one, as `ztypesetFaceCreate` already
  refused a released font.
- **Everything ztypeset allocates for a handle now comes from the allocator that
  issued that handle.** A face's glyph buffer is allocated lazily, the first
  time something is drawn, and was charged to whatever was installed at that
  moment — so one handle's memory could sit in two heaps, with only a host's
  own accounting to notice. `ffi/ztypeset.h` states the whole split beside
  `ztypesetSetAllocator`: handle-owned memory follows its handle, HarfBuzz's and
  SheenBidi's follows whatever is installed when they ask, because their seams
  are compile-time and carry no context.

### Fixed

- A test whose subject was `Version.format` asserted the literals `"0.1.0"`
  and `"2.14.3"` — a third home for ztypeset's version and a second for
  FreeType's. It now formats a synthetic version it owns and compares the real
  one against its own fields.
- `ztypesetFontSetVariations` held the only copy of the commit-and-notify path —
  hand the coordinates to FreeType, update the font's own copy, then move
  every face's HarfBuzz coordinates, generation and MVAR-dependent size with
  them. `ztypesetFontSetNamedInstance` needed the same path, so it was extracted
  into one helper rather than copied; a second copy that forgot one of those
  four steps produces text that merely spaces wrongly.

## 0.1.0

The initial ABI: the entry points, the plain-data types, the
`ztypesetAbiLayout`/`ztypesetAbiProbe` handshake, and the vendored FreeType,
HarfBuzz, SheenBidi and libunibreak that stand behind them.

This file starts here rather than reconstructing what came before it. The
commit history is the record of that, and a summary written after the fact
would be a second home for it with none of its precision.
