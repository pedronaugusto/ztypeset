# Test fonts

Four Noto files — three static faces and one variable font — each under the
**SIL Open Font License 1.1**, committed so the suite needs no network and no
system fonts, and so the golden shaping results mean something, which they only
can against exact bytes.

`SHA256SUMS` records every font and licence file here — the bytes the goldens
depend on — and `ci/verify-vendor.sh` checks it, in the same job that proves
`libs/` matches its pinned upstreams. A font that changed silently would turn a
real shaping regression into a puzzling one. (This document and the sums file
itself are not listed, for the obvious reason.)

| File | Source | Why this font |
|---|---|---|
| `NotoSans-Regular.ttf` | [notofonts/latin-greek-cyrillic](https://github.com/notofonts/latin-greek-cyrillic) | Standard ligatures and real kerning pairs, so `liga` and `kern` can be switched off and the difference observed. |
| `NotoNaskhArabic-Regular.ttf` | [notofonts/arabic](https://github.com/notofonts/arabic) | Cursive joining and mark attachment — the shaping behaviour no advance table can fake, and the one that catches a shaper silently falling back to nominal glyphs. |
| `NotoSansHebrew-Regular.ttf` | [notofonts/hebrew](https://github.com/notofonts/hebrew) | Right-to-left *without* joining, so a direction bug cannot hide behind a joining bug. Also the smallest of the static three, which is why the truncation sweep and the C smoke test use it. |
| `NotoSansHebrew[wdth,wght].ttf` | [notofonts/hebrew](https://github.com/notofonts/hebrew) | The `fvar` axes the variable-font tests need. Two axes, `wdth` and `wght`, so a test can prove that moving one leaves the other alone — one axis could not. The same family as the static face above, so the two are directly comparable. |

The three static faces were taken from the built-font mirror,
`https://raw.githubusercontent.com/notofonts/notofonts.github.io/main/fonts/<Family>/hinted/ttf/<Family>-Regular.ttf`,
at the hashes in `SHA256SUMS`.

The variable font came from the same mirror but a different directory,
`https://raw.githubusercontent.com/notofonts/notofonts.github.io/main/fonts/NotoSansHebrew/unhinted/variable-ttf/NotoSansHebrew%5Bwdth,wght%5D.ttf`
— **`unhinted/variable-ttf`, not `hinted/ttf`**. That is a real difference and
not a transcription slip: the mirror keeps its variable builds under a
different path from its static ones, and the URL pattern above does not reach
them. Being the unhinted build, it has neither `fpgm` nor `cvt ` where the
static face has both, so FreeType hints it with the autohinter rather than
with a program of the font's own — the goldens in `src/integration_test.zig`
were measured against these bytes and mean nothing against a hinted build.

It does carry `HVAR`, which is what makes it a useful fixture: advances vary
with the axes rather than staying put, so a test can see HarfBuzz's half of a
variation change and not only FreeType's.

## Licences

Each *family* ships with the licence text from **its own** upstream repository
(`NotoSans-OFL.txt`, `NotoNaskhArabic-OFL.txt`, `NotoSansHebrew-OFL.txt`)
rather than one shared copy. The bodies are the same OFL 1.1 up to the URL it
cites, but the copyright line differs per family, and a licence is not
something to normalise on the reader's behalf.

There are three licence files for four fonts, and that is deliberate rather
than an omission: `NotoSansHebrew[wdth,wght].ttf` is the same family from the
same repository as `NotoSansHebrew-Regular.ttf`, so `NotoSansHebrew-OFL.txt`
already covers it. A second, identical copy would only be a second thing to
keep in step.

Each begins:

> Copyright 2022 The Noto Project Authors (https://github.com/notofonts/\<family\>)

Note that the `LICENSE` file at the root of `notofonts.github.io` is Apache-2.0
— that covers the repository's tooling, **not** the fonts. The fonts are OFL.
This is an easy and consequential thing to get wrong, which is why the licences
here came from the per-family source repositories instead.

## If you replace a font

Regenerate `SHA256SUMS`, and expect the golden tests in
`src/integration_test.zig` to fail — they encode glyph ids and advances
specific to these bytes. Read the diff before updating them.
