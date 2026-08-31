//===----------------------------------------------------------------------===//
// ztext -- the implementation-private contracts, exercised directly.
//
// ffi/ztext_internal.h declares helpers ztext.h never exposes: a decoder, a
// character-boundary test, a pixels-to-26.6 conversion. Each carries a
// documented contract, and until this file existed nothing could reach them.
// The Zig suite enters through ztext.h; tests/c_smoke.c and tests/null_sweep.c
// link the installed library and so see only ZTEXT_API. An internal
// precondition was therefore checkable in exactly one way -- by reading every
// caller and finding none that violates it -- and "no caller does that today"
// is not a property a header can promise about tomorrow.
//
// That gap is what let ztextTextDecode read text[index] before comparing
// index to length. Both of its callers are in bounds by construction, so no
// test could have gone red; the read was one past the end for any caller that
// ever passed the end, and the contract said nothing either way.
//
// This binary compiles the ffi/*.c units into itself rather than linking
// libztext, because these symbols are deliberately not exported: a shared
// build hides them behind -fvisibility=hidden and an MSVC DLL never declares
// them, so a test that linked the library would run in the static arm alone
// -- which is the arm where the ABI matters least.
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <string.h>

#include "ztext_internal.h"

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
  g_checks++;
  if (!ok) {
    printf("  FAIL %s\n", what);
    g_failures++;
  }
}

//===----------------------------------------------------------------------===//
// ztextTextDecode -- the bound
//
// Every buffer below carries ONE unit past the length it declares, and that
// unit is chosen so that a decoder which reads it cannot return what a
// decoder which respects the bound returns. A 0xC0 lead byte claims a
// continuation; a high surrogate claims a low one; a non-zero scalar is not
// U+FFFD. So the assertion is on the returned value, not on a fault.
//
// Blind spot, stated: this proves the FUNCTION stops at the bound. It does
// not prove a process would fault without it -- the sentinel unit is inside
// this test's own allocation. What the bound prevents is a caller whose
// buffer has no spare byte, and no test here can own that caller's memory.
//===----------------------------------------------------------------------===//

static void decodeStopsAtTheBound(void) {
  {
    // "AB" plus a lead byte claiming one continuation.
    const char utf8[] = {'A', 'B', (char)0xC3, (char)0xA9};
    uint32_t out = 0u;
    const size_t step =
        ztextTextDecode(utf8, 2u, ZTEXT_ENCODING_UTF8, 2u, &out);
    check(step == 1u, "utf-8: decode at index == length returns 1, not 0");
    check(out == 0xFFFDu, "utf-8: decode at index == length yields U+FFFD");
  }
  {
    // 'A' plus a high surrogate, which would claim the unit after it.
    const uint16_t utf16[] = {0x0041u, 0xD800u, 0xDC00u};
    uint32_t out = 0u;
    const size_t step =
        ztextTextDecode(utf16, 1u, ZTEXT_ENCODING_UTF16, 1u, &out);
    check(step == 1u, "utf-16: decode at index == length returns 1, not 0");
    check(out == 0xFFFDu, "utf-16: decode at index == length yields U+FFFD");
  }
  {
    const uint32_t utf32[] = {0x0041u, 0x0042u};
    uint32_t out = 0u;
    const size_t step =
        ztextTextDecode(utf32, 1u, ZTEXT_ENCODING_UTF32, 1u, &out);
    check(step == 1u, "utf-32: decode at index == length returns 1, not 0");
    check(out == 0xFFFDu, "utf-32: decode at index == length yields U+FFFD");
  }
  {
    // A length of zero is the same bound with nothing before it.
    const char utf8[] = {(char)0xE2, (char)0x82, (char)0xAC};
    uint32_t out = 0u;
    const size_t step =
        ztextTextDecode(utf8, 0u, ZTEXT_ENCODING_UTF8, 0u, &out);
    check(step == 1u, "utf-8: decode of an empty buffer returns 1, not 0");
    check(out == 0xFFFDu, "utf-8: decode of an empty buffer yields U+FFFD");
  }
}

