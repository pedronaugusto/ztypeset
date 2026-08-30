# Changelog

Every version of ztext, newest first, and the rules the numbers follow.

**No dates.** Git holds when each change landed, precisely and without anyone
typing it; a date written here would be a second home for that, and a second
home is the one that goes quietly wrong. What this file records is what
changed and what it costs a consumer.

## How ztext is versioned

`ffi/ztext.h`'s `ZTEXT_VERSION_MAJOR`, `ZTEXT_VERSION_MINOR` and
`ZTEXT_VERSION_PATCH` are the one home for ztext's version. `build.zig.zon`
and the newest heading in this file mirror them, and `ci/measurements.sh
--check` fails if the three disagree — so a bump that reaches two of the three
is a red gate, not a discovery someone makes later.

The major is 0, so the **minor is the breaking position**:

| bump | means |
|---|---|
| **minor** | a consumer compiled against the previous header must be recompiled. The layout of a shared struct changed, an enum gained or renumbered an enumerator or changed tag size, an entry point's signature changed, or one was added or removed. |
| **patch** | behaviour, fixes, tests, documentation, or a re-vendor that moves no declaration in `ffi/ztext.h`. |

A **new** entry point or a **new** enumerator is a minor bump too, and
deliberately: `ztextAbiLayout` reports each enum's last value and
`ci/api-surface.sh` counts the entry points, so a consumer that checks the
handshake can see the difference — and code that switches exhaustively on an
enum is entitled to be told rather than to fall through a default.

Three rules bind every ABI change, and each is gated rather than written down
and hoped for:

1. **It lands as one commit.** The header, `build.zig.zon`, this file, the
   `ZtextAbiLayout` handshake, the `ztextAbiProbe` markers and the Zig mirror
   in `src/c.zig` move together. `src/abi_check.zig` fails the build if the
   mirror lags the header; the layout and probe tests fail if the handshake
   lags either.
2. **The handshake grows with the struct.** A field added to a shared struct
   without its offset appearing in `ZtextAbiLayout` is a field no consumer can
   check for, which is the whole failure mode the handshake exists to prevent.
3. **A version is not a promise about the library you linked.** Compare
   `ztextVersion()` against the `ZTEXT_VERSION_*` macros at startup, and read
   `ztextAbiLayout` if the two builds could ever differ — the macros describe
   the header you compiled with, the function describes the library you got.

The vendored upstreams have their own versions, recorded in `UPSTREAM.md` with
`src/pins.zig` as their one home and `ci/verify-vendor.sh` as their gate. They
move independently of this number.

## 0.2.0

### Added

- `ZtextGlyph.flags`, an OR of the new `ZtextGlyphFlag`: `UNSAFE_TO_BREAK`,
  `UNSAFE_TO_CONCAT`, `SAFE_TO_INSERT_TATWEEL` and `DEFINED`. These are
  HarfBuzz's own values, republished under ztext's names with a static
  assertion per flag. A host that wraps a paragraph can now break lines
  without re-shaping them.
- All three flags are produced on **every** shape. HarfBuzz withholds two of
  them by default because computing them costs something; ztext asks anyway,
  because a flag that is computed on some builds and not others cannot be told
  apart from a flag that is unset. The cost was measured and is below what the
  bench resolves — see README.md.
- `ZtextGlyphBitmap.format`, a `ZtextBitmapFormat` (`A8` or `SDF`), written on
  every successful render including a glyph with no ink. A8 coverage and an
  SDF are both one byte per pixel, so a consumer that remembered the wrong
  `ZtextRenderMode` previously got a washed-out picture rather than an error.
- Zig: `ztext.GlyphFlag`, `ztext.BitmapFormat` and `ztext.glyphHas`.
- `ztextFaceMetric` and `ztextFaceMetricWithFallback`, reading any of the 28
  OpenType metrics through HarfBuzz: x-height, cap-height, strikeout, the
  caret slope, the sub/superscript boxes and the rest. The new `ZtextMetric`
  carries HarfBuzz's own tag VALUES, so the mapping is the identity and
  `ffi/ztext_abi.c` asserts each against its `HB_OT_METRICS_TAG_`
  counterpart. `ZtextFaceMetrics` remains FreeType's view, which is `hhea`;
  these honour the USE_TYPO_METRICS bit and apply MVAR, and the two answer
  different questions rather than disagreeing.
