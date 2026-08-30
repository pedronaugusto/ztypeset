//===----------------------------------------------------------------------===//
// ztext -- the C boundary on its own.
//
// Nothing here is Zig. That is the point: ztext.h has to be a real C contract
// that a plain C host can use, and the allocator seam has to be genuinely in
// use rather than a parameter nobody passes. This asserts both, and asserts
// that every byte handed out comes back.
//
// Usage: ztext-c-smoke <path-to-font.ttf>
//===----------------------------------------------------------------------===//

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define ZTEXT_SMOKE_WRITE(fd, buf, len) (void)_write((fd), (buf), (unsigned)(len))
#else
#include <unistd.h>
#define ZTEXT_SMOKE_WRITE(fd, buf, len) (void)write((fd), (buf), (len))
#endif

#include "ztext.h"

/// Exit code this test uses when the process faults. Distinct from 1 (a
/// check failed) and from 2 (it could not start), so a harness can tell a
/// memory fault from a wrong answer without parsing anything.
#define ZTEXT_SMOKE_EXIT_FAULT 86

static int failures = 0;

#define CHECK(condition, ...)                                     \
  do {                                                            \
    if (!(condition)) {                                           \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);               \
      printf(__VA_ARGS__);                                        \
      printf("\n");                                               \
      failures++;                                                 \
    }                                                             \
  } while (0)

#define CHECK_OK(expression)                                      \
  do {                                                            \
    ZtextResult result_ = (expression);                           \
    CHECK(result_ == ZTEXT_RESULT_OK, "%s -> %s (%s)", #expression,      \
          ztextResultName(result_), ztextLastErrorDetail());       \
  } while (0)

//===----------------------------------------------------------------------===//
// Phase markers.
//
// stdout is a fully-buffered pipe whenever this runs under a script, and the
// whole transcript is well under one buffer -- so a process that dies takes
// every line it ever printed with it and the report is "exit 139" and nothing
// else. That is how an intermittent segfault here stayed unlocated.
//
// Two things fix that, and both are permanent: stdout is switched to
// unbuffered in main, and each phase announces itself on stderr, which C
// guarantees is never fully buffered. ci/crash-loop.sh reads the last marker,
// so a crash names the phase it died in with nothing to rebuild.
//===----------------------------------------------------------------------===//

/// The phase now running, kept so the fault reporter below can name it. It is
/// only ever read from a signal handler, so it is written in one snprintf and
/// is always NUL-terminated.
static char g_phase[96] = "start";

static void phase(const char* name) {
  snprintf(g_phase, sizeof(g_phase), "%s", name);
  fprintf(stderr, "phase: %s\n", name);
  fflush(stderr);
}

/// The same, for a phase that repeats: the injection sweep runs the whole
/// pipeline 220 times, so "died in the sweep" is not a location and "died at
/// point 62" is.
static void phaseAt(const char* name, long index) {
  snprintf(g_phase, sizeof(g_phase), "%s %ld", name, index);
  fprintf(stderr, "phase: %s %ld\n", name, index);
  fflush(stderr);
}

//===----------------------------------------------------------------------===//
// A fault names the phase it died in.
//
// Without this, a process that faults leaves an exit code and nothing else --
// "error code 5" from the Zig build runner on Windows, "exit 139" under a
// POSIX shell -- and neither says where. That is not a theoretical loss: the
// mutation ci/check-guards.sh plants over the SheenBidi seam produces a memory
// fault, and that script's whole contract is that a mutation must be caught by
// a NAMED check rather than by something merely going wrong. A bare exit code
// names nothing, so the guard could not be written at all.
//
// The handler runs in async-signal context: no printf, no malloc, only write()
// to a fixed descriptor, and _Exit rather than exit so no atexit handler or
// crash reporter runs on top of whatever is already broken.
//===----------------------------------------------------------------------===//

static void emit(const char* text) {
  ZTEXT_SMOKE_WRITE(2, text, strlen(text));
}

static void reportFault(const char* what) {
  emit("\nc smoke: FAULT (");
  emit(what);
  emit(") during phase: ");
  emit(g_phase);
  emit("\n");
  _Exit(ZTEXT_SMOKE_EXIT_FAULT);
}

static void onSignal(int sig) {
  reportFault(sig == SIGSEGV   ? "SIGSEGV"
              : sig == SIGILL  ? "SIGILL"
              : sig == SIGFPE  ? "SIGFPE"
              : sig == SIGABRT ? "SIGABRT"
                               : "a fatal signal");
}

#ifdef _WIN32
/// Windows delivers an access violation as a structured exception first, and
/// whether the C runtime goes on to translate it into SIGSEGV depends on which
/// runtime is linked. The unhandled-exception filter does not depend on that.
static LONG WINAPI onWindowsFault(EXCEPTION_POINTERS* info) {
  (void)info;
  reportFault("a structured exception");
  return EXCEPTION_EXECUTE_HANDLER; /* unreachable: reportFault exits */
}
#endif

static void installFaultReporter(void) {
  signal(SIGSEGV, onSignal);
  signal(SIGILL, onSignal);
  signal(SIGFPE, onSignal);
  signal(SIGABRT, onSignal);
#ifdef _WIN32
  SetUnhandledExceptionFilter(onWindowsFault);
#endif
}

//===----------------------------------------------------------------------===//
// A counting allocator, so the seam is measurably in use.
//
// Deliberately does NOT provide `reallocate`, which exercises ztext's
// allocate-copy-deallocate fallback path. The Zig side covers the other case.
//===----------------------------------------------------------------------===//

typedef struct Counters {
  size_t live_bytes;
  size_t live_blocks;
  size_t total_allocations;
} Counters;

