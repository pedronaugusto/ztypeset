#!/usr/bin/env bash
#
# ztext -- prove the vendored trees really are unmodified upstream.
#
# UPSTREAM.md says libs/ holds pristine copies of three specific commits. On
# its own that is a claim in a markdown file: nothing stops an edit to libs/
# from landing while the documentation still says otherwise. This script turns
# the claim into a check by fetching those exact commits and diffing.
#
# It is also why git submodules are not used here. A submodule would have git
# record the upstream commit, which is genuinely useful -- but Zig's package
# manager fetches a source archive and never resolves submodules, so a consumer
# would receive an empty libs/ and a build that cannot work. This gets the
# guarantee without the breakage.
#
# It checks the committed test fonts too. The golden shaping results in the
# suite are only meaningful against those exact bytes, so "the fonts have not
# changed" is part of the same guarantee.
#
# Needs network, so it is a separate CI job rather than part of ci/run.sh.
#
# Usage: ci/verify-vendor.sh

set -uo pipefail
cd "$(dirname "$0")/.."

if [ -t 1 ]; then
  RED=$'\033[31m'; GREEN=$'\033[32m'; DIM=$'\033[2m'; BOLD=$'\033[1m'; OFF=$'\033[0m'
else
  RED=; GREEN=; DIM=; BOLD=; OFF=
fi

status=0
fail() { printf '%s%s%s\n' "$RED" "$1" "$OFF" >&2; status=1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# verify <name> <local-dir> <url> <tag> <commit> <exclude-args...> -- <paths...>
verify() {
  local name="$1" local_dir="$2" url="$3" tag="$4" commit="$5"; shift 5
  local excludes=() paths=()
  while [ "$1" != "--" ]; do excludes+=("$1"); shift; done
  shift
  paths=("$@")

  printf '\n%s%s%s %s%s @ %s%s\n' "$BOLD" "$name" "$OFF" "$DIM" "$url" "$tag" "$OFF"

  # The pin must appear in UPSTREAM.md verbatim. Bumping one and not the other
  # is exactly the drift this script exists to catch.
  if ! grep -q "$commit" UPSTREAM.md; then
    fail "  UPSTREAM.md does not mention $commit -- the pin has drifted."
    return
  fi

  local clone="$work/$name"
  if ! git clone --quiet --depth 1 --branch "$tag" "$url" "$clone" 2>/dev/null; then
    fail "  clone failed"
    return
  fi

  # A tag can be moved; the commit cannot. Check the SHA, not the label.
  local actual
  actual=$(git -C "$clone" rev-parse HEAD)
  if [ "$actual" != "$commit" ]; then
    fail "  tag $tag is $actual, expected $commit"
    return
  fi
  printf '  %scommit %s confirmed%s\n' "$DIM" "$commit" "$OFF"

  local path
  for path in "${paths[@]}"; do
    if [ ! -e "$local_dir/$path" ]; then
      printf '  %-26s %sMISSING locally%s\n' "$path" "$RED" "$OFF"
      status=1
      continue
    fi
    if diff -r ${excludes[@]+"${excludes[@]}"} "$clone/$path" "$local_dir/$path" \
         > "$work/diff" 2>&1; then
      printf '  %-26s %sidentical%s\n' "$path" "$GREEN" "$OFF"
    else
      printf '  %-26s %sDIFFERS%s\n' "$path" "$RED" "$OFF"
      sed 's/^/      /' "$work/diff" | head -20
      status=1
    fi
  done
}

# Exclusions are documented in UPSTREAM.md under "What was excluded, and why".
verify freetype libs/freetype \
  https://gitlab.freedesktop.org/freetype/freetype.git \
  VER-2-14-3 0a0221a1347e2f1e07c395263540026e9a0aa7c7 \
  -x tools -- include src LICENSE.TXT README docs/FTL.TXT docs/GPLv2.TXT

verify harfbuzz libs/harfbuzz \
  https://github.com/harfbuzz/harfbuzz.git \
  14.3.1 ab5ecbb83985034a76214ac0b2b833dcd590d774 \
  -x __pycache__ -- src COPYING AUTHORS THANKS

verify sheenbidi libs/sheenbidi \
  https://github.com/Tehreer/SheenBidi.git \
  v3.0.0 cfe430e7375a7845b679adae9d51dac6deaa8858 \
  -- Headers Source LICENSE README.md

# The excludes here are upstream's generators, its conformance-test data and
# its own build files -- everything that is not a translation unit ztext
# compiles. They are listed in UPSTREAM.md; keep the two in step.
verify libunibreak libs/libunibreak \
  https://github.com/adah1972/libunibreak.git \
  libunibreak_7_0 3ce4bfa3129ff3738046a44a6db533d2ce25af2b \
  -x 'generate_*.py' -x unicode_data_property.py -x '*.sed' -x '*.tmpl' \
  -x '*Test.txt' -x 'Makefile.*' -x tests.c -x test_skips.h \
  -- src LICENCE README.md

#-----------------------------------------------------------------------------
# Test fonts.
#
# Not upstream source, but held to the same standard: the golden shaping
# results are only meaningful against these exact bytes, so a font that
# changed silently would turn a real regression into a puzzling one.
#-----------------------------------------------------------------------------

printf '\n%sfonts%s %stests/fonts/SHA256SUMS%s\n' "$BOLD" "$OFF" "$DIM" "$OFF"
if command -v sha256sum > /dev/null 2>&1; then
  checker="sha256sum -c --quiet"
elif command -v shasum > /dev/null 2>&1; then
  checker="shasum -a 256 -c --status"
else
  checker=""
fi

if [ -z "$checker" ]; then
  fail "  no sha256sum or shasum available"
elif (cd tests/fonts && $checker SHA256SUMS); then
  printf '  %-26s %sidentical%s\n' "$(ls tests/fonts/*.ttf | wc -l | tr -d ' ') fonts + licences" "$GREEN" "$OFF"
else
  fail "  a committed font or licence file does not match tests/fonts/SHA256SUMS"
fi

#-----------------------------------------------------------------------------
printf '\n'
if [ $status -eq 0 ]; then
  printf '%severy vendored tree matches its pinned upstream exactly%s\n' "$GREEN" "$OFF"
  exit 0
fi

printf '%sthe vendored tree is not a pristine copy of what UPSTREAM.md claims.%s\n' "$RED" "$OFF" >&2
printf 'Either revert the local change, or -- if the divergence is intended --\n' >&2
printf 'record it in UPSTREAM.md and teach this script about it.\n' >&2
exit 1
