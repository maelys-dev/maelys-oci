# Licensing

Copyright 2026 David Bromberg.

This repository's source, terminal, schemas, tests and documentation remain
under MPL-2.0. See [LICENSE](LICENSE) for the complete terms; maintained source
files carry `SPDX-License-Identifier: MPL-2.0`.

## Installed agent texts: CC-BY-4.0

The texts written for agents are CC-BY-4.0, attributed to David Bromberg: the
managed blocks of `AGENTS.md` and `CLAUDE.md`, every `.claude/skills/*/SKILL.md`
and `docs/maelys-cli-guide.md`, installed by maelys-release and maelys-cli.
Each carries its `SPDX-License-Identifier: CC-BY-4.0` notice with its source
and license link; when sharing adaptations, retain those notices and indicate
your changes. This license covers those texts alone: what this repository
writes outside a managed block stays MPL-2.0.

[The mandatory open-core boundary](docs/open-core.md) keeps the open product
complete for workstation and CI use. A future proprietary product uses a
separate repository and executable and consumes public contracts and verifiable
artifacts. This repository has no proprietary compiled extension mechanism.
Direct modifications of its MPL-covered files are kept under MPL-2.0.

The TLS fixture under `tests/fixtures/oci-registry/` is generated test data.
libarchive, e2fsprogs, Mbed TLS and the pinned Maelys libraries retain their own
licenses and are linked separately, never embedded in `libmaelys-oci.a`.