static void* countingAllocate(void* user, size_t size, size_t alignment) {
  Counters* counters = (Counters*)user;
  // Every alignment ztext asks for is at most max_align_t's, which is what
  // malloc already guarantees; assert rather than silently mis-serve it.
  if (alignment > _Alignof(max_align_t)) return NULL;
  void* block = malloc(size);
  if (block == NULL) return NULL;
  counters->live_bytes += size;
  counters->live_blocks += 1u;
  counters->total_allocations += 1u;
  return block;
}

static void countingDeallocate(void* user, void* block, size_t size,
                               size_t alignment) {
  Counters* counters = (Counters*)user;
  (void)alignment;
  // The size comes back from ztext, which is the whole point of the seam
  // carrying one -- a plain C host with a pool or an arena can use it.
  counters->live_bytes -= size;
  counters->live_blocks -= 1u;
  free(block);
}

//===----------------------------------------------------------------------===//
// Proof that FreeType's allocation really is per-library.
//
// ztext.h claims a ZtextLibrary captures the allocator installed when it was
// created, and that FreeType memory follows that copy rather than the
// process-wide one. That claim was false once -- the callbacks ignored their
// FT_Memory and dispatched through the global -- and nothing noticed, because
// every test installed one allocator and kept it.
//
// ztextLibraryCountFaces is the lever: it opens and closes an FT_Face and
// touches nothing else, so it is pure FreeType traffic. Run it after swapping
// the process-wide allocator and watch which allocator sees the work.
//===----------------------------------------------------------------------===//

static int checkPerLibraryAllocator(const unsigned char* font,
                                    size_t font_size) {
  Counters first;
  Counters second;
  memset(&first, 0, sizeof(first));
  memset(&second, 0, sizeof(second));

  ZtextAllocator a = {countingAllocate, NULL, countingDeallocate, &first};
  ZtextAllocator b = {countingAllocate, NULL, countingDeallocate, &second};

  ZtextLibrary* library = NULL;
  ZtextFont* the_font = NULL;
  int failed = 0;
  size_t first_used = 0u;

  ztextSetAllocator(&a);
  if (ztextLibraryCreate(&library) != ZTEXT_RESULT_OK) {
    printf("  FAIL per-library: could not create a library\n");
    failed = 1;
  }
  // A font is created here too, so the OTHER half of the claim is covered:
  // its FreeType memory is the library's, but its hb_face_t comes from the
  // process-wide seam, which HarfBuzz makes compile-time and therefore
  // unswitchable. Both have to come back to `a` below.
  if (!failed && ztextFontCreateFromMemory(library, font, font_size, 0,
                                           &the_font) != ZTEXT_RESULT_OK) {
    printf("  FAIL per-library: could not create a font\n");
    failed = 1;
  }

  if (!failed) {
    // Everything from here on is nominally the second allocator's.
    ztextSetAllocator(&b);
    const size_t second_before = second.total_allocations;
    const size_t first_before = first.total_allocations;

    uint32_t faces = 0;
    if (ztextLibraryCountFaces(library, font, font_size, &faces) !=
        ZTEXT_RESULT_OK) {
      printf("  FAIL per-library: counting faces failed\n");
      failed = 1;
    } else {
      first_used = first.total_allocations - first_before;
      const size_t second_used = second.total_allocations - second_before;

      if (first_used == 0) {
        printf("  FAIL per-library: FreeType made no allocations to "
               "observe\n");
        failed = 1;
      } else if (second_used != 0) {
        printf("  FAIL per-library: %zu FreeType allocations leaked to the "
               "process-wide allocator; the library did not capture its own\n",
               second_used);
        failed = 1;
      }
    }
  }

  // Destroyed while the OTHER allocator is installed, deliberately. Every
  // byte must go back to the one that issued it -- the FreeType memory
  // because the library recorded its allocator, and the HarfBuzz memory
  // because the block itself did. Getting the second one wrong is what a
  // reviewer found and what the block header now makes impossible.
  ztextFontDestroy(the_font);
  ztextLibraryDestroy(library);
  ztextSetAllocator(NULL);

  if (!failed && (first.live_blocks != 0 || first.live_bytes != 0)) {
    printf("  FAIL per-library: %zu blocks / %zu bytes never returned to the "
           "creating allocator\n", first.live_blocks, first.live_bytes);
    failed = 1;
  }
  if (!failed && second.live_blocks != 0) {
    printf("  FAIL per-library: %zu blocks left with the wrong allocator\n",
           second.live_blocks);
    failed = 1;
  }

  if (!failed) {
    printf("  per-library allocator: %zu FreeType allocations followed the "
           "library across a global swap, 0 went to the new global, and the "
           "font's HarfBuzz memory came back to the allocator that made it\n",
           first_used);
  }
  return failed;
}

//===----------------------------------------------------------------------===//
// Proof that a warm shaper allocates nothing.
//
// README calls this "steady state, zero allocations". It was an inference from
// the code -- the buffer and both arrays are reused and never shrink -- until
// something counted.
//===----------------------------------------------------------------------===//

