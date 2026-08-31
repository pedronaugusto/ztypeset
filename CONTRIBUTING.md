# Contributing to ztext

The rules live in [README.md](README.md#contributing), and this file does not
repeat them — a second copy of a rule is a copy that can disagree with the
first, which is the defect class this repository gates against everywhere else.
What follows is the map: where each thing is written down, so you can find it
once rather than reading two versions of it.

| you want to know | it is here |
|---|---|
| what not to edit, and what to run before pushing | [README § Contributing](README.md#contributing) |
| every gate, what it checks, and how to run it | [README § Testing](README.md#testing) |
| what the vendored upstreams are and how they are pinned | [UPSTREAM.md](UPSTREAM.md) |
| which licence each upstream carries, and whether it reaches your binary | [LICENSES.md](LICENSES.md) |
| what changed, and what a version bump means | [CHANGELOG.md](CHANGELOG.md) |
| how to report a vulnerability | [SECURITY.md](SECURITY.md) |

Two things worth saying here because they shape a patch before it is written:

**A claim in this repository is expected to have something that can fail.** A
comment describing an invariant is a comment; the invariant is whatever a test
or a script can refuse. `ci/check-guards.sh` exists to prove the tests can
actually fail — it applies each of ~90 deliberate bugs and requires a *named*
test to catch it — and `ci/measurements.sh --check` recomputes every number the
documentation states. If you add a rule, add the thing that enforces it; if you
add a number, add the line that recomputes it.

**`libs/` is upstream, byte for byte.** Nothing in a patch may touch it, not
even a warning fix. ztext's own C compiles with `-Wall -Wextra -Werror`;
upstream's does not, deliberately, because turning ztext's standards into build
failures for four other projects' code would mean patching them locally.
Workarounds go in `ffi/` and are recorded in [UPSTREAM.md](UPSTREAM.md).
