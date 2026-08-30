# Licences and third-party notices

ztext's own code is **MIT** — see [LICENSE](LICENSE). Everything below concerns
the third-party code it vendors, and is written to be usable: if you ship a
binary containing ztext, the "What you must ship" section is the work.

None of these licences requires you to publish your source, pay a royalty, or
attribute anything on screen. All of them permit static linking into a closed
commercial product.

| Component | Licence | Where the text is |
|---|---|---|
| FreeType | **FreeType License (FTL)** — see the election below | `libs/freetype/LICENSE.TXT`, `libs/freetype/docs/FTL.TXT` |
| HarfBuzz | "Old MIT" | `libs/harfbuzz/COPYING` |
| SheenBidi | Apache-2.0 | `libs/sheenbidi/LICENSE` |
| libunibreak | zlib | `libs/libunibreak/LICENCE` |
| Noto test fonts | SIL Open Font License 1.1 | `tests/fonts/*-OFL.txt` |

**No version numbers in that table, on purpose.** Which version of each
upstream is vendored is in [UPSTREAM.md](UPSTREAM.md), whose one home is
`src/pins.zig` and which `ci/measurements.sh --check` gates. This file carried
its own copy of those numbers once and two of them were stale — a licence
summary that names the wrong version is worse than one that names none, because
it reads as though someone checked.

