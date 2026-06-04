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
(`\\.\pipe\ArgusDisplay`, renamed from `MTTVirtualDisplayPipe` — see Fix 7 below) and the Rust SYSTEM broker is contractually wired to
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
  consumer that wants pure idle-at-0 must write `<count>0</count>` (now honored). The
  no-config-at-all fallback still defaults to 1 for standalone use.

---

## Code-review fixes (post-refactor pass)

Seven bugs found in review of the refactor above, fixed while keeping the driver **agnostic**
(it knows nothing about any specific consumer — any process with the right ACL drives the pipe)
and **IddCx-API-correct**. Still **not built here** (no WDK); written against documented IddCx
signatures. The MIT notice and all unrelated behavior are intact.

1. **Watchdog use-after-free (`WatchdogProc` → `LiveMonitorCount`/`RemoveAllMonitors`).**
   `GetDeviceContext()` mutex-protected only the *pointer read*, not the *object lifetime*: the
   watchdog dereferenced the context after releasing the lock, racing `~IndirectDeviceContext`'s
   `delete`. **Fix:** `~IndirectDeviceContext()` now calls `StopWatchdog()` (signal + JOIN the
   thread) **FIRST**, before clearing `g_DeviceContext` and before `RemoveAllMonitors()`. Once
   the join returns, the watchdog is guaranteed not to be mid-call into the dying context. The
   destructor holds no locks when it stops the thread, so there's no stop-vs-watchdog deadlock.
   Because the watchdog is process-global (started in `StartNamedPipeServer`), `InitAdapter` now
   restarts it idempotently after a device teardown+recreate. (D0Exit was deliberately *not*
   used as the stop hook — it fires on every sleep and would over-stop the watchdog.)

2. **D0Entry re-inited the adapter on EVERY D0 transition.** `EvtDeviceD0Entry` → `InitAdapter`
   called `IddCxAdapterInitAsync` on every wake from sleep, creating a SECOND adapter, overwriting
   `m_Adapter`, orphaning monitors bound to the first. **Fix:** new `m_AdapterInitialized` flag
   (Driver.h) guards the one-time init; it's set only on a successful `IddCxAdapterInitAsync`.
   Subsequent D0 entries log + return early (IddCx re-establishes swapchains itself per its D0
   semantics) — no second adapter.

3. **Settings stopped applying at runtime.** `HDRPlus/SDR10/customEdid/hardwareCursor/`
   `preventManufacturerSpoof/edidCeaOverride/HDRCOLOUR/SDRCOLOUR/ColourFormat/gpuname` were
   assigned only in `DriverEntry` and never refreshed; the "applies on next AddMonitor" comment
   was false. **Fix:** each setting pipe command (`HDRPLUS`/`SDR10`/`CUSTOMEDID`/`PREVENTSPOOF`/
   `CEAOVERRIDE`/`HARDWARECURSOR`/`SETGPU`) now updates the **in-memory global** directly (plus
   the derived `HDRCOLOUR`/`SDRCOLOUR`), not just the XML. `AddMonitor` now calls `maincalc()`
   to rebuild the EDID from the current globals, and the per-monitor description callbacks already
   read the colour/format globals live — so changes apply to every monitor **added after** the
   change. Existing monitors keep their description until re-added (documented; the false comment
   on `ReloadDriver` was corrected).

4. **Watchdog made OPT-IN (standalone-safe / agnostic).** Previously armed in `DriverEntry`, so a
   standalone install with monitors pre-connected and NO pinger lost its monitors ~3 s after boot.
   **Fix:** the watchdog thread starts **disarmed** (new `g_WatchdogArmed`). It NEVER barks until
   the **first PING** arms it (`WatchdogArm`, called only from the `PING` handler). `ADD`/`REMOVE`/
   `SETDISPLAYCOUNT` only *reset* the countdown **if already armed** — they never arm it. With no
   pinger, monitors persist unconditionally (normal driver behavior); a consumer that PINGs opts
   into the self-heal.

