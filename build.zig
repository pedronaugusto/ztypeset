const std = @import("std");

//=============================================================================
// Vendored upstream sources.
//
// Every list below is explicit. A directory glob would silently start
// compiling whatever the next re-vendor drops in, and would just as silently
// stop compiling a file upstream renamed; both are the kind of change that
// should be a decision, not a side effect. The cost is that adding a file is a
// deliberate edit here, which is the point.
//=============================================================================

/// FreeType, reduced to the modules ztypeset registers.
///
/// Kept in step with `ffi/ztypeset_ftmodules.h` by the linker: a module named
/// there whose sources are missing here is an undefined symbol. The `.c` files
/// named here are FreeType's own aggregate translation units -- `ftbase.c`
/// includes eighteen further sources, `smooth.c` two, `sdf.c` four -- which is
/// how upstream intends the library to be compiled outside its own makefiles.
const freetype_sources = [_][]const u8{
    // base -- objects, streams, outlines, the module registry
    "libs/freetype/src/base/ftbase.c",
    "libs/freetype/src/base/ftbbox.c",
    "libs/freetype/src/base/ftbitmap.c",
    "libs/freetype/src/base/ftdebug.c",
    "libs/freetype/src/base/ftinit.c",
    "libs/freetype/src/base/ftmm.c",
    "libs/freetype/src/base/ftsystem.c",
    // base extensions -- separate translation units upstream, each the
    // implementation of entry points a header installed below declares.
    // ci/header-link.sh is what decides this list: it references every
    // declared entry point and makes the linker resolve it, so a header
    // installed without its implementation fails rather than reaching a
    // consumer. All three arrived that way.
    "libs/freetype/src/base/ftfstype.c",
    "libs/freetype/src/base/ftglyph.c",
    "libs/freetype/src/base/ftpatent.c",
    "libs/freetype/src/base/ftstroke.c",
    // hinting
    "libs/freetype/src/autofit/autofit.c",
    "libs/freetype/src/pshinter/pshinter.c",
    // font formats: OpenType's two outline flavours and their shared container
    "libs/freetype/src/truetype/truetype.c",
    "libs/freetype/src/cff/cff.c",
    "libs/freetype/src/sfnt/sfnt.c",
    "libs/freetype/src/psaux/psaux.c",
    "libs/freetype/src/psnames/psnames.c",
    // rasterisers: A8 coverage, and FreeType's native SDF renderers
    "libs/freetype/src/smooth/smooth.c",
    "libs/freetype/src/sdf/sdf.c",
};

/// FreeType's public headers, for consumers that link the artifact directly.
///
/// Explicit for the same reason the source lists are: `installHeadersDirectory`
/// is recursive, and pointing it at `include/freetype` would also publish
/// `internal/`, which upstream does not install. A header upstream adds is not
/// published until it is added here, which is a visible omission rather than a
/// silent one.
/// Only headers whose implementation is in `freetype_sources`. A header for a
/// module ztypeset does not compile would compile fine for a consumer and then
/// fail at link with an undefined symbol -- `ftsynth.h` is the standing
/// example: `FT_GlyphSlot_Embolden` is declared there, `ftsynth.c` is not
/// compiled here, and the header is therefore not installed.
const freetype_public_headers = [_][]const u8{
    "freetype.h",
    "ftadvanc.h",
    "ftbbox.h",
    "ftbitmap.h",
    "ftchapters.h",
    "ftcolor.h",
    "ftdriver.h",
    "fterrdef.h",
    "fterrors.h",
    "ftfntfmt.h",
    "ftglyph.h",
    "ftimage.h",
    "ftincrem.h",
    "ftlcdfil.h",
    "ftlist.h",
    "ftlogging.h",
    "ftmm.h",
    "ftmodapi.h",
    "ftmoderr.h",
    "ftoutln.h",
    "ftparams.h",
    "ftrender.h",
    "ftsizes.h",
    "ftsnames.h",
    "ftstroke.h",
    "ftsystem.h",
    "fttrigon.h",
    "fttypes.h",
    "ttnameid.h",
    "tttables.h",
    "tttags.h",
};

