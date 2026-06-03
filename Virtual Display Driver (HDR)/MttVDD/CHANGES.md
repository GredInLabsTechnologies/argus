# CHANGES — On-demand monitor refactor (Istro/argus fork of MttVDD)

This documents the refactor that turns MttVDD's "fixed monitor count at boot + full
adapter reload" model into a spacedesk-style **idle-at-0 / add-remove-one-by-index at
runtime** model, with **no adapter reload**.

The MIT notice at the top of `Driver.cpp` is preserved. All previously-working pipe
commands and IddCx callbacks are preserved; only the monitor lifecycle and the display-count
control path changed.

---

## SudoVDA evaluation (STEP 0) — decision: ADOPT MECHANISM, KEEP ARGUS TRANSPORT

`SudoMaker/SudoVDA` is a full rewrite of MttVDD and already implements exactly the target
behavior: idle-at-0, per-monitor add/remove, free-index reuse, a 3s watchdog, and
`IddCxMonitorArrival`/`IddCxMonitorDeparture`.

- **License:** "MIT and CC0 or Public Domain … choose the least restrictive option."
  Fully permissive — portable into our MIT fork with attribution. No blocker.
- **Mechanism (confirmed by reading its source):**
  - `AdapterCaps.MaxMonitorsSupported = MaxVirtualMonitorCount` (registry `maxMonitors`,
    default 10), decoupled from any boot count.
  - Idle-at-0: no monitors created at `AdapterFinishInit`.
  - `CreateMonitor` → `IddCxMonitorCreate` + `IddCxMonitorArrival`; remove →
    `IddCxMonitorDeparture`; a `freeConnectorSlots` queue reuses indices.
  - Control plane is a **kernel IOCTL device interface** (`Common/Include/sudovda-ioctl.h`:
    `IOCTL_ADD_VIRTUAL_DISPLAY`, `IOCTL_REMOVE_VIRTUAL_DISPLAY`, `IOCTL_DRIVER_PING`,
    `IOCTL_GET_WATCHDOG`, …), keyed by **MonitorGuid**.
  - Watchdog: a `std::thread` decrements a countdown each second, reset on every IOCTL;
    on expiry it departs all monitors. Default 3s, `watchdog=0` disables.

**Why we did NOT wholesale-port SudoVDA:** argus's control plane is a **named pipe**
(`\\.\pipe\MTTVirtualDisplayPipe`) and the Rust SYSTEM broker is contractually wired to
ASCII/UTF-16 text commands (`ADD`, `REMOVE <index>`, `PING`) that return an **integer index**.
The task fixes this contract ("the Rust broker codes to this"). Replacing the pipe with
SudoVDA's GUID-keyed IOCTL interface would force a rewrite of the Rust broker and break the
contract. SudoVDA also carries a large amount of unrelated rewritten surface (EDID generation,
registry config, render-adapter selection) that would be a risky drop-in over argus's existing
HDR/gamma/EDID code.

**Decision:** hand-refactor argus per the change plan, **adopting SudoVDA's proven patterns**
(idle-at-0, `Arrival`/`Departure`, free-index reuse, separate list mutex, depart-outside-lock,
1Hz countdown watchdog) while keeping argus's pipe transport and the exact text contract.

---

## File-by-file diff summary

### `Driver.h`
- Added `MAX_MONITORS` (default 10) — the fixed adapter ceiling, decoupled from the configured
  display count.
- Replaced `IDDCX_MONITOR m_Monitor; m_Monitor2;` with
  `std::map<UINT, IDDCX_MONITOR> m_Monitors;` + a **dedicated** `std::mutex m_MonitorsMutex;`
  (kept separate from `m_ProcessingThreadsMutex`).
- New public API: `bool AddMonitor(UINT index)`, `bool RemoveMonitor(UINT index)`,
  `void RemoveAllMonitors()`, `int LowestFreeIndex()`, `size_t LiveMonitorCount()`.
