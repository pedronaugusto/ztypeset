//===----------------------------------------------------------------------===//
// ztypeset -- the implementation-private contracts, exercised directly.
//
// ffi/ztypeset_internal.h declares helpers ztypeset.h never exposes: a decoder,
// a character-boundary test, a pixels-to-26.6 conversion. Each carries a
// documented contract, and until this file existed nothing could reach them.
// The Zig suite enters through ztypeset.h; tests/c_smoke.c and
// tests/null_sweep.c link the installed library and so see only ZTYPESET_API.
// An internal precondition was therefore checkable in exactly one way -- by
// reading every caller and finding none that violates it -- and "no caller does
// that today" is not a property a header can promise about tomorrow.
//
// That gap is what let ztypesetTextDecode read text[index] before comparing
// index to length. Both of its callers are in bounds by construction, so no
// test could have gone red; the read was one past the end for any caller that
// ever passed the end, and the contract said nothing either way.
//
// This binary compiles the ffi/*.c units into itself rather than linking
// libztypeset, because these symbols are deliberately not exported: a shared
// build hides them behind -fvisibility=hidden and an MSVC DLL never declares
// them, so a test that linked the library would run in the static arm alone
// -- which is the arm where the ABI matters least.
//===----------------------------------------------------------------------===//

#include <fenv.h>
#include <stdio.h>
#include <string.h>

#include "ztypeset_internal.h"

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
// ztypesetTextDecode -- the bound
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
        ztypesetTextDecode(utf8, 2u, ZTYPESET_ENCODING_UTF8, 2u, &out);
    check(step == 1u, "utf-8: decode at index == length returns 1, not 0");
    check(out == 0xFFFDu, "utf-8: decode at index == length yields U+FFFD");
  }
  {
    // 'A' plus a high surrogate, which would claim the unit after it.
    const uint16_t utf16[] = {0x0041u, 0xD800u, 0xDC00u};
    uint32_t out = 0u;
    const size_t step =
        ztypesetTextDecode(utf16, 1u, ZTYPESET_ENCODING_UTF16, 1u, &out);
    check(step == 1u, "utf-16: decode at index == length returns 1, not 0");
    check(out == 0xFFFDu, "utf-16: decode at index == length yields U+FFFD");
  }
  {
    const uint32_t utf32[] = {0x0041u, 0x0042u};
    uint32_t out = 0u;
    const size_t step =
        ztypesetTextDecode(utf32, 1u, ZTYPESET_ENCODING_UTF32, 1u, &out);
    check(step == 1u, "utf-32: decode at index == length returns 1, not 0");
    check(out == 0xFFFDu, "utf-32: decode at index == length yields U+FFFD");
  }
  {
    // A length of zero is the same bound with nothing before it.
    const char utf8[] = {(char)0xE2, (char)0x82, (char)0xAC};
    uint32_t out = 0u;
    const size_t step =
        ztypesetTextDecode(utf8, 0u, ZTYPESET_ENCODING_UTF8, 0u, &out);
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
        ztypesetTextDecode(text, length, ZTYPESET_ENCODING_UTF8, i, &cp);
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
  const size_t step = ztypesetTextDecode(text, 3u, ZTYPESET_ENCODING_UTF8, 1u,
                                         &cp);
  check(step == 2u, "utf-8: a whole character before the end still spans 2");
  check(cp == 0xE9u, "utf-8: and decodes to U+00E9");
}

//===----------------------------------------------------------------------===//
// ztypesetTextSplitsCharacter -- the same bound, from the other side
//===----------------------------------------------------------------------===//

