/*
 * ztypeset's FreeType build configuration.
 *
 * FreeType includes this file wherever it would have included
 * <freetype/config/ftoption.h>; build.zig points FT_CONFIG_OPTIONS_H here.
 *
 * Note what this file is NOT: an edited copy of upstream's ftoption.h. FreeType
 * documents customisation as "copy ftoption.h and change it", which would leave
 * ~1000 lines of derived upstream in this repository to re-merge on every
 * re-vendor. Including upstream's file and adjusting the handful of macros that
 * matter keeps libs/ pristine and keeps this file readable, and it inherits any
 * new default upstream adds instead of silently pinning an old one.
 */

#ifndef ZTYPESET_FTOPTION_H_
#define ZTYPESET_FTOPTION_H_

#include <freetype/config/ftoption.h>

/*
 * Compressed and container formats. Upstream enables zlib by default, which
 * pulls in src/gzip and buys gzip-compressed PCF plus WOFF. ztypeset consumes
 * TrueType and OpenType from an asset pack, where the pack is already the
 * compression layer, so both are dead weight -- and each one is parser surface
 * on untrusted bytes that we would otherwise be carrying for nothing.
 *
 * The consequence is stated rather than hidden: a WOFF or WOFF2 file handed to
 * ztypeset is rejected as an unsupported format. Cook to TTF/OTF.
 */
#undef FT_CONFIG_OPTION_USE_ZLIB

/*
 * OT-SVG requires the caller to install an SVG rendering hook; without one the
 * module can only report that it cannot render. That hook would be a second
 * dependency of its own, so the module is not compiled and the option goes with
 * it. Colour glyphs generally are out of scope -- see README for which of the
 * three routes are shut and why.
 */
#undef FT_CONFIG_OPTION_SVG

/*
 * Classic Mac resource-fork fonts (LWFN, .dfont, FOND). Reachable only through
 * path-based entry points on macOS,
    and ztypeset has no path-based entry point at
 * all: faces come from memory.
 */
#undef FT_CONFIG_OPTION_MAC_FONTS

/*
 * And the resource-fork GUESSING heuristics, which are a SEPARATE switch.
 *
 * The paragraph above used to end by saying that undefining MAC_FONTS dropped
 * src/base/ftrfork.c's guessing logic as well. It does not, and the file was
 * in every binary ztypeset has ever produced with a comment saying it was not.
 * ftrfork.c is compiled -- src/base/ftbase.c #includes it and ftbase.c is in
 * build.zig's list -- and its lines 319-893, the whole table of heuristics
 * over attacker-visible bytes, sit under this macro alone. MAC_FONTS gates
 * only FT_Raccess_Guess's outer entry point at :468.
 *
 * Undefining it selects ftrfork.c's other branch (:894-925), the stub that
 * reports the format is unsupported. ffi/ztypeset_abi.c refuses to compile if
    any
 * of these switches comes back, because every claim in this file is about a
 * macro, and a macro is exactly the kind of claim a build can check.
 */
#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK

/*
 * Report FreeType's own error strings. ztypeset maps FT errors onto its flat
 * result enum, which necessarily loses detail; keeping the strings means a
 * BadFont can still say *why* in a log line instead of only that it was bad.
 * Costs a few kilobytes of static text.
 */
#ifndef FT_CONFIG_OPTION_ERROR_STRINGS
#define FT_CONFIG_OPTION_ERROR_STRINGS
#endif

/*
 * The autohinter's coverage, taken from GSUB rather than from the character
 * map alone.
 *
 * FreeType gives every glyph a "style" -- a script, a width and a hinting
 * mode -- and the style is what chooses the blue zones the autohinter snaps
 * outlines to. Without this macro the only thing that can assign one is
 * `af_shaper_get_coverage_nohb` (src/autofit/afshaper.c), which walks the
 * character map: a glyph that no character maps to gets no script, and falls
 * back to a styleless default. That is precisely the set of glyphs shaping
 * produces -- every Arabic contextual form, every Indic conjunct, every
 * ligature, every small-cap -- and shaping is what this package is for.
 *
 * With it, FreeType runs each script's GSUB features over the characters that
 * ARE mapped and takes the outputs, so a derived glyph inherits the style of
 * the character it came from.
 *
 * This reaches more of ztypeset than it looks. `ZTYPESET_HINTING_LIGHT` is the
 * autohinter and nothing else for these faces: FT_LOAD_TARGET_LIGHT falls
 * through to it whenever the driver does not hint lightly itself, and the CFF
 * driver is the only one in FreeType that sets that flag (`cffdrivr.c`), so
 * every TrueType face takes the autohinter in light mode. A TrueType face
 * with no `fpgm` and a `prep` of seven bytes or fewer takes it in EVERY mode.
 *
 * Not a new dependency: HarfBuzz is already linked into every configuration
 * of this package, because it is what does the shaping. FreeType does not
 * include a HarfBuzz header for this either -- `ft-hb-types.h` and
 * `ft-hb-decls.h` are its own condensed declarations -- so the FreeType
 * translation units still compile with no HarfBuzz include path and the calls
 * resolve when the two static libraries are linked together.
 *
 * What it does change: `freetype` alone now has undefined `hb_*` symbols and
 * has to be linked alongside `harfbuzz`. Both are installed, ci/header-link.sh
 * links every installed library together, and this is the same property
 * upstream's own HarfBuzz-enabled build has.
 *
 * FT_CONFIG_OPTION_USE_HARFBUZZ_DYNAMIC is deliberately NOT defined: it makes
 * FreeType dlopen a system libharfbuzz at run time, which for a package that
 * vendors and statically links its own would mean hinting against a different
 * HarfBuzz than the one that shaped the text.
 */
#ifndef FT_CONFIG_OPTION_USE_HARFBUZZ
#define FT_CONFIG_OPTION_USE_HARFBUZZ
#endif

/*
 * Left exactly as upstream has it, deliberately:
 *
 *   TT_CONFIG_OPTION_GX_VAR_SUPPORT   on. Variable fonts, no extra dependency.
 *
 *   FT_CONFIG_OPTION_SUBPIXEL_RENDERING
 *                                     off, which is upstream's default.
 *
 * The second one is not "no LCD". FreeType has TWO subpixel implementations
 * and this macro chooses between them: with it OFF, FT_RENDER_MODE_LCD renders
 * in HARMONY mode -- three coverage samples a third of a pixel apart, one per
 * stripe -- and with it ON, FreeType renders once at triple width and runs a
 * ClearType-style FIR filter over the result, which the caller must select
 * with FT_Library_SetLcdFilter or get colour fringing.
 *
 * ztypeset exposes ZTYPESET_RENDER_MODE_LCD and ZTYPESET_RENDER_MODE_LCD_V,
    so both
 * paths reach a consumer either way. Harmony is chosen because it needs no
 * filter to be selected, has no filter to be selected WRONGLY, and produces no
 * fringing of its own. The Microsoft patents that once shadowed the filtered
 * path have expired, so this is a technical choice and not a legal one.
 *
 * It is a COMPILE-time choice in FreeType, not a runtime one, which is why
 * ztypeset does not offer both and does not expose FT_Library_SetLcdFilter: in
 * this configuration that function returns FT_Err_Unimplemented_Feature, and
 * an entry point that can only fail is worse than none.
 */

#endif /* ZTYPESET_FTOPTION_H_ */
