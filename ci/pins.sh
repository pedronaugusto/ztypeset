#!/usr/bin/env bash
#
# ztypeset -- reading src/pins.zig from a shell script. SOURCED, not executed.
#
# src/pins.zig is the one home for what libs/ holds. Two scripts need those
# values -- ci/verify-vendor.sh fetches the pinned commits, ci/measurements.sh
# compares UPSTREAM.md's table against them -- and a parser written twice is a
# parser that can disagree with itself, so it is written here once.
#
#   pin <name> <field>   one field of one pin: url, tag or commit
#   pin_version <name>   the three-component version, as "2.14.3"
#   pin_names            every pin, in the order src/pins.zig declares them
#
# Each function exits 2 rather than returning an empty string when the pin is
# not found: a missing pin has to stop the script, not silently compare "" with
# "".

# One quoted field of one pin.
pin() {
  local value
  value=$(sed -n 's/^pub const '"$1"': Pin = .*[.]'"$2"' = "\([^"]*\)".*/\1/p' src/pins.zig)
  if [ -z "$value" ]; then
    printf 'pins: src/pins.zig has no %s for %s\n' "$2" "$1" >&2
    exit 2
  fi
  printf '%s' "$value"
}

# The version, reassembled from the three numeric fields.
pin_version() {
  local line major minor patch
  line=$(grep -E "^pub const $1: Pin = " src/pins.zig)
  if [ -z "$line" ]; then
    printf 'pins: src/pins.zig has no pin named %s\n' "$1" >&2
    exit 2
  fi
  major=$(printf '%s' "$line" | sed -n 's/.*[.]major = \([0-9]*\).*/\1/p')
  minor=$(printf '%s' "$line" | sed -n 's/.*[.]minor = \([0-9]*\).*/\1/p')
  patch=$(printf '%s' "$line" | sed -n 's/.*[.]patch = \([0-9]*\).*/\1/p')
  if [ -z "$major" ] || [ -z "$minor" ] || [ -z "$patch" ]; then
    printf 'pins: %s has no complete version in src/pins.zig\n' "$1" >&2
    exit 2
  fi
  printf '%s.%s.%s' "$major" "$minor" "$patch"
}

# In declaration order, which is also UPSTREAM.md's column order.
pin_names() {
  sed -n 's/^pub const \([a-z]*\): Pin = .*/\1/p' src/pins.zig
}
