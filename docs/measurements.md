# Measurements

Run them yourself — the harness is committed:

```sh
zig build bench -Doptimize=ReleaseFast -Dsanitize_c=false
ci/measurements.sh          # every number the README claims, recomputed
ci/measurements.sh --check  # ... and compared against what it says
```

Numbers below are from that harness on x86_64-windows-gnu with Noto Sans at
32 px, reporting CPU time, over four consecutive runs of an unchanged tree.
Absolute values will differ on your machine; the ratios will not move much.

Timings vary by 30-40% run to run on a loaded machine, so they are quoted to
two significant figures with the spread stated. The byte counts were identical
to the byte in all four runs, which is what you would expect of an allocation
total and is worth checking rather than assuming.

| | Cost | Read this as |
|---|---|---|
| Shape a 43-character run | ~2.5 µs | 2.25-2.70 µs. Reusing one `Shaper`. A separate test proves warm shapes allocate nothing. |
| The same run, 4300 characters around it, through `shape` | ~3.3 µs | HarfBuzz decodes the run and at most five characters either side, so its cost does not move. The extra ~1.05 µs is ztypeset validating the whole borrowed buffer — about 0.25 ns per code unit, paid on every call. |
| Analyse a 4300-character paragraph, all three segmentation passes | ~265 µs | 26 898 B live. Bidi, itemisation, the copy of the text, UAX #14 and both of #29. |
| The same paragraph, no segmentation | ~115 µs | 13 982 B live. One variable between the two arms: the three passes are 56.6% of the time and 12 916 B of the memory, which is why they are a choice and not a policy. |
| The same run, through `shapeRun` | ~2.25 µs | Identical to the 43-character case: a paragraph's text was validated when the paragraph was created. Shaping an N-unit paragraph as R runs costs R walks of N through `shape` and none through `shapeRun`. |
| Rasterise one glyph, A8 | ~2.3 µs | 2.16-2.50 µs. Uncached; `FT_Load_Glyph` every time, and one memcpy of the result. Atlas it anyway. |
| Rasterise one glyph, SDF | ~2.6 ms | ~1100× the A8 cost (1091-1216 across the four runs). |
| One font, first face | 34 349 B | The parse, plus everything the first size needs. |
| Each additional size | 15 333 B | Against 34 703 B for a single collapsed handle. |

Blind spot: the bench counts what ztypeset's allocator seam is asked for, so it
cannot attribute a byte to FreeType rather than HarfBuzz, and it does not see
the host allocator's own per-block overhead.

## Font and Face, measured

Both rows come from `zig build bench -Doptimize=ReleaseFast
-Dsanitize_c=false`, which builds four sizes over one shared `Font` and then
four independent `Font`s at the same four sizes — the second being exactly the
handle a collapsed design gives you. Everything is shaped and rasterised with
before it is measured, on the same schedule in both arms, because a handle that
has never been used has not built its table accelerators, scaled state or glyph
buffers yet and measuring it would flatter the design.

Noto Sans, x86_64-windows-gnu:

| | Per additional size | Four sizes |
|---|---|---|
| One handle carrying its own size | 34 703 B | 138 459 B |
| Font shared, faces per size | 15 333 B | 80 350 B |

That is 56% off a size. The absolute bytes move with the platform, the
allocator and the build options; the ratio is the claim, and
`ci/measurements.sh` reprints both rows on whatever machine you run it.

Why 56% and not 90% is in [design.md](design.md).

## SDF

SDF is a bake-time tool and the ratio is not a typo. Two hundred glyphs is a
little over half a second, once, into an atlas. Generating SDF glyphs during a
frame would exhaust a 16 ms budget at about six glyphs. The API exposes it
because a distance field is the right way to draw text that scales, rotates or
lives in world space; the number is here so nobody finds the cost out the hard
way.

FreeType's SDF output is verified rather than assumed: a test asserts the field
grows by the spread on every side, that a scanline across an `o` crosses the
128 mid-point exactly four times, and that it ramps rather than steps.

## The optional glyph flags

ztypeset asks HarfBuzz for `UNSAFE_TO_CONCAT` and `SAFE_TO_INSERT_TATWEEL` on
every shape, which upstream withholds by default because computing them costs
something. That cost was measured on the shaping row rather than argued about:
three runs of the unchanged tree gave 2.35, 2.45 and 2.60 µs, and three runs
with the flags asked for gave 2.30, 2.25 and 2.30 µs — the second set inside
the spread of the first, so the difference is below what this harness resolves.

The blind spot is the sentence itself: it is Latin, where little joins and
nothing elongates. A cursive script does more work for these flags and is not
on that row.
