# Security policy

## Reporting a vulnerability

Report a suspected vulnerability privately, through the GitHub advisory form
of this repository (`Security` tab, `Report a vulnerability`). Do not open a
public issue for it, and do not describe it in a pull request.

Include the version, platform, store layout and the smallest OCI layout or
registry response that reproduces the boundary violation.

Expect an acknowledgement within a few days. A confirmed report gets a fix,
a released version and a credit in `CHANGELOG.md` unless the reporter asks
otherwise.

## Supported versions

The latest released `vX.Y.Z` receives fixes. Older versions do not: the fix
lands in a new release, and a deployment upgrades to it.

## Scope

maelys-oci as published from this repository. A vulnerability in a pinned
Maelys dependency belongs to that repository; report it there.
