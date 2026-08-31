/*
 * The exact set of FreeType modules ztypeset compiles in.
 *
 * FreeType includes this file wherever it would have included
 * <freetype/config/ftmodule.h>; build.zig points FT_CONFIG_MODULES_H here. It
 * lives in ffi/ rather than in libs/ so the vendored tree stays a pristine copy
 * of upstream -- see UPSTREAM.md.
 *
 * This list and the FreeType source list in build.zig must agree. They are
 * checked against each other by the linker: a module named here whose sources
 * are not compiled is an undefined symbol, and a module compiled but not named
 * here is dead weight that FT_Add_Default_Modules never registers. Loud either
 * way, so the list is here and nowhere else.
 *
 * Dropped relative to upstream's default list, with reasons:
 *
 *   t1, t1cid, t42     Type 1 / CID / Type 42.  PostScript outline formats that
 *                      predate OpenType; no game ships them.
 *   pfr, winfnt,       Bitmap and legacy formats: PFR, Windows FNT/FON, PCF,
 *   pcf, bdf           BDF.  X11 and DOS-era, not asset-pack material.
 *   raster1            The monochrome rasteriser.  ztypeset renders A8 coverage
 *                      and SDF; there is no API path that asks for 1-bit.
 *   svg                OT-SVG needs a caller-supplied SVG rendering hook, which
 *                      would be a whole second dependency.
 *
 * Kept: TrueType and CFF/CFF2 (every OpenType font on disk), sfnt (their shared
 * container), psaux/psnames/pshinter (CFF's parser and hinter), autofit (the
 * hinter used when a face has no usable hints of its own), smooth (the A8
 * rasteriser) and sdf (FreeType's native signed-distance-field renderers).
 */

FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_sdf_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_bitmap_sdf_renderer_class )

/* EOF */
