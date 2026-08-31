# Security

## Reporting a vulnerability

Open a **private security advisory** through this repository's Security tab
(*Report a vulnerability*). That keeps the report out of the public issue
tracker until there is something to release. Please do not open a normal issue
for a memory-safety or parsing defect.

Include the font or the text that triggers it if you can. A reproducing input
is worth more than a description here: everything ztypeset does is a function of
bytes it was handed, so an input plus the call that consumed it is a complete
report.

## What ztypeset is, for the purpose of a threat model

ztypeset is a **text-shaping library**, not a sandbox. It parses attacker-supplied
font files and attacker-supplied text through four upstream libraries, and the
overwhelming majority of the attack surface is theirs:

| upstream | what it parses | continuously fuzzed by OSS-Fuzz? |
|---|---|---|
| FreeType | the font file: every table, every outline | **yes** — `projects/freetype2` |
| HarfBuzz | the font's OpenType layout tables | **yes** — `projects/harfbuzz`, five fuzzers |
| SheenBidi | the text, for the bidirectional algorithm | **no project** |
| libunibreak | the text, for line, word and grapheme breaks | **no project** |

Checked against `google/oss-fuzz` on 2026-08-31; the two "no" rows are a code
search for each name returning nothing. That asymmetry is stated rather than
smoothed over: half of what ztypeset feeds untrusted bytes to has a continuous
fuzzing program behind it and half does not.

**A defect in an upstream is best reported to that upstream**, where it will be
fixed for every consumer rather than worked around in one. If you are not sure
which side a defect is on, report it here and it will be triaged; ztypeset pins
exact upstream commits (see [UPSTREAM.md](UPSTREAM.md)), so reproducing against
a specific version is straightforward.

## What ztypeset does about it

None of this makes ztypeset safe against a hostile font. It is what ztypeset does
instead of claiming that.

- **The parser surface is cut down at build time.** zlib and gzip decompression,
  OT-SVG, and the classic Mac resource-fork loaders and their guessing
  heuristics are all compiled out. Every one of those switches is asserted in
  `ffi/ztypeset_abi.c`, so the build fails rather than quietly acquiring one back
  — see `ffi/ztypeset_ftoption.h` for what is off and why.
- **Text is validated at the ztypeset boundary**, before SheenBidi or libunibreak
  see it: an ill-formed UTF-8, UTF-16 or UTF-32 buffer is refused with
  `ZTYPESET_RESULT_INVALID_TEXT` rather than passed down. That is a boundary check
  and not a substitute for fuzzing those two.
- **Every entry point is called with nothing**, by `tests/null_sweep.c` --
  every one of them, checked against the header by `ci/api-surface.sh
  --sweep`, on every host CI runs.
- **Every allocation is failure-injected.** The C smoke test drives the library
  with each allocation in turn made to fail, and asserts no leak and no crash at
  any of them.
- **ztypeset's own C compiles with `-Wall -Wextra -Werror`**, and CI runs the
  suite with the C sanitiser on in Debug. The vendored upstreams are compiled
  as their authors configured them; ztypeset does not patch `libs/`.

## Supported versions

ztypeset is pre-1.0 and only the latest release is supported. The **minor** is the
breaking position while the major is 0 — see [CHANGELOG.md](CHANGELOG.md) for
what a bump means.