/// HarfBuzz.
///
/// This is upstream's own `src/harfbuzz.cc` manifest minus the platform shaper
/// integrations (CoreText, DirectWrite, GDI, Uniscribe, GLib, Graphite2,
/// WASM). Those compile to nothing without their `HAVE_*` macros, so excluding
/// them changes no behaviour -- it just means this list describes what is
/// actually built.
///
/// Deliberately NOT compiled as `harfbuzz.cc` itself. That amalgam is a single
/// translation unit whose contents upstream can change, which would let a
/// re-vendor silently alter what compiles. Listed individually, a translation
/// unit that appears or disappears is a link error.
const harfbuzz_sources = [_][]const u8{
    "libs/harfbuzz/src/OT/Var/VARC/VARC.cc",
    "libs/harfbuzz/src/hb-aat-layout.cc",
    "libs/harfbuzz/src/hb-aat-map.cc",
    "libs/harfbuzz/src/hb-blob.cc",
    "libs/harfbuzz/src/hb-buffer-serialize.cc",
    "libs/harfbuzz/src/hb-buffer-verify.cc",
    "libs/harfbuzz/src/hb-buffer.cc",
    "libs/harfbuzz/src/hb-common.cc",
    "libs/harfbuzz/src/hb-draw.cc",
    "libs/harfbuzz/src/hb-face-builder.cc",
    "libs/harfbuzz/src/hb-face.cc",
    "libs/harfbuzz/src/hb-fallback-shape.cc",
    "libs/harfbuzz/src/hb-font.cc",
    "libs/harfbuzz/src/hb-ft.cc",
    "libs/harfbuzz/src/hb-map.cc",
    "libs/harfbuzz/src/hb-number.cc",
    "libs/harfbuzz/src/hb-ot-cff1-table.cc",
    "libs/harfbuzz/src/hb-ot-cff2-table.cc",
    "libs/harfbuzz/src/hb-ot-color.cc",
    "libs/harfbuzz/src/hb-ot-face.cc",
    "libs/harfbuzz/src/hb-ot-fetch.cc",
    "libs/harfbuzz/src/hb-ot-font.cc",
    "libs/harfbuzz/src/hb-ot-layout.cc",
    "libs/harfbuzz/src/hb-ot-map.cc",
    "libs/harfbuzz/src/hb-ot-math.cc",
    "libs/harfbuzz/src/hb-ot-meta.cc",
    "libs/harfbuzz/src/hb-ot-metrics.cc",
    "libs/harfbuzz/src/hb-ot-name.cc",
    "libs/harfbuzz/src/hb-ot-shape-fallback.cc",
    "libs/harfbuzz/src/hb-ot-shape-normalize.cc",
    "libs/harfbuzz/src/hb-ot-shape.cc",
    "libs/harfbuzz/src/hb-ot-shaper-arabic.cc",
    "libs/harfbuzz/src/hb-ot-shaper-default.cc",
    "libs/harfbuzz/src/hb-ot-shaper-hangul.cc",
    "libs/harfbuzz/src/hb-ot-shaper-hebrew.cc",
    "libs/harfbuzz/src/hb-ot-shaper-indic-table.cc",
    "libs/harfbuzz/src/hb-ot-shaper-indic.cc",
    "libs/harfbuzz/src/hb-ot-shaper-khmer.cc",
    "libs/harfbuzz/src/hb-ot-shaper-myanmar.cc",
    "libs/harfbuzz/src/hb-ot-shaper-syllabic.cc",
    "libs/harfbuzz/src/hb-ot-shaper-thai.cc",
    "libs/harfbuzz/src/hb-ot-shaper-use.cc",
    "libs/harfbuzz/src/hb-ot-shaper-vowel-constraints.cc",
    "libs/harfbuzz/src/hb-ot-tag.cc",
    "libs/harfbuzz/src/hb-ot-var.cc",
    "libs/harfbuzz/src/hb-outline.cc",
    "libs/harfbuzz/src/hb-paint-bounded.cc",
    "libs/harfbuzz/src/hb-paint-extents.cc",
    "libs/harfbuzz/src/hb-paint.cc",
    "libs/harfbuzz/src/hb-set.cc",
    "libs/harfbuzz/src/hb-shape-plan.cc",
    "libs/harfbuzz/src/hb-shape.cc",
    "libs/harfbuzz/src/hb-shaper.cc",
    "libs/harfbuzz/src/hb-static.cc",
    "libs/harfbuzz/src/hb-style.cc",
    "libs/harfbuzz/src/hb-ucd.cc",
    "libs/harfbuzz/src/hb-unicode.cc",
};

/// HarfBuzz's public headers, restricted to those whose translation units are
/// in `harfbuzz_sources`. Upstream ships headers for every integration it can
/// be configured with -- subsetting, Cairo, CoreText, DirectWrite, ICU,
/// Graphite2, WASM -- and installing them advertises an API that compiles and
/// then fails at link.
///
/// Upstream's `src/meson.build` does not build one library. It builds
/// `libharfbuzz` and then four more beside it, each behind its own option and
/// its own external dependencies: `libharfbuzz-subset` (`hb_subset_sources`),
/// `-raster` (`hb_raster_sources`, libpng), `-vector` (`hb_vector_sources`,
/// zlib) and `-gpu` (`hb_gpu_sources`, plus generated shader sources). ztypeset
/// vendors `libharfbuzz`. So `hb-subset-depend.h`, `hb-raster.h`,
/// `hb-vector.h` and `hb-gpu.h` are NOT installed here -- they are the public
/// faces of libraries this package does not build, and each was installed with
/// nothing behind it until ci/header-link.sh said so.
///
/// hb-subset-depend.h could not even be compiled: it opens with
/// `#error "Include <hb-subset.h> instead."`, and hb-subset.h belongs to the
/// subset library. The other three compile and then fail at link.
///
/// This is upstream's own division, not a ceiling ztypeset invented. Building any
/// of the four is a decision about a dependency (libpng, zlib, a shader
/// pipeline), and it is made by adding that library's sources here and its
/// header below -- at which point the gate proves the two agree.
const harfbuzz_public_headers = [_][]const u8{
    "hb-aat-layout.h",
    "hb-aat.h",
    "hb-blob.h",
    "hb-buffer.h",
    "hb-common.h",
    "hb-deprecated.h",
    "hb-draw.h",
    "hb-face.h",
    "hb-font.h",
    "hb-ft.h",
    "hb-map.h",
    "hb-ot-color.h",
    "hb-ot-deprecated.h",
    "hb-ot-fetch.h",
    "hb-ot-font.h",
    "hb-ot-layout.h",
    "hb-ot-math.h",
    "hb-ot-meta.h",
    "hb-ot-metrics.h",
    "hb-ot-name.h",
    "hb-ot-shape.h",
    "hb-ot-var.h",
    "hb-ot.h",
    "hb-paint.h",
    "hb-script-list.h",
    "hb-set.h",
    "hb-shape-plan.h",
    "hb-shape.h",
    "hb-style.h",
    "hb-unicode.h",
    "hb-version.h",
    "hb.h",
};

