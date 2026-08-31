const std = @import("std");

/// Builds ztext the way a real consumer does, which is not the way its own
/// test suite does.
///
/// This exists because the two are genuinely different code paths and the
/// difference has already bitten once: every artifact used to be registered
/// behind `if (b.pkg_hash.len == 0)`, so `dep.artifact("harfbuzz")` panicked
/// for anyone who took ztext as a dependency, while every in-repo test passed.
/// A README promise that nothing exercises is a README promise that breaks.
pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const ztext = b.dependency("ztext", .{ .target = target, .optimize = optimize });

    // 1. The Zig module.
    const zig_consumer = b.addExecutable(.{
        .name = "zig-consumer",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "ztext", .module = ztext.module("ztext") },
                // The suite's own font module, reached by path rather than by
                // copying a font in here. A second copy of an OFL face would
                // have to carry its own licence text and its own hash entry,
                // and would be one more thing to keep in step.
                .{ .name = "fonts", .module = b.createModule(.{
                    .root_source_file = b.path("../fonts.zig"),
                    .target = target,
                    .optimize = optimize,
                }) },
            },
        }),
    });

    // 2. The upstream libraries as artifacts, with their headers, which is
    //    what the README tells a C or C++ host it can do.
    const c_consumer = b.addExecutable(.{
        .name = "c-consumer",
        .root_module = b.createModule(.{ .target = target, .optimize = optimize }),
    });
    c_consumer.root_module.link_libc = true;
    c_consumer.root_module.addCSourceFile(.{
        .file = b.path("src/main.c"),
        .flags = &.{"-std=c11"},
    });
    //    Every artifact ztext installs is named here, and that is the point
    //    of the step: `dependency.artifact(name)` PANICS on a name the
    //    dependency does not register, so this is the only thing in the repo
    //    that can prove all five spellings resolve. libunibreak was missing
    //    from this list for as long as it has been vendored -- the same hole
    //    the doc comment above describes, one library over.
    //
    //    Its artifact is "unibreak", not "libunibreak": Zig prefixes `lib` on
    //    the platforms that use one, so `.name = "libunibreak"` would install
    //    liblibunibreak.a. The name here is upstream's own library name, and
    //    `src/pins.zig` calls the PROJECT libunibreak, which is also
    //    upstream's. `ci/measurements.sh --check` compares this list against
    //    build.zig's rather than against the pins, so the two namespaces
    //    cannot be confused for one.
    c_consumer.root_module.linkLibrary(ztext.artifact("ztext"));
    c_consumer.root_module.linkLibrary(ztext.artifact("harfbuzz"));
    c_consumer.root_module.linkLibrary(ztext.artifact("freetype"));
    c_consumer.root_module.linkLibrary(ztext.artifact("sheenbidi"));
    c_consumer.root_module.linkLibrary(ztext.artifact("unibreak"));

    const step = b.step("run", "Build and run both consumers");
    step.dependOn(&b.addRunArtifact(zig_consumer).step);
    step.dependOn(&b.addRunArtifact(c_consumer).step);
    b.getInstallStep().dependOn(step);
}