/// And the contract it exists to keep: never zero, so a loop terminates.
static void decodeNeverReturnsZero(void) {
  const char text[] = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  const size_t length = sizeof(text) - 1u;
  size_t i = 0u;
  size_t characters = 0u;
  while (i < length) {
    uint32_t cp = 0u;
    const size_t step =
        ztextTextDecode(text, length, ZTEXT_ENCODING_UTF8, i, &cp);
    if (step == 0u) {
      check(false, "utf-8: a step of 0 would not terminate");
      return;
    }
    i += step;
    characters++;
  }
  check(i == length, "utf-8: the steps land exactly on the end");
  check(characters == 4u, "utf-8: 1 + 2 + 3 + 4 bytes is four characters");

  // A truncated tail is reported as one replacement character spanning what
  // is left, which is also non-zero.
  uint32_t cp = 0u;
  const size_t step = ztextTextDecode(text, 3u, ZTEXT_ENCODING_UTF8, 1u, &cp);
  check(step == 2u, "utf-8: a whole character before the end still spans 2");
  check(cp == 0xE9u, "utf-8: and decodes to U+00E9");
}

//===----------------------------------------------------------------------===//
// ztextTextSplitsCharacter -- the same bound, from the other side
//===----------------------------------------------------------------------===//

static void splitsCharacterIsBounded(void) {
  const char text[] = "A\xC3\xA9";
  const size_t length = sizeof(text) - 1u;

  check(!ztextTextSplitsCharacter(text, length, ZTEXT_ENCODING_UTF8, 0u),
        "utf-8: index 0 is a boundary");
  check(!ztextTextSplitsCharacter(text, length, ZTEXT_ENCODING_UTF8, 1u),
        "utf-8: index 1 is a boundary");
  check(ztextTextSplitsCharacter(text, length, ZTEXT_ENCODING_UTF8, 2u),
        "utf-8: index 2 is inside a character");
  // The end of a half-open range, which is what lets a caller test both ends.
  check(!ztextTextSplitsCharacter(text, length, ZTEXT_ENCODING_UTF8, length),
        "utf-8: index == length is not a split");
  check(!ztextTextSplitsCharacter(text, length, ZTEXT_ENCODING_UTF8,
                                  length + 9u),
        "utf-8: an index past the end is not a split");
}

//===----------------------------------------------------------------------===//
// ztextToFixed266 -- one conversion, one domain
//
// The face's pixel size and the stroker's radius are the same conversion with
// the same requirement, and they had two copies of it. Zero is the refusal
// value, so every rejected input has to produce it.
//===----------------------------------------------------------------------===//

static void toFixed266Domain(void) {
  check(ztextToFixed266(1.0f) == 64, "26.6: one pixel is 64");
  check(ztextToFixed266(0.5f) == 32, "26.6: half a pixel is 32");
  check(ztextToFixed266(12.0f) == 768, "26.6: twelve pixels are 768");
  check(ztextToFixed266(16384.0f) == 1048576,
        "26.6: the largest size converts");

  check(ztextToFixed266(0.0f) == 0, "26.6: zero is refused");
  check(ztextToFixed266(-1.0f) == 0, "26.6: a negative size is refused");
  check(ztextToFixed266(16384.5f) == 0,
        "26.6: past the largest size is refused");

  // A NaN fails every comparison, which is why the guard is written as two
  // negations rather than as a range test: `pixels <= 0 || pixels > 16384`
  // would let a NaN through.
  volatile float zero = 0.0f;
  const float nan = zero / zero;
  check(ztextToFixed266(nan) == 0, "26.6: a NaN is refused");
}

int main(void) {
  printf("ztext internal contracts\n");
  decodeStopsAtTheBound();
  decodeNeverReturnsZero();
  splitsCharacterIsBounded();
  toFixed266Domain();

  if (g_failures != 0) {
    printf("internal: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  printf("internal: ok (%d checks)\n", g_checks);
  return 0;
}