/// SheenBidi. Plain C99, no configuration, no generated sources.
///
/// `Source/SheenBidi.c` is upstream's unity wrapper and is skipped: it compiles
/// to nothing unless `SB_CONFIG_UNITY` is defined, and the files it would
/// include are all listed here individually.
/// libunibreak: UAX #14 line breaking and UAX #29 grapheme and word
/// segmentation. Explicit, like every other list here -- upstream's `src/`
/// also holds table generators and its own test driver, and a glob would
/// compile both.
///
/// Note what is NOT here: an allocator seam. libunibreak allocates nothing at
/// all -- its tables are static and its results go into a buffer the caller
/// provides -- so unlike the other three there is nothing to route.
const libunibreak_sources = [_][]const u8{
    "libs/libunibreak/src/eastasianwidthdata.c",
    "libs/libunibreak/src/eastasianwidthdef.c",
    "libs/libunibreak/src/emojidata.c",
    "libs/libunibreak/src/emojidef.c",
    "libs/libunibreak/src/graphemebreak.c",
    "libs/libunibreak/src/graphemebreakdata.c",
    "libs/libunibreak/src/indicconjunctbreakdata.c",
    "libs/libunibreak/src/linebreak.c",
    "libs/libunibreak/src/linebreakdata.c",
    "libs/libunibreak/src/linebreakdef.c",
    "libs/libunibreak/src/unibreakbase.c",
    "libs/libunibreak/src/unibreakdef.c",
    "libs/libunibreak/src/wordbreak.c",
    "libs/libunibreak/src/wordbreakdata.c",
};

const sheenbidi_sources = [_][]const u8{
    "libs/sheenbidi/Source/API/SBAlgorithm.c",
    "libs/sheenbidi/Source/API/SBAllocator.c",
    "libs/sheenbidi/Source/API/SBAttributeList.c",
    "libs/sheenbidi/Source/API/SBAttributeRegistry.c",
    "libs/sheenbidi/Source/API/SBBase.c",
    "libs/sheenbidi/Source/API/SBCodepoint.c",
    "libs/sheenbidi/Source/API/SBCodepointSequence.c",
    "libs/sheenbidi/Source/API/SBLine.c",
    "libs/sheenbidi/Source/API/SBLog.c",
    "libs/sheenbidi/Source/API/SBMirrorLocator.c",
    "libs/sheenbidi/Source/API/SBParagraph.c",
    "libs/sheenbidi/Source/API/SBScriptLocator.c",
    "libs/sheenbidi/Source/API/SBText.c",
    "libs/sheenbidi/Source/API/SBTextConfig.c",
    "libs/sheenbidi/Source/API/SBTextIterators.c",
    "libs/sheenbidi/Source/Core/List.c",
    "libs/sheenbidi/Source/Core/Memory.c",
    "libs/sheenbidi/Source/Core/Object.c",
    "libs/sheenbidi/Source/Core/Once.c",
    "libs/sheenbidi/Source/Data/BidiTypeLookup.c",
    "libs/sheenbidi/Source/Data/GeneralCategoryLookup.c",
    "libs/sheenbidi/Source/Data/PairingLookup.c",
    "libs/sheenbidi/Source/Data/ScriptLookup.c",
    "libs/sheenbidi/Source/Script/ScriptStack.c",
    "libs/sheenbidi/Source/Text/AttributeDictionary.c",
    "libs/sheenbidi/Source/Text/AttributeManager.c",
    "libs/sheenbidi/Source/UBA/BidiChain.c",
    "libs/sheenbidi/Source/UBA/BracketQueue.c",
    "libs/sheenbidi/Source/UBA/IsolatingRun.c",
    "libs/sheenbidi/Source/UBA/LevelRun.c",
    "libs/sheenbidi/Source/UBA/RunQueue.c",
    "libs/sheenbidi/Source/UBA/StatusStack.c",
};

/// The ztypeset boundary. One translation unit per concern.
///
/// C, not C++: every upstream exposes a C API, so there is nothing here for
/// C++ to do. See README -- this layer exists to stop FreeType's large,
/// config-conditional structs at the C boundary, not to re-present HarfBuzz to
/// the world under a different name.
const ztypeset_sources = [_][]const u8{
    "ffi/ztypeset_core.c",
    "ffi/ztypeset_face.c",
    "ffi/ztypeset_shape.c",
    "ffi/ztypeset_bidi.c",
    "ffi/ztypeset_raster.c",
    "ffi/ztypeset_abi.c",
};

/// The three environment variables HarfBuzz reads, set to values that change
/// what it does. Every run they are applied to must produce exactly the result
/// it produces without them -- which is true only because `harfbuzz_defines`
/// passes -DHB_NO_GETENV, and false the moment someone removes it.
///
/// One home for the three names: two run steps need them, and a list with two
/// copies is a list that can disagree with itself.
fn setHostileEnvironment(run: *std.Build.Step.Run) void {
    // The fallback shaper: no OpenType layout at all.
    run.setEnvironmentVariable("HB_SHAPER_LIST", "fallback");
    // FreeType's font funcs instead of the OpenType ones. They allocate an
    // FT_Face of their own, outside ztypeset's allocator and outside its
    // lifetime.
    run.setEnvironmentVariable("HB_FONT_FUNCS", "ft");
    // A face loader that does not exist. Inert today because ztypeset never
    // opens a face by file name, and here so that it stays inert.
    run.setEnvironmentVariable("HB_FACE_LOADER", "no-such-loader");
}

//=============================================================================
// Compiler flags, as comptime fragments.
//
// Split this way because the C++ base differs by ABI and Zig needs the whole
// concatenation to be comptime-known.
//=============================================================================

