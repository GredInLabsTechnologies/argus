# SignPath Foundation — application package for `argus`

Everything needed to submit argus to the [SignPath Foundation](https://signpath.org) free OSS code-signing
program, so releases load **without test-signing** on x64. Prepared 2026-06-18.

> **Prerequisite (the real blocker):** SignPath requires the project to *"already be released in the form
> that should be signed."* argus must therefore have a **published GitHub release** (v0.1.0) with the
> driver artifact **before** the application is sent. The release can ship the not-production-signed `.cat`
> (it carries only an untrusted WDK test signature); SignPath signs it afterwards. See "Order of operations" below.

## Eligibility checklist (argus vs. SignPath's criteria)

| SignPath requirement | argus | Notes |
|---|---|---|
| No malware / PUP | ✅ | A virtual display driver (IddCx); functionality openly documented. |
| OSI-approved license, **no commercial dual-licensing** | ✅ | MIT (`LICENSE`), single license. |
| No proprietary / non-OSS components | ✅ | Fork of MIT `Virtual-Display-Driver`; all sources public. |
| Actively maintained | ✅ | Recent commits; green CI. |
| **Already released in the form to be signed** | ⏳ | **Needs the v0.1.0 release published first.** |
| Functionality described on the download page | ✅ | `README.md` (what it is, how it works, diagrams). |
| Built from source verifiably | ✅ | Public CI (`.github/workflows/ci-validation.yml`), reproducible. |
| Per-release manual sign approval | ✅ | Accepted — that's the SignPath model. |

## Application form — field → value

Submit at <https://signpath.org/apply> (form, sent by email). Suggested answers:

- **Project name:** Argus
- **Project description:** An open-source Windows Indirect Display Driver (IddCx) that adds and removes
  virtual monitors on demand — idle-at-0 adapter with live, deadlock-free per-index hotplug. The
  virtual-display engine behind the Istro project.
- **Repository URL:** https://github.com/GredInLabsTechnologies/argus
- **License:** MIT (OSI-approved)
- **Upstream / fork:** Fork of https://github.com/VirtualDrivers/Virtual-Display-Driver (also SignPath-signed)
- **Programming languages / build:** C++ (UMDF/IddCx driver), built with the WDK via MSBuild in GitHub Actions
- **Artifact to sign:** the driver catalog `mttvdd.cat` (covers `MttVDD.dll` + `MttVDD.inf`), x64 (and ARM64)
- **Maintainer / contact:** Gred In Labs Technologies — gredinlabstechnologies@gmail.com (signing@giltech.dev)
- **Why signing is needed:** so the IddCx user-mode driver loads without enabling test-signing on end-user
  x64 machines (exactly as the upstream does). User-mode → OV Authenticode suffices; no EV/attestation.
- **Build reproducibility:** the public CI builds the driver from source; the artifact uploaded for signing
  is produced by that workflow.

## Order of operations

1. **Publish release v0.1.0** on GitHub (driver `.cat`/`.dll`/`.inf` per arch; a not-production-signed
   build is fine — note in the release that signing via SignPath is pending). This satisfies "already released".
2. **Send the application** (form above) referencing the repo + the v0.1.0 release.
3. **Wait for manual approval** (days).
4. On approval, in SignPath create: project (slug `argus`), an artifact-configuration that signs
   `mttvdd.cat` inside the uploaded artifact (slug `driver-cat`), and signing-policies `test-signing` +
   `release-signing` (the CI already references these slugs).
5. In GitHub → repo `argus` → Settings → Secrets and variables → Actions:
   - Secret `SIGNPATH_API_TOKEN` = the SignPath CI token.
   - Variable `SIGNPATH_ORGANIZATION_ID` = your SignPath organization GUID.
6. Push to `master` → CI builds, uploads the unsigned `.cat`, SignPath signs it (per-release approval),
   and the `VDD-…-SIGNED` artifact appears. The CI signing steps are already wired and inert until these
   secrets exist (see `docs/SIGNING.md`).

## Attribution (already in place)
The README footer credits SignPath ("Free code signing on Windows provided by SignPath.io, certificate by
SignPath Foundation") — SignPath requires this acknowledgement; it's already there.