static int checkSteadyStateAllocations(const unsigned char* font,
                                       size_t font_size) {
  Counters counters;
  memset(&counters, 0, sizeof(counters));
  ZtextAllocator allocator = {countingAllocate, NULL, countingDeallocate,
                              &counters};
  ztextSetAllocator(&allocator);

  ZtextLibrary* library = NULL;
  ZtextFont* ffont = NULL;
  ZtextFace* face = NULL;
  ZtextShaper* shaper = NULL;
  int failed = 0;

  // Every path below this point falls through to the single teardown at the
  // end: a `return` here would leave a stack allocator installed and a stack
  // Counters registered under it, and the next allocation through either
  // would be writing into a frame that no longer exists.
  if (ztextLibraryCreate(&library) != ZTEXT_RESULT_OK ||
      ztextFontCreateFromMemory(library, font, font_size, 0, &ffont) !=
          ZTEXT_RESULT_OK ||
      ztextFaceCreate(ffont, 0, 18, &face) != ZTEXT_RESULT_OK ||
      ztextShaperCreate(&shaper) != ZTEXT_RESULT_OK) {
    printf("  FAIL steady-state: set-up failed\n");
    failed = 1;
  }

  if (!failed) {
    ZtextShapeParams params;
    memset(&params, 0, sizeof(params));
    params.direction = ZTEXT_DIRECTION_RTL;
    params.script = ZTEXT_TAG('H', 'e', 'b', 'r');
    const char* text =
        "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
    const size_t length = strlen(text);

    // Warm: let every buffer reach its high-water mark.
    for (int i = 0; i < 8; i++) {
      if (ztextShaperShapeUtf8(shaper, face, text, length, 0, length,
                               &params) !=
          ZTEXT_RESULT_OK) {
        printf("  FAIL steady-state: warm-up shape failed\n");
        failed = 1;
        break;
      }
    }

    if (!failed) {
      const size_t before = counters.total_allocations;
      for (int i = 0; i < 500; i++) {
        if (ztextShaperShapeUtf8(shaper, face, text, length, 0, length,
                                 &params) !=
            ZTEXT_RESULT_OK) {
          printf("  FAIL steady-state: shape failed\n");
          failed = 1;
          break;
        }
      }
      const size_t during = counters.total_allocations - before;
      if (!failed && during != 0) {
        printf("  FAIL steady-state: %zu allocations across 500 warm shapes; "
               "the zero-allocation claim is wrong\n", during);
        failed = 1;
      } else if (!failed) {
        printf("  steady state: 500 shapes, 0 allocations\n");
      }
    }
  }

  ztextShaperDestroy(shaper);
  ztextFaceDestroy(face);
  ztextFontDestroy(ffont);
  ztextLibraryDestroy(library);
  ztextSetAllocator(NULL);

  if (!failed && (counters.live_blocks != 0 || counters.live_bytes != 0)) {
    printf("  FAIL steady-state: %zu blocks leaked\n", counters.live_blocks);
    failed = 1;
  }
  return failed;
}

//===----------------------------------------------------------------------===//
// An allocator that fails the Nth allocation, for injection testing.
//
// Every step of the pipeline is run with the failure point walked forward one
// allocation at a time. At each point ztext must do three things: report a
// typed error rather than crashing, leave the caller's out-parameter NULL
// rather than handing back a half-built handle, and free everything it had
// already allocated.
//
// This is the check that a `goto cleanup` chain is actually correct, and it
// cannot be written in Zig -- the failure has to be injected below the C
// boundary, into the allocations FreeType, HarfBuzz and SheenBidi make.
//===----------------------------------------------------------------------===//

typedef struct Injector {
  long budget;   /* allocations to serve before failing; negative = fail */
  size_t live;   /* blocks currently handed out */
  int poison;    /* fill every block with 0xCD before handing it over */
} Injector;

static void* injectAllocate(void* user, size_t size, size_t alignment) {
  Injector* injector = (Injector*)user;
  (void)alignment;
  if (injector->budget-- <= 0) return NULL;
  void* block = malloc(size);
  if (block != NULL) {
    // Malloc's leftovers are whatever the process last had there, which on a
    // short-lived test is mostly zeros -- so a read of uninitialised memory
    // looks like a rare crash instead of a bug. Poisoning removes the luck:
    // see the poisoned arm below.
    if (injector->poison) memset(block, 0xCD, size);
    injector->live += 1u;
  }
  return block;
}

static void injectDeallocate(void* user, void* block, size_t size,
                             size_t alignment) {
  Injector* injector = (Injector*)user;
  (void)size;
  (void)alignment;
  injector->live -= 1u;
  free(block);
}