/// Tells FreeType it is being compiled (rather than consumed), and points it at
/// ztypeset's configuration instead of the vendored defaults. Every translation
/// unit that includes a FreeType header while ztypeset's build is in effect needs
/// these, or it sees a different FreeType than the one that was compiled.
const freetype_defines = [_][]const u8{
    "-DFT2_BUILD_LIBRARY",
    "-DFT_CONFIG_OPTIONS_H=<ztypeset_ftoption.h>",
    "-DFT_CONFIG_MODULES_H=<ztypeset_ftmodules.h>",
};

/// Switches HarfBuzz onto ztypeset's allocator. Defining all four macros is what
/// makes HarfBuzz define HB_CUSTOM_MALLOC for itself; it then declares these
/// names `extern "C"` and ztypeset_core.c implements them. Naming them ztypeset_hb_*
/// rather than accepting HarfBuzz's default hb_*_impl keeps the symbols out of
/// a namespace a host might also be using.
const harfbuzz_defines = [_][]const u8{
    "-DHAVE_FREETYPE=1",
    // HarfBuzz's process-lifetime caches are freed from an atexit handler ONLY
    // when HAVE_ATEXIT is defined, and nothing here defines it: hb.hh:471-476
    // then sets HB_USE_ATEXIT to 0 and hb.hh:479 expands hb_atexit(f) to
    // `if (0) f()`. So in this build they are never freed at all.
    //
    // That was true before this line existed, by accident -- the absence of a
    // define. It is stated here so it is a DECISION, and so that anyone who
    // adds HAVE_ATEXIT has to delete this line and read why it was here.
    //
    // Freeing them at exit is not wanted. An atexit handler that calls the
    // installed allocator runs after a host has torn its own allocator down,
    // and the ordering between the two is not something a library can promise.
    // ztypesetWarmup() exists so a host can populate the caches before it starts
    // auditing instead; see ffi/ztypeset.h.
    "-DHB_NO_ATEXIT",
    // HarfBuzz reads three environment variables: HB_SHAPER_LIST
    // (hb-shaper.cc:48), HB_FONT_FUNCS (hb-font.cc:2599) and HB_FACE_LOADER
    // (hb-face.cc:371). Two of them change what ztypeset renders. Measured on
    // this tree before this define existed:
    //
    //   HB_SHAPER_LIST=fallback -- five golden tests fail. Standard ligatures
    //     stop applying and moving a variation axis stops moving the shaped
    //     result. The picture changes and nothing says so.
    //   HB_FONT_FUNCS=ft -- the C smoke test reports 216 bytes leaked, plus
    //     26 blocks under the injection sweep: hb-ft's font funcs open an
    //     FT_Face of their own from an FT_Library ztypeset does not own or free.
    //   HB_FACE_LOADER -- inert here, because ztypeset builds faces from memory
    //     and never from a file name. Covered anyway; it costs nothing and
    //     the next entry point that takes a path would make it live.
    //
    // HB_NO_GETENV makes getenv(Name) expand to nullptr (hb.hh:427-429), so
    // all three read empty and the defaults stand. ffi/ztypeset_face.c already
    // refused FreeType's FREETYPE_PROPERTIES for the same reason; this is the
    // other half of the same argument, and it was the half that was live.
    //
    // The gate is not this comment. build.zig runs the suite and the C smoke
    // test a second time with all three set to hostile values, and every
    // assertion in both has to hold.
    "-DHB_NO_GETENV",
    // The other environment-shaped input, and the one that is not an
    // environment variable at all. hb_language_get_default() answers with
    // hb_setlocale(LC_CTYPE, nullptr) (hb-common.cc:374), which is the C
    // library's locale rather than getenv -- so HB_NO_GETENV does not cover
    // it. It reaches this build through FreeType: with
    // FT_CONFIG_OPTION_USE_HARFBUZZ the autohinter shapes each script's
    // blue-zone strings and lets HarfBuzz guess their segment properties,
    // which asks for the default language. Nothing ztypeset shapes does --
    // ztypeset names a language on every buffer.
    //
    // Without this define the behaviour is already what is wanted, twice
    // over: hb.hh:493-495 turns HB_NO_SETLOCALE on whenever HAVE_NEWLOCALE or
    // HAVE_USELOCALE is missing and neither is defined here, so hb_setlocale
    // is the literal "C" (hb.hh:515); and a library that never calls
    // setlocale(LC_ALL, "") sees the C locale regardless of the machine's
    // settings anyway. Two accidents pointing the same way are still
    // accidents. Stated here so that adding HAVE_NEWLOCALE for something else
    // cannot quietly make hinting depend on the host's locale.
    //
    // Blind spot, stated because the harness cannot cover this one: no
    // mutation discriminates it on a host whose locale is already C, which is
    // every host that has not called setlocale. It is held by this define and
    // by the two conditions above, not by ci/check-guards.sh.
    "-DHB_NO_SETLOCALE",
    "-Dhb_malloc_impl=ztypeset_hb_malloc",
    "-Dhb_calloc_impl=ztypeset_hb_calloc",
    "-Dhb_realloc_impl=ztypeset_hb_realloc",
    "-Dhb_free_impl=ztypeset_hb_free",
};

/// Zig defaults the Windows ABI to gnu, so an `abi == .msvc` branch is dead in
/// every configuration nobody explicitly asked for -- it compiles for the
/// first time on whoever's machine finally targets MSVC. CI runs both.
///
/// Under the MSVC ABI the Microsoft standard library headers assume exceptions
/// are available, and switching them off through Clang flags is a well-known
/// way to produce header errors. The saving is a little code size; the cost
/// would be a toolchain-specific build failure, so the MSVC ABI keeps the
/// defaults.
const cxx_base_msvc = [_][]const u8{"-std=c++17"};
const cxx_base_other = [_][]const u8{ "-std=c++17", "-fno-exceptions", "-fno-rtti" };

