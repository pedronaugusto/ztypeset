/* A downstream C consumer, using the vendored upstreams directly.
 *
 * The point is the #includes: they resolve only if build.zig installs each
 * library's headers under the spelling that library documents. */
#include <stdio.h>

#include <ztypeset.h>

#include <hb.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <SheenBidi/SheenBidi.h>

int main(void) {
  FT_Library ft;
  if (FT_Init_FreeType(&ft) != 0) return 1;
  int major = 0, minor = 0, patch = 0;
  FT_Library_Version(ft, &major, &minor, &patch);
  FT_Done_FreeType(ft);

  const uint32_t v = ztypesetVersion();
  const uint32_t u = ztypesetUnibreakVersion();
  /* Three components for every one of them. This printed unibreak as
   * "%u.%u" while the Zig consumer printed "7.0.0" for the same number: two
   * formatters for one fact, and two outputs that cannot be diffed. */
  printf("c consumer ok: ztypeset %u.%u.%u, freetype %d.%d.%d, harfbuzz %s, "
         "sheenbidi %s, unibreak %u.%u.%u\n",
         v >> 16, (v >> 8) & 0xFFu, v & 0xFFu, major, minor, patch,
         hb_version_string(), SHEENBIDI_VERSION_STRING, u >> 16,
         (u >> 8) & 0xFFu, u & 0xFFu);

  /* What ztypeset REPORTS must be what it LINKED. The pinned values themselves
   * are asserted by the suite against src/pins.zig; repeating them here would
   * be a second copy to keep in step, and this checks something that one
   * cannot -- that ztypeset's version entry points agree with the libraries
   * standing behind them. */
  {
    const uint32_t ft = ztypesetFreetypeVersion();
    const uint32_t hb = ztypesetHarfbuzzVersion();
    if ((int)(ft >> 16) != major || (int)((ft >> 8) & 0xFFu) != minor ||
        (int)(ft & 0xFFu) != patch) {
      printf("c consumer: ztypeset reports FreeType %u.%u.%u, the library says "
             "%d.%d.%d\n",
             ft >> 16, (ft >> 8) & 0xFFu, ft & 0xFFu, major, minor, patch);
      return 1;
    }
    if (hb_version_atleast(hb >> 16, (hb >> 8) & 0xFFu, hb & 0xFFu) == 0) {
      printf("c consumer: ztypeset reports HarfBuzz %u.%u.%u, the library says "
             "%s\n",
             hb >> 16, (hb >> 8) & 0xFFu, hb & 0xFFu, hb_version_string());
      return 1;
    }
  }
  return 0;
}