- `ztextFontNamedInstanceCount`, `...Coords`, `...Name` and
  `ztextFontSetNamedInstance`: the points in a variable font's axis space that
  its designers named. Nothing can derive them from the axes — they are data,
  not a rule — and the name is decoded out of the font's `name` table, so a
  caller never meets UTF-16BE.
- `ztextFontVariantGlyphIndex`: a base character plus a variation selector,
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
- `ztextWarmup` touches a fifth process-lifetime HarfBuzz singleton: the
  default language, interned by the buffer the autohinter's coverage pass
  guesses the properties of. It is reached by hinting, not by shaping, so a
  host that audits its heap and never called it would have seen two permanent
  allocations appear at the first hinted glyph.
- Zig: `ztext.Metric`, `Face.metric`, `Face.metricWithFallback`,
  `Font.variantGlyphIndex`, `Font.namedInstanceCount`,
  `Font.namedInstanceCoords`, `Font.namedInstanceName`,
  `Font.namedInstanceNameLen` and `Font.setNamedInstance`.

### Changed — ABI

Measured on x86_64-windows, both the gnu and the MSVC ABI:

| type | 0.1.0 | 0.2.0 | |
|---|---|---|---|
| `ZtextGlyph` | 24 B | 28 B | `flags` at offset 8, after `cluster` |
| `ZtextGlyphBitmap` | 32 B | 40 B | `format` at offset 8, immediately after `pixels` — it has to be read before they are interpreted |
| `ZtextCharmap` | — | 8 B | new: `platform_id`, `encoding_id`, `encoding` |
| `ZtextAbiLayout` | 192 B | 248 B | `charmap_size`, `charmap_align`, `glyph_offset_flags`, `glyph_bitmap_offset_format`, `bitmap_format_size`, `bitmap_format_last`, `encoding_size`, `encoding_last`, `segmentation_size`, `segmentation_last`, `glyph_flag_size`, `glyph_flag_last`, `metric_size`, `metric_count` |

`ZtextMetric` reports a **count** rather than a last value, alone among the
enums in the handshake: its enumerators are four-character OpenType tags, so
"the highest one" is an accident of spelling and says nothing about the range.
The count is generated from `ZTEXT_METRIC_LIST`, which is also what the enum
itself is generated from, so it cannot be a number someone forgot to raise.

`ztextAbiProbe` writes a marker into each new field, so a consumer's mirror is
checked against what the library does rather than against what the header
says.

### Added

- **A paragraph runs the segmentation passes it is asked for.**
  `ztextParagraphCreate` takes a `segmentation` mask of `ZtextSegmentation`
  (`LINES`, `GRAPHEMES`, `WORDS`, `ALL`), and `Paragraph.init` takes it in an
  options struct that defaults to all three. It used to run all three always
  and allocate three bytes per code unit for their arrays, with no way to
  decline: measured on a 4300-character paragraph, that is 56.6% of the time
  to build one (265 µs against 115 µs) and 12 916 B of its 26 898 B — more
  than the copied text and the embedding levels together. The accessor for a
  pass that was not run answers NULL (an empty slice in Zig) and
  `ztextParagraphSegmentation` reports what ran, so an absent array is never
  mistaken for a text with no boundaries. A bit this build has no name for is
  `ZTEXT_RESULT_INVALID_ARGUMENT`, as an unknown encoding is.
