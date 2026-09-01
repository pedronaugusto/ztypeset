#!/usr/bin/env bash
#
# ztypeset -- every installed header compiles, is reachable, and links.
#
# The rule this enforces: a header in the install prefix is a promise. A
# consumer who includes it must be able to compile it, and every entry point it
# declares must resolve against the libraries installed beside it. ztypeset builds
# a REDUCED configuration of four upstreams -- FreeType without several base
# extensions, HarfBuzz without its subset, raster, vector and GPU libraries --
# and upstream ships headers for all of it. Installing a header whose
# implementation was not compiled advertises an API that compiles for the
# consumer and then fails at link, which is the worst place to find out.
#
# The defect that named this gate: four HarfBuzz headers (hb-gpu.h,
# hb-raster.h, hb-subset-depend.h, hb-vector.h) were installed with no compiled
# translation unit behind them, and one FreeType header (ftrender.h) included a
# header that was not installed at all, so it could not even be compiled. A
# comment in build.zig said the lists were "restricted to those whose
# translation units are in <sources>". Prose does not hold a rule. This does.
#
# Usage: ci/header-link.sh [--target=<zig target triple>]
# Exit:  0 when all three checks pass.
#
# The three checks, in order:
#   1. COMPILES. Every root header below is included, in one translation unit,
#      in the documented order, and that unit must compile.
#   2. REACHED. The preprocessor's own dependency list must name every
#      installed header. Anything installed that no root reaches is dead
#      advertising -- unless it is in `unreached_by_design` with a reason, and
#      an entry there that IS reached fails too, so the list cannot rot.
#   3. LINKS. Every function and variable those headers declare is referenced
#      by a generated probe, which is linked against the installed libraries
#      and run. An undefined symbol fails the link.
#
# Blind spots, stated:
#   - Check 3 filters the translated declaration list down to names that appear
#     textually somewhere in the install prefix. A libc name that also appears
#     in an upstream header is therefore referenced too; that is harmless (it
#     resolves against libc) and errs towards checking more, not less.
#   - It proves a symbol is DEFINED, not that it works. A stub that returns
#     failure -- HarfBuzz's PNG entry points without HAVE_PNG, for instance --
#     passes this gate. That is upstream's own runtime contract, not a lie
#     about the surface.
#   - `zig translate-c` is what enumerates the declarations, so a declaration
#     it cannot translate is skipped. Those become `@compileError` entries,
#     which are counted and printed; a non-zero count is information rather
#     than a failure, because untranslatable macros land there too.
#   - One target per run. Both ABIs are covered by running it twice, which
#     ci/run.sh and the hosted workflow do.

set -uo pipefail
cd "$(dirname "$0")/.."

target=
cc_target=()
build_target=()
for arg in "$@"; do
  case "$arg" in
    --target=*)
      target=${arg#--target=}
      cc_target=(-target "$target")
      build_target=("-Dtarget=$target")
      ;;
    *)
      printf 'usage: ci/header-link.sh [--target=<zig target>]\n' >&2
      exit 2
      ;;
  esac
done

RED=$'\033[31m'
GREEN=$'\033[32m'
DIM=$'\033[2m'
OFF=$'\033[0m'
if [ ! -t 1 ]; then RED=; GREEN=; DIM=; OFF=; fi

work=$(mktemp -d 2>/dev/null || mktemp -d -t ztypeset-hdr)
prefix="$work/prefix"
trap 'rm -rf "$work"' EXIT INT TERM

#-----------------------------------------------------------------------------
# The root headers: the ones a consumer includes directly.
#
# Not every installed header is a root, and the two families disagree about
# which are: FreeType's public headers are each includable on their own after
# <ft2build.h>, while HarfBuzz's refuse to compile unless reached through an
# umbrella -- hb-blob.h says `#error "Include <hb.h> instead."` in as many
# words. Check 2 is what proves the non-roots are still reached, so this list
# stays short and does not have to be complete.
#-----------------------------------------------------------------------------
roots_head=(
  ft2build.h
  freetype/freetype.h
)
roots_tail=(
  hb.h
  hb-ot.h
  hb-aat.h
  hb-ft.h
  SheenBidi/SheenBidi.h
  linebreak.h
  graphemebreak.h
  wordbreak.h
  ztypeset.h
)

# Installed headers that no root reaches, and why that is correct. An entry
# here that turns out to BE reached fails the gate, so the list cannot outlive
# its reason.
unreached_by_design=(
  "freetype/config/ftmodule.h|upstream's default module list, which ztypeset replaces at build time with ffi/ztypeset_ftmodules.h; installed because upstream installs it"
  "freetype/ftchapters.h|documentation structure for FreeType's own doc generator; it declares nothing"
  "SheenBidi/SBConfig.h|SheenBidi's build configuration, read only by its internal Source/API/SBBase.h; build.zig defines no SB_CONFIG_* macro, so the installed file is the configuration the library was built with"
)

