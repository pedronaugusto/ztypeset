//===----------------------------------------------------------------------===//
// Reading a font file into memory -- the one home, for every C test.
//
// bench.c, c_smoke.c and null_sweep.c each open the font named on their
// command line, and each had written this out for itself. Three copies of one
// routine turned out to be three qualities of it: c_smoke's checked fseek and
// ftell and closed the file on every path, while the other two checked
// neither and leaked the FILE* whenever the read came up short. Nothing made
// the careful one the copy a fourth test would be written from.
//
// It is a header rather than a fourth translation unit because each C test is
// linked on its own, from a single .c file plus the installed library, which
// is the shape that proves a consumer can do the same.
//===----------------------------------------------------------------------===//

#ifndef ZTYPESET_TEST_IO_H_
#define ZTYPESET_TEST_IO_H_

#include <stdio.h>
#include <stdlib.h>

/// Reads `path` whole into a malloc'd buffer and reports its size.
///
/// Returns NULL on any failure -- unopenable, unseekable, empty, short read,
/// or out of memory -- with nothing allocated and nothing left open. The
/// caller frees the result with `free`.
static unsigned char* ztypesetTestReadFile(const char* path, size_t* size_out) {
  // `fopen` is ISO C11 7.21.5.3. Microsoft's UCRT marks it deprecated in
  // favour of `fopen_s`, which is Annex K -- an optional annex that neither
  // glibc nor musl implements -- so taking that advice would make these tests
  // Windows-only. Under -Werror on the msvc ABI the deprecation is a build
  // failure, so it is suppressed HERE, around the one call, rather than by
  // putting _CRT_SECURE_NO_WARNINGS on the command line: every other
  // deprecation the CRT reports still fails the build.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  FILE* file = fopen(path, "rb");
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  if (file == NULL) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  const long size = ftell(file);
  if (size <= 0) {
    fclose(file);
    return NULL;
  }
  rewind(file);
  unsigned char* buffer = (unsigned char*)malloc((size_t)size);
  if (buffer == NULL) {
    fclose(file);
    return NULL;
  }
  const size_t read = fread(buffer, 1, (size_t)size, file);
  fclose(file);
  if (read != (size_t)size) {
    free(buffer);
    return NULL;
  }
  *size_out = read;
  return buffer;
}

#endif  // ZTYPESET_TEST_IO_H_