- New private helper: `IDDCX_MONITOR CreateMonitorObject(UINT index)` (create + arrival, no
  bookkeeping/locks). `CreateMonitor(unsigned int)` is retained as a thin shim to `AddMonitor`.

### `Driver.cpp`
- **Globals/state:** added `g_DeviceContext` (+ `g_DeviceContextMutex`) cached in `InitAdapter`
  so the pipe/watchdog thread can reach the live context without a WDFOBJECT; added watchdog
  state (`g_WatchdogTimeoutSeconds=3`, atomic countdown/running, thread handle). Added
  `g_HdrMetadataStoreMutex` and `g_GammaRampStoreMutex` to guard the per-monitor side tables
  now that `RemoveMonitor` erases them off-thread. Added `<atomic>`/`<thread>` includes.
- **Type-confusion bug FIXED:** the old `ReloadDriver(HANDLE hPipe)` called
  `WdfObjectGet_IndirectDeviceContextWrapper(hPipe)` — passing a pipe HANDLE where a WDFOBJECT
  is required. Replaced with `GetDeviceContext()` reading the cached pointer. `ReloadDriver` is
  now a logged no-op shim (the on-demand model never reloads the adapter), so the ~14 existing
  call sites still compile and behave (settings apply on next `AddMonitor`).
- **Idle-at-0:**
  - `InitAdapter`: `AdapterCaps.MaxMonitorsSupported = MAX_MONITORS` (was `numVirtualDisplays`).
  - `FinishInit`: creates **0** monitors by default. If a config explicitly sets
    `numVirtualDisplays > 0` it pre-connects that many (capped at `MAX_MONITORS`) for backward
    compatibility.
  - Config loader: a `<count>` of `0` is now **valid** (= idle) instead of being forced to 1.
- **AddMonitor / RemoveMonitor:**
  - `AddMonitor` = old `CreateMonitor` body (now `CreateMonitorObject`) + bookkeeping; rejects
    `index >= MAX_MONITORS` or an already-live index; stores the handle in `m_Monitors[index]`
    under `m_MonitorsMutex`; reuses freed indices via `LowestFreeIndex`.
  - `RemoveMonitor` = under lock fetch+erase the handle, **UNLOCK**, then call
    **`IddCxMonitorDeparture(handle)`** (NET-NEW primitive — never called in the original),
    then erase from `g_HdrMetadataStore` + `g_GammaRampStore`.
  - `RemoveAllMonitors` snapshots indices under lock then removes each (used by the watchdog and
    the context destructor).
- **LOCK DISCIPLINE:** `IddCxMonitorDeparture` is **never** called while `m_MonitorsMutex` or
  `m_ProcessingThreadsMutex` is held (departure can synchronously call `UnassignSwapChain`,
  which takes `m_ProcessingThreadsMutex` → self-deadlock). Each removal path locks, copies the
  handle, unlocks, then departs.
- **Pipe control (CONTRACT):** in `HandleClient`, `SETDISPLAYCOUNT→ReloadDriver` is superseded by:
  - `ADD` → `AddMonitor(LowestFreeIndex())`; writes the chosen index back to the pipe client as an
    ASCII integer string via `SendToPipe` (or `ERR` on failure). ASCII chosen so every on-demand
    reply (`<index>`/`OK`/`ERR`/`PONG`) is uniform; the task allows ASCII or UTF-16.
  - `REMOVE <index>` → `RemoveMonitor(index)`; replies `OK`/`ERR`.
  - `PING` → kicks the watchdog and replies `PONG` (unchanged contract).
  Every command kicks the watchdog. `SETDISPLAYCOUNT` is kept but now only **persists** the count
  (no reload). All other commands (LOGGING, HDRPLUS, SDR10, CUSTOMEDID, SETGPU, GETSETTINGS, …)
  are unchanged.
- **Watchdog:** 1Hz countdown thread reset on every command/PING; on expiry calls
  `RemoveAllMonitors()` (parsec-vdd/SudoVDA self-heal). Started in `StartNamedPipeServer`,
  stopped in `StopNamedPipeServer`. Timeout 3s; set `g_WatchdogTimeoutSeconds = 0` to disable.
