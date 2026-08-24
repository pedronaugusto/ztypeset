# Licences and third-party notices

ztext's own code is **MIT** — see [LICENSE](LICENSE). Everything below concerns
the third-party code it vendors, and is written to be usable: if you ship a
binary containing ztext, the "What you must ship" section is the work.

None of these licences requires you to publish your source, pay a royalty, or
attribute anything on screen. All of them permit static linking into a closed
commercial product.

| Component | Licence | Where the text is |
|---|---|---|
| FreeType 2.14.3 | **FreeType License (FTL)** — see the election below | `libs/freetype/docs/FTL.TXT` |
| HarfBuzz 14.3.1 | "Old MIT" | `libs/harfbuzz/COPYING` |
| SheenBidi 3.0.0 | Apache-2.0 | `libs/sheenbidi/LICENSE` |
| libunibreak 7.0 | zlib | `libs/libunibreak/LICENCE` |
| Noto test fonts | SIL Open Font License 1.1 | `tests/fonts/*-OFL.txt` |

## The FreeType election, stated

FreeType is distributed under **either** the FreeType License **or** GPLv2, at
the recipient's choice. **ztext elects the FreeType License.** GPLv2 does not
apply to ztext or to anything that links it.

This is recorded explicitly because an unmade election is an argument waiting
to happen: a reader who finds only "dual licensed" can reasonably assert the
copyleft branch. Both texts are vendored (`docs/FTL.TXT` and `docs/GPLv2.TXT`)
because upstream ships both; keeping only one would misrepresent the package.

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

The full FTL text should accompany it; copy `libs/freetype/docs/FTL.TXT`.

### HarfBuzz

Old MIT requires the copyright notice and two specific paragraphs to appear in
all copies. Copy `libs/harfbuzz/COPYING` whole — it carries both the notice
list and the paragraphs, and abbreviating it is what puts you out of
compliance.

HarfBuzz's own `COPYING` points out that some subdirectories carry their own.
One is vendored: `libs/harfbuzz/src/ms-use/COPYING` (Microsoft, MIT), covering
the Universal Shaping Engine data files. **Nothing under it compiles** — the
`.txt` files there are inputs to a generator ztext does not run — so it cannot
reach your binary, and it is listed here for completeness rather than as an
obligation.

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
the combination to be distributable.

**Subpixel rendering is off.** FreeType's LCD filter once sat near Microsoft's
ClearType patents. Those have expired, so this is no longer a legal question —
but the option is off regardless, because ztext produces A8 coverage and SDF,
and an engine wanting LCD filtering wants it in its own shader against its own
subpixel layout. See `ffi/ztext_ftoption.h`.

*This file is a summary written by an engineer, not legal advice. The
authoritative texts are the ones vendored under `libs/`.*
