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
| `ZtextAbiLayout` | 192 B | 224 B | `glyph_offset_flags`, `glyph_bitmap_offset_format`, `bitmap_format_size`, `bitmap_format_last`, `glyph_flag_size`, `glyph_flag_last`, `metric_size`, `metric_count` |

`ZtextMetric` reports a **count** rather than a last value, alone among the
enums in the handshake: its enumerators are four-character OpenType tags, so
"the highest one" is an accident of spelling and says nothing about the range.
The count is generated from `ZTEXT_METRIC_LIST`, which is also what the enum
itself is generated from, so it cannot be a number someone forgot to raise.

`ztextAbiProbe` writes a marker into each new field, so a consumer's mirror is
checked against what the library does rather than against what the header
says.

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
