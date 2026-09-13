# Maelys OCI

Bounded OCI registry acquisition, canonical OCI-layout materialization and an
immutable content-addressed store. `libmaelys-oci` exposes the artifact
resolver, opaque pull API and lease lifecycle; `maelys-oci` is
its terminal, built on [maelys-cli](../maelys-cli) and installable as the
`maelys oci` external command.

```sh
maelys-oci inspect /absolute/layout
maelys-oci import /absolute/layout --platform linux/arm64            # plan
maelys-oci import /absolute/layout --platform linux/arm64 --apply    # materialize
maelys-oci pull registry.example/team/tool@sha256:... --platform linux/arm64
maelys-oci list --format jsonl
maelys-oci verify --json                                             # exit 2 on integrity errors
maelys-oci gc --grace-seconds 3600                                   # plan, then --apply
maelys-oci remove team/tool@sha256:... --platform linux/arm64 --apply
```

`maelys-oci completion bash|zsh|fish` prints the completion generated from
the catalog. Every command follows the `agent-cli/v2` contract: `describe` publishes the
catalog and the JSON Schema of each result, every command prints that
schema-described document (no prose output), `--format json` wraps it in
one envelope, transactions plan by default and write with `--apply`, exit `0`
is success, `1` failure and `2` a completed report with violations. See the
generated [docs/cli.md](docs/cli.md) and
[machine-readable contract](docs/cli-contract.json).

## Layout

```text
include/maelys/oci.h   public library API (pull, artifact resolve, revalidate, lease)
src/common/            diagnostics, bounded I/O, SHA-256, OCI descriptor vocabulary
src/store/             private store mechanics, ordered locks, seal format, artifact API
src/materializer/      layout access, logical Linux graph, ext4 and tar writers, store operations
src/puller/            HTTPS registry acquisition over maelys-http
src/materializer/api.c the public store operations of <maelys/oci.h>
cli/main.c             the maelys-cli catalog and handlers of maelys-oci
cli/schemas/           JSON Schema of every command result, embedded at build time
cli/command.json.in    manifest template declaring `maelys oci`
packaging/            installation metadata and distribution templates
```

`src/` builds `libmaelys-oci.a` and nothing else: the library never prints
and never parses `argv`; every operation returns an opaque document or a
status and reports failures through a result code and a message. `cli/` is
the terminal, a consumer of `include/maelys/oci.h` like any other program:
it compiles without `-I.` and includes nothing from `src/`, which
`tests/check_source.py` enforces. The CLI owns its schema and manifest
sources in `cli/`; the build generates the embedded schemas and the
installation tree under `build/`, and the manifest is installed as
`PREFIX/share/maelys/commands/oci.json` for the dispatcher.

## Building and testing

```sh
make                 # libmaelys-oci.a, maelys-oci, pkg-config file, extension manifest
make check           # CLI contract, materializer, adversarial and registry gates
make analyze         # clang static analysis, any diagnostic fails
make asan-ubsan      # make check under AddressSanitizer and UBSan
make install PREFIX=/opt/homebrew   # binary, library, manifest for `maelys oci`, shell completions
make dist
```

All compilation outputs, dependency archives and generated files live under
`build/<platform>/release/`, where the platform is `linux-x86_64`, `linux-arm64`
or `macos-arm64`. Sanitizers use `build/<platform>/sanitizers/`. Select another
profile with `make BUILD=build/<platform>/<profile>`; output paths outside
`build/` and `build/` itself are refused, including symlink escapes.
`make clean` removes only the selected profile. The release script also stages
its package there; only the final archives and checksums go to `dist/`, as
required by the release socle. Generated build outputs are never committed.

The Maelys dependencies are pinned by release tag and immutable commit in
`dependencies/*.pin` and built from one root the release socle gives, never
from a sibling checkout: `maelys-release dependencies . --apply` materialises
them and prints `MAELYS_DEPENDENCIES_DIR`, which the build reads
(`make check MAELYS_DEPENDENCIES_DIR=...`; one dependency at a time overrides
through its `MAELYS_*_DIR`). Mbed TLS is pinned the same way and built from
source on Linux, where the distribution ships below maelys-http's floor; on
macOS Homebrew's is used. System libraries: libarchive, e2fsprogs and
Mbed TLS 3.6.7+ or 4.1.2+ development files. The build
also checks the linked Mbed TLS runtime. Libarchive must provide gzip and zstd
decoding in process; external decoder fallbacks are refused. Dependency
checkouts must match their pins, including build rules and untracked inputs.
Use a fresh profile or `make clean` when changing the compiler or compile/link
flags; Make does not track command-line flag changes in existing objects.

`make install` places the terminal in `PREFIX/bin/maelys-oci` and its
manifest in `PREFIX/share/maelys/commands/oci.json`, so `maelys oci ...`
resolves it after digest verification.
Changing `PREFIX` regenerates the pkg-config file and manifest without
requiring a rebuild; installation is also tested through a temporary `DESTDIR`.

## Acquisition and store contracts

See [docs/open-core.md](docs/open-core.md) for the mandatory MPL/proprietary
boundary.

ABI 3 and artifact schema v7 keep materialization standalone. Materializer 9
verifies the ordered layer DiffIDs and applies whiteouts only to parent layers.
No Warden helper or mount point is injected. A store still holding an
artifact of a former schema is refused by every command, naming the way out;
recreate the store and reimport. Store v2, closures, leases and execution-lease
formats remain unchanged. The store now defaults to `MAELYS_OCI_STORE` or
`$XDG_DATA_HOME/maelys-oci` (fallback `$HOME/.local/share/maelys-oci`).

`make check` includes independent C API linking, C++ public-header compilation,
parser mutations, HTTPS adversarial cases and process crash/concurrency gates.
`make fuzz` runs a bounded libFuzzer campaign when supported by Clang.
CI runs `make check` and static analysis on all three release platforms,
plus ASan/UBSan and fuzzing on Linux x86_64.

Licensed under MPL-2.0.