#-----------------------------------------------------------------------------
# 1. Install, then generate the translation unit.
#-----------------------------------------------------------------------------
printf '%sinstalling%s into a temporary prefix\n' "$DIM" "$OFF"
if ! zig build ${build_target[@]+"${build_target[@]}"} --prefix "$prefix" \
     > "$work/build.log" 2>&1; then
  cat "$work/build.log" >&2
  printf '%sthe build failed; nothing to check%s\n' "$RED" "$OFF" >&2
  exit 1
fi

inc="$prefix/include"
[ -d "$inc" ] || {
  printf '%sno headers were installed%s\n' "$RED" "$OFF" >&2
  exit 1
}

installed=$( (cd "$inc" && find . -name '*.h' | sed 's|^\./||' | sort) )

# The FreeType public headers, in upstream's own order: freetype.h first
# because the rest lean on its macros, then the others alphabetically. config/
# is not a root -- it is reached through ftheader.h's macro indirections -- and
# fterrdef.h is the body of an X-macro list that fterrors.h includes, not a
# header anything includes on its own.
ft_rest=$(printf '%s\n' "$installed" \
  | grep '^freetype/' \
  | grep -v '^freetype/config/' \
  | grep -v '^freetype/freetype[.]h$' \
  | grep -v '^freetype/fterrdef[.]h$' \
  | grep -v '^freetype/ftchapters[.]h$')

{
  printf '/* generated by ci/header-link.sh -- not a source file */\n'
  for h in "${roots_head[@]}"; do printf '#include <%s>\n' "$h"; done
  printf '%s\n' "$ft_rest" | sed 's|.*|#include <&>|'
  for h in "${roots_tail[@]}"; do printf '#include <%s>\n' "$h"; done
} > "$work/surface.c"
cp "$work/surface.c" "$work/surface.h"
printf 'int main(void) { return 0; }\n' >> "$work/surface.c"

printf '%s1. compiles%s  %s roots in one translation unit\n' "$DIM" "$OFF" \
  "$(grep -c '^#include' "$work/surface.h")"
if ! zig cc ${cc_target[@]+"${cc_target[@]}"} -c -I "$inc" \
     "$work/surface.c" -o "$work/surface.o" > "$work/cc.log" 2>&1; then
  grep -E 'error|fatal' "$work/cc.log" | head -20 >&2
  printf '%san installed header does not compile%s\n' "$RED" "$OFF" >&2
  exit 1
fi
printf '   %severy installed header compiles%s\n' "$GREEN" "$OFF"

#-----------------------------------------------------------------------------
# 2. Reached: the preprocessor names every header it actually opened.
#-----------------------------------------------------------------------------
if ! zig cc ${cc_target[@]+"${cc_target[@]}"} -M -I "$inc" "$work/surface.c" \
     > "$work/deps.raw" 2> "$work/deps.log"; then
  cat "$work/deps.log" >&2
  printf '%sthe dependency scan failed, so check 2 measured nothing%s\n' \
    "$RED" "$OFF" >&2
  exit 1
fi

# -M emits one backslash-continued line of paths. Normalise the separators and
# the case, then split on whitespace.
tr 'A-Z' 'a-z' < "$work/deps.raw" | tr -d '\r' | tr -s ' \t\n' '\n' \
  | sed 's|\\|/|g' | grep '[.]h$' | sort -u > "$work/reached.txt"

