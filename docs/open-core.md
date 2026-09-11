# Mandatory open-core boundary

All work in this repository belongs to the MPL-2.0 product. A single workstation
or conventional CI can acquire, import, materialize and independently verify a
complete OCI image. No indispensable capability may be artificially weakened
for a paid edition.

The open product provides sequential OCI registry pull, immutable digest
references, explicit Linux arm64/amd64 selection, anonymous and standard Bearer
access, explicit tokens, Docker `auths`, verified HTTPS, safe redirects, bounded
transfers, exact size/digest checks, atomic local CAS publication, locks, leases,
GC and transactional crash recovery. Complete verified objects are reused;
partial downloads start again at byte zero. Import, materialization and
verification remain independent public operations.

A separately authored product may extend the uses of the open core while
keeping its own implementation proprietary. It belongs in a separate repository
and executable, and consumes the same public libraries, commands and artifact
contracts available to any other consumer. The open product remains usable
without that optional product; no feature category is reserved for a paid
edition by this boundary.

This is a separation of implementations and responsibilities, not a roadmap
for a proprietary edition. Extending the product here means composing it with
an external program or service, not installing an in-process `maelys-oci` plugin.
Any objects supplied by that external product must remain independently
verifiable by the open tool.
There are no opaque proprietary callbacks, enterprise preprocessor branches,
unused hooks, secret protocols or conditionals that weaken open behavior.

Any future API must be justified as a generic, safe public API available to
any consumer. The boundary consists of descriptors, store format, closures,
seals, public import/verify commands, digests and receipts, without privileged
store namespaces or bypasses in verification.

Repository policy keeps all maintained files under MPL-2.0. The future product
uses separate files and components in its own repository, consumes public
libraries/binaries/contracts, does not copy covered sources for private
modification, and returns direct modifications to MPL-covered files under MPL.
The exact license text remains authoritative: see [LICENSE](../LICENSE).
