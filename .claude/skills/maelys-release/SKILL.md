---
name: maelys-release
description: Release a Maelys product through the shared maelys-release socle, or change its release, packaging, dependency pins or Homebrew formula files, without breaking the signed-tag, attestation and tap contracts.
---

<!-- SPDX-License-Identifier: CC-BY-4.0
Copyright 2026 David Bromberg.
Source: https://github.com/maelys-dev/maelys-release/blob/main/share/agents/claude-skill.md
License: https://creativecommons.org/licenses/by/4.0/
When sharing adaptations, retain attribution and indicate your changes.
-->

# Releasing a Maelys product

The product is `maelys-oci`; its release mechanics come from maelys-release
at the version `.github/workflows/release.yml` pins (`docs/conventions.md`
there is normative).

## Cut a release

1. On `main`, up to date with `origin/main`: title the `CHANGELOG.md` entry
   `## X.Y.Z — YYYY-MM-DD` with the date of the day, never a later one
   (`check` notes a title `cut` will not read; `cut` refuses it, and a
   date in the future), regenerate any generated documentation, and
   commit everything else. Until that day the changes sit under
   `## Unreleased`. `VERSION` is written by `cut`, whose commit carries
   the bump and nothing else: until then it says what was published
   last, `0.0.0` before a first release, and a `VERSION` moved by hand
   to the version to cut is refused as not coming after itself.
2. Run `bin/maelys-release cut . X.Y.Z --apply` from a maelys-release
   checkout at the version you want cutting it: `cut` runs as that
   checkout, not as the socle this repository pins, and its first line
   says which. It exits 2 on
   anything the workflow would refuse (signing configuration, previous
   tag, existing `vX.Y.Z`, `release` environment not limited to tags
   `v*`) before writing anything, then commits `VERSION` signed on
   `release/vX.Y.Z`, opens the pull request and waits for the checks of
   that commit to exist and to finish. Without `--apply` it reads the
   release as `cut` will and changes no file of the product and nothing
   on GitHub — it fetches, so it needs `gh`, the network and a `main`
   level with `origin`. Run it that way first. What it says of the
   version, the entry and the worktree is about what was prepared: fix
   that. Its lines on `tag.gpgsign`, `user.signingkey` and the signing
   key are the signer's own, and exit 2 on those alone is expected of
   anyone else: leave them, and never configure a key to turn them
   green. The verify command, `[cut] after-version`, GitHub's verdict on
   the release commit and the checks run under `--apply` only.
3. Merge that pull request under this repository's own rules. `cut` never
   merges its own: a command that did would work only where the default
   branch is unprotected.
4. Run `bin/maelys-release cut . X.Y.Z --tag --apply`. It verifies the
   merge commit of that pull request is on `main` and carries `VERSION` =
   `X.Y.Z`, waits for every check of that exact commit, then signs
   `vX.Y.Z` on it — the merge commit, never `origin/main`, which another
   merge can move between the two — and pushes it. The tag is annotated
   and signed with a key registered on GitHub; the workflow refuses
   anything else.
5. Watch the `release` workflow; the `publish` job runs in the `release`
   environment. Verify with `gh release view vX.Y.Z` and
   `gh attestation verify <asset> --repo <owner>/<repo> --signer-repo maelys-dev/maelys-release`
   (the attestation is signed by the socle's reusable workflow, so `gh` must
   be told that signer; without it verification fails with "verifying with
   issuer sigstore.dev").

## Before the first tag, or after changing packaging

Run `bin/maelys-release rehearse . linux-arm64` (and `linux-x86_64`,
emulated) from a maelys-release checkout: it replays the Linux build job of the
release in Docker on a copy of the working tree. The release must never be
the first Linux build of the product.

## Change release or packaging files

- `.github/workflows/release.yml` and the two `scripts/checkout-dependenc*.sh`:
  never by hand. They are generated from `dependencies/*.pin`, `dependencies/packages`
  and `packaging/homebrew/*.rb.in`; change those, then run
  `bin/maelys-release adopt DIR --apply` from a maelys-release checkout at
  the target tag; check drift with `bin/maelys-release check DIR`. Every
  command answers `--format json` with an agent-cli/v2 envelope,
  `describe --format json` returns the catalog, and `--field NAME` reads one
  member of the result without a `jq` expression.
- `dependencies/<name>.pin`: pinned commit on line 2, and on line 1 the tag
  that commit carries — never the nearest tag of a commit that carries
  none, which `maelys-release dependencies` refuses. A Maelys repository is
  pinned on a tag. A third party's commit that no tag carries yet adds
  `repository <https URL>` and `unreleased <branch>`, with the last tag
  before it on line 1. `scripts/checkout-dependency.sh NAME` clones the
  dependency at that commit; write no other checkout script.
- `dependencies/packages`: the apt (`[linux]`) and brew (`[macos]`) packages the
  build needs, one per line. Nothing else installs packages in a release
  or in CI: `.github/workflows/ci.yml` calls the socle's
  `check-product.yml`, which reads the declarations itself; keep that job,
  add yours next to it. `adopt` updates its socle line, `check` warns when
  it is missing.
- `scripts/package-release.sh TARGET`: must leave every artifact and its
  `.sha256` in `dist/`; keep it runnable locally.
- packaging/homebrew/libmaelys-oci.rb.in, packaging/homebrew/maelys-oci.rb.in: placeholders `@URL@`, `@VERSION@`,
  `@SHA256@`, plus any pin placeholder your renderer fills. Validate with
  `brew style`, `brew audit --strict` through a temporary tap, and
  `brew install --build-from-source` followed by `brew test`.
- Bottles: the tap workflow builds them on the macOS runners listed in
  `bottles`; the formula keeps its source URL so `--build-from-source`
  always works for an open product.

## Never

- push a tag whose commit did not pass `make check`;
- force-push or delete a published tag;
- edit a generated file by hand: `release.yml`, either `checkout-dependenc*.sh`,
  anything under `docs/generated/`;
- install packages from a checkout script or from `package-release.sh`;
  declare them in `dependencies/packages`;
- put credentials in the repository; the tap secrets are
  `HOMEBREW_TAP_TOKEN` and `HOMEBREW_TAP_SIGNING_KEY`;
- narrow a protection to get an adoption through: the legs renamed in 0.54.0
  keep reporting their old names as aliases, so adopt, merge, then
  `protect . --apply` replaces each alias by its leg in one write.
