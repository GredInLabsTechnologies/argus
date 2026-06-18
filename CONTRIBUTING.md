# Contributing to Argus

Thanks for taking the time. Argus is the virtual-display engine behind [Istro](https://github.com/GredInLabsTechnologies),
a small fork of [VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
focused on one thing: a **deadlock-free, on-demand monitor hotplug** lifecycle for IddCx.

## Building

Argus is a user-mode UMDF/IddCx driver. Two supported ways to build:

- **Enterprise WDK (EWDK)** locally — no Visual Studio install required. Open the EWDK environment and
  build `Virtual Display Driver (HDR)/MttVDD.sln` (Release, x64 or ARM64).
- **CI / WDK NuGet** — the [`ci-validation.yml`](.github/workflows/ci-validation.yml) workflow builds
  from source on the `windows-2025-vs2026` runner using the WDK NuGet package. It's reproducible: push
  a branch or open a PR and the same build runs.

The CI validates that the driver **compiles** (x64 + ARM64). WHQL-grade static analysis (PREfast),
INF verification and ApiValidator are disabled *only* for the NuGet build (their x86 plug-ins don't load
on hosted runners — see `Directory.Build.targets`); a local EWDK build keeps all of them on.

## The one invariant you must not break

Argus's whole reason to exist is correct, deadlock-free monitor hotplug. The lock order is **strictly
acyclic** and is load-bearing:

```
g_DeviceContextMutex  →  m_MonitorsMutex  →  (IddCxMonitorDeparture is called OUTSIDE m_MonitorsMutex)
```

`IddCxMonitorDeparture` must never be called while holding `m_MonitorsMutex`, because the framework's
teardown path (`UnassignSwapChain`) reaches back for that lock and you'll self-deadlock. If you touch
`AddMonitor` / `RemoveMonitor` / the watchdog, keep this order and keep departure outside the lock. The
rationale is documented inline in `Driver.cpp`.

## Pull requests

- Keep changes scoped and explain the *why*, not just the *what*.
- Don't regress the upstream feature set (HDR, EDID, custom resolutions) — Argus is a superset.
- Preserve attribution: upstream copyright stays intact (see [`LICENSE`](LICENSE)).
- If you change the control protocol (`ADD` / `REMOVE` / `PING` / `SETDISPLAYCOUNT`) or the pipe SDDL,
  say so explicitly — consumers depend on it.

## Reporting issues

Open an issue with: Windows build, how the driver was installed (signed release vs. test-signed), and
whether a controller (e.g. Istro) was driving the control pipe. Logs from the driver's `vddlog` output
are gold.

## Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).
