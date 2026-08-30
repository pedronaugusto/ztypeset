# Vendored upstream

`libs/` holds pinned copies of four upstream projects, unmodified.

| | FreeType | HarfBuzz | SheenBidi | libunibreak |
|---|---|---|---|---|
| Source | <https://gitlab.freedesktop.org/freetype/freetype> | <https://github.com/harfbuzz/harfbuzz> | <https://github.com/Tehreer/SheenBidi> | <https://github.com/adah1972/libunibreak> |
| Version | 2.14.3 | 14.4.0 | 3.0.0 | 7.0.0 |
| Tag | `VER-2-14-3` | `14.4.0` | `v3.0.0` | `libunibreak_7_0` |
| Commit | `0a0221a1347e2f1e07c395263540026e9a0aa7c7` | `36cb489cb02ce4b92099669ba9f9bea348eff93f` | `cfe430e7375a7845b679adae9d51dac6deaa8858` | `3ce4bfa3129ff3738046a44a6db533d2ce25af2b` |
| Licence | **FTL** (see below) | Old MIT | Apache-2.0 | zlib |
| Local | `libs/freetype` | `libs/harfbuzz` | `libs/sheenbidi` | `libs/libunibreak` |

Commits are the tag's *peeled* target — the commit, not the tag object — which
is what `ci/verify-vendor.sh` compares `git rev-parse HEAD` against.

## The FreeType licence election

FreeType is offered under **either** the FreeType License (FTL, a BSD-style
licence with a credit requirement) **or** GPLv2. ztext takes the **FTL**, and
says so here because an unrecorded election is an argument waiting to happen:
a reader who finds only "dual licensed" can reasonably claim the GPL branch
applies.

The FTL requires the credit line to appear in documentation. `LICENSES.md`
carries it, along with the notices HarfBuzz and SheenBidi require, in the form
a consumer needs to ship.

## What was taken, and what was left

