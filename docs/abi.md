# The ABI guard, in both directions

ztypeset sits between four C and C++ libraries and a Zig module. There are two
places the declarations can drift, and a guard at each.

## Downwards, at the upstreams

`_Static_assert`s in `ffi/ztypeset_abi.c` fail the build if a re-vendor changes
the shape of something ztypeset depends on: `hb_position_t`'s width,
`FT_Bitmap::pitch` staying signed, `SBLevel` being a byte.

The pitch one is worth spelling out. A width check alone would let an upstream
change from signed to unsigned through, and every bottom-up glyph would then
render as garbage. That is the payoff for stopping upstream types at the C
boundary rather than mirroring them into Zig.

## Upwards, at Zig

The wrapper hand-writes `extern struct`s mirroring `ztypeset.h`, and nothing in
either compiler checks they still agree. Three guards close that, each
answering a question the other two cannot, and each validated by breaking it on
purpose rather than by passing.

**The comptime cross-check** (`src/abi_check.zig`) `@cImport`s the real header —
in a test only, so the shipped module stays translate-c-free — and compares it
against `src/c.zig` declaration by declaration. There is no hand-written list
of what to check: every public declaration is discovered by reflection, paired
by naming convention, and compared; one that fits no category is a compile
error, so the guard cannot quietly stop covering something. Structs are
compared field by name, so two same-sized adjacent fields swapping places fails
even though the sequence of offsets is unchanged. Functions are compared on
arity and on the size and alignment of every parameter and the return value.
Enumerators are compared one by one, which a check on the last enumerator alone
cannot do — renumbering a middle one leaves the last unchanged. And the sweep
runs in both directions: a function the header exports that `c.zig` never
declared fails too.

**`ztypesetAbiProbe()`** writes a distinct marker into every field of every
shared struct so the Zig side reads each one back. This is the only guard that
compares the declarations against the compiled library rather than against the
header, and the two diverge exactly when a header is preprocessed with
different macros than the library was built with — the axis a header-only
comparison is blind to by construction. It is also the guard that needs no
source at all: it would catch a prebuilt `libztypeset` from a different
configuration.

**`tests/c_smoke.c`** drives the whole API from C, with no Zig in the picture.
That is what covers the cross-check's documented residue: translate-c renders
every C pointer as `[*c]T`, so pointee types are compared only by size and
alignment, and a `float*` declared as `*i32` passes. The C test also proves the
allocator seam is genuinely in use, by asserting allocations balance.

`ztypesetAbiLayout()` remains part of the C API — a non-Zig host still wants a
way to self-check what it linked against — but it is no longer the primary
guard and is not grown by hand as the API grows.

## The naming convention is load-bearing

translate-c flattens a C enum to an integer alias and loses which enumerators
belonged to it, so `ZTYPESET_<TYPE>_<FIELD>` is the only thing that can pair
them back up. That is why the header's enumerators are named strictly, with no
readable-but-irregular exceptions.

## One thing the cross-check does not need

Macro wiring. `ffi/ztypeset.h` includes only `<stddef.h>` and `<stdint.h>` and
is sensitive to exactly one macro, `ZTYPESET_SHARED`, which changes an
attribute and no type; every FreeType and HarfBuzz configuration macro reaches
the implementation, never the installed header. A package whose public header
changed type widths with its build options would have to forward those macros
into the `@cImport`; ztypeset's does not.

## Versions

[CHANGELOG.md](../CHANGELOG.md) is the one home for ztypeset's release history
and for the rules its numbers follow: with the major at 0 the MINOR position is
the breaking one, an ABI change lands as one commit, and the `ZtypesetAbiLayout`
handshake grows with the struct that changed.

The version itself is written in four files — `ffi/ztypeset.h`'s macros,
`build.zig.zon`, the changelog's newest heading and README.md's status line —
and `ci/measurements.sh --check` fails if they disagree, including if one of
the four patterns stops matching.

The ABI went 0.1 to 0.2 in one change, when `ZtypesetGlyph` gained `flags` and
`ZtypesetGlyphBitmap` gained `format`. Both structs changed size, so the
header's version macros, the manifest, the `ZtypesetAbiLayout` handshake, the
`ztypesetAbiProbe` markers and the Zig mirror moved together.
