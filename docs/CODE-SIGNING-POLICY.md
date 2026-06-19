# Code Signing Policy — Argus

This document describes how the official **Argus** driver binaries are built, signed, and verified.
It exists so users can trust the provenance of a signed release, and to support the project's
[SignPath Foundation](https://signpath.org) application.

## Scope

Applies to the official Argus driver package distributed via the GitHub Releases of
[`GredInLabsTechnologies/argus`](https://github.com/GredInLabsTechnologies/argus):
`MttVDD.dll`, `mttvdd.cat`, `MttVDD.inf`, and `vdd_settings.xml`, for x64 and ARM64.

## Signing service

- Release binaries are signed with **Authenticode (OV)** through **SignPath Foundation**.
- The private key is generated and held on **SignPath's HSM**. The Argus maintainers **never
  receive, export, or handle the key**.
- The certificate is issued to **SignPath Foundation** on behalf of the project (standard for the
  Foundation's free OSS program).
- Argus is a **user-mode** UMDF/IddCx driver, so OV Authenticode is sufficient to load it without
  test-signing on x64; it does **not** require kernel attestation/EV.

## Build provenance

- Every release artifact is **built from source by the public CI**
  ([`.github/workflows/ci-validation.yml`](../.github/workflows/ci-validation.yml)) on
  GitHub-hosted runners. **No local or otherwise unverifiable builds are released.**
- GitHub Actions records the exact **commit** and **workflow run** that produced each artifact.
- The build is **reproducible**: anyone can re-run the workflow from the tagged source and obtain
  the same driver.

## Roles & authorization

- **Maintainer:** Gred In Labs Technologies — `gredinlabstechnologies@gmail.com`.
- Only a maintainer may authorize a signing request.
- **Every signing request requires explicit manual approval** in SignPath (per-release approval).
  There is **no automatic / unattended signing**.

## Release & signing flow

1. A release commit is tagged on `master`.
2. CI builds the driver from source (x64 + ARM64) and uploads the **unsigned** artifact.
3. After **manual maintainer approval**, SignPath signs the driver catalog (`mttvdd.cat`).
4. The **signed** artifact is attached to the GitHub Release.

The CI already contains the SignPath signing steps, gated on the project's SignPath secrets/variables
(inactive until enrollment). Setup details: [`docs/SIGNING.md`](SIGNING.md).

## Verifying a signed build

- File Explorer: right-click `MttVDD.dll` or `mttvdd.cat` → **Properties → Digital Signatures**.
- Command line: `signtool verify /pa /v mttvdd.cat`
- The signer is **SignPath Foundation** (the OV certificate holder); the certificate description
  names the Argus project.

## Security & reporting

- The signing key never leaves SignPath's HSM; a leaked maintainer credential cannot exfiltrate it.
- Report a suspected compromised release, misuse, or a binary that fails signature verification to
  `gredinlabstechnologies@gmail.com`.