| Project | Taken | Excluded, and why |
|---|---|---|
| FreeType | `include/`, `src/`, `LICENSE.TXT`, `README`, `docs/FTL.TXT`, `docs/GPLv2.TXT` | `src/tools/` (upstream's own dev scripts). `builds/`, `tests/`, `devel/`, `objs/`, `subprojects/` and every build system — superseded by `build.zig`. The rest of `docs/` (2 MB of manual). |
| HarfBuzz | `src/`, `COPYING`, `AUTHORS`, `THANKS` | `src/__pycache__/` — upstream tracks a stray `.pyc`; it is build output and has no business in a source tree. `test/` (89 MB), `perf/`, `util/`, `docs/`, `subprojects/`. |
| SheenBidi | `Headers/`, `Source/`, `LICENSE`, `README.md` | `Tools/` (18 MB of Unicode data-file generators), `Tests/`, and the build systems. |
| libunibreak | `src/`, `LICENCE`, `README.md` | `src/generate_*.py` and `unicode_data_property.py` (the table generators), `src/*.sed` and `*.tmpl` (their inputs), `src/*Test.txt` (2 MB of UCD conformance data), `src/tests.c` and `test_skips.h` (upstream's own driver), `Makefile.*`, and `doc/`, `tools/`, `autogen.sh`, `configure.ac` — the autotools build, superseded by `build.zig`. |

The whole `src`/`include` trees are taken even where only part compiles, so the
vendored copy is a straight path-for-path match with upstream and re-vendoring
is a copy rather than a merge. **Which translation units actually compile is
decided explicitly in `build.zig`**, never by a directory glob — see the source
lists at the top of that file.

## FreeType build configuration

ztext compiles a reduced FreeType. Both configuration headers live in `ffi/`,
not in `libs/`, so the vendored tree stays pristine:

* `ffi/ztext_ftoption.h` includes upstream's `ftoption.h` and then adjusts a
  handful of macros. It is deliberately **not** an edited copy of that file,
  which is what FreeType's own `docs/CUSTOMIZE` suggests — a copy would leave
  ~1000 lines of derived upstream to re-merge on every re-vendor.
* `ffi/ztext_ftmodules.h` lists the modules registered.

Dropped: Type 1/CID/Type 42, PFR, Windows FNT, PCF, BDF, the monochrome
rasteriser, OT-SVG, zlib/gzip (and with it WOFF), and classic Mac resource-fork
fonts. Kept: TrueType, CFF/CFF2, sfnt, psaux/psnames/pshinter, the autohinter,
the smooth (A8) rasteriser, the SDF renderers, and variable-font support.

**The user-visible consequence:** a WOFF, WOFF2 or Type 1 file is reported as
`ZTEXT_RESULT_UNSUPPORTED`, not as a broken font. Cook to TTF or OTF.

## Known upstream behaviour, worked around or recorded

Written down so a future re-vendor can check whether any of it has changed, and
so the workarounds are not mistaken for arbitrary defensiveness. Every item was
found by running the code, not by reading it.

### FreeType

**`FT_Init_FreeType` lets an environment variable change rasterisation.** It
calls `FT_Set_Default_Properties` (`src/base/ftinit.c:230`), which reads
`FREETYPE_PROPERTIES` and can switch the TrueType interpreter version, the
autohinter's warping and more. Glyph output that depends on the environment is
not reproducible, and ztext's golden tests would inherit that. Worked around by
building the library with `FT_New_Library` + `FT_Add_Default_Modules` and
**not** calling `FT_Set_Default_Properties`. This also happens to be the only
way to install a per-library allocator.

### HarfBuzz

**Three environment variables change what HarfBuzz does, and two changed what
ztext rendered.** `HB_SHAPER_LIST` (`src/hb-shaper.cc:48`) replaces the shaper
list; `HB_FONT_FUNCS` (`src/hb-font.cc:2599`) replaces the default font funcs a
new `hb_font_t` gets; `HB_FACE_LOADER` (`src/hb-face.cc:371`) replaces the
loader used when a face is opened by file name.

Measured on this tree before the fix:

| variable | value | effect |
|---|---|---|
| `HB_SHAPER_LIST` | `fallback` | five golden tests fail: standard ligatures stop applying, and moving a variation axis stops moving the shaped result |
| `HB_FONT_FUNCS` | `ft` | the C smoke test reports 216 bytes leaked, and 26 blocks under the injection sweep — hb-ft's funcs open an `FT_Face` from an `FT_Library` ztext does not own |
| `HB_FACE_LOADER` | any | none: ztext builds faces from memory with `hb_face_create_or_fail` and never from a file name |

Worked around by compiling HarfBuzz with `-DHB_NO_GETENV`, which makes
`getenv(Name)` expand to `nullptr` (`src/hb.hh:427-429`) so all three read
empty. This is the same argument `ffi/ztext_face.c` already made for
FreeType's `FREETYPE_PROPERTIES`, applied to the library where it was live.
`build.zig` runs the suite and the C smoke test a second time with all three
set, so the claim is checked rather than asserted.

### FreeType, continued

**The two rasterisers disagree about `num_grays`.** The general glyph-slot path
reports 256 (`src/base/ftobjs.c:537`); the SDF renderer overwrites it with 255
(`src/sdf/ftsdfrend.c:316` and `:539`). Both
produce one byte per pixel over the full range, so it is a bookkeeping
inconsistency rather than a format difference — but a validity check that
demands 256 rejects every SDF glyph, which is how this was found. `ztext_raster.c`
accepts either and says why.

**A face index past the end of a collection is `Invalid_Argument`,** not
`Unknown_File_Format`. Reasonable — the bytes are fine, the request is not —
and ztext passes it through as `ZTEXT_RESULT_INVALID_ARGUMENT`.

**`hb_ft_font_changed` cannot read variation coordinates in this build.** The
block in `hb-ft.cc` that calls `FT_Get_Var_Blend_Coordinates` is behind
`HAVE_FT_GET_VAR_BLEND_COORDINATES`, which upstream's own configure defines and
`build.zig` does not — it sets `HAVE_FREETYPE=1` only. Advances would be right
either way, because those come from `FT_Load_Glyph`, but a shape *plan* is
built from the `hb_font_t`'s own coordinates, so a run shaped with
`use_freetype_metrics` would resolve GSUB feature variations against the
default instance. Worked around by telling that font the design coordinates
directly as well as calling `hb_ft_font_changed`; the alternative, defining the
macro, would make ztext's HarfBuzz configuration diverge from the explicit list
in `build.zig` for one feature.

**`FT_Set_Var_Design_Coordinates` does not recompute scaled metrics.** It calls
`tt_apply_mvar`, which rewrites `face->ascender`, `descender` and `height` in
*design* units; nothing re-runs `FT_Request_Metrics`. So a face whose font's
variations changed reports the previous instance's line height until its size
is set again. `ztextFontSetVariations` re-applies each face's exact stored 26.6
size for that reason — it is not defensive, it is required.

**Colour bitmap strikes need libpng, which is not vendored.** CBDT and sbix
store their strikes as PNG, and FreeType decodes them only under
`FT_CONFIG_OPTION_USE_PNG`, which upstream leaves commented out
(`include/freetype/config/ftoption.h:276`) and ztext does not enable. Turning it
on means vendoring libpng and zlib as a fourth and fifth upstream. Recorded
rather than worked around, because the alternative reading — "colour fonts are
broken" — is wrong: they load, they simply have no decodable strike.
`TT_CONFIG_OPTION_COLOR_LAYERS` **is** on, so COLRv0 layer enumeration
(`FT_Get_Color_Glyph_Layer`, `FT_Palette_Select`) is available with no new
dependency, and is the route to take if colour is ever wanted.

**Face metrics are grid-fitted to whole pixels; glyph metrics are not.**
`ft_recompute_scaled_metrics` (`src/base/ftobjs.c:3177`) rounds `ascender`,
`descender`, `height` and `max_advance` with `FT_PIX_CEIL`/`FT_PIX_FLOOR`/
`FT_PIX_ROUND`, under a `GRID_FIT_METRICS` that is `#define`d unconditionally
at `src/base/ftobjs.c:102` — there is no build option that turns it off. The
visible consequence, once `ztextFaceSetPixelSize` takes a float, is that
18.0 px and 18.5 px report the *same* `line_height` while every advance
differs. Not worked around: it is FreeType's answer and inventing a smoother
one would mean disagreeing with the rasteriser about where the baselines are.
Recorded, pinned by a test, and documented in `ffi/ztext.h` so it does not read
as a ztext bug.

### HarfBuzz

**Shaping reads the process locale by default.** `hb_buffer_guess_segment_properties`
fills an unset language from `hb_language_get_default()`, which calls
`setlocale(LC_CTYPE, NULL)` (`src/hb-common.cc:374`). Two machines running the
same build would then shape the same string differently. Worked around by
seeding the buffer with `"und"` before the guess and clearing it afterwards, so
language-specific features apply only when a caller explicitly asks. A test
asserts the default result is stable across intervening explicit-language
calls.

**`hb_ft_font_set_funcs` does not use the FT_Face you think it does.** It opens
an FT_Face of its own, from an FT_Library of its own, out of the face's blob
(`src/hb-ft.cc:1714`, `reference_ft_library`). Metrics would come from a
different face than the one being rasterised, which defeats the entire purpose
of asking FreeType for them. ztext uses `hb_ft_font_create`, which wraps the
face it is given.

**hb-ft is unhinted by default.** `_hb_ft_font_create` sets load flags to
`FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING` (`src/hb-ft.cc:115`), so out of the box
"FreeType metrics" are the same unhinted numbers `hb-ot-font` already gives,
with a lock and a cache in the way. ztext sets `FT_LOAD_DEFAULT` explicitly so
the option means what its name suggests.

**An allocation failure degrades silently rather than failing.** HarfBuzz treats
a failed allocation as a reason to abandon optional work and carry on, so the
symptom is not an error but text shaped *without* its OpenType layout — nominal
glyphs, no joining, no ligatures. This is why `ztextRealloc` falls back to
allocate-copy-free when a host's `reallocate` declines instead of reporting
failure upward: Zig's `std.mem.Allocator` only ever resizes in place, so
declining is the common case, and propagating it turned Arabic into unjoined
letters with no error anywhere. Found by a test, not by reading the code.

**A blob HarfBuzz rejects produces a working-looking face.** `hb_face_create`
returns a normal, non-empty `hb_face_t` with zero tables when
`sanitize_blob<OT::OpenTypeFontFile>` refuses the bytes, and likewise for an
out-of-range face index. Shaping through it returns `.notdef` for every
character and reports no error, while FreeType — which accepts a wider set of
sfnt version tags — rasterises the same font correctly. The failure therefore
presents as a broken font rather than a failed load.

Worked around with `hb_face_create_or_fail` (`src/hb-face.cc:289`), which
returns NULL for both cases, plus an independent `hb_face_get_glyph_count`
check. A test doctors the sfnt version word of a real font to reproduce it.

**Several caches live until `exit`, and in this build they are never freed at
all.** HarfBuzz builds lazy singletons on first use: the shaper list
(`src/hb-shape.cc`), the Unicode character database (`src/hb-ucd.cc`), the
OpenType font-functions (`src/hb-ot-font.cc`), the FreeType font-functions
(`src/hb-ft.cc`), and an intern table holding one entry per distinct language
tag ever passed (`src/hb-common.cc:258`, `lang_find_or_insert`).

Upstream frees them from an `atexit` handler — but only where `HAVE_ATEXIT` is
defined. `build.zig` does not define it, so `HB_USE_ATEXIT` is 0
(`src/hb.hh:471-476`) and `hb_atexit(f)` expands to `if (0) f()`
(`src/hb.hh:479`): the handler is never registered and the caches are still
allocated when the process ends. This document said the handler fires, and it
does not.

That is deliberate rather than accidental, and `build.zig` now passes
`-DHB_NO_ATEXIT` to say so: an atexit handler that calls the installed
allocator runs after a host has torn its own allocator down, and the ordering
between the two is not something a library can promise. A bounded, documented
cache is the better trade.

These are caches with process lifetime, not leaks — but they are allocated
through whichever allocator is installed when they are first touched, and they
are still live when a host audits its heap at shutdown. `ztextWarmup()` is
therefore load-bearing for any host that audits, not a convenience. `ztextWarmup()` touches
the ones it can reach so a host can populate them before installing a tracking
allocator. It cannot reach two of them: the FreeType font-functions singleton
and per-language entries both need a real face, so a host that uses
`use_freetype_metrics` or explicit language tags pays one small permanent
allocation for each, once. ztext's own suite warms those in its fixture and
then asserts the heap balances exactly.

**New in 14.4.0: `hb_set_intersects()`.** Recorded rather than adopted --
ztext calls no `hb_set_*` entry point, because nothing it exposes hands a
character or glyph set across the boundary. It is here so the surface change is
on the record: the next time ztext needs to ask whether two coverage sets meet,
this is the call, and it exists from 14.4.0 onwards.

**A stray `.pyc` in `src/__pycache__/` is gone as of 14.4.0.** Upstream tracked
one until then and it was excluded here. The exclusion is kept in
`ci/verify-vendor.sh` because it costs nothing and an upstream that once
committed build output can do it again; if the directory is absent, `diff -r -x`
simply has nothing to skip.

### SheenBidi

**`SBAllocatorSetDefault` does not retain the allocator** it is given
(`Source/API/SBAllocator.c:278` — a bare atomic store). The caller has to keep
the reference alive; ztext creates one allocator object and holds it for the
life of the process.

That object is itself allocated *before* ztext's seam is live — by SheenBidi's
own default allocator, i.e. `malloc` — so it never reaches a host allocator at
all. `ztextWarmup()` makes when that happens predictable rather than dependent
on which paragraph came first.

**A paragraph that fails to resolve releases a pointer it never wrote.**
`ObjectCreate` (`Source/Core/Object.c:33`) hands out a raw block and fills in
only the object base; `AllocateParagraph` (`Source/API/SBParagraph.c:93`) then
sets `fixedLevels` and nothing else, leaving `SBParagraph::_algorithm`
uninitialised. When `ResolveParagraph` fails — which it does when one of the
allocations behind it fails — `CreateParagraph` calls `ObjectRelease`, whose
finalizer reads `_algorithm` and calls `SBAlgorithmRelease` on whatever
happened to be in those eight bytes.

Measured here, not read off: with `malloc`'s leftovers the C smoke test
segfaulted **11 times in 600 runs**, always at the one allocation-failure point
where the paragraph object is allocated and the resolve after it is refused.
Filling every block SheenBidi is handed with `0xCD` made it **60 of 60**;
zeroing made it **0 of 400**. Upstream has fixed two defects of exactly this
shape before (issues #19 and #21, both closed), so this one is worth reporting
— it is not reported yet, and the report is owed.

ztext does not patch `libs/`, and there is no route to that failure path that
does not pass through ztext's allocator seam, so the containment lives there:
`sbAllocateBlock` zeroes every block and `sbReallocateBlock` zeroes the tail a
grow adds, so SheenBidi never reads a byte ztext has not written. The gate is
the poisoned arm of the injection sweep in `tests/c_smoke.c`, which runs an
allocator that returns `0xCD` rather than leftovers: remove the memset and it
dies on every run at the same injection point rather than on one run in fifty.
`ci/check-guards.sh` plants exactly that mutation.

## Test fonts

`tests/fonts/` holds three Noto faces under the SIL Open Font License 1.1, each
with the licence text from its own upstream repository. Provenance, source URLs
and hashes are in `tests/fonts/PROVENANCE.md` and `tests/fonts/SHA256SUMS`.

`ci/verify-vendor.sh` checks those hashes alongside the vendored source,
because the suite's golden shaping results are only meaningful against these
exact bytes.

## Re-vendoring procedure

`ci/verify-vendor.sh` fetches each pinned commit and diffs it against `libs/`,
so the claim that these copies are unmodified is checked rather than asserted.
It runs as its own CI job. Run it after any step below.

1. Clone upstream at the new tag and copy the paths in "What was taken" over
   `libs/<project>/`, re-applying the exclusions.
2. Update `src/pins.zig`, which is the one home for the version, tag and
   commit. `ci/verify-vendor.sh` reads it, the suite asserts the LINKED
   library reports exactly that version, and `ci/measurements.sh --check`
   compares the table at the top of this file against it — so the pin, the
   fetch, the compiled library and this document cannot drift apart. Update
   the table here too; the check will tell you if you forget.
3. Re-read "Known upstream behaviour" above and check each item still holds.
   Several are file-and-line references that a re-vendor can invalidate.
4. Write down what you expect to move BEFORE running anything, from the
   upstream's own NEWS for every release between the two pins. A prediction
   made afterwards is a rationalisation.
5. `zig build test`. The `_Static_assert`s in `ffi/ztext_abi.c` fail the build
   if a type ztext depends on has changed shape; the comptime cross-check in
   `src/abi_check.zig` fails the build if the Zig externs and `ffi/ztext.h`
   have drifted apart; `ztextAbiProbe` fails the test if the header and the
   compiled library disagree; the golden tests fail if shaping output moved.
   Bracket mirroring is pinned too, so a HarfBuzz that stopped applying rule
   L4 would fail rather than quietly render RTL brackets backwards.
6. **A golden test failing after a re-vendor is information, not an obstacle.**
   Read the diff before updating the numbers — a changed advance is a changed
   layout for every consumer.
7. Add any new source files to the explicit lists in `build.zig` deliberately.
   The lists exist so a re-vendor cannot silently change what gets compiled.
