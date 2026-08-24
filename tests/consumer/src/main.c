/* A downstream C consumer, using the vendored upstreams directly.
 *
 * The point is the #includes: they resolve only if build.zig installs each
 * library's headers under the spelling that library documents. */
#include <stdio.h>

#include <ztext.h>

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

  const uint32_t v = ztextVersion();
  const uint32_t u = ztextUnibreakVersion();
  printf("c consumer ok: ztext %u.%u.%u, freetype %d.%d.%d, harfbuzz %s, "
         "sheenbidi %s, unibreak %u.%u\n",
         v >> 16, (v >> 8) & 0xFFu, v & 0xFFu, major, minor, patch,
         hb_version_string(), SHEENBIDI_VERSION_STRING, u >> 16,
         (u >> 8) & 0xFFu);

  /* The versions the artifacts report must be the pinned ones. */
  if (major != 2 || minor != 14) return 1;
  if (hb_version_atleast(14, 3, 0) == 0) return 1;
  if ((u >> 16) != 7u) return 1;
  return 0;
}