What this file *is* pinned to is the licence **texts**, by digest, in
[The texts this file summarises](#the-texts-this-file-summarises) at the
bottom. `ci/measurements.sh --check` recomputes them, so a re-vendor that
changes a licence turns this document red instead of leaving it quietly wrong.

## The FreeType election, stated

FreeType is distributed under **either** the FreeType License **or** GPLv2, at
the recipient's choice. **ztext elects the FreeType License.** GPLv2 does not
apply to ztext or to anything that links it.

This is recorded explicitly because an unmade election is an argument waiting
to happen: a reader who finds only "dual licensed" can reasonably assert the
copyleft branch. Both texts are vendored (`docs/FTL.TXT` and `docs/GPLv2.TXT`)
because upstream ships both; keeping only one would misrepresent the package.

**The two branches are not interchangeable, and this is why the choice exists.**
`libs/freetype/LICENSE.TXT` states it in upstream's own words: the FTL "is
compatible to the GNU General Public License version 3, but not version 2",
and "the FTL is incompatible with GPLv2 due to its advertisement clause". So:

- Shipping a **proprietary** product, or one under a permissive licence: elect
  the FTL, as ztext does. Nothing further follows.
- Shipping **GPLv3** code: the FTL is compatible with it. The election stands.
- Shipping **GPLv2** code that must combine with FreeType: the FTL election
  does not help you. You would have to elect GPLv2 for FreeType instead — the
  election is each recipient's to make, and ztext making one for its own
  distribution does not bind yours. Note that electing GPLv2 for FreeType has
  consequences for everything linked with it, which is a decision about your
  whole product rather than about this package.

ztext's own MIT licence is compatible with either branch.

## FreeType is not one licence

The FTL (or GPLv2) covers most of FreeType, but upstream's `LICENSE.TXT` names
four bodies of code inside the same tree that carry their own terms. All four
are compatible with both branches — upstream says so, in that file — so none of
them changes the analysis above. What they change is the notices you ship, and
that depends on whether the code reaches your binary:

| Code | Its licence | Reaches your binary? |
|---|---|---|
| `src/base/fthash.c`, `include/freetype/internal/fthash.h` | X Window System style, inherited from the BDF driver | **Yes.** `fthash.c` is one of the eighteen sources `ftbase.c` includes, and ztext builds `ftbase.c`. |
| `src/bdf/`, `src/pcf/` | X Window System style | No. Neither driver is in `build.zig`'s source list or `ffi/ztext_ftmodules.h`. |
| `src/gzip/` | zlib | No. Not built; ztext reads no compressed PCF. |
| `src/autofit/ft-hb.c`, `ft-hb-ft.c`, `ft-hb-decls.h`, `ft-hb-types.h`, `hb-script-list.h` | "Old MIT", taken from HarfBuzz | **Yes.** Their contents sit behind `FT_CONFIG_OPTION_USE_HARFBUZZ`, which `ffi/ztext_ftoption.h` defines: the autohinter takes its glyph coverage from GSUB, and these files are what call HarfBuzz to do it. `ci/measurements.sh --check` compares this cell against that macro. |
| `src/base/md5.c` | Public domain | No. Included by `ftobjs.c` only under `FT_DEBUG_LEVEL_TRACE`, which this build does not define. |

Two consequences, and they point in different directions:

- **In a binary** you carry two: the X11-style `fthash.c` and the "Old MIT"
  `ft-hb` files. `fthash.c`'s terms are the BDF driver's, in
  `libs/freetype/src/bdf/README` — a permissive notice-retention licence with
  no advertising clause. Shipping `libs/freetype/LICENSE.TXT` alongside
  `FTL.TXT` covers it, because that file is what names the sub-licences and
  where their texts are. The "Old MIT" one is HarfBuzz's own licence, and you
  already ship it for HarfBuzz itself, so it asks nothing of you that
  `libs/harfbuzz/COPYING` did not already ask.
- **In a source distribution** — which is what a Zig package *is*: `libs/` is
  in `build.zig.zon`'s `paths`, so a consumer fetching ztext receives every
  file above whether or not it compiles — you redistribute all four. Each
  carries its own notice in its own file, which is what those licences ask for,
  and `ci/verify-vendor.sh` proves those files are upstream's own bytes.

The same reasoning, in the same words, is why the HarfBuzz `ms-use` note below
says what it says.

## The test fonts are not shipped

`tests/fonts/` exists for the test suite. Those fonts are **not** part of the
library, are not compiled into it, and end up in your binary only if you embed
them yourself. If you do, the OFL comes with you — including its rule that a
font under the OFL may not be sold on its own.

Worth knowing if you have met the OFL before: none of these three declares a
Reserved Font Name. OFL §1 reserves only names "specified as such after the
copyright statement(s)", and all three copyright lines are a bare
`Copyright 2022 The Noto Project Authors (…)`. So the reserved-name rule, which
is the clause that usually bites, does not apply here.

Their bytes, and their licence texts, are pinned in `tests/fonts/SHA256SUMS`
and checked by `ci/verify-vendor.sh`.

## What you must ship

Reproduce the following in your product's third-party notices — a file, an
about screen, or the manual. Nothing has to appear in the running interface.

### FreeType

The FTL asks that you acknowledge the use of FreeType in your documentation,
and gives a preferred wording. Use it verbatim:

```
Portions of this software are copyright © 2026 The FreeType
Project (https://freetype.org).  All rights reserved.
```

The year is the one in the version you actually ship, as the FTL instructs;
2026 is what `libs/freetype/include/freetype/freetype.h` carries here.

Ship `libs/freetype/docs/FTL.TXT` with it, and `libs/freetype/LICENSE.TXT`
too: that second file is what names the sub-licences above, including the two
that do reach your binary — `fthash.c`'s and the `ft-hb` files'.

### HarfBuzz

Old MIT requires the copyright notice and two specific paragraphs to appear in
all copies. Copy `libs/harfbuzz/COPYING` whole — it carries both the notice
list and the paragraphs, and abbreviating it is what puts you out of
compliance.

HarfBuzz's own `COPYING` points out that some subdirectories carry their own.
One is vendored: `libs/harfbuzz/src/ms-use/COPYING` (Microsoft, MIT), covering
the Universal Shaping Engine data files. **Nothing under it compiles** — the
`.txt` files there are inputs to a generator ztext does not run — so it cannot
reach your binary, though it does travel in a source distribution of ztext,
where its own file is its own notice.

### SheenBidi

Apache-2.0 requires you to keep the licence and any NOTICE, and to state
changes if you make any. Copy `libs/sheenbidi/LICENSE` and this line:

```
Copyright (C) 2014-2026 Muhammad Tayyab Akram
```

ztext makes no changes to SheenBidi — `ci/verify-vendor.sh` proves that on
every run — so there is nothing to declare under section 4(b).

### libunibreak

zlib asks three things, and only the third can reach a binary: do not
misrepresent the origin, mark altered versions as altered, and do not remove
the notice from a source distribution. ztext alters nothing —
`ci/verify-vendor.sh` proves it — and ships the notice at
`libs/libunibreak/LICENCE`.

For a **binary** product zlib requires nothing at all. Acknowledgement "would
be appreciated but is not required", and this file takes it up:

```
This software uses libunibreak, copyright © Wu Yongwei and contributors.
```

## Two things worth knowing

**Apache-2.0 is one-way incompatible with GPLv2** (not v3). Irrelevant to a
proprietary product; it matters only if you also ship something GPLv2 and need
the combination to be distributable. Note that this is the *second* GPLv2
obstacle on this page: the FTL is the first. A product that must be GPLv2 has
work to do with both SheenBidi and FreeType, and neither is ztext's to solve.

**Subpixel rendering is off.** FreeType's LCD filter once sat near Microsoft's
ClearType patents. Those have expired, so this is no longer a legal question —
but the option is off regardless, because ztext produces A8 coverage and SDF,
and an engine wanting LCD filtering wants it in its own shader against its own
subpixel layout. See `ffi/ztext_ftoption.h`.

## The texts this file summarises

Every statement above was written against these exact bytes.
`ci/measurements.sh --check` recomputes each digest and fails if one has moved,
so a re-vendor that changes a licence cannot land with this page still claiming
to describe it. Adding a row here adds it to the check; there is no second list
to keep in step.

| Text | SHA-256 |
|---|---|
| `libs/freetype/LICENSE.TXT` | `bd36c8b474855fa294c2ec5c184544478ef3720aad37d65a6296a4f264fd2d3b` |
| `libs/freetype/docs/FTL.TXT` | `5a5ee54c5001bbad1cdc1a57cc3dd4c42199b2da09d39c7ee41fab002d02967f` |
| `libs/freetype/docs/GPLv2.TXT` | `c4120c6752c910c299e3bd9cb3a46ff262c268303ca2069b61f92f10a5656c18` |
| `libs/harfbuzz/COPYING` | `ba8f810f2455c2f08e2d56bb49b72f37fcf68f1f4fade38977cfd7372050ad64` |
| `libs/harfbuzz/src/ms-use/COPYING` | `c2cfccb812fe482101a8f04597dfc5a9991a6b2748266c47ac91b6a5aae15383` |
| `libs/sheenbidi/LICENSE` | `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30` |
| `libs/libunibreak/LICENCE` | `0c57e2ec42ece17791a75d2f9b2f8e0663181be008667a507f765b3ef22522db` |

The Noto licence texts are not repeated here: they are already pinned in
`tests/fonts/SHA256SUMS`, which `ci/verify-vendor.sh` checks. One home each.

*This file is a summary written by an engineer, not legal advice. The
authoritative texts are the ones vendored under `libs/`.*