/// Walks a failure point across the whole pipeline. Returns non-zero on a
/// violation, ALWAYS with the process-wide allocator restored and every
/// handle destroyed -- see the note on the single exit below.
///
/// `poison` decides what an allocation that succeeds contains. Both values
/// matter and both are run:
///
///   0  what a host's allocator normally returns, which is malloc's leftovers.
///   1  0xCD in every byte, which is what an allocator returns when nothing
///      is allowed to depend on the previous contents.
///
/// The poisoned arm exists because of a measured defect. SheenBidi 3.0.0
/// reads a field it has not written on its own allocation-failure path
/// (Core/Object.c ObjectCreate, API/SBParagraph.c FinalizeParagraph), and
/// ztext's SheenBidi seam zeroes every block it hands over so that read finds
/// a NULL rather than a pointer. With malloc's leftovers the defect segfaulted
/// about one run in fifty and stayed unlocated; with 0xCD it is every run.
/// Remove the memset in ztext_core.c's sbAllocateBlock and this arm dies
/// immediately and always, which is the only kind of gate worth having over an
/// intermittent fault.
static int runInjectionSweep(const unsigned char* font, size_t font_size,
                             int poison) {
  const long points = 220;
  const char* arm = poison ? "poisoned" : "plain";
  int completed = 0, out_of_memory = 0, other_error = 0;
  int violated = 0;

  for (long limit = 0; limit < points && !violated; limit++) {
    phaseAt(poison ? "injection-poisoned" : "injection", limit);
    Injector injector;
    injector.budget = limit;
    injector.live = 0u;
    injector.poison = poison;

    ZtextAllocator allocator;
    allocator.allocate = injectAllocate;
    allocator.reallocate = NULL;
    allocator.deallocate = injectDeallocate;
    allocator.user = &injector;
    ztextSetAllocator(&allocator);

    ZtextLibrary* library = NULL;
    ZtextFont* fnt = NULL;
    ZtextFace* face = NULL;
    ZtextShaper* shaper = NULL;
    ZtextParagraph* paragraph = NULL;
    ZtextLine* line = NULL;
    ZtextResult result = ZTEXT_RESULT_OK;
    int stopped = 0;

    // A failed step that still wrote its handle is a violation, and it is
    // recorded rather than returned from: `injector` and `allocator` are on
    // this frame and the process-wide allocator still points at them, so
    // leaving here without tearing down and restoring would hand the next
    // allocation a pointer into a dead frame. Every exit from this loop body
    // is the bottom of it.
#define STEP(call, handle)                                                  \
    if (!stopped) {                                                         \
      result = (call);                                                      \
      if (result != ZTEXT_RESULT_OK) {                                      \
        stopped = 1;                                                        \
        if ((handle) != NULL) {                                             \
          printf("  FAIL injection %s %ld: %s failed but wrote its handle\n",\
                 arm, limit, #call);                                        \
          violated = 1;                                                     \
        }                                                                   \
      }                                                                     \
    }

    STEP(ztextLibraryCreate(&library), library)
    STEP(ztextFontCreateFromMemory(library, font, font_size, 0, &fnt), fnt)
    STEP(ztextFaceCreate(fnt, 0, 24.5f, &face), face)
    STEP(ztextShaperCreate(&shaper), shaper)
    STEP(ztextParagraphCreateUtf8("a \xd7\xa9\xd7\x9c b", 8,
                                  ZTEXT_BASE_DIRECTION_AUTO, &paragraph),
         paragraph)
    STEP(ztextLineCreate(paragraph, 0, 6, &line), line)
#undef STEP

    if (!stopped) {
      ZtextShapeParams params;
      memset(&params, 0, sizeof(params));
      params.direction = ZTEXT_DIRECTION_RTL;
      params.script = ZTEXT_TAG('H', 'e', 'b', 'r');
      result = ztextShaperShapeUtf8(
          shaper, face, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", 8, 0, 8,
          &params);
      if (result != ZTEXT_RESULT_OK) stopped = 1;
    }

    if (!stopped) completed++;
    else if (result == ZTEXT_RESULT_OUT_OF_MEMORY) out_of_memory++;
    else other_error++;

    ztextLineDestroy(line);
    ztextParagraphDestroy(paragraph);
    ztextShaperDestroy(shaper);
    // Deliberately the "wrong" way round: the font is released while its face
    // is still alive, so every one of the 220 injection points also walks the
    // order-free teardown.
    ztextFontDestroy(fnt);
    ztextFaceDestroy(face);
    ztextLibraryDestroy(library);

    if (injector.live != 0u) {
      printf("  FAIL injection %s %ld: %zu blocks leaked after teardown (%s)\n",
             arm, limit, injector.live, ztextResultName(result));
      violated = 1;
    }

    // Restored before the frame holding `injector` goes away, on every path
    // through this body including the two violations above.
    ztextSetAllocator(NULL);
  }

  if (violated) return 1;

  printf("  injection (%s): %ld failure points - %d completed, %d "
         "out-of-memory, %d other typed error, 0 leaks, 0 crashes\n",
         arm, points, completed, out_of_memory, other_error);

  // Both ends must occur, or the sweep is not testing what it claims.
  if (completed == 0) {
    printf("  FAIL injection %s: no failure point ever let the pipeline "
           "finish\n", arm);
    return 1;
  }
  if (out_of_memory == 0) {
    printf("  FAIL injection %s: no failure point ever reported "
           "out-of-memory\n", arm);
    return 1;
  }
  return 0;
}

//===----------------------------------------------------------------------===//

static unsigned char* readFile(const char* path, size_t* size_out) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
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

//===----------------------------------------------------------------------===//
// Outline decomposition: a callback context that just counts events, so the
// smoke test proves the C callback shape works without a real path renderer.
//===----------------------------------------------------------------------===//

typedef struct OutlineCounts {
  int move_to;
  int line_to;
  int conic_to;
  int cubic_to;
  int close;
} OutlineCounts;

static ZtextResult countMoveTo(void* user, int32_t x, int32_t y) {
  (void)x;
  (void)y;
  ((OutlineCounts*)user)->move_to++;
  return ZTEXT_RESULT_OK;
}
static ZtextResult countLineTo(void* user, int32_t x, int32_t y) {
  (void)x;
  (void)y;
  ((OutlineCounts*)user)->line_to++;
  return ZTEXT_RESULT_OK;
}
static ZtextResult countConicTo(void* user, int32_t control_x, int32_t control_y,
                                int32_t x, int32_t y) {
  (void)control_x;
  (void)control_y;
  (void)x;
  (void)y;
  ((OutlineCounts*)user)->conic_to++;
  return ZTEXT_RESULT_OK;
}
static ZtextResult countCubicTo(void* user, int32_t control1_x, int32_t control1_y,
                                int32_t control2_x, int32_t control2_y, int32_t x,
                                int32_t y) {
  (void)control1_x;
  (void)control1_y;
  (void)control2_x;
  (void)control2_y;
  (void)x;
  (void)y;
  ((OutlineCounts*)user)->cubic_to++;
  return ZTEXT_RESULT_OK;
}
static ZtextResult countClose(void* user) {
  ((OutlineCounts*)user)->close++;
  return ZTEXT_RESULT_OK;
}

int main(int argc, char** argv) {
  // See the phase-marker note above: a buffered transcript is lost on a
  // crash, which is the one run where it is worth having.
  setvbuf(stdout, NULL, _IONBF, 0);
  installFaultReporter();

  if (argc < 2) {
    printf("usage: %s <font.ttf>\n", argv[0]);
    return 2;
  }

  size_t font_size = 0;
  unsigned char* font = readFile(argv[1], &font_size);
  if (font == NULL) {
    printf("could not read %s\n", argv[1]);
    return 2;
  }

  printf("ztext %u.%u.%u  freetype %u.%u.%u  harfbuzz %u.%u.%u  "
         "sheenbidi %u.%u.%u\n",
         ztextVersion() >> 16, (ztextVersion() >> 8) & 0xFF,
         ztextVersion() & 0xFF,
         ztextFreetypeVersion() >> 16, (ztextFreetypeVersion() >> 8) & 0xFF,
         ztextFreetypeVersion() & 0xFF,
         ztextHarfbuzzVersion() >> 16, (ztextHarfbuzzVersion() >> 8) & 0xFF,
         ztextHarfbuzzVersion() & 0xFF,
         ztextSheenbidiVersion() >> 16, (ztextSheenbidiVersion() >> 8) & 0xFF,
         ztextSheenbidiVersion() & 0xFF);

  CHECK(ztextVersion() == (uint32_t)((ZTEXT_VERSION_MAJOR << 16) |
                                     (ZTEXT_VERSION_MINOR << 8) |
                                     ZTEXT_VERSION_PATCH),
        "the header and the library disagree about ztext's version");

  // The upstreams' process-lifetime caches are populated before the counting
  // allocator goes in, so what it counts is ztext's own working set.
  phase("warmup");
  ztextWarmup();

  phase("install-counting-allocator");
  Counters counters;
  memset(&counters, 0, sizeof(counters));
  ZtextAllocator allocator;
  allocator.allocate = countingAllocate;
  allocator.reallocate = NULL;
  allocator.deallocate = countingDeallocate;
  allocator.user = &counters;
  CHECK_OK(ztextSetAllocator(&allocator));

  // A partial allocator must be refused without disturbing the working one.
  ZtextAllocator broken;
  memset(&broken, 0, sizeof(broken));
  CHECK(ztextSetAllocator(&broken) == ZTEXT_RESULT_INVALID_ARGUMENT,
        "an allocator with no allocate should be refused");

  phase("library+font");
  ZtextLibrary* library = NULL;
  CHECK_OK(ztextLibraryCreate(&library));

  ZtextFont* the_font = NULL;
  CHECK_OK(ztextFontCreateFromMemory(library, font, font_size, 0, &the_font));
  CHECK(ztextFontGlyphCount(the_font) > 0, "the font should have glyphs");
  CHECK(ztextFontUnitsPerEm(the_font) > 0, "units_per_em should be positive");

  // Variable axes, from C. This smoke test is handed ONE font path (see
  // build.zig) and it is a static one, so what is provable here is the half
  // that does not need an fvar table: that the four symbols link, that the
  // structs are a real C type a caller can declare on the stack, and that a
  // font with no axes says 0 and refuses the rest rather than answering with
  // whatever was already in the caller's struct. The Zig suite drives the
  // other half against a variable font.
  phase("variations");
  ZtextVariationAxis axis;
  memset(&axis, 0xFF, sizeof(axis));
  ZtextVariation wanted;
  wanted.tag = ZTEXT_TAG('w', 'g', 'h', 't');
  wanted.value = 700.0f;
  float current = -1.0f;

  CHECK(ztextFontAxisCount(the_font) == 0,
        "a static font should report 0 axes, got %u",
        ztextFontAxisCount(the_font));
  CHECK(ztextFontAxis(the_font, 0, &axis) == ZTEXT_RESULT_INVALID_ARGUMENT,
        "asking a static font for axis 0 should be refused");
  CHECK(axis.tag == 0u, "a refused axis query should clear its output");
  CHECK(ztextFontVariation(the_font, 0, &current) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "reading an axis of a static font should be refused");
  CHECK(ztextFontSetVariations(the_font, &wanted, 1) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "setting an axis on a static font should be refused, not ignored");
  CHECK(ztextFontSetVariations(the_font, NULL, 1) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "a NULL array with a non-zero count should be refused");
  CHECK(ztextFontAxisCount(NULL) == 0, "a NULL font should report 0 axes");

  // Named instances, on the same static font: none, and every call that takes
  // an index says so rather than describing an instance that is not there.
  float instance_coords[4];
  size_t instance_count = 4u;
  char instance_name[32];
  size_t instance_size = sizeof(instance_name);
  CHECK(ztextFontNamedInstanceCount(the_font) == 0,
        "a static font should report 0 named instances, got %u",
        ztextFontNamedInstanceCount(the_font));
  CHECK(ztextFontNamedInstanceCoords(the_font, 0, instance_coords,
                                     &instance_count) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "asking a static font for instance 0 should be refused");
  CHECK(instance_count == 0u, "a refused instance query should clear its count");
  CHECK(ztextFontNamedInstanceName(the_font, 0, instance_name,
                                   &instance_size) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "asking a static font for an instance name should be refused");
  CHECK(ztextFontSetNamedInstance(the_font, 0) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "setting an instance on a static font should be refused, not ignored");

  // Variation sequences: this font has no cmap format 14, so every pair is 0
  // while the base character on its own is not.
  CHECK(ztextFontGlyphIndex(the_font, 0x05D0u) != 0u,
        "the font should map ALEF");
  CHECK(ztextFontVariantGlyphIndex(the_font, 0x05D0u, 0xFE00u) == 0u,
        "a font with no format-14 cmap should name no variation sequence");

  // A fractional size through the C boundary, where the 26.6 conversion is.
  phase("faces");
  ZtextFace* fractional_face = NULL;
  CHECK_OK(ztextFaceCreate(the_font, 0, 24.5f, &fractional_face));
  ZtextFaceMetrics fractional;
  CHECK_OK(ztextFaceMetrics(fractional_face, &fractional));
  CHECK(fractional.pixel_size == 24.5f, "expected 24.5 px, got %f",
        (double)fractional.pixel_size);
  ztextFaceDestroy(fractional_face);

  ZtextFace* nothing = NULL;
  CHECK(ztextFaceCreate(the_font, 0, 0, &nothing) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "a zero size should be refused");
  CHECK(ztextFaceCreate(the_font, 0, -1.0f, &nothing) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "a negative size should be refused");
  CHECK(nothing == NULL, "a refused face should not write a handle");

  ZtextFace* face = NULL;
  CHECK_OK(ztextFaceCreate(the_font, 0, 24, &face));
  CHECK(ztextFaceFont(face) == the_font, "a face should name its own font");

  ZtextFaceMetrics metrics;
  CHECK_OK(ztextFaceMetrics(face, &metrics));
  CHECK(metrics.units_per_em > 0, "units_per_em should be positive");
  CHECK(metrics.num_glyphs > 0, "the face should have glyphs");
  CHECK(metrics.ascender > 0.0f, "ascender should be above the baseline");
  CHECK(metrics.descender < 0.0f, "descender should be below the baseline");
  // None of the committed fonts has a vmtx, so this is the synthesised path,
  // not the real one -- the flag has to say so rather than claim otherwise.
  CHECK(!metrics.has_vertical_metrics,
        "this font has no vhea/vmtx; vertical metrics should be synthesised");
  CHECK(metrics.vert_line_height > 0.0f,
        "even synthesised column spacing should be positive");
  printf("  face: %s %s, %u glyphs, %u upem\n", ztextFontFamilyName(the_font),
         ztextFontStyleName(the_font), metrics.num_glyphs,
         metrics.units_per_em);

  // OpenType metrics. The interesting check is the one Zig cannot write: a
  // ZtextMetric this build does not name, which only C can construct, has to
  // be refused rather than handed to HarfBuzz as a tag.
  float metric = -1.0f;
  CHECK_OK(ztextFaceMetric(face, ZTEXT_METRIC_X_HEIGHT, &metric));
  CHECK(metric > 0.0f, "this font declares an x-height; it should be positive");
  metric = -1.0f;
  CHECK(ztextFaceMetric(face, ZTEXT_METRIC_VERTICAL_ASCENDER, &metric) ==
            ZTEXT_RESULT_UNSUPPORTED,
        "a font with no vhea should report its vertical ascender unsupported");
  CHECK(metric == 0.0f, "an unsupported metric should clear its output");
  CHECK_OK(ztextFaceMetricWithFallback(face, ZTEXT_METRIC_VERTICAL_ASCENDER,
                                       &metric));
  CHECK(metric != 0.0f, "the fallback should always produce a value");
  metric = -1.0f;
  CHECK(ztextFaceMetric(face, (ZtextMetric)ZTEXT_TAG('n', 'o', 'p', 'e'),
                        &metric) == ZTEXT_RESULT_INVALID_ARGUMENT,
        "a tag this build does not name should be refused, not forwarded");
  CHECK(metric == 0.0f, "a refused metric should clear its output");
  CHECK(ztextFaceMetricWithFallback(face,
                                    (ZtextMetric)ZTEXT_TAG('n', 'o', 'p', 'e'),
                                    &metric) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "the fallback should refuse an unnamed tag too");

  // Shape a right-to-left run: the font passed in is Hebrew.
  phase("shape");
  ZtextShaper* shaper = NULL;
  CHECK_OK(ztextShaperCreate(&shaper));

  // shin lamed vav mem
  const char* shalom = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
  ZtextShapeParams params;
  memset(&params, 0, sizeof(params));
  params.direction = ZTEXT_DIRECTION_RTL;
  params.script = ZTEXT_TAG('H', 'e', 'b', 'r');
  CHECK_OK(ztextShaperShapeUtf8(shaper, face, shalom, strlen(shalom), 0,
                                strlen(shalom), &params));

  const size_t glyph_count = ztextShaperGlyphCount(shaper);
  const ZtextGlyph* glyphs = ztextShaperGlyphs(shaper);
  CHECK(glyph_count == 4, "expected 4 glyphs, got %zu", glyph_count);
  CHECK(glyphs != NULL, "glyph array should not be NULL after a shape");
  CHECK(ztextShaperDirection(shaper) == ZTEXT_DIRECTION_RTL,
        "the shaper should report the direction it used");
  if (glyphs != NULL && glyph_count == 4) {
    // Right-to-left output runs in visual order, so clusters descend.
    CHECK(glyphs[0].cluster == 6 && glyphs[3].cluster == 0,
          "RTL clusters should descend: got %u..%u", glyphs[0].cluster,
          glyphs[3].cluster);
    int saw_unsafe_to_concat = 0;
    for (size_t i = 0; i < glyph_count; i++) {
      CHECK(glyphs[i].glyph_id != 0, "glyph %zu is .notdef", i);
      CHECK(glyphs[i].x_advance > 0.0f, "glyph %zu has no advance", i);
      // Every bit set is a bit this header names, so a switch on the mask
      // cannot meet a value it has no meaning for.
      CHECK((glyphs[i].flags & ~(uint32_t)ZTEXT_GLYPH_FLAG_DEFINED) == 0u,
            "glyph %zu carries an undefined flag: 0x%x", i, glyphs[i].flags);
      if ((glyphs[i].flags & (uint32_t)ZTEXT_GLYPH_FLAG_UNSAFE_TO_CONCAT) !=
          0u) {
        saw_unsafe_to_concat = 1;
      }
    }
    // The optional flags reach a C consumer too, not only the Zig wrapper:
    // HarfBuzz withholds unsafe-to-concat unless ztext asks for it on every
    // shape, and a withheld flag is indistinguishable from an absent one.
    CHECK(saw_unsafe_to_concat,
          "no glyph reported unsafe-to-concat; the optional glyph flags are "
          "not being produced");
  }

  ZtextExtents extents;
  CHECK_OK(ztextShaperExtents(shaper, face, &extents));
  CHECK(extents.x_advance > 0.0f, "a shaped run should advance the pen");
  CHECK(extents.x_max > extents.x_min, "extents should enclose some ink");

  phase("raster");
  // Rasterise.
  if (glyphs != NULL && glyph_count > 0) {
    ZtextGlyphBitmap bitmap;
    CHECK_OK(ztextFaceRenderGlyph(face, glyphs[0].glyph_id,
                                  ZTEXT_RENDER_MODE_A8, ZTEXT_HINTING_NORMAL,
                                  0, 0, &bitmap));
    CHECK(bitmap.width > 0 && bitmap.height > 0,
          "a letter should rasterise to a non-empty bitmap");
    CHECK(bitmap.pixels != NULL, "a non-empty bitmap needs pixels");
    // The bytes say what they are. A8 coverage and an SDF are both one byte
    // per pixel, so a consumer that remembers the wrong mode gets a picture
    // rather than an error.
    CHECK(bitmap.format == ZTEXT_BITMAP_FORMAT_A8,
          "an A8 render should report A8, got %d", (int)bitmap.format);

    ZtextGlyphBitmap field;
    CHECK_OK(ztextFaceRenderGlyph(face, glyphs[0].glyph_id,
                                  ZTEXT_RENDER_MODE_SDF, ZTEXT_HINTING_NONE, 0,
                                  0, &field));
    CHECK(field.format == ZTEXT_BITMAP_FORMAT_SDF,
          "an SDF render should report SDF, got %d", (int)field.format);

    // Light hinting, which is the AUTOHINTER and nothing else for a
    // TrueType face: FT_LOAD_TARGET_LIGHT falls through to it unless the
    // driver hints lightly itself, and only FreeType's CFF driver does. It
    // belongs in this function specifically because this function is the one
    // that counts every byte in and out: the autohinter's coverage pass builds
    // a HarfBuzz buffer and interns the host locale's language tag, which is a
    // process-lifetime allocation like the other four ztextWarmup touches.
    // Without a light render here, that byte count never met the path that
    // makes it.
    ZtextGlyphBitmap hinted;
    CHECK_OK(ztextFaceRenderGlyph(face, glyphs[0].glyph_id,
                                  ZTEXT_RENDER_MODE_A8, ZTEXT_HINTING_LIGHT, 0,
                                  0, &hinted));
    CHECK(hinted.width > 0 && hinted.height > 0,
          "a light-hinted letter should rasterise to a non-empty bitmap");

    // Subpixel offset: a half-pixel shift must not crash and must still
    // rasterise to ink, exercising ztextFaceRenderGlyph's new parameters from
    // C directly rather than only through the Zig wrapper.
    ZtextGlyphBitmap shifted;
    CHECK_OK(ztextFaceRenderGlyph(face, glyphs[0].glyph_id,
                                  ZTEXT_RENDER_MODE_A8, ZTEXT_HINTING_NORMAL,
                                  32, 0, &shifted));
    CHECK(shifted.width > 0 && shifted.height > 0,
          "an offset render should still produce a non-empty bitmap");
  }

  phase("outline");
  // Outline decomposition: a letter's outline has at least one contour, and
  // every contour opened by move_to is closed exactly once.
  if (glyphs != NULL && glyph_count > 0) {
    OutlineCounts counts;
    memset(&counts, 0, sizeof(counts));
    ZtextOutlineFuncs outline_funcs = {countMoveTo,  countLineTo, countConicTo,
                                       countCubicTo, countClose,  &counts};
    CHECK_OK(ztextFaceDecomposeOutline(face, glyphs[0].glyph_id,
                                       ZTEXT_HINTING_NONE, &outline_funcs));
    CHECK(counts.move_to > 0, "a letter's outline should have a contour");
    CHECK(counts.move_to == counts.close,
          "every contour opened should be closed exactly once");
  }

  phase("synthetic");
  // Synthetic bold widens the advance; synthetic oblique moves the ink
  // without touching it. Reset each afterwards so nothing below inherits it.
  if (glyphs != NULL && glyph_count > 0) {
    ZtextExtents plain;
    CHECK_OK(ztextFaceGlyphExtents(face, glyphs[0].glyph_id,
                                   ZTEXT_HINTING_NONE, &plain));

    CHECK_OK(ztextFaceSetSyntheticBold(face, 1));
    ZtextExtents bold;
    CHECK_OK(ztextFaceGlyphExtents(face, glyphs[0].glyph_id,
                                   ZTEXT_HINTING_NONE, &bold));
    CHECK(bold.x_advance > plain.x_advance,
          "synthetic bold should widen the advance");
    CHECK_OK(ztextFaceSetSyntheticBold(face, 0));

    CHECK_OK(ztextFaceSetSyntheticOblique(face, 1));
    ZtextExtents sheared;
    CHECK_OK(ztextFaceGlyphExtents(face, glyphs[0].glyph_id,
                                   ZTEXT_HINTING_NONE, &sheared));
    CHECK(sheared.x_advance == plain.x_advance,
          "a shear should not change the advance");
    CHECK(sheared.x_max != plain.x_max || sheared.x_min != plain.x_min,
          "a shear should move the ink");
    CHECK_OK(ztextFaceSetSyntheticOblique(face, 0));
  }

  phase("bidi");
  // Bidi, with no face involved at all.
  const char* mixed = "a \xd7\xa9\xd7\x9c b";
  ZtextParagraph* paragraph = NULL;
  CHECK_OK(ztextParagraphCreateUtf8(mixed, strlen(mixed),
                                    ZTEXT_BASE_DIRECTION_AUTO, &paragraph));
  CHECK(ztextParagraphBaseLevel(paragraph) == 0,
        "a paragraph starting with Latin should resolve to an LTR base");
  CHECK(ztextParagraphVisualRunCount(paragraph) == 3,
        "expected 3 visual runs, got %zu",
        ztextParagraphVisualRunCount(paragraph));
  CHECK(ztextParagraphScriptRunCount(paragraph) >= 2,
        "expected at least 2 script runs");

  // And one line of it, which is where rules L1 and L2 are actually applied.
  ZtextLine* line = NULL;
  const size_t whole = ztextParagraphLength(paragraph);
  CHECK_OK(ztextLineCreate(paragraph, 0, whole, &line));
  CHECK(ztextLineOffset(line) == 0, "line offset should be as given");
  CHECK(ztextLineVisualRunCount(line) ==
            ztextParagraphVisualRunCount(paragraph),
        "a line spanning the paragraph should agree with it");
  CHECK(ztextLineShapingRuns(line) != NULL, "a non-empty line needs runs");
  ztextLineDestroy(line);

  // Into a fresh handle: a refused create writes NULL to its out-parameter,
  // so reusing `line` here would drop the one above on the floor -- which is
  // what the leak count at the end of this function is for.
  ZtextLine* rejected = NULL;
  CHECK(ztextLineCreate(paragraph, 0, whole + 1, &rejected) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "a line past the end of the paragraph should be refused");
  CHECK(rejected == NULL, "a refused line should not write a handle");

  ztextParagraphDestroy(paragraph);

  phase("malformed");
  // Malformed input must be refused rather than substituted.
  const char bad_utf8[] = {(char)0xC3, (char)0x28, 0};
  CHECK(ztextShaperShapeUtf8(shaper, face, bad_utf8, 2, 0, 2, &params) ==
            ZTEXT_RESULT_INVALID_UTF8,
        "malformed UTF-8 should be refused by the shaper");
  CHECK(ztextParagraphCreateUtf8(bad_utf8, 2, ZTEXT_BASE_DIRECTION_AUTO,
                                 &paragraph) == ZTEXT_RESULT_INVALID_UTF8,
        "malformed UTF-8 should be refused by the bidi analyser");

  phase("count-faces");
  // Face counting: a plain TTF has exactly one face.
  uint32_t face_count = 0;
  CHECK_OK(ztextLibraryCountFaces(library, font, font_size, &face_count));
  CHECK(face_count == 1, "expected 1 face in a plain TTF, got %u", face_count);
  CHECK(ztextLibraryCountFaces(library, font, 0, &face_count) ==
            ZTEXT_RESULT_INVALID_ARGUMENT,
        "counting faces in an empty buffer should be refused");

  // The SDF spread is validated rather than clamped.
  CHECK_OK(ztextLibrarySetSdfSpread(library, 8));
  CHECK(ztextLibrarySetSdfSpread(library, 1) == ZTEXT_RESULT_INVALID_ARGUMENT,
        "a spread below FreeType's range should be refused");
  CHECK(ztextLibrarySetSdfSpread(library, 33) == ZTEXT_RESULT_INVALID_ARGUMENT,
        "a spread above FreeType's range should be refused");

  phase("null-destructors");
  // NULL handles are tolerated by every destructor.
  ztextShaperDestroy(NULL);
  ztextFaceDestroy(NULL);
  ztextFontDestroy(NULL);
  ztextLineDestroy(NULL);
  ztextLibraryDestroy(NULL);
  ztextParagraphDestroy(NULL);

  ztextShaperDestroy(shaper);
  ztextFaceDestroy(face);
  ztextFontDestroy(the_font);
  ztextLibraryDestroy(library);

  phase("main-teardown");
  printf("  allocations: %zu total, %zu live, %zu bytes live\n",
         counters.total_allocations, counters.live_blocks, counters.live_bytes);
  CHECK(counters.total_allocations > 0,
        "the installed allocator was never called -- the seam is not in use");
  CHECK(counters.live_blocks == 0, "%zu blocks leaked", counters.live_blocks);
  CHECK(counters.live_bytes == 0, "%zu bytes leaked", counters.live_bytes);

  ztextSetAllocator(NULL);

  // These install allocators of their own, so they come after the balance
  // check above rather than inside it.
  phase("per-library");
  if (checkPerLibraryAllocator(font, font_size) != 0) failures++;
  phase("steady-state");
  if (checkSteadyStateAllocations(font, font_size) != 0) failures++;

  // Injection sweeps last: they walk a failure point across the whole
  // pipeline. The POISONED arm runs first, deliberately. Anything that reads
  // memory before writing it faults there on every run, and in the plain arm
  // on roughly one run in fifty -- so running the deterministic arm first is
  // what makes a fault reproducible at the point it is reported, instead of
  // landing wherever the previous contents of the heap happened to send it.
  phase("injection-sweep-poisoned");
  if (runInjectionSweep(font, font_size, 1) != 0) failures++;

  // The same sweep over an allocator that returns malloc's leftovers, which
  // is what a host's allocator normally hands back.
  phase("injection-sweep");
  if (runInjectionSweep(font, font_size, 0) != 0) failures++;
  ztextSetAllocator(NULL);

  phase("done");
  free(font);

  if (failures != 0) {
    printf("c smoke: %d FAILED\n", failures);
    return 1;
  }
  printf("c smoke: ok\n");
  return 0;
}