- **Which character map a font uses is now a choice.** `ztextFontCharmapCount`,
  `ztextFontCharmap`, `ztextFontActiveCharmap`, `ztextFontSelectCharmap` and
  `ztextFontSelectCharmapEncoding`, with `ZtextCharmap` carrying the
  `(platform_id, encoding_id)` pair the font records and FreeType's reading of
  it as a four-character tag (`ZTEXT_CHARMAP_UNICODE`,
  `ZTEXT_CHARMAP_MS_SYMBOL` and the rest of `FT_Encoding`, republished). Zig:
  `Font.charmapCount`, `charmap`, `activeCharmap`, `selectCharmap`,
  `selectCharmapEncoding`, `ztext.Charmap` and the tag constants.

  FreeType selects a Unicode map when it opens a font, which is what every
  previous version got and could not change. An icon font whose glyphs live
  only in a (3, 0) MS Symbol map has no Unicode map to select, so its glyphs
  were reachable by index alone. The selection governs `ztextFontGlyphIndex`
  and `ztextFontCoveredPrefix`; it does not reach shaping, because HarfBuzz
  maps characters through its own reader of the same tables and never consults
  FreeType's selection. Selecting an encoding the font does not carry is
  `ZTEXT_RESULT_INVALID_ARGUMENT`, which is how "does this font have a symbol
  map" is asked.
- **`ztextShaperShapeRun` / `Shaper.shapeRun(face, paragraph, run, params)`** —
  shape one run of a paragraph, with the paragraph as its own text. It removes
  three things at once: a run applied to the wrong buffer cannot be expressed,
  the direction and script come from the run (and a `params` that also sets
  them is refused rather than quietly losing), and the text is not validated
  again — `ztextShaperShape` walks the whole borrowed buffer on every call,
  so iterating an N-unit paragraph's R runs through it cost R walks of N.
  Measured at ~1.05 µs a call for a 4300-unit paragraph; see README.md.

### Changed

- **Synthetic bold and oblique are amounts, not switches, and they reach
  shaping.** `ztextFaceSetSyntheticBold(face, float strength)` and
  `ztextFaceSetSyntheticOblique(face, float slant)` replace the `int enabled`
  pair; `ZTEXT_SYNTHETIC_BOLD_DEFAULT` (0.041656494, FreeType's own
  `0x0AAA/65536`) and `ZTEXT_SYNTHETIC_OBLIQUE_DEFAULT` (0.212554932, its
  `0x0366A/65536`) are the reference weights, exported to Zig as
  `ztext.synthetic_bold_default` and `ztext.synthetic_oblique_default`.
  Nothing is clamped, and a strength that is not finite is
  `ZTEXT_RESULT_INVALID_ARGUMENT`.

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
- **`ztextParagraphNextGrapheme` and `ztextParagraphPreviousGrapheme` return
  `offset` unchanged** when the paragraph has no grapheme pass, rather than
  jumping to the end of the text and to 0 respectively.
- **`Shaper.shapeRun` no longer takes the text**, and the range form of the old
  one is now `Shaper.shapeRange(face, text, offset, length, params)` — same
  call as before, named for what it does. `shapeRun` is for runs a `Paragraph`
  or a `Line` produced; `shapeRange` is for a range of a buffer ztext has not
  seen.
- **Text may be UTF-8, UTF-16 or UTF-32.** Every entry point that takes text
  takes a `ZtextEncoding` with it, and every offset and length beside it — a
  run's, a line's, a glyph's cluster, an index into a break array — is counted
  in that encoding's code units. All three upstreams take all three natively
  (SheenBidi's `SBStringEncoding`, libunibreak's `set_*_utf8/utf16/utf32`,
  `hb_buffer_add_utf8/utf16/utf32`), so ztext transcodes nothing and a host
  whose strings are UTF-16 pays no copy, no allocation and no offset mapping.
  Three entry points are renamed rather than duplicated, because an adapter
  taking UTF-8 alone would be a second way to say the same thing:
  `ztextShaperShapeUtf8` → `ztextShaperShape`, `ztextParagraphCreateUtf8` →
  `ztextParagraphCreate`, and `ztextFontCoveredPrefix` gains the same
  parameter. `ztextParagraphEncoding` reports what a paragraph was built from,
  because a run list is routinely passed on without its text.
- **`ZTEXT_RESULT_INVALID_UTF8` is `ZTEXT_RESULT_INVALID_TEXT`** (same value,
  3), and the Zig error `InvalidUtf8` is `InvalidText`: it now means "not
  well-formed in the encoding it was passed in", which for UTF-16 is an
  unpaired surrogate and for UTF-32 is a unit that is not a scalar value.
