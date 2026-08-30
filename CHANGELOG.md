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

### Changed — ABI

Measured on x86_64-windows, both the gnu and the MSVC ABI:

| type | 0.1.0 | 0.2.0 | |
|---|---|---|---|
| `ZtextGlyph` | 24 B | 28 B | `flags` at offset 8, after `cluster` |
| `ZtextGlyphBitmap` | 32 B | 40 B | `format` at offset 8, immediately after `pixels` — it has to be read before they are interpreted |
| `ZtextAbiLayout` | 192 B | 216 B | `glyph_offset_flags`, `glyph_bitmap_offset_format`, `bitmap_format_size`, `bitmap_format_last`, `glyph_flag_size`, `glyph_flag_last` |

`ztextAbiProbe` writes a marker into each new field, so a consumer's mirror is
checked against what the library does rather than against what the header
says.

### Fixed

- A test whose subject was `Version.format` asserted the literals `"0.1.0"`
  and `"2.14.3"` — a third home for ztext's version and a second for
  FreeType's. It now formats a synthetic version it owns and compares the real
  one against its own fields.

## 0.1.0

The initial ABI: the entry points, the plain-data types, the
`ztextAbiLayout`/`ztextAbiProbe` handshake, and the vendored FreeType,
HarfBuzz, SheenBidi and libunibreak that stand behind them.

This file starts here rather than reconstructing what came before it. The
commit history is the record of that, and a summary written after the fact
would be a second home for it with none of its precision.
