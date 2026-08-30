#!/usr/bin/env bash
#
# ztext -- the one place that decides how to compute a SHA-256.
#
# Sourced, never executed. Two scripts need digests and the hosts they run on
# disagree about the tool: coreutils ships `sha256sum`, macOS ships `shasum`.
# Choosing between them in two places is how one of the two ends up silently
# skipping its check on the host nobody develops on -- the same shape as every
# other second home in this repository, in four lines of shell.
#
# Provides:
#   sha256_tool            names the tool found, or nothing
#   sha256_of <file>       the 64-hex digest on stdout, nothing if no tool
#   sha256_verify <file>   checks a SHA256SUMS-format file, quietly;
#                          exit 127 if no tool is available

sha256_tool() {
  if command -v sha256sum > /dev/null 2>&1; then
    printf 'sha256sum\n'
  elif command -v shasum > /dev/null 2>&1; then
    printf 'shasum\n'
  fi
}

sha256_of() {
  case "$(sha256_tool)" in
    sha256sum) sha256sum "$1" | cut -d' ' -f1 ;;
    shasum) shasum -a 256 "$1" | cut -d' ' -f1 ;;
  esac
}

sha256_verify() {
  case "$(sha256_tool)" in
    sha256sum) sha256sum -c --quiet "$1" ;;
    shasum) shasum -a 256 -c --status "$1" ;;
    *) return 127 ;;
  esac
}
