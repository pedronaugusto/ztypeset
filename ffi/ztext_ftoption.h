/*
 * ztext's FreeType build configuration.
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

#ifndef ZTEXT_FTOPTION_H_
#define ZTEXT_FTOPTION_H_

#include <freetype/config/ftoption.h>

/*
 * Compressed and container formats. Upstream enables zlib by default, which
 * pulls in src/gzip and buys gzip-compressed PCF plus WOFF. ztext consumes
 * TrueType and OpenType from an asset pack, where the pack is already the
 * compression layer, so both are dead weight -- and each one is parser surface
 * on untrusted bytes that we would otherwise be carrying for nothing.
 *
 * The consequence is stated rather than hidden: a WOFF or WOFF2 file handed to
 * ztext is rejected as an unsupported format. Cook to TTF/OTF.
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
 * path-based entry points on macOS, and ztext has no path-based entry point at
 * all: faces come from memory. Dropping it also drops src/base/ftrfork.c's
 * guessing logic, which is a pile of heuristics over attacker-visible bytes.
 */
#undef FT_CONFIG_OPTION_MAC_FONTS

/*
 * Report FreeType's own error strings. ztext maps FT errors onto its flat
 * result enum, which necessarily loses detail; keeping the strings means a
 * BadFont can still say *why* in a log line instead of only that it was bad.
 * Costs a few kilobytes of static text.
 */
#ifndef FT_CONFIG_OPTION_ERROR_STRINGS
#define FT_CONFIG_OPTION_ERROR_STRINGS
#endif

/*
 * Left exactly as upstream has it, deliberately:
 *
 *   TT_CONFIG_OPTION_GX_VAR_SUPPORT   on. Variable fonts, no extra dependency.
 *
 *   FT_CONFIG_OPTION_SUBPIXEL_RENDERING
 *                                     off, which is upstream's default. This is
 *                                     the ClearType-style LCD filter; the
 *                                     Microsoft patents that once shadowed it
 *                                     have expired, so this is a technical
 *                                     choice and not a legal one -- ztext
 *                                     produces A8 coverage and SDF, neither of
 *                                     which is subpixel, and an engine that
 *                                     wants LCD filtering wants it in its own
 *                                     shader against its own subpixel layout.
 */

#endif /* ZTEXT_FTOPTION_H_ */