- **SDDL tightened:** pipe ACL `D:(A;;GA;;;WD)` (Everyone) → `D:(A;;GA;;;SY)(A;;GA;;;BA)`
  (SYSTEM + Administrators only).
- **Tier-1 wins:**
  - Set `IDDCX_ADAPTER_FLAGS_ALL_TARGET_MODES_MONITOR_COMPATIBLE` in `AdapterCaps.Flags`
    (`#ifdef`-guarded for older IddCx headers) so the exact tablet panel resolution survives mode
    filtering.
  - Per-monitor container ID is now **stable per connector index** (`MakeStableContainerId`)
    instead of a random `CoCreateGuid` every plug, so Windows remembers each tablet's layout
    across add/remove cycles.

---

## What compiles / build status

**Not built on this machine.** No WDK is installed: `IddCx.h` and the IddCx class-extension
headers are absent from the SDK include tree (`...\Windows Kits\10\Include\10.0.26100.0\um\`
has no `idd*` headers), and the WDK MSBuild integration / `WindowsUserModeDriver10.0` toolset
required by `MttVDD.vcxproj` is not present (VS2022 BuildTools only). Therefore `MttVDD.sln`
(Release x64) cannot be compiled here.

The edits were written for **IddCx API correctness** against the documented signatures
(`IddCxMonitorCreate`, `IddCxMonitorArrival`, `IddCxMonitorDeparture`,
`WdfObjectGet_IndirectDeviceContextWrapper`, `IDDCX_ADAPTER_CAPS`) and cross-checked against the
working SudoVDA implementation. **Building on a WDK-equipped machine is a required follow-up.**

### Build follow-up checklist
1. Install the WDK matching the SDK (10.0.26100) + the "Windows Driver Kit" VS extension.
2. Open `MttVDD.sln`, build **Release | x64**.
3. Expect to verify: the `#ifdef IDDCX_ADAPTER_FLAGS_ALL_TARGET_MODES_MONITOR_COMPATIBLE` guard
   resolves on this IddCx version; no `/WX` warning from the watchdog branches (timeout is a
   runtime `int`, not `constexpr`, specifically to avoid C4127).

---

## Remaining TODOs / edge cases

- **Build + HW validation** (above) — never loaded/tested on this machine, as instructed.
- **Stable identity per tablet:** `MakeStableContainerId(index)` is stable per *connector index*,
  not per *physical tablet*. If two tablets can occupy the same index across sessions, their
  remembered layouts could collide. TODO: have the `ADD` command carry a per-tablet token
  (serial/GUID) and derive the container ID + an EDID serial from it (SudoVDA passes
  `MonitorGuid`/`SerialNumber`/`DeviceName` in its ADD IOCTL — mirror that).
- **Exact panel EDID:** we still serve the single hard-coded `s_KnownMonitorEdid`. To truly give
  "the exact tablet panel resolution", the `ADD` path should accept width/height/refresh and
  generate a matching EDID (see SudoVDA `generate_edid`). Left as a follow-up; the
  monitor-compatible flag is in place to support it.
- **HDR/gamma store locking:** the erase path and the structural mutations in the
  set-HDR-metadata / set-gamma callbacks are now mutex-guarded. The remaining read-only
  `.size()` diagnostic logging is unguarded (benign). If those callbacks ever start running
  concurrently for the *same* monitor (IddCx serializes per-monitor today), revisit.
- **Watchdog auto-disable while idle:** the watchdog only barks when `LiveMonitorCount() > 0`, so
  an idle adapter with no broker is harmless. If the broker intends long idle periods without
  PING while holding monitors, raise `g_WatchdogTimeoutSeconds` or have it keep PINGing.
- **`numVirtualDisplays` semantics:** still read from XML/option.txt as a pre-connect hint. A
  broker that wants pure idle-at-0 must write `<count>0</count>` (now honored). The
  no-config-at-all fallback still defaults to 1 for standalone use.
