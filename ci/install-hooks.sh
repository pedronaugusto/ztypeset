#!/usr/bin/env bash
#
# ztypeset -- run the local CI matrix before every push.
#
# ci/run.sh is the same matrix CI runs, so a failure caught here is a failure
# not caught in a pull request. Install once; skip an individual push with
# `git push --no-verify` when you know what you are doing.
#
# Usage: ci/install-hooks.sh

set -euo pipefail
cd "$(dirname "$0")/.."

hooks=$(git rev-parse --git-path hooks)
mkdir -p "$hooks"

cat > "$hooks/pre-push" <<'HOOK'
#!/usr/bin/env bash
# Installed by ci/install-hooks.sh
exec "$(git rev-parse --show-toplevel)/ci/run.sh"
HOOK

chmod +x "$hooks/pre-push"
printf 'installed %s\n' "$hooks/pre-push"
printf 'it runs ci/run.sh; use `git push --no-verify` to skip one push.\n'