- **Zig callers never name an encoding.** `Paragraph.init`, `Shaper.shape`,
  `Shaper.shapeRun` and `Font.coveredPrefix` take the text itself, and the
  encoding is read off the slice's ELEMENT type — `u8`, `u16` or `u32`. The
  one mistake the C ABI cannot prevent, text in one encoding declared as
  another, does not compile here. The cost is that `&.{ 0xFF, 0xFE }` no
  longer infers a `[]const u8`: write `&[_]u8{ ... }`.
- **`ztext.setAllocator` takes a `std.mem.Allocator` by value.** It took a
  `*const std.mem.Allocator` whose pointee the caller had to keep alive for
  longer than a caller can compute: the C side copies the bridge — `user`
  included — into every `Library`, and a library's FreeType memory outlives
  the install that made it. ztext now copies the allocator into a slot of its
  own, one per distinct allocator ever installed, allocated with `malloc` and
  never freed — the same bargain `ffi/ztext_core.c` already makes for its
  registry entries. Installing the same allocator twice reuses its slot, which
  is gated.
- **`ztextSetAllocator` warms the process-lifetime caches before it installs.**
  HarfBuzz's singletons are never freed in this build, so whichever allocator
  paid for one can never report a balanced heap; the fix was a documented rule
  saying to call `ztextWarmup` first, which a host could only discover by not
  following it. The warm-up now happens inside `ztextSetAllocator`, before the
  swap, so the caches are charged to whatever was installed before the call.
  `ztextWarmup` stays public for the two caches that need a real face and so
  cannot be reached from inside ztext. No test warms up by hand any more,
  which is what makes the mutation that deletes the internal call meaningful.
- **No handle has a destruction order.** A `ZtextLibrary` may be destroyed
  before the fonts and faces made from it: whichever of a pair is released
  second frees what they share. Previously a library destroyed first left every
  font holding a freed `FT_Face` — `FT_Done_Library` destroys the faces still
  registered with it — and the rule against it lived only in a comment. The
  cost is one `size_t` and one `bool` per library. Building on a released
  handle is now `ZTEXT_RESULT_INVALID_ARGUMENT` rather than undefined:
  `ztextFontCreateFromMemory`, `ztextLibraryCountFaces` and
  `ztextLibrarySetSdfSpread` all refuse one, as `ztextFaceCreate` already
  refused a released font.
- **Everything ztext allocates for a handle now comes from the allocator that
  issued that handle.** A face's glyph buffer is allocated lazily, the first
  time something is drawn, and was charged to whatever was installed at that
  moment — so one handle's memory could sit in two heaps, with only a host's
  own accounting to notice. `ffi/ztext.h` states the whole split beside
  `ztextSetAllocator`: handle-owned memory follows its handle, HarfBuzz's and
  SheenBidi's follows whatever is installed when they ask, because their seams
  are compile-time and carry no context.

### Fixed

- A test whose subject was `Version.format` asserted the literals `"0.1.0"`
  and `"2.14.3"` — a third home for ztext's version and a second for
  FreeType's. It now formats a synthetic version it owns and compares the real
  one against its own fields.
- `ztextFontSetVariations` held the only copy of the commit-and-notify path —
  hand the coordinates to FreeType, update the font's own copy, then move
  every face's HarfBuzz coordinates, generation and MVAR-dependent size with
  them. `ztextFontSetNamedInstance` needed the same path, so it was extracted
  into one helper rather than copied; a second copy that forgot one of those
  four steps produces text that merely spaces wrongly.

## 0.1.0

The initial ABI: the entry points, the plain-data types, the
`ztextAbiLayout`/`ztextAbiProbe` handshake, and the vendored FreeType,
HarfBuzz, SheenBidi and libunibreak that stand behind them.

This file starts here rather than reconstructing what came before it. The
commit history is the record of that, and a summary written after the fact
would be a second home for it with none of its precision.