const c_base = [_][]const u8{"-std=c11"};

/// Warnings, as errors, for the C ztypeset WROTE -- never for the C it vendors.
///
/// The two cannot share a flag list. libs/ is pristine upstream and stays
/// that way: FreeType, HarfBuzz, SheenBidi and libunibreak are compiled with
/// whatever their authors chose to leave warning, and turning ztypeset's
/// standards into build failures for their code would either break the build
/// or force a local patch, which is the one thing the vendor rules forbid.
///
/// So these apply to ffi/*.c and to the C test translation units, and to
/// nothing else. Without them ztypeset's own C was the only code in the
/// repository whose warnings nobody had to read: -Wall and -Wextra were never
/// passed, so an unused parameter, a signed/unsigned comparison or a missing
/// field initialiser in a ztypeset source compiled silently.
///
/// -Wno-unused-parameter is the one exception, and it is upstream's shape
/// rather than ztypeset's: a callback matching a FreeType or HarfBuzz function
/// pointer takes the parameters that signature has, whether or not this
/// implementation reads them, and every one of those would otherwise need a
/// (void) cast that says nothing.
const ztypeset_warnings = [_][]const u8{
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wno-unused-parameter",
};

/// Applied only to a shared build; see the comment at its use site.
const visibility_flags = [_][]const u8{ "-fvisibility=hidden", "-DZTYPESET_SHARED", "-DZTYPESET_BUILD" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const options = .{
        .shared = b.option(
            bool,
            "shared",
            "Build the C library as a shared object",
        ) orelse false,
        // Off by default, and deliberately NOT tied to `optimize`.
        //
        // Zig's full C sanitizer emits calls into a runtime that is linked
        // only into a compilation that is itself sanitized. Defaulting this on
        // in Debug means a consumer who writes `b.dependency("ztypeset", .{})` --
        // forgetting to forward `optimize`, the most common Zig packaging
        // mistake -- gets a Debug ztypeset inside a release executable and a link
        // failure reading `undefined symbol: __ubsan_handle_shift_out_of_bounds`,
        // which names nothing they can act on.
        //
        // ztypeset's own suite turns it on explicitly instead: ci/run.sh and CI
        // both pass -Dsanitize_c=true for the Debug runs. A library should not
        // decide that its consumers are running a sanitizer.
        .sanitize_c = b.option(
            bool,
            "sanitize_c",
            "Compile the C with Zig's undefined-behaviour sanitizer",
        ) orelse false,
    };

    // Every option that changes what the C compiles to is mirrored into a Zig
    // module, so the wrapper can never disagree with the library it links.
    const options_step = b.addOptions();
    inline for (std.meta.fields(@TypeOf(options))) |field| {
        options_step.addOption(field.type, field.name, @field(options, field.name));
    }
    const options_module = options_step.createModule();

    const sanitize: std.zig.SanitizeC = if (options.sanitize_c) .full else .off;
    const msvc = target.result.abi == .msvc;

    // A shared ztypeset must not re-export the upstreams it statically contains.
    // Hidden visibility costs nothing in a static build -- the static linker
    // ignores it -- but it is only needed for the shared one, so it is only
    // applied there and the default build's flags stay minimal.
    const shared_elf = options.shared and !msvc;

    const harfbuzz_flags: []const []const u8 = if (msvc)
        &(cxx_base_msvc ++ freetype_defines ++ harfbuzz_defines)
    else if (shared_elf)
        &(cxx_base_other ++ freetype_defines ++ harfbuzz_defines ++ visibility_flags)
    else
        &(cxx_base_other ++ freetype_defines ++ harfbuzz_defines);

    const c_flags: []const []const u8 = if (shared_elf)
        &(c_base ++ freetype_defines ++ visibility_flags)
    else
        &(c_base ++ freetype_defines);

    // The same flags plus ztypeset's own warning settings. Used for ffi/*.c and
    // for the C tests; never for anything under libs/.
    const ztypeset_c_flags: []const []const u8 = if (shared_elf)
        &(c_base ++ freetype_defines ++ visibility_flags ++ ztypeset_warnings)
    else
        &(c_base ++ freetype_defines ++ ztypeset_warnings);

    // A C test links the installed library and includes only ffi/ztypeset.h, so
    // it needs the warnings without any of FreeType's build-time defines.
    const c_test_flags: []const []const u8 = &(c_base ++ ztypeset_warnings);

    // SheenBidi wants no FreeType defines, but does want the visibility flag:
    // its SB_PUBLIC is empty outside Windows, so hiding actually takes effect
    // there. FreeType's does not -- it marks its public API
    // `visibility("default")` itself (config/public-macros.h:76), which
    // overrides -fvisibility=hidden, so a shared ztypeset still re-exports
    // FreeType's API. That is upstream's decision and not overridable without
    // editing the vendored tree; the README says so.
    const sheenbidi_flags: []const []const u8 = if (shared_elf)
        &([_][]const u8{"-std=c99"} ++ visibility_flags)
    else
        &[_][]const u8{"-std=c99"};

    //=====================================================================
    // FreeType
    //=====================================================================

    const freetype = b.addLibrary(.{
        .name = "freetype",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    freetype.root_module.link_libc = true;
    freetype.root_module.addIncludePath(b.path("libs/freetype/include"));
    // ztypeset_ftoption.h and ztypeset_ftmodules.h live here, not in libs/.
    freetype.root_module.addIncludePath(b.path("ffi"));
    freetype.root_module.addCSourceFiles(.{
        .files = &freetype_sources,
        .flags = c_flags,
    });
    freetype.root_module.sanitize_c = sanitize;
    // Public headers only. `installHeadersDirectory` is recursive and would
    // otherwise also install `freetype/internal/`, which upstream deliberately
    // does not install -- those headers are implementation-private and require
    // FT2_BUILD_LIBRARY to even compile.
    freetype.installHeader(b.path("libs/freetype/include/ft2build.h"), "ft2build.h");
    freetype.installHeadersDirectory(
        b.path("libs/freetype/include/freetype/config"),
        "freetype/config",
        .{ .include_extensions = &.{".h"} },
    );
    for (freetype_public_headers) |header| {
        freetype.installHeader(
            b.path(b.fmt("libs/freetype/include/freetype/{s}", .{header})),
            b.fmt("freetype/{s}", .{header}),
        );
    }

    //=====================================================================
    // SheenBidi
    //=====================================================================

    const sheenbidi = b.addLibrary(.{
        .name = "sheenbidi",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    sheenbidi.root_module.link_libc = true;
    sheenbidi.root_module.addIncludePath(b.path("libs/sheenbidi/Headers"));
    // Its own translation units include each other as <API/...>, <UBA/...>.
    sheenbidi.root_module.addIncludePath(b.path("libs/sheenbidi/Source"));
    sheenbidi.root_module.addCSourceFiles(.{
        .files = &sheenbidi_sources,
        .flags = sheenbidi_flags,
    });
    sheenbidi.root_module.sanitize_c = sanitize;
    sheenbidi.installHeadersDirectory(
        b.path("libs/sheenbidi/Headers"),
        "",
        .{ .include_extensions = &.{".h"} },
    );

    //=====================================================================
    // libunibreak
    //=====================================================================

    const libunibreak = b.addLibrary(.{
        .name = "unibreak",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    libunibreak.root_module.link_libc = true;
    libunibreak.root_module.addIncludePath(b.path("libs/libunibreak/src"));
    libunibreak.root_module.addCSourceFiles(.{
        .files = &libunibreak_sources,
        .flags = sheenbidi_flags,
    });
    libunibreak.root_module.sanitize_c = sanitize;
    // Only the three headers whose implementation is compiled, plus the base
    // types they need. The *def.h headers are upstream's internals.
    libunibreak.installHeadersDirectory(
        b.path("libs/libunibreak/src"),
        "",
        .{ .include_extensions = &.{
            "linebreak.h",
            "graphemebreak.h",
            "wordbreak.h",
            "unibreakbase.h",
        } },
    );

    //=====================================================================
    // HarfBuzz
    //=====================================================================

    const harfbuzz = b.addLibrary(.{
        .name = "harfbuzz",
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    harfbuzz.root_module.link_libc = true;
    if (!msvc) harfbuzz.root_module.link_libcpp = true;
    harfbuzz.root_module.addIncludePath(b.path("libs/harfbuzz/src"));
    harfbuzz.root_module.addIncludePath(b.path("libs/freetype/include"));
    harfbuzz.root_module.addIncludePath(b.path("ffi"));
    harfbuzz.root_module.addCSourceFiles(.{
        .files = &harfbuzz_sources,
        .flags = harfbuzz_flags,
    });
    harfbuzz.root_module.sanitize_c = sanitize;
    harfbuzz.root_module.linkLibrary(freetype);
    // At the include ROOT, not under a `harfbuzz/` subdirectory. Upstream
    // installs to `<prefix>/include/harfbuzz/` and has pkg-config put that
    // directory on the include path, so the spelling HarfBuzz documents and
    // every consumer writes is `#include <hb.h>`. There is no pkg-config here
    // to add the -I, so the headers go where that spelling resolves.
    for (harfbuzz_public_headers) |header| {
        harfbuzz.installHeader(
            b.path(b.fmt("libs/harfbuzz/src/{s}", .{header})),
            header,
        );
    }

    //=====================================================================
    // ztypeset
    //=====================================================================

    const lib = b.addLibrary(.{
        .name = "ztypeset",
        .linkage = if (options.shared) .dynamic else .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    lib.root_module.link_libc = true;
    if (!msvc) lib.root_module.link_libcpp = true;
    lib.root_module.addIncludePath(b.path("ffi"));
    lib.root_module.addIncludePath(b.path("libs/freetype/include"));
    lib.root_module.addIncludePath(b.path("libs/harfbuzz/src"));
    lib.root_module.addIncludePath(b.path("libs/sheenbidi/Headers"));
    lib.root_module.addIncludePath(b.path("libs/libunibreak/src"));
    if (options.shared and msvc) {
        lib.root_module.addCMacro("ZTYPESET_SHARED", "");
        lib.root_module.addCMacro("ZTYPESET_BUILD", "");
    }
    // Non-MSVC shared builds get the same two macros through visibility_flags,
    // where they turn ZTYPESET_API into an explicit default-visibility marker.
    lib.root_module.addCSourceFiles(.{
        .files = &ztypeset_sources,
        .flags = ztypeset_c_flags,
    });
    lib.root_module.sanitize_c = sanitize;
    lib.root_module.linkLibrary(freetype);
    lib.root_module.linkLibrary(harfbuzz);
    lib.root_module.linkLibrary(sheenbidi);
    lib.root_module.linkLibrary(libunibreak);
    lib.installHeader(b.path("ffi/ztypeset.h"), "ztypeset.h");

    //=====================================================================
    // The Zig module.
    //=====================================================================

    const module = b.addModule("ztypeset", .{
        .root_source_file = b.path("src/ztypeset.zig"),
        .target = target,
        .optimize = optimize,
        .imports = &.{
            .{ .name = "ztypeset_options", .module = options_module },
        },
    });
    // No include path: the wrapper hand-writes its externs against ztypeset.h
    // rather than @cImport-ing it, so nothing Zig-side compiles C.
    module.linkLibrary(lib);

    //=====================================================================
    // Tests
    //=====================================================================

    // The committed OFL fonts, embedded through a module rooted at tests/ so
    // @embedFile stays inside its own module directory.
    const fonts_module = b.createModule(.{
        .root_source_file = b.path("tests/fonts.zig"),
        .target = target,
        .optimize = optimize,
    });

    const tests = b.addTest(.{
        .name = "ztypeset-tests",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/ztypeset.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "ztypeset_options", .module = options_module },
                .{ .name = "fonts", .module = fonts_module },
            },
        }),
    });
    tests.root_module.linkLibrary(lib);

    // The ABI cross-check @cImport-s ffi/ztypeset.h. It is wired here, on the test
    // module, and deliberately NOT on the module above: the shipped module has
    // no include path and never runs translate-c.
    //
    // No macro wiring accompanies it, and that is a measured claim rather than
    // an omission. ffi/ztypeset.h includes only <stddef.h> and <stdint.h> and is
    // sensitive to exactly one macro -- ZTYPESET_SHARED, which changes the
    // ZTYPESET_API attribute and no type. Every FreeType and HarfBuzz
    // configuration macro reaches the implementation, never the installed
    // header, so a header preprocessed without them still describes what the
    // library ships. A package whose public header changed type widths with its
    // build options would have to forward those macros here; ztypeset's does not.
    tests.root_module.link_libc = true;
    tests.root_module.addIncludePath(b.path("ffi"));

    // The documented examples and the documents that quote them, as bytes the
    // suite can compare. src/example_test.zig is the one home for the rule;
    // these three names are how it reaches the files, since @embedFile cannot
    // leave its own module's directory.
    tests.root_module.addAnonymousImport("example_quickstart", .{
        .root_source_file = b.path("examples/quickstart.zig"),
    });
    tests.root_module.addAnonymousImport("example_readme", .{
        .root_source_file = b.path("README.md"),
    });
    tests.root_module.addAnonymousImport("example_module_doc", .{
        .root_source_file = b.path("src/ztypeset.zig"),
    });

    const test_step = b.step("test", "Run the ztypeset test suite");
    test_step.dependOn(&b.addRunArtifact(tests).step);

    // And again, in an environment engineered to change what HarfBuzz does.
    // -DHB_NO_GETENV (see `harfbuzz_defines`) is what makes these three inert;
    // without it, HB_SHAPER_LIST=fallback fails five golden tests here.
    //
    // A second run rather than the only run, so the clean arm stays clean and
    // the difference between them is the measurement. It costs one execution
    // of an already-built binary.
    const hostile_env = b.addRunArtifact(tests);
    setHostileEnvironment(hostile_env);
    test_step.dependOn(&hostile_env.step);

    // The C boundary on its own, with no Zig in the picture: the header is a
    // real C contract and the allocator seam is genuinely in use. It asserts
    // allocations balance, which no Zig-side test can prove about the C side.
    const c_smoke = b.addExecutable(.{
        .name = "ztypeset-c-smoke",
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    c_smoke.root_module.link_libc = true;
    // The same sanitizer setting as the libraries. Without it a trap in
    // the TEST -- a shift past the width, a signed overflow, a misaligned
    // load -- arrives as a bare SIGSEGV with the library name on it,
    // which is a whole class of wrong diagnosis for a fault that is not
    // in the library at all.
    c_smoke.root_module.sanitize_c = sanitize;
    c_smoke.root_module.addIncludePath(b.path("ffi"));
    c_smoke.root_module.addCSourceFile(.{
        .file = b.path("tests/c_smoke.c"),
        .flags = c_test_flags,
    });
    c_smoke.root_module.linkLibrary(lib);

    const run_c_smoke = b.addRunArtifact(c_smoke);
    // Passed as a path argument rather than embedded, so the C test stays
    // plain C with no generated array to regenerate.
    run_c_smoke.addFileArg(b.path("tests/fonts/NotoSansHebrew-Regular.ttf"));

    // The same binary under the same hostile environment. This is the arm that
    // catches HB_FONT_FUNCS: with the variable live, hb-ft's font funcs open
    // an FT_Face from an FT_Library ztypeset never frees, and the C boundary's
    // own byte accounting reports 216 bytes leaked.
    const run_c_smoke_hostile = b.addRunArtifact(c_smoke);
    setHostileEnvironment(run_c_smoke_hostile);
    run_c_smoke_hostile.addFileArg(
        b.path("tests/fonts/NotoSansHebrew-Regular.ttf"),
    );

    // The harness behind README's measurements, so those numbers can be
    // reproduced rather than taken on trust. Not part of `test`: timings are
    // not assertions, and a loaded machine would fail them.
    // The documented example, compiled and run. An example that no longer
    // compiles is a red build here rather than a reader's afternoon, and
    // src/example_test.zig is what keeps README.md and the module doc quoting
    // this file rather than paraphrasing it.
    const quickstart = b.addExecutable(.{
        .name = "ztypeset-quickstart",
        .root_module = b.createModule(.{
            .root_source_file = b.path("examples/quickstart.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "ztypeset", .module = module },
                .{ .name = "fonts", .module = fonts_module },
            },
        }),
    });
    test_step.dependOn(&b.addRunArtifact(quickstart).step);

    const bench = b.addExecutable(.{
        .name = "ztypeset-bench",
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    bench.root_module.link_libc = true;
    bench.root_module.sanitize_c = sanitize;
    bench.root_module.addIncludePath(b.path("ffi"));
    bench.root_module.addCSourceFile(.{
        .file = b.path("tests/bench.c"),
        .flags = c_test_flags,
    });
    bench.root_module.linkLibrary(lib);

    const run_bench = b.addRunArtifact(bench);
    run_bench.addFileArg(b.path("tests/fonts/NotoSans-Regular.ttf"));
    b.step("bench", "Measure shaping, rasterisation and per-size cost")
        .dependOn(&run_bench.step);

    // Every entry point, called with nothing. A sweep rather than a scenario:
    // c_smoke drives the library the way a consumer would, this drives it the
    // way nobody should. Its own translation unit so the two do not blur.
    const null_sweep = b.addExecutable(.{
        .name = "ztypeset-null-sweep",
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    null_sweep.root_module.link_libc = true;
    null_sweep.root_module.sanitize_c = sanitize;
    null_sweep.root_module.addIncludePath(b.path("ffi"));
    null_sweep.root_module.addCSourceFile(.{
        .file = b.path("tests/null_sweep.c"),
        .flags = c_test_flags,
    });
    null_sweep.root_module.linkLibrary(lib);

    const run_null_sweep = b.addRunArtifact(null_sweep);
    run_null_sweep.addFileArg(b.path("tests/fonts/NotoSansHebrew-Regular.ttf"));

    // The implementation-private contracts, exercised directly.
    //
    // ffi/ztypeset_internal.h declares helpers ztypeset.h never exposes, and until
    // this existed nothing could reach them: the Zig suite enters through
    // ztypeset.h and the other two C tests link the installed library, so an
    // internal precondition was checkable only by reading every caller --
    // and "no caller does that today" is not a property a header can promise
    // about tomorrow.
    //
    // The ffi units are compiled INTO it rather than linked from libztypeset,
    // because those symbols are deliberately unexported: a shared build hides
    // them behind -fvisibility=hidden and an MSVC DLL never declares them, so
    // a test that linked the library would run in the static arm alone --
    // which is the arm where the ABI matters least.
    const c_internal = b.addExecutable(.{
        .name = "ztypeset-internal",
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    c_internal.root_module.link_libc = true;
    if (!msvc) c_internal.root_module.link_libcpp = true;
    c_internal.root_module.sanitize_c = sanitize;
    c_internal.root_module.addIncludePath(b.path("ffi"));
    c_internal.root_module.addIncludePath(b.path("libs/freetype/include"));
    c_internal.root_module.addIncludePath(b.path("libs/harfbuzz/src"));
    c_internal.root_module.addIncludePath(b.path("libs/sheenbidi/Headers"));
    c_internal.root_module.addIncludePath(b.path("libs/libunibreak/src"));
    c_internal.root_module.addCSourceFiles(.{
        .files = &ztypeset_sources,
        .flags = ztypeset_c_flags,
    });
    c_internal.root_module.addCSourceFile(.{
        .file = b.path("tests/c_internal.c"),
        .flags = ztypeset_c_flags,
    });
    c_internal.root_module.linkLibrary(freetype);
    c_internal.root_module.linkLibrary(harfbuzz);
    c_internal.root_module.linkLibrary(sheenbidi);
    c_internal.root_module.linkLibrary(libunibreak);

    const run_c_internal = b.addRunArtifact(c_internal);

    const c_test_step = b.step("test-c", "Run the C-level smoke test");
    c_test_step.dependOn(&run_c_internal.step);
    c_test_step.dependOn(&run_c_smoke.step);
    c_test_step.dependOn(&run_c_smoke_hostile.step);
    c_test_step.dependOn(&run_null_sweep.step);
    test_step.dependOn(c_test_step);

    // The C test executables in a stable place, for a harness that has to
    // run one of them many times rather than once. ci/crash-loop.sh is the
    // reason this exists: an intermittent fault is not something
    // `zig build test` can measure, because one run of a fault that
    // happens one time in fifty is indistinguishable from no fault.
    //
    // A step of its own rather than b.installArtifact, so a consumer that
    // depends on ztypeset never finds three test binaries in its own prefix.
    const install_c_tests = b.step(
        "install-c-tests",
        "Install the C test executables into zig-out/bin for ci/crash-loop.sh",
    );
    install_c_tests.dependOn(&b.addInstallArtifact(c_smoke, .{}).step);
    install_c_tests.dependOn(&b.addInstallArtifact(null_sweep, .{}).step);
    install_c_tests.dependOn(&b.addInstallArtifact(c_internal, .{}).step);
    install_c_tests.dependOn(&b.addInstallArtifact(bench, .{}).step);

    // Registered unconditionally, including when ztypeset is consumed as a
    // dependency. `std.Build.Dependency.artifact` finds an artifact by
    // scanning the dependency's install step, so anything NOT installed here
    // is invisible to a consumer -- `dep.artifact("harfbuzz")` panics rather
    // than failing gracefully.
    //
    // This does not put ztypeset's libraries in a consumer's prefix: a
    // dependency's install step only runs when something the consumer builds
    // actually depends on it.
    b.installArtifact(lib);
    b.installArtifact(freetype);
    b.installArtifact(harfbuzz);
    b.installArtifact(sheenbidi);
    b.installArtifact(libunibreak);
}