static void splitsCharacterIsBounded(void) {
  const char text[] = "A\xC3\xA9";
  const size_t length = sizeof(text) - 1u;

  check(!ztypesetTextSplitsCharacter(text, length, ZTYPESET_ENCODING_UTF8, 0u),
        "utf-8: index 0 is a boundary");
  check(!ztypesetTextSplitsCharacter(text, length, ZTYPESET_ENCODING_UTF8, 1u),
        "utf-8: index 1 is a boundary");
  check(ztypesetTextSplitsCharacter(text, length, ZTYPESET_ENCODING_UTF8, 2u),
        "utf-8: index 2 is inside a character");
  // The end of a half-open range, which is what lets a caller test both ends.
  check(!ztypesetTextSplitsCharacter(text, length, ZTYPESET_ENCODING_UTF8,
        length),
        "utf-8: index == length is not a split");
  check(!ztypesetTextSplitsCharacter(text, length, ZTYPESET_ENCODING_UTF8,
                                  length + 9u),
        "utf-8: an index past the end is not a split");
}

//===----------------------------------------------------------------------===//
// ztypesetToFixed266 -- one conversion, one domain
//
// The face's pixel size and the stroker's radius are the same conversion with
// the same requirement, and they had two copies of it. Zero is the refusal
// value, so every rejected input has to produce it.
//
// NaN is the ONLY input that separates the guard's two negations from the
// range test they read like: De Morgan makes the two spellings identical for
// every ordered value, endpoints included. And the returned value cannot see
// that boundary. Converting a NaN to an integer is undefined, and on AArch64
// it yields 0 -- the refusal value itself -- so `== 0` holds either way. What
// separates them is whether the conversion RAN, and IEEE 754 makes a
// conversion handed a NaN an invalid operation, which sets a readable flag.
//
// Blind spot, stated: that flag is only evidence while the refusal is a
// branch. Optimised, LLVM makes it branchless -- the multiply and the
// conversion run unconditionally and a csel picks between the two answers --
// so a REFUSED NaN is converted too and the flag stops separating them. The
// probe is therefore taken in an unoptimised build alone, which is the build
// ci/check-guards.sh runs. The x86 arms need none of it: cvttss2si answers a
// NaN with INT_MIN, so there the returned value is the whole story.
//===----------------------------------------------------------------------===//

/// Whether the invalid-operation flag can separate a refused NaN from a
/// converted one in THIS build; see the note above. Clang defines
/// __OPTIMIZE__ exactly when it is optimising, which is exactly when the
/// refusal stops being a branch.
#ifdef __OPTIMIZE__
#define ZTYPESET_FLAG_SEPARATES_A_NAN 0
#else
#define ZTYPESET_FLAG_SEPARATES_A_NAN 1
#endif

static void toFixed266Domain(void) {
  check(ztypesetToFixed266(1.0f) == 64, "26.6: one pixel is 64");
  check(ztypesetToFixed266(0.5f) == 32, "26.6: half a pixel is 32");
  check(ztypesetToFixed266(12.0f) == 768, "26.6: twelve pixels are 768");
  check(ztypesetToFixed266(16384.0f) == 1048576,
        "26.6: the largest size converts");

  check(ztypesetToFixed266(0.0f) == 0, "26.6: zero is refused");
  check(ztypesetToFixed266(-1.0f) == 0, "26.6: a negative size is refused");
  check(ztypesetToFixed266(16384.5f) == 0,
        "26.6: past the largest size is refused");

  // 0.0f/0.0f raises the same FE_INVALID a float-to-integer conversion of a
  // NaN does, so the flag is read once before a refusal is judged by it.
  volatile float zero = 0.0f;
  feclearexcept(FE_ALL_EXCEPT);
  const float nan = zero / zero;
  const int flag_readable = fetestexcept(FE_INVALID) != 0;

  feclearexcept(FE_ALL_EXCEPT);
  const int32_t refused = ztypesetToFixed266(nan);
  const int converted = fetestexcept(FE_INVALID) != 0;

  check(refused == 0, "26.6: a NaN is refused");
  if (ZTYPESET_FLAG_SEPARATES_A_NAN) {
    check(flag_readable,
          "26.6: an invalid operation raises a flag this test can read");
    // Refused, and refused BEFORE the conversion. The range test satisfies
    // the first of those here and cannot satisfy the second.
    check(!converted, "26.6: a NaN is refused before the conversion");
  }
}

int main(void) {
  printf("ztypeset internal contracts\n");
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
