# Changelog

## 0.6.3 - 2026-09-12

- **The Homebrew formulas build.** 0.6.2's tap jobs failed while rendering
  the install metadata: in a prefix build the Mbed TLS floor was read from
  a maelys-http checkout that does not exist, and `brew --prefix` is not on
  the path inside Homebrew's sandbox, which put `-L/lib` in the pkg-config
  file. The floor now comes from the pkg-config file the installed
  maelys-http carries for its Mbed TLS provider, a build that finds no floor
  stops at once, and the Homebrew prefix is taken from `HOMEBREW_PREFIX`
  when the sandbox sets it. The local prefix test now runs with no sibling
  checkout and no `brew` reachable, which is what had hidden both.
- Release socle maelys-release v0.44.0 (from v0.40.1): `[runners]` lets a
  private repository name the runner of the socle's macOS jobs, `protect`
  derives a default branch's protection from the declarations,
  `dependencies` materialises the pins; `preflight` reports a formula the
  tap serves. Nothing in this repository's release mechanism changes.

## 0.6.2 - 2026-09-12

- **Homebrew formulas.** `packaging/homebrew/libmaelys-oci.rb.in` installs
  the library, its header and its pkg-config file; `maelys-oci.rb.in`
  installs the terminal, the manifest that registers `maelys oci` and the
  shell completions. Both depend on the tap's `libmaelys-sys`,
  `libmaelys-json` and `libmaelys-http` (build-time for the terminal, which
  links them), on `mbedtls`, `libarchive` and `e2fsprogs`, and stage
  maelys-cli at the tag's pin as a build resource, as maelys-egress does.
  `scripts/render-homebrew-formula.sh TAG OUTPUT NAME` renders one from the
  tag's own copy; the socle's tap jobs build bottles on macos-15 and
  macos-26 and push to `maelys-dev/homebrew-tap`.
- **The Makefile builds against installed Maelys libraries.**
  `MAELYS_SYSTEM_PREFIX`, `MAELYS_JSON_PREFIX` and `MAELYS_HTTP_PREFIX` name
  an installed library instead of its pinned checkout (the default, what
  every gate runs). An installed library must carry the ABI this tree was
  written against (`MAELYS_SYSTEM_ABI` 1, `MAELYS_JSON_ABI` 2,
  `MAELYS_HTTP_ABI` 1, now also asserted on the checkouts) and at least the
  pinned version, read from its version header or pkg-config file; older is
  refused. An installed maelys-http was built against its host's Mbed TLS,
  so a prefix build takes that one. maelys-cli is always built from its
  checkout: the framework is linked into the terminal. `install` splits
  into `install-library` and `install-command`.
- Pin maelys-http v0.1.13 (from v0.1.11): signing keys read from the
  default branch, its Homebrew formula renamed `libmaelys-http` with Mbed TLS
  mandatory; the version the tap's formula carries.
- Pin maelys-json v0.2.0 (from v0.1.5): ABI 2, which removes two functions
  added in 0.1.6 that this repository never called; the version the tap's
  formula carries and maelys-cli pins.
- Pin maelys-cli v0.5.25 (from v0.5.23): the dispatcher's sources move to
  `cli/`, maelys-json v0.2.0; installed agent texts refreshed.
- Release socle maelys-release v0.40.1 (from v0.35.0): `verify_command` and
  `package_command` both run under bash; a replay of a tag runs the workflow
  at that tag with `--ref`, and a socle at fault is fixed by a patch release,
  never a replay; declarations move to `maelys-release.conf` (none here);
  SBOM attestation for products that declare one (none here); `cut` audits
  its own write. Nothing in this repository's release mechanism changes.

## 0.6.1 - 2026-09-11

- Pin maelys-http v0.1.11 (from v0.1.5): the client rejects forbidden
  `Content-Length` and `Transfer-Encoding` fields on 1xx, 204 and CONNECT
  responses before a connection is reused, bounds chunk extensions over the
  whole message, and delivers a 3xx whose `Location` it will not follow
  instead of failing the exchange. No code of the puller changed; the
  integrity suite passes against it.