# Reduce that to the headers under our own prefix, spelled exactly the way
# `installed` spells them, so that the comparison below is set membership and
# nothing cleverer.
#
# The reduction is anchored on the trailing components of the install prefix --
# `prefix/include`, which this script chose itself -- rather than on its
# absolute path, deliberately: on Windows the shell's idea of the temporary
# directory (/tmp/...) and the compiler's (C:/Users/.../AppData/Local/Temp/...)
# are the same directory spelled two ways, and a full-prefix comparison
# silently matches nothing. Nothing is what the first run of this reported --
# every header "unreached", including the ones the compiler had visibly just
# opened.
#
# What the anchor must NOT be is a bare filename suffix. It was, and the test
# for it was written as arithmetic over an awk index():
#
#     index($0, s) == length($0) - length(s) + 1
#
# index() returns 0 when the substring is absent, so for every line that did
# not contain the header at all the test collapsed to
# `length($0) == length(s) - 1`, and any path of the right LENGTH read as a
# match. On a native Linux host zig cc resolves libc through the system's own
# headers, which is where three short dependency paths come from:
#
#     /usr/include/stdio.h        20 == len "sheenbidi/sbconfig.h"
#     /usr/include/stdint.h       21 == len "freetype/ftchapters.h"
#     /usr/include/stdc-predef.h  26 == len "freetype/config/ftmodule.h"
#
# -- which is exactly the three entries of unreached_by_design, and all three
# were reported as "declared unreachable and IS reached" on ubuntu-latest and
# on no other host. Windows and macOS reach their libc headers through long
# toolchain- and SDK-relative paths, so the same gate returned the opposite
# verdict on the same tree. A check whose answer depends on how long a path
# happens to be is not a check, so this one is now whole-line equality against
# a set: `grep -qxF`, no arithmetic and no pattern metacharacters.
anchor=${inc#"$work"/}
sed -n "s|^.*/$anchor/||p" "$work/reached.txt" | sort -u > "$work/ours.reached"

# The failure mode that reduction can have is naming nothing, and a silent
# nothing would surface as every installed header being unreached at once --
# a hundred lines that name the wrong cause. Say it once instead.
[ -s "$work/ours.reached" ] || {
  printf '%sthe dependency scan named no header under %s%s\n' \
    "$RED" "$anchor" "$OFF" >&2
  exit 1
}

fail2=0
while IFS= read -r h; do
  [ -n "$h" ] || continue
  key=$(printf '%s' "$h" | tr 'A-Z' 'a-z')
  reason=
  for entry in "${unreached_by_design[@]}"; do
    case "$entry" in "$h|"*) reason=${entry#*|} ;; esac
  done
  if grep -qxF -- "$key" "$work/ours.reached"; then
    if [ -n "$reason" ]; then
      printf '   %s%s is declared unreachable and IS reached%s\n' \
        "$RED" "$h" "$OFF" >&2
      fail2=1
    fi
  elif [ -z "$reason" ]; then
    printf '   %s%s is installed and no root reaches it%s\n' "$RED" "$h" "$OFF" >&2
    fail2=1
  fi
done <<EOF
$installed
EOF

if [ "$fail2" -ne 0 ]; then
  printf '%sthe installed set and the reachable set disagree%s\n' "$RED" "$OFF" >&2
  exit 1
fi
printf '%s2. reached%s   %s installed, %s opened, %s of them ours, %s deliberately neither\n' \
  "$DIM" "$OFF" "$(printf '%s\n' "$installed" | wc -l | tr -d ' ')" \
  "$(wc -l < "$work/reached.txt" | tr -d ' ')" \
  "$(wc -l < "$work/ours.reached" | tr -d ' ')" "${#unreached_by_design[@]}"

#-----------------------------------------------------------------------------
# 3. Links: reference every declared entry point and make the linker resolve it.
#-----------------------------------------------------------------------------
if ! zig translate-c -lc ${cc_target[@]+"${cc_target[@]}"} -I "$inc" \
     "$work/surface.c" > "$work/surface.zig" 2> "$work/translate.log"; then
  cat "$work/translate.log" >&2
  printf '%szig translate-c could not read the installed headers%s\n' \
    "$RED" "$OFF" >&2
  exit 1
fi

# Every identifier that appears anywhere in the install prefix, so that a libc
# declaration pulled in by an upstream header is not mistaken for one of ours.
grep -rhoE '[A-Za-z_][A-Za-z0-9_]*' "$inc" | sort -u > "$work/words.txt"

grep -oE '^pub extern (fn|var) [A-Za-z_][A-Za-z0-9_]*' "$work/surface.zig" \
  | awk '{ print $4 }' | sort -u > "$work/declared.txt"
comm -12 "$work/declared.txt" "$work/words.txt" > "$work/ours.txt"
untranslated=$(grep -c '@compileError' "$work/surface.zig")

{
  cat "$work/surface.h"
  printf '#include <stdint.h>\n'
  printf 'volatile uintptr_t ztypeset_header_link_sink;\n'
  printf 'int main(void) {\n'
  while IFS= read -r n; do
    printf '  ztypeset_header_link_sink ^= (uintptr_t)(void *)&%s;\n' "$n"
  done < "$work/ours.txt"
  printf '  return 0;\n}\n'
} > "$work/probe.c"

libs=()
for a in "$prefix"/lib/*.lib "$prefix"/lib/*.a; do
  [ -e "$a" ] && libs+=("$a")
done
[ "${#libs[@]}" -gt 0 ] || {
  printf '%sno libraries were installed%s\n' "$RED" "$OFF" >&2
  exit 1
}

# HarfBuzz is C++ everywhere but the MSVC ABI, where build.zig links the
# platform runtime instead. Same condition, same reason, one arm each.
cxx=()
case "$target" in
  *msvc*) ;;
  *) cxx=(-lc++) ;;
esac

if ! zig cc ${cc_target[@]+"${cc_target[@]}"} -I "$inc" \
     -Wno-deprecated-declarations "$work/probe.c" "${libs[@]}" \
     ${cxx[@]+"${cxx[@]}"} -o "$work/probe" > "$work/link.log" 2>&1; then
  grep -iE 'undefined|error' "$work/link.log" | head -30 >&2
  printf '%san installed header declares an entry point nothing defines%s\n' \
    "$RED" "$OFF" >&2
  exit 1
fi

if ! "$work/probe"; then
  printf '%sthe probe linked but did not run%s\n' "$RED" "$OFF" >&2
  exit 1
fi

printf '%s3. links%s     %s declared entry points, every one defined\n' \
  "$DIM" "$OFF" "$(wc -l < "$work/ours.txt" | tr -d ' ')"
printf '%s           %s declarations zig translate-c could not read%s\n' \
  "$DIM" "$untranslated" "$OFF"
printf 'installed headers compile, are reachable, and link%s\n' \
  "${target:+ (}${target}${target:+)}"
