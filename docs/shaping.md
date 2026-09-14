# The text pipeline

Why the shaping and ordering calls take the arguments they do.

## `shapeRun` takes the paragraph, not the text

A run is a pair of offsets, and offsets are only meaningful against the buffer
they were computed from. Passing a slice of that buffer together with offsets
computed against the whole is the most common way to get this wrong, and it
produces text that is almost right. With the paragraph as the argument there is
no second buffer to disagree with.

It settles two other things. The run carries the direction and the script, so
`ShapeParams` must not also carry them; and the paragraph's text was validated
when it was created, so it is not walked again for every run.

`shape` and `shapeRange` remain for text a host has not made a paragraph of:
one string, or one range of one string with the rest as context, with the
direction and script the host names.

## `Line` is a correctness boundary, not a convenience

UAX #9 resolves embedding levels over the paragraph but applies rules L1 and L2
per line, so a paragraph's run list is only the right answer when the paragraph
is one line.

The difference shows up as trailing whitespace between two right-to-left words,
which the paragraph puts in the middle and the line puts at the end — an indent
on the wrong side, visible only when the text wraps and only in a right-to-left
locale.

## Shaping runs: the intersection is provided

Visual runs are in visual order and script runs are in logical order, so inside
a right-to-left visual run the script pieces have to come out backwards; and a
single visual run can span three scripts, which HarfBuzz will happily shape as
one and get wrong. That is the one part of itemisation that is easy to get
subtly wrong, so `shapingRuns()` does it. The raw `visualRuns()` and
`scriptRuns()` are still exposed for callers doing something else with them.

## Bidi mirroring is already done, and needs no API

Rule L4 — the one that turns `(` into `)` inside a right-to-left run — is
applied by HarfBuzz itself, at shaping time, before OpenType features run
(`hb_ot_rotate_chars`). ztypeset neither duplicates it nor exposes a hook for
it, and a test pins the behaviour so a re-vendor that changed it would fail
rather than silently produce backwards brackets.

## Which segmentation passes run is the caller's choice

`Paragraph.init(text, .{ .segmentation = ... })` takes an OR of `lines`,
`graphemes` and `words`, and the default is all three. They are the largest
thing a paragraph holds: on a 4300-character paragraph the three passes are
56.6% of the time to build it (265 µs against 115 µs) and 12 916 B of its
26 898 B — more than the copied text and the embedding levels together.

A single-line label that only wants bidi runs asks for none; a wrapping text
box asks for `lines`. The accessor for a pass that was not run returns an empty
slice, and `segmentation()` says which ran, so an empty array is never mistaken
for a text with no boundaries.

## Two numbers from two libraries that meant two different things

Adding `Face.metric` put `post`'s underline beside the one `FaceMetrics`
already carried, and they disagreed: HarfBuzz reports `underlinePosition` as
the font stores it, the top edge of the stroke, while FreeType subtracts half
the thickness to convert the TrueType meaning to its own
(`libs/freetype/src/sfnt/sfobjs.c`). For Noto Sans that is -100 against -125
design units. Neither is wrong and a caller drawing a rectangle needs to know
which edge it was handed, so the difference is stated in `ffi/ztypeset.h` and
pinned by a test that asserts the gap is exactly half the thickness.

## Variation sequences

`ztypesetFontVariantGlyphIndex` returns nonzero exactly when this font draws
this exact base-plus-selector pair. The first draft of the contract said 0 also
meant "this font maps the pair by default"; FreeType does the opposite, and for
a sequence recorded as the default it resolves the base character through the
Unicode cmap and returns that glyph (`tt_cmap14_char_var_index`,
`libs/freetype/src/sfnt/ttcmap.c`). It was corrected by reading upstream rather
than by testing, because no vendored font has a format-14 cmap.
`tests/fonts.zig` now builds one: `withVariationSequences` rewrites a font's
cmap with an extra format-14 subtable naming sequences the test states outright.

That fixture produced a finding of its own. Appending the new `(0,5)` encoding
record at the end of the cmap left the records unsorted, and FreeType picks a
face's default charmap by walking them backwards and taking the last Unicode
one (`find_unicode_charmap`, `libs/freetype/src/base/ftobjs.c`). The format-14
subtable became the default charmap, its `char_index` answered 0 for
everything, and the font's ordinary lookups all returned `.notdef` while the
variation sequences worked perfectly. Sorted records fix it, and a mutation
case unsorts them again so the reason is a gate rather than a comment.

## Hinting takes its coverage from GSUB

FreeType's autohinter can take its glyph coverage from GSUB instead of the
character map, and that matters here more than for most consumers: `light`
hinting is the autohinter and nothing else for a TrueType face (only the CFF
driver sets `HINTS_LIGHTLY`), and a glyph shaping produced — an Arabic medial
form, a ligature — is reachable from no character at all, so through the cmap
alone it is hinted against no script's blue zones.

Turning the option on changed the raster of every light-hinted glyph of all
four test fonts, and of every glyph of the variable font in every mode, since
it carries no `fpgm` and FreeType autohints it unconditionally.

It also changed a HarfBuzz allocation nobody was watching. The coverage pass
builds a HarfBuzz buffer and lets it guess its segment properties, which interns
the default language — a fifth process-lifetime HarfBuzz singleton, on a path
that did not exist in this package before, and charged to whichever allocator
was installed the first time a glyph was hinted rather than shaped.
`ztypesetWarmup()` now touches it, and the C smoke test hints with the
autohinter, because a byte count that never meets a path cannot say anything
about it.