- **Mbed TLS is pinned and built from source on Linux.** maelys-http 0.1.6
  refuses at compile time a Mbed TLS below its security floor (3.6.7), and
  the distributions ship below it (Ubuntu 26.04: 3.6.5). `dependencies/mbedtls.pin`
  names the upstream commit maelys-http itself builds, with its repository
  and submodule; `scripts/checkout-dependency.sh mbedtls` fetches it and the
  Makefile builds it with cmake into the build tree, static and private,
  before maelys-http (`MBEDTLS_SOURCE=pinned`, the Linux default; macOS
  keeps Homebrew's, `system`). `dependencies/packages` trades
  `libmbedtls-dev` for `cmake` on Linux. The installed pkg-config file now
  requires a consumer's Mbed TLS through `Requires.private` at the floor
  maelys-http declares, read from its pinned checkout, instead of listing
  the libraries of the build host.
- Pin maelys-cli v0.5.23 (from v0.5.22), which pins agent-cli-spec v2.4.0:
  the framework's trunk gains `--field NAME`, rendering one top-level member
  of a command's `data`; visible in the regenerated `docs/cli.md` and
  `docs/cli-contract.json`. Installed agent texts refreshed.
- Pin maelys-json v0.1.5 (from v0.1.3): two additions to the writer and
  error API, unused here; no change in behaviour.
- Release socle maelys-release v0.35.0 (from v0.33.0): the managed blocks
  and the skill name `--field` and the branch-naming convention;
  `declarations` names a workflow that runs twice on every pull request
  and recognises a branch protected by a ruleset. Nothing in the release
  mechanism changes.

## 0.6.0 - 2026-09-11

- Release socle maelys-release v0.33.0 (from v0.27.0): the job that runs
  `scripts/package-release.sh` no longer holds a write token; the assets cross
  the run as one-day workflow artifacts and `publish` is the only job that
  writes, behind one approval; `preflight` verifies the gate the repository
  declares. The managed `AGENTS.md`/`CLAUDE.md` blocks and
  `.claude/skills/maelys-release/SKILL.md` carry their CC-BY-4.0 attribution,
  and the block says where this repository's prose lives; the skill
  describes `maelys-release cut`, optional, the ceremony by hand being
  unchanged.
- maelys-cli v0.5.22 (from v0.5.16): the framework's trunk gains
  `--progress`, `--verbose` and `--pager`, visible in the regenerated
  `docs/cli.md` and `docs/cli-contract.json`; the installed block, guide and
  `.claude/skills/maelys-cli-command/SKILL.md` carry the same CC-BY-4.0
  attribution. No code of this repository changed.
- `LICENSING.md` states the boundary: the installed agent texts are
  CC-BY-4.0, attributed to David Bromberg, each carrying its notice
  (licensing policy of maelys-platform); everything else stays MPL-2.0.
- Name the HTTP seam in `tests/check_source.py`: only `src/puller` may reach
  maelys-http. The property already held, so this changes no code; it keeps
  a later widening deliberate and names the offending file before the link
  step. The twin rule of the original proposal, that only the terminal
  reaches maelys-cli, is now stronger than a name check: `src/` refuses
  `<maelys/cli.h>` and `cli/` lives outside `src/`.
- **ABI 4: the store operations are public.** `<maelys/oci.h>` publishes
  `maelys_oci_inspect`, `maelys_oci_import` (opaque options: store,
  platform, digest, apply), `maelys_oci_store_list`, `maelys_oci_store_verify`,
  `maelys_oci_store_gc`, `maelys_oci_store_remove` and
  `maelys_oci_unpack_portable_root`, each returning a result code, an owned
  diagnostic and an opaque `maelys_oci_document_t` whose canonical JSON is
  its only representation (`maelys_oci_document_text`, and
  `maelys_oci_document_count`/`_item_text` for record members);
  `maelys_oci_store_default_path`, `maelys_oci_platform_valid`,
  `MAELYS_OCI_ENV_STORE` and `MAELYS_OCI_GC_DEFAULT_GRACE_SECONDS` join
  the header. Existing declarations are unchanged.
- **The terminal moves to `cli/`** at the root (layout policy of
  maelys-platform): `cli/main.c` is a consumer of the public header alone,
  compiled without `-I.`, and includes nothing from `src/`;
  `tests/check_source.py` refuses the opposite in both directions and a
  `src/cli/`. `cli/schemas/` and `cli/command.json.in` move with it. The
  catalog, the schemas and the generated reference are unchanged.
  `tests/public/operations.c` exercises the published operations through
  the installed header.

- Prepare the public opening of this repository (`bin/maelys-platform audit`
  of maelys-platform): the prose of `docs/` (`agent-cli.md`,
  `audit-remediation.md`, `command-conventions.md`, `development-audit.md`,
  `directive-claude-pull-oci.md`, `pull.md`, `validation-pull.md`) moves to
  `maelys-dev/maelys-docs`, directory `maelys-oci/`, with its history
  (`maelys-release migrate`). `README.md` names nothing in its place:
  `maelys-docs` is private and not a publication, and no public site exists
  yet for this product to point to instead (the automatic pointer `migrate`
  writes there contradicts maelys-platform's own documentation policy,
  which says a README never names `maelys-docs`). `docs/`
  keeps only what a machine writes or `LICENSING.md` engages:
  `docs/open-core.md`, and the generated `docs/cli.md`,
  `docs/cli-contract.json` and `docs/maelys-cli-guide.md`.
- Release socle maelys-release v0.27.0: the socle now generates
  `docs/cli.md` itself, from maelys-cli's generator at the pinned commit and
  `docs/cli.reference` (the build directory, the programs, the
  `--neutral-availability unpack-rootfs` flag); `make generate-cli-reference`
  and the CLI-reference part of `make contract-check` are removed, the
  socle's `check` compares the committed copy instead. `docs/cli-reference.md`
  is `docs/cli.md`. `SECURITY.md` added.
- `.github/workflows/ci.yml` opts into the socle's documentation contract
  (`docs_contract: true`): CI refuses prose left in `docs/` or a CLI
  reference outside `docs/cli.md`.

## 0.5.0 - 2026-09-06

- Release socle maelys-release v0.15.3: the Homebrew tap publication
  serializes and retries its push; the vendored Python module follows
  maelys-cli v0.5.19 (agent-cli-spec v2.3.1). No managed text changed.
- Correct OCI whiteouts by applying them to inherited entries before the
  current layer's additions. Require empty regular-file markers and preserve
  additions regardless of tar order, including opaque directory whiteouts.
  Refuse a path carrying a `.wh.` segment anywhere but last: no pass consumes
  it, and materializing it would publish a marker name inside the root.
- Share config validation between pull and import: require supported Linux
  platforms, `rootfs.type=layers` and one canonical SHA-256 DiffID per layer.
  Verify each complete decompressed tar against its ordered DiffID before
  application, and require compression to match the declared media type. Allow
  supported Linux images to be selected from mixed-platform indexes by omitting
  well-formed unsupported siblings; malformed and unknown descriptors remain
  errors.
- Reconstruct source closures from verified manifest/config blobs. Compare
  ordered layer descriptors and artifact metadata, and block GC on divergence.
- Bound local blob copies before every write, check the opened source size,
  release failed descriptor reads and cap cumulative import staging at 16 GiB.
- **Artifact compatibility, no migration:** artifact schema v7, MWOCI/7
  seal and maelys-oci-materializer/9 identify the corrected semantics. A
  store still holding an artifact of a former schema (v5, v6) is refused
  by every command: `list`, `remove` and a reimport fail with
  `PRECONDITION_FAILED`, `verify` and `gc` report the artifact as invalid,
  each naming the former schema and the way out: recreate the store and
  reimport its sources. No former artifact is ever reused, relabeled,
  overwritten or retired in place. Store v2, closures, leases, migration
  witnesses and dependency pins stay unchanged. Add conformance,
  bounded-copy, GC preservation and refusal regressions.
- Refused content is a protocol failure on every path: a layer whose
  encoding, expansion, DiffID, entries or whiteout markers are invalid, and
  a descriptor that is absent or not its declared size and digest, now fail
  `import --apply` with `PROTOCOL_FAILED` and the diagnostic names the
  layer, instead of `IO_FAILED` with a retry hint. Host failures during
  application (no space, quota, I/O error) stay `IO_FAILED`.
- **Contract change:** the staging bound of 16 GiB over the manifest, the
  config and every layer occurrence also applies to `pull`, where the
  unique-closure bound was 64 GiB; an image whose staged occurrences exceed
  16 GiB is refused at inspection.
- Pin maelys-system v0.9.1: no ABI change; `publish_noreplace` anchors both
  parent directories by descriptor and keeps only its two states (staging or
  destination, crash included), `write_exclusive` always cleans up through
  `unlink_same`, and the socket layer reports `ERR_WOULD_BLOCK` on
  `ECONNABORTED` and `ERR_RESET` then `ERR_CLOSED` on a reset seen at send.

## 0.4.2 - 2026-09-05

- Retry a GET once on a fresh connection when a registry closes before response
  headers arrive. Preserve request and time limits, and fail without retrying
  an incomplete body. Cover the keep-alive race and retry bounds over HTTPS.
- Pin maelys-cli v0.5.16 for literal schema-embed substitutions, safe terminal
  diagnostics and hardened dispatcher manifest and executable handling.
- Adopt maelys-release v0.15.1 to publish verified package assets through a
  protected draft release without consuming GitHub Actions artifact storage.
  Publish from a new signed tag with the existing tag-only deployment policy;
  v0.4.1 remains immutable after its publication failed at the artifact quota.

## 0.4.1 - 2026-09-05

- Adopt maelys-release v0.15.0 and its `dependencies/` declaration layout,
  including Ubuntu 26.04 GitHub-hosted runners and refreshed workflow action
  pins.
  Refresh the build pins to maelys-system v0.9.0, maelys-json v0.1.3,
  maelys-http v0.1.5 and maelys-cli v0.5.15 so CI and release builds consume
  the latest immutable dependency releases. maelys-http v0.1.5 selects the
  non-deprecated TLS-minimum API on Mbed TLS 3.6+, keeping Ubuntu 26.04 builds
  warning-clean under `-Werror`.
- Security: reject unsupported PAX metadata such as POSIX ACLs instead of
  silently weakening filesystem permissions; accept only the documented
  path, ownership, size, timestamp and charset keys.
- Validate OCI source archives to a private cache in one bounded pass. Reject
  malformed trailing headers and duplicate members, cap archive bytes, members
  and cached files, enforce a scan deadline, and avoid repeated decompression
  for every descriptor.
- Densely renumber logical inodes after graph compaction so repeated layer
  replacements cannot inflate writer allocations.
- Refuse Mbed TLS versions affected by CVE-2025-27810 at compile time and at
  runtime. The supported baselines are 2.28.10+, 3.6.3+ and 4+;
  the Linux test container now uses Ubuntu 26.04.
- Audit fixes: prevent archive input from selecting an external decompressor,
  remove shared temporary-name counters from concurrent materialization, and
  preserve existing ext4 destinations when exclusive creation fails.
- Correct incremental dependency builds under `make -j`, recover missing
  dependency outputs and verify dependency build rules and untracked inputs.
  Dependency pins are unchanged.
- Regenerate installation metadata when `PREFIX` changes, with atomic writes
  and stable mtimes for unchanged content; test the actual installed tree.
- Add concurrent writer, no-replacement, external-decoder and build regressions.
  Keep test assertions active; make UBSan findings fatal. CI now includes
  static analysis and a bounded fuzzing campaign alongside the existing gates.
- Group CLI output schemas and the command manifest template under `src/cli/`.
  Generated schemas stay in the build directory; the public command contracts
  and installed `share/maelys/commands/oci.json` path are unchanged.
- Keep the library's pkg-config template under `packaging/`.

- Remove 75 obsolete generated files from Git. Build output defaults to
  `build/<platform>/release/`, with a separate sanitizer profile; custom
  output directories must stay below `build/`. Cleaning removes only the
  selected profile. Package staging also stays there, while `dist/` retains
  the release archives. Checks reject generated build directories in Git.

## 0.4.0 - 2026-09-05

- Complete sequential OCI pull in `libmaelys-oci`: opaque ABI 3 options and
  receipt, nested index resolution, explicit platforms, bounded HTTPS/Bearer,
  static Docker credentials, verified CAS reuse and atomic publication.
- Acquisition leases v1 protect complete blobs after interruption; partial
  transfers restart from zero. Ordered bounded locks coordinate pull/GC/remove.
- **Breaking:** standalone artifact schema v6, MWOCI/6 seal and materializer/8.
  Runtime helpers and injected mount points are removed; image files survive.
  Reimport older artifacts into a fresh store. `MAELYS_OCI_STORE` and
  `~/.local/share/maelys-oci` replace Warden settings and defaults.
- Remove Jansson and proprietary compiled-extension/store-namespace hooks.
  `maelys-json` alone parses and serializes JSON. MPL open-core boundary is
  explicit; future proprietary functionality uses separate public consumers.
- Registry/crash/concurrency gates, C/C++ public consumer, parser mutation and
  fuzz harness. Release workflow regenerated with maelys-release v0.9.0 after
  removing Jansson from package declarations; dependency commits are unchanged.
- Installed static-link metadata includes Homebrew's library directory for
  libarchive's private dependencies; consumers need no ambient `LDFLAGS`.

## 0.3.1 - 2026-09-05

- Release replay of 0.3.0, whose workflow built and checked every package
  but failed at the provenance attestation: GitHub reserves attestations
  on a private repository to paid plans, and the tag never moves. Release
  socle maelys-release v0.9.0 adopted: the attestation follows the
  repository's visibility (`auto`), so a private release ships the signed
  tag, the `.sha256` files and `SHA256SUMS` without provenance. No code
  change since 0.3.0.

## 0.3.0 - 2026-09-05

- Dependency pins: maelys-system `v0.8.0` (file primitives, `unlink_same`,
  `rmdir_same`), maelys-http `v0.1.3-1-gfa295a7` (MPL-2.0; a connection
  with a partially written request is never parked for reuse), maelys-cli
  `v0.5.10` (agent-cli-spec v2.1.0: `describe --summary --prefix`, option
  to operand conflicts, option patterns; the guide and the command skill
  are refreshed).
- **ABI 2.** `maelys_oci_artifact_t` is opaque: `maelys_oci_artifact_resolve`
  returns a heap handle through `maelys_oci_artifact_t **`, ended by
  `maelys_oci_artifact_release` (which replaces `maelys_oci_artifact_clear`),
  and the members are read through accessors (`image_digest`, `platform`,
  `root_path`, `rootfs_tar_path`, `root_digest`, `rootfs_tar_digest`,
  `config_digest`, `lease_path`, `root_identity`). A consumer that embedded
  the struct by value keeps a pointer instead.
- **Leases `maelys.warden.oci-lease/v3`.** The lease carries no process
  identity (`ownerPid` and `ownerStart` are gone, `liveness` is
  `kernel-lock`): the handle keeps an exclusive `flock(2)` on the lease
  file (`maelys_sys_file_lock_t` of maelys-system), `revalidate` requires
  the lock to be held and the path to still name the locked file, and
  `release` unlinks the lease by identity while locked before syncing the
  directory. `gc` asks the kernel with a non-blocking exclusive lock: busy
  is live, a free lock is a dead holder retired after expiry plus grace,
  anything else is retained with a warning. `verify` warns about a lease
  nobody holds. The former process-identity code
  (`maelys_oci_store_process_identity`) is removed with the v2 schema;
  there is no transition: a v2 lease file is malformed and blocks `gc`
  until removed by hand. The lock's descriptor is close-on-exec and the
  lock lives with it: a holder that forks without exec must close
  inherited descriptors.
- Retirements remove by identity with maelys-system 0.8.0: `gc` retires a
  blob, a lease and abandoned temporary state through `unlink_same` and
  `rmdir_same`, so a name that came to point elsewhere between the plan
  and the apply is left alone.
- Tests: `build/tests/test_lease` exercises the public header (resolve,
  two leases for two handles, revalidate, release by identity) from the
  store lifecycle suite, which also holds a lease from a live process and
  proves that `gc` sees it; the adversarial suite refuses every lease that
  is not the exact v3 shape.

- The store's file mechanics come from maelys-system 0.7 (`maelys/sys/file.h`)
  instead of local code: directory sync, identity-checked `flock` (verified
  before and after the lock, path re-resolved to the locked inode), no-replace
  publication of files and directories, exclusive durable write and bounded
  read. Observable changes: a staged file is published by
  `renameat2(RENAME_NOREPLACE)` / `renamex_np(RENAME_EXCL)` instead of
  `link` then `unlink`, so no `nlink == 2` window and no staged name left
  behind; a lock file with mode 0400 or 0200 is accepted where exactly 0600
  was required; on macOS durability goes through `F_FULLFSYNC`. The pin
  is maelys-system 0.8.0, which also brings `unlink_same` and
  `rmdir_same`; the adversarial suite is unchanged and green.
- Release socle maelys-release v0.6.1 adopted: signed-tag verification,
  Linux x86_64, Linux arm64 and macOS arm64 packages with provenance,
  GitHub release with `SHA256SUMS`, CI from the same declarations
  (`.github/workflows/ci.yml` calling `check-product.yml`, which reads
  the declarations itself since 0.6.0). Dependency
  pins move to `adapter/` (tag and commit) and are fetched by the socle's
  managed `scripts/checkout-dependency.sh`; `adapter/PACKAGES` declares
  the system packages; `scripts/package-release.sh TARGET`; `make check`
  runs `maelys-release check`.
- `make asan-ubsan` is the sanitizer target (the socle's CI default);
  `make sanitizers` remains an alias.
- Mbed TLS without pkg-config files (Debian 2.28) links through the
  default library names.
- The committed CLI contract is host-neutral: the reference generator of
  maelys-cli 0.5.7 describes the Linux-only `unpack-rootfs` as available
  on every host (`--neutral-availability`), so `contract-check` passes on
  macOS and Linux with one file and without a product script.
- Every object depends on `VERSION`: a version bump rebuilds the binaries
  that embed it (the 0.2.0 build reused 0.1.0 objects locally; the CLI
  test now reads `VERSION` instead of a hard-coded number).

## 0.2.0 - 2026-09-03

### Added

- Build-variant seam: `src/cli/extension.h`, `OCI_EXTENSION_SOURCES`,
  catalog composed with `maelys_cli_catalog_concat()` (maelys-cli 0.5.6;
  an extension may provide an `.unavailable` command or add new ones, never
  shadow a provided one), store root entries and artifact members declared
  by the variant and accepted by `verify`; proven by `make extension-check`.
- maelys-cli 0.5.6 features: `absolute-path` and `digest` kinds replace
  the hand-written path and digest checks; typed defaults
  (`MAELYS_CLI_DEFAULT_OF`) bind `--grace-seconds` and `--timeout-ms` to the
  library constants; `maelys_cli_replied()` and
  `maelys_cli_resolve_helper()` replace the private reply convention and
  the `argv[0]` plumbing; `unpack-rootfs` is `.unavailable` outside Linux
  and stays described; documents are emitted through the trusted writers;
  `MAELYS_CLI_FORMAT` and catalog-driven shell completion
  (`maelys-oci completion bash|zsh|fish`, installed by `make install`).
- `LICENSING.md` and `SPDX-License-Identifier: MPL-2.0` on every source
  file.

### Changed

- Dependency pins: maelys-cli 0.5.6, maelys-system 0.5.4, maelys-json
  0.1.2. A pin bump now rebuilds the dependency it names.

## 0.1.0 - 2026-09-03

First release of the extracted repository. Relative to the Warden-embedded
tools it replaces:

### Changed (breaking CLI contract)

- `maelys-oci` is now built on `libmaelys_cli` and honours the
  `agent-cli/v2` contract: `describe`, `--format json|jsonl`, `--compact`,
  `--non-interactive`, envelopes on stdout (success) and stderr (failure),
  exit codes `0`, `1` and `2`. The sysexits codes (64, 65, 69, 73) are gone.
- `import`, `gc` and `remove` are plan/apply transactions: they report a
  plan by default and write only with `--apply`. `gc --dry-run` is refused
  with the migration hint; use `gc` for the plan.
- `verify` and `gc` exit `2` with `data.valid == false` when the store has
  integrity errors instead of failing.
- `list` emits one record per artifact (`data.count`, `data.records`);
  `--format jsonl` streams them.
- There is no prose output any more. Without `--format` a command prints
  its schema-described `data` document indented; `--format json` wraps it
  in the envelope. The historical parseable lines (`oci@DIGEST PLATFORM
  PATH`, `removed oci@DIGEST OS/ARCH`) are gone; scripts read the `data`
  members described by `schemas/*.json` and `docs/cli-contract.json`.
- `pull` imports the acquired layout in-process instead of re-entering the
  materializer through a fake `argv` and a captured stdout.
- Diagnostics name `maelys-oci`, not the historical Warden helper names.
- The pkg-config file declares `-lmaelys-json` and the private system
  libraries the archive actually needs.

### Added

- `share/maelys/commands/oci.json` manifest (`maelys.cli-extension/v1`) with
  the executable digest, installed under `PREFIX/share/maelys/commands/`.
- `tests/test_oci_cli.sh`: catalog, validation order, envelopes, plan/apply
  doctrine and dispatch through `maelys oci`.
- `make contract-check`, `make generate-cli-reference`, `make agents-install`.
- The store lifecycle tests (leases, gc plan/apply, remove plan/apply) now
  run on every `make check`; they previously depended on the Warden CLI.

### Fixed

- Every path is formatted through a truncation-checking `oci_snprintf`.
- The FILE stream of `write_json_file` was leaked on a failed write.
- `make sanitizers` no longer drops the language level, warnings and
  feature macros when `CFLAGS` is overridden on the command line.
- `-Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2`
  are enforced; external headers are system headers.

### Internal

- One artifact seal reader and writer (`src/store/seal.c`) replaces the two
  hand-written parsers of the artifact API and of `verify`.
- One OCI vocabulary (`src/common/descriptor.c`) replaces the duplicated
  digest, platform, media-type and descriptor parsing of the materializer
  and the puller.
- Library operations report failures through `oci_error_t`; no library
  function prints.
