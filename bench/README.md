# Argus benchmark & validation harness

Turns the README's qualitative claims into **measured, reproducible numbers** — and deliberately
probes the failure modes, not just the happy path.

## What it measures

| Probe | Metric | Method |
|---|---|---|
| `argus-bench.ps1` (external) | **ADD round-trip** | pipe connect→`ADD`→reply, `Stopwatch` (QPC) |
| | **ADD → visible** | poll `GetSystemMetrics(SM_CMONITORS)` and `GetDisplayConfigBufferSizes(QDC_ALL_PATHS)` until the new monitor appears |
| | **REMOVE round-trip / → gone** | symmetric |
| | **PING round-trip** | the protocol/transport floor — subtract it mentally from ADD/REMOVE |
| | **no adapter reload** | pre-existing monitors compared byte-for-byte (DeviceName+Bounds) before/after an ADD; must be untouched |
| | **no re-enum storm** | `SystemEvents.DisplaySettingsChanged` events per single ADD (a reload re-configs everything) |
| | **cold vs warm** | first ADD after idle-0 reported separately from steady state |
| Driver `BENCH` log probes (internal) | **pure `IddCxMonitorArrival` / `IddCxMonitorDeparture` latency** | `QueryPerformanceCounter` bracketing the IddCx call in `Driver.cpp`; isolates driver time from OS propagation |

Timing is reported as **min / p50 / p95 / p99 / max / mean** — percentiles, not just an average,
because tail latency is what users feel.

## Edge cases it exercises (on purpose)

None of these may crash the UMDF host or unplug live monitors:

- `REMOVE` with nothing live, negative index, huge index, missing index, non-numeric index → all `ERR`
- garbage / empty / binary commands → `ERR`, **and live monitors stay live** (regression guard for the
  historical "one bad pipe message unplugs everything" crash)
- double `REMOVE` of the same index → `OK` then `ERR`
- legacy `SETDISPLAYCOUNT` (no reply) → must not hang the client
- **saturation**: `ADD` until `ERR` — records `MAX_MONITORS` and that it degrades gracefully
- **burst**: 50× rapid ADD/REMOVE churn, then `PING` → must stay responsive (`PONG`), 0 errors
- **watchdog**: persists with no PING; self-heals (retires all monitors) ~`g_WatchdogTimeoutSeconds`
  after PINGs stop once armed
- last `REMOVE` returns cleanly to idle-0

## Running it

Prerequisites: the Argus driver is **loaded and active** (the control pipe `\\.\pipe\ArgusDisplay`
exists), on a Windows box (test-signing is fine — SignPath is **not** required to benchmark).

```powershell
# elevated PowerShell (the pipe SDDL is SYSTEM + Administrators only)
cd argus\bench
.\argus-bench.ps1 -Iterations 200 -Warmup 20            # full run
.\argus-bench.ps1 -Iterations 50 -SkipWatchdog          # quick run, skip the slow watchdog waits
```

Results land in `bench\results\argus-bench-<timestamp>.json` (+ `.csv` for the timing table).

### Extracting the internal (driver-side) probes

The probes are **compiled out of release builds**. Build the driver with `-DARGUS_BENCH` (MSBuild:
`/p:DefineConstants=ARGUS_BENCH` or add `ARGUS_BENCH` to the project's preprocessor definitions) to
include them. Such a build logs one `BENCH IddCxMonitorArrival idx=… us=…` /
`BENCH IddCxMonitorDeparture …` line per operation; pull them from the driver log (`vddlog` output)
to get pure arrival/departure latency, independent of pipe + OS-propagation overhead. **Production /
release binaries ship without any benchmark instrumentation.**

## Honesty notes / limitations

- **PowerShell floor.** Per-call PS + managed-pipe overhead is on the order of a few tenths of a ms.
  The `PING round-trip` row is exactly this floor — if ADD/REMOVE land in the same order of magnitude,
  re-measure with the internal `BENCH` probes (QPC inside the driver) before quoting a number.
- **"Visible" is two things.** A monitor can *arrive* (IddCx) before Windows *extends the desktop* onto
  it. We measure both `SM_CMONITORS` (active desktop) and total connected paths, and the internal probe
  measures arrival itself. Quote whichever the claim is actually about.
- **No-flicker** on the *physical* outputs is not captured here (it needs an external capture); we
  instead assert the structural proxies (pre-existing monitors untouched, one display-change event).