5. **Destructor INFINITE wait (`~SwapChainProcessor`).** `WaitForSingleObject(m_hThread, INFINITE)`
   on the teardown path could hang WUDFHost if the worker wedged. **Fix:** bounded the join to
   **5000 ms** after signaling the terminate event; on timeout it logs and proceeds (the OS
   reclaims the orphaned thread at process exit) instead of hanging forever.

6. **`g_pipeHandle` data race.** The handle was written by the pipe thread and read by the
   watchdog/log threads (via `vddlog`→`SendToPipe`), with the close+reset at the end of
   `HandleClient` unsynchronized — a write-to-closed-handle hazard. **Fix:** new dedicated
   `g_pipeHandleMutex` guards **every** access: `SendToPipe` holds it across the handle test +
   `WriteFile`; `HandleClient` sets it under the lock; the teardown clears it to `INVALID`
   **under the lock before** `CloseHandle` (so loggers see `INVALID` and skip); `vddlog` no longer
   reads `g_pipeHandle` directly (it just calls `SendToPipe`, which re-checks atomically). The
   mutex is leaf-level — never re-entered from under itself.

7. **Naming hygiene (agnostic).** Control pipe renamed `\\.\pipe\MTTVirtualDisplayPipe` →
   **`\\.\pipe\ArgusDisplay`** (the Rust consumer already uses this name). Removed cosmetic
   product-coupled wording ("tablet"/"MTT"/"the Rust broker") from strings/comments where it
   implied a specific consumer. The container-ID GUID **byte values are unchanged** (changing them
   would lose every display's remembered layout) — only the surrounding comments were neutralized.
   The INF hardware IDs / device class were **left intact** with a `TODO(inf)` at `PIPE_NAME`:
   renaming them is a coordinated, separately-signed package change and must not be done piecemeal.

### IddCx API points to verify on the WDK build
- **Fix 2 — D0 re-entry semantics:** confirm that on this IddCx version, *skipping* adapter re-init
  on a wake-from-sleep D0 entry (relying on IddCx to re-establish swapchains) is correct and that
  `m_Adapter`/monitors survive the low-power cycle without an explicit re-arm. (Driver was not run.)
- **Fix 1 — watchdog stop ordering:** `StopWatchdog()` (a `WaitForSingleObject` join) runs inside
  `~IndirectDeviceContext`, which executes from the WDF `EvtCleanupCallback`. Verify this join does
  not violate any IddCx/WDF teardown-context constraint (it takes no driver locks and the watchdog
  thread only calls public IddCx departure APIs, which are already exercised on this path).
- **Fix 5 — 5 s bound:** if a real worker legitimately needs >5 s to drain on this hardware, the
  timeout log will fire spuriously; tune `kSwapChainThreadJoinTimeoutMs` after observing real
  teardown timings.

---

## Round-2 fixes (adversarial re-verification of the post-refactor pass)

An adversarial verifier found that three of the previous fixes were **incomplete or introduced a
new concurrency bug**. These round-2 fixes replace timing-based mitigations with **lifetime
guarantees / mutual exclusion**. Still **not built here** (no WDK); written for IddCx/WDF API
correctness against documented signatures. MIT notice and all unrelated behavior intact.

### Round-2 Issue 1 — residual watchdog use-after-free (supersedes the old Fix 1)
**Problem.** The old Fix 1 relied on `StopWatchdog()` doing a **bounded** `WaitForSingleObject(
g_WatchdogThread, 2000)` inside `~IndirectDeviceContext`. That is a *timeout*, not a join: if
`WatchdogProc` was mid-`ctx->RemoveAllMonitors()` and exceeded 2 s, the wait timed out, the
destructor deleted the context, and the watchdog thread then dereferenced freed memory (UAF).

**Fix (mutual exclusion, not a longer timeout).**
- `WatchdogProc` now acquires **`g_DeviceContextMutex` and HOLDS it across the ENTIRE**
  `LiveMonitorCount()` + `RemoveAllMonitors()` sequence, reading `g_DeviceContext` under the lock
  and using it only while held.
- `~IndirectDeviceContext` sets `g_DeviceContext = nullptr` **UNDER `g_DeviceContextMutex` as the
  FIRST thing it does** (before `StopWatchdog()`, before `RemoveAllMonitors()`). Acquiring the
  mutex there blocks until any in-flight watchdog self-heal (holding the same mutex) finishes — and
  the context is still fully alive during that whole window. After the null, the watchdog's next
  iteration reads `nullptr` and skips.
- Delete and the watchdog's use of the context are now **mutually exclusive**: the watchdog either
  runs fully before the null (valid context) or sees null and skips. No UAF, and no unbounded hang
  (the work is the same depart sequence the destructor would otherwise run). `StopWatchdog()` is
  now only handle cleanup, not the correctness barrier.
- The bare `g_WatchdogThread` HANDLE store/close (StartWatchdog vs StopWatchdog race) is now guarded
  by a small leaf-level mutex **`g_WatchdogThreadMutex`** (copy-handle-under-lock, wait/close
  outside the lock so a concurrent StartWatchdog isn't blocked for the wait duration).

### Round-2 Issue 2 — Fix 3 introduced a data race (new `g_SettingsMutex`)
**Problem.** Fix 3 made settings apply at runtime by writing in-memory globals from the pipe
handlers, but those globals are read concurrently by IddCx callback threads with **no
synchronization**: the scalars `SDRCOLOUR`/`HDRCOLOUR` (written in the SDR10/HDRPLUS handlers, read
in `CreateTargetMode2` and `ParseMonitorDescription2`) → torn scalar reads; and the vectors
`s_KnownMonitorEdid` (reassigned by `maincalc()` from `AddMonitor`) and `s_KnownMonitorModes2`
(cleared+refilled by `RebuildKnownMonitorModesCache()` in the description callbacks) → container UB
(reassign/clear while another thread indexes).

**Fix.** New **`g_SettingsMutex`**.
- **WRITE-lock:** the `SDRCOLOUR`/`HDRCOLOUR` stores in the SDR10/HDRPLUS handlers; the
  `s_KnownMonitorEdid` reassignment in `maincalc()` (compute EDID outside the lock, take the lock
  only for the swap); and (implicitly via the read-side hold) the `s_KnownMonitorModes2` rebuilds in
  the two description callbacks.
- **READ-lock:** `CreateTargetMode2` snapshots `SDRCOLOUR`/`HDRCOLOUR` into locals under the lock;
  `CreateMonitorObject` **copies** `s_KnownMonitorEdid` into a local `edidSnapshot` under the lock,
  then releases the lock **before** `IddCxMonitorCreate` (pData points at the local, kept in scope
  through the call); `ParseMonitorDescription` and `ParseMonitorDescription2` **hold the lock across
  the rebuild AND the indexing** of `s_KnownMonitorModes2` (and the scalar reads) — both callbacks
  make no IddCx call, so this cannot re-enter the driver.
- **`ColourFormat` is intentionally NOT locked**: it is written ONLY at DriverEntry (load time,
  before any monitor/callback), so it is not raced. The diagnostic-only `monitorModes` is likewise
  load-time-only.
- **No `g_SettingsMutex` is ever held across an IddCx call that could re-enter the driver**
  (snapshot/copy under the lock, release, then call).

### Round-2 Issue 3 — Fix 5 timeout UAF (`SwapChainProcessor` lifetime hold)
**Problem.** Fix 5's bounded 5 s join in `~SwapChainProcessor` freed the object on `WAIT_TIMEOUT`
while the worker thread could still deref `this`/`m_Device`/`m_hSwapChain` and call
`WdfObjectDelete((WDFOBJECT)m_hSwapChain)` → UAF + possible double swap-chain delete.

**Fix (preferred refactor: `shared_ptr` lifetime hold).**
- `m_ProcessingThreads` is now `std::map<IDDCX_MONITOR, std::shared_ptr<SwapChainProcessor>>`
  (was `unique_ptr`). `SwapChainProcessor` derives `std::enable_shared_from_this`.
- The worker is no longer started in the constructor; a new `SwapChainProcessor::Start()` (called
  right after a `shared_ptr` owns the object, via `make_shared` in `AssignSwapChain`) hands the
  worker its **own `shared_ptr` copy** (a heap-allocated hold). `RunThread` adopts that hold into a
  local for its entire lifetime, so the object **can never be freed while the worker can touch it**.
- The destructor runs only when the **last** reference drops. If the worker held it, the destructor
  runs **on the worker thread** — so it **skips the self-join** (`m_ThreadId == GetCurrentThreadId()`);
  otherwise it does an **unbounded** join (safe: the worker already released its hold, so it has
  finished, and we are not on the worker thread). The free-after-timeout behavior is gone.
- Because the destructor may run on the worker (and thus can't be what *wakes* the worker), all
  teardown paths now **explicitly `SetEvent(m_hTerminateEvent)`** before dropping their reference,
  done OUTSIDE `m_ProcessingThreadsMutex`: `AssignSwapChain` (replaced processor), `UnassignSwapChain`,
  and `~IndirectDeviceContext` (any residual processors).

### Exact lock order (must stay acyclic)
```
g_DeviceContextMutex
   -> m_MonitorsMutex
      -> (IddCxMonitorDeparture is called OUTSIDE m_MonitorsMutex by RemoveMonitor)
         -> m_ProcessingThreadsMutex     (via the synchronous UnassignSwapChain during departure)
```
Invariants that keep it acyclic:
- **Nothing acquires `g_DeviceContextMutex` while holding `m_MonitorsMutex` or
  `m_ProcessingThreadsMutex`.** The only `g_DeviceContextMutex` acquisitions are: `GetDeviceContext`
  (no other lock), `WatchdogProc` self-heal (top of the chain), `~IndirectDeviceContext` STEP 1 (no
  other lock), and `InitAdapter` caching (no monitor/processing lock — runs before any monitor).
  So the watchdog holding `g_DeviceContextMutex` across `RemoveAllMonitors()` (which transitively
  takes `m_MonitorsMutex` then `m_ProcessingThreadsMutex`) cannot deadlock.
- **`IddCxMonitorDeparture` is never called while `m_MonitorsMutex` or `m_ProcessingThreadsMutex`
  is held** (copy-handle-under-lock, unlock, then depart) — unchanged from the prior pass.
- **Leaf locks** (no other driver lock taken while held, and never held across an IddCx call that
  re-enters): `g_SettingsMutex`, `g_pipeHandleMutex`, `g_WatchdogThreadMutex`,
  `g_HdrMetadataStoreMutex`, `g_GammaRampStoreMutex`, `s_DeviceCacheMutex`. `CreateTargetMode2`
  takes `g_SettingsMutex` while `QueryTargetModes2` holds NO lock, so there is no nesting; the two
  description callbacks hold `g_SettingsMutex` across their bodies but make no IddCx call.

### Round-2 IddCx/WDF points to verify on the WDK build
- **Issue 1 — holding `g_DeviceContextMutex` across `RemoveAllMonitors()` (and therefore across the
  synchronous `IddCxMonitorDeparture` -> `UnassignSwapChain`).** This is a *driver-internal* mutex,
  not a WDF/IddCx object lock, and the departure path was already exercised under the prior pass; but
  confirm IddCx does not re-enter the driver on a path that itself needs `g_DeviceContextMutex`
  (none does today — only `GetDeviceContext`, used by the pipe thread, takes it, and the pipe thread
  is not an IddCx callback).
- **Issue 3 — destructor possibly running on the worker thread.** Confirm WUDFHost/IddCx tolerate
  `~SwapChainProcessor` (and the `WdfObjectDelete` already issued by the worker in `Run()`) completing
  on the worker thread in the timed-out-teardown case. The WRL `Thread` member self-`CloseHandle`s on
  the worker — closing one's own thread handle is legal on Windows.
- **Issue 3 — unbounded join in case (a).** It only runs when the worker already dropped its hold
  (i.e. is finishing), so it returns promptly in practice; verify no legitimate teardown path leaves
  a worker holding its hold indefinitely while a non-worker thread blocks on the join.
