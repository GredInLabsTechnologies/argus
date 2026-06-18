#requires -Version 5.1
<#
.SYNOPSIS
    Argus virtual-display driver benchmark & validation harness.

.DESCRIPTION
    Drives the Argus control pipe (\\.\pipe\ArgusDisplay) exactly like a real broker — one
    connection per command, ASCII wire — and measures, with QueryPerformanceCounter-grade timing
    (System.Diagnostics.Stopwatch), the claims the README makes:

      * ADD  latency  : pipe round-trip + time until Windows EXPOSES the monitor (visibility)
      * REMOVE latency: pipe round-trip + time until the monitor DISAPPEARS
      * PING latency  : the protocol floor (connect+write+read with no display work)
      * no adapter reload / no re-enum storm : DisplaySettingsChanged events per operation,
                                               and that pre-existing monitors are untouched
      * watchdog self-heal : monitors retire ~g_WatchdogTimeoutSeconds after PINGs stop;
                             and that WITHOUT any PING the monitors persist
      * edge cases   : saturation (MAX_MONITORS), remove-not-live, invalid/garbage commands
                       (must NOT crash the host), double-remove, burst add/remove, cold-vs-warm,
                       last-remove returns to idle-0.

    Reports min / p50 / p95 / p99 / max (percentiles, not just the mean) and writes JSON + CSV.

    MUST run elevated: the pipe SDDL is SYSTEM + Administrators only.

.PARAMETER Iterations   Samples per timed scenario (default 200).
.PARAMETER Warmup       Discarded warm-up samples before timing (default 20).
.PARAMETER OutDir       Where to write results (default .\results).
.PARAMETER SkipWatchdog Skip the watchdog scenarios (they take ~timeout*2 seconds each).
.PARAMETER SettleMs     Max ms to wait when polling for a display to appear/disappear (default 5000).
#>
[CmdletBinding()]
param(
    [int]$Iterations = 200,
    [int]$Warmup = 20,
    [string]$OutDir = (Join-Path $PSScriptRoot 'results'),
    [switch]$SkipWatchdog,
    [int]$SettleMs = 5000
)

$ErrorActionPreference = 'Stop'
$PipeName = 'ArgusDisplay'

# ---------------------------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $isAdmin) {
    throw "Must run elevated. The Argus pipe SDDL is D:(A;;GA;;;SY)(A;;GA;;;BA) (SYSTEM + Admins only)."
}
$pipes = [System.IO.Directory]::GetFiles('\\.\pipe\')
if (-not ($pipes | Where-Object { $_ -match [regex]::Escape($PipeName) })) {
    throw "\\.\pipe\$PipeName not found. The Argus driver is not loaded/active. Run deploy-argus.ps1 first."
}
Add-Type -AssemblyName System.Windows.Forms
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

# ---------------------------------------------------------------------------------------------
# Display enumeration — distinguish ACTIVE desktop screens from TOTAL connected paths.
# A monitor can ARRIVE (IddCx) before Windows EXTENDS the desktop onto it; measuring both keeps
# us honest about what "visible" means.
# ---------------------------------------------------------------------------------------------
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Disp {
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);
    [DllImport("user32.dll")] public static extern int GetDisplayConfigBufferSizes(uint flags, out uint np, out uint nm);
    public const int SM_CMONITORS = 80;
    public const uint QDC_ALL_PATHS = 1;
    public static int ActiveMonitors() { return GetSystemMetrics(SM_CMONITORS); }
    public static uint TotalPaths() { uint np=0, nm=0; GetDisplayConfigBufferSizes(QDC_ALL_PATHS, out np, out nm); return np; }
}
"@
function Get-ActiveMonitors { [Disp]::ActiveMonitors() }
function Get-TotalPaths    { [Disp]::TotalPaths() }

# DisplaySettingsChanged counter (detects re-enumeration storms without needing a window/msg-loop).
$script:DisplayChangeCount = 0
$dscAction = { $script:DisplayChangeCount++ }
Register-ObjectEvent -InputObject ([Microsoft.Win32.SystemEvents]) -EventName DisplaySettingsChanged `
    -Action $dscAction | Out-Null
function Reset-DisplayChangeCount { $script:DisplayChangeCount = 0 }
function Get-DisplayChangeCount { $script:DisplayChangeCount }

# ---------------------------------------------------------------------------------------------
# The wire: one connection per command (matches the driver's HandleClient: 1 ReadFile + 1 reply
# + DisconnectNamedPipe). Times connect and round-trip (write->reply) separately.
# ---------------------------------------------------------------------------------------------
function Invoke-ArgusCommand {
    param(
        [Parameter(Mandatory)][string]$Command,
        [int]$ConnectTimeoutMs = 3000,
        [int]$ReadTimeoutMs = 3000,
        [switch]$NoReply   # SETDISPLAYCOUNT sends no reply
    )
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $PipeName,
        [System.IO.Pipes.PipeDirection]::InOut)
    $swConnect = [System.Diagnostics.Stopwatch]::StartNew()
    try { $pipe.Connect($ConnectTimeoutMs) } catch { $pipe.Dispose(); throw "Connect failed: $_" }
    $swConnect.Stop()

    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Command)
    $reply = $null
    $swRt = [System.Diagnostics.Stopwatch]::StartNew()
    $pipe.Write($bytes, 0, $bytes.Length); $pipe.Flush()
    if (-not $NoReply) {
        $buf = New-Object byte[] 256
        $t = $pipe.ReadAsync($buf, 0, $buf.Length)
        if ($t.Wait($ReadTimeoutMs)) {
            $n = $t.Result
            $reply = ([System.Text.Encoding]::ASCII.GetString($buf, 0, $n)).Trim([char]0).Trim()
        } else { $reply = '<timeout>' }
    }
    $swRt.Stop()
    $pipe.Dispose()
    [pscustomobject]@{
        Command     = $Command
        Reply       = $reply
        ConnectMs   = [math]::Round($swConnect.Elapsed.TotalMilliseconds, 4)
        RoundTripMs = [math]::Round($swRt.Elapsed.TotalMilliseconds, 4)
    }
}

# Poll until a predicate flips, measuring how long it took (the "propagation" latency).
function Measure-Until {
    param([Parameter(Mandatory)][scriptblock]$Predicate, [int]$TimeoutMs = $SettleMs, [int]$PollMs = 2)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalMilliseconds -lt $TimeoutMs) {
        if (& $Predicate) { $sw.Stop(); return @{ Ms = $sw.Elapsed.TotalMilliseconds; Hit = $true } }
        Start-Sleep -Milliseconds $PollMs
    }
    $sw.Stop(); return @{ Ms = $sw.Elapsed.TotalMilliseconds; Hit = $false }
}

function Get-Stats {
    param([double[]]$Samples, [string]$Name, [string]$Unit = 'ms')
    if (-not $Samples -or $Samples.Count -eq 0) {
        return [pscustomobject]@{ Metric=$Name; Unit=$Unit; N=0 }
    }
    $s = $Samples | Sort-Object
    function P([double]$q) { $i = [math]::Min($s.Count-1, [math]::Max(0, [int][math]::Ceiling($q*$s.Count)-1)); $s[$i] }
    [pscustomobject]@{
        Metric = $Name; Unit = $Unit; N = $s.Count
        Min = [math]::Round($s[0],3); P50 = [math]::Round((P 0.50),3); P95 = [math]::Round((P 0.95),3)
        P99 = [math]::Round((P 0.99),3); Max = [math]::Round($s[-1],3)
        Mean = [math]::Round(($s | Measure-Object -Average).Average,3)
    }
}

# Best-effort: drain every live monitor so we always start/end at idle-0.
function Reset-ToIdle {
    for ($i = 0; $i -lt 32; $i++) { try { Invoke-ArgusCommand "REMOVE $i" | Out-Null } catch {} }
    Start-Sleep -Milliseconds 300
}

$results = [ordered]@{}
$edgeFindings = [System.Collections.Generic.List[object]]::new()
Write-Host "=== Argus benchmark @ $stamp (iters=$Iterations warmup=$Warmup) ===" -ForegroundColor Cyan
Write-Host ("baseline: ActiveMonitors={0} TotalPaths={1}" -f (Get-ActiveMonitors), (Get-TotalPaths))

# ---------------------------------------------------------------------------------------------
# Scenario 0 — protocol floor: PING round-trip (no display work). This is the harness/transport
# overhead we must subtract mentally from ADD/REMOVE round-trips.
# ---------------------------------------------------------------------------------------------
Reset-ToIdle
$ping = @()
1..($Iterations+$Warmup) | ForEach-Object { $r = Invoke-ArgusCommand 'PING'; if ($_ -gt $Warmup) { $ping += $r.RoundTripMs } }
$results['ping_roundtrip'] = Get-Stats $ping 'PING round-trip'

# ---------------------------------------------------------------------------------------------
# Scenario 1 — ADD: round-trip (driver processing incl. IddCxMonitorArrival) AND visibility
# (time until SM_CMONITORS / total paths reflect the new monitor). Cold (first after idle-0) is
# captured separately from warm (steady state).
# ---------------------------------------------------------------------------------------------
$addRt=@(); $addVisActive=@(); $addVisPath=@(); $removeRt=@(); $removeVis=@()
$coldAddRt=$null; $coldAddVis=$null
Reset-ToIdle
for ($i = 0; $i -lt ($Iterations + $Warmup); $i++) {
    $beforeActive = Get-ActiveMonitors; $beforePaths = Get-TotalPaths
    $add = Invoke-ArgusCommand 'ADD'
    if ($add.Reply -notmatch '^\d+$') { $edgeFindings.Add([pscustomobject]@{ Case='ADD unexpected reply'; Detail=$add.Reply }); continue }
    $idx = [int]$add.Reply
    $visA = Measure-Until { (Get-ActiveMonitors) -gt $beforeActive }
    $visP = Measure-Until { (Get-TotalPaths) -gt $beforePaths }
    if ($i -eq 0) { $coldAddRt = $add.RoundTripMs; $coldAddVis = $visA.Ms }
    elseif ($i -ge $Warmup) { $addRt += $add.RoundTripMs; if ($visA.Hit) { $addVisActive += $visA.Ms }; if ($visP.Hit) { $addVisPath += $visP.Ms } }
    # remove it again to keep steady state at exactly 1 add in flight
    $beforeRem = Get-ActiveMonitors
    $rem = Invoke-ArgusCommand "REMOVE $idx"
    $rv = Measure-Until { (Get-ActiveMonitors) -lt $beforeRem }
    if ($i -ge $Warmup) { if ($rem.Reply -eq 'OK') { $removeRt += $rem.RoundTripMs }; if ($rv.Hit) { $removeVis += $rv.Ms } }
}
$results['add_roundtrip']        = Get-Stats $addRt        'ADD round-trip (driver)'
$results['add_visibility_active']= Get-Stats $addVisActive 'ADD -> active monitor (desktop)'
$results['add_visibility_path']  = Get-Stats $addVisPath   'ADD -> connected path'
$results['remove_roundtrip']     = Get-Stats $removeRt     'REMOVE round-trip (driver)'
$results['remove_visibility']    = Get-Stats $removeVis    'REMOVE -> monitor gone'
$results['cold_add'] = [pscustomobject]@{ Metric='COLD ADD (first after idle-0)'; RoundTripMs=$coldAddRt; VisibilityMs=$coldAddVis }

# ---------------------------------------------------------------------------------------------
# Scenario 2 — no adapter reload / no re-enum storm: one ADD should disturb ONLY the new monitor.
# Record the pre-existing monitors, ADD, and check they are byte-for-byte unchanged; count the
# DisplaySettingsChanged events caused by a single ADD (a reload would re-config everything).
# ---------------------------------------------------------------------------------------------
Reset-ToIdle
$before = [System.Windows.Forms.Screen]::AllScreens | ForEach-Object { "{0}|{1}" -f $_.DeviceName, $_.Bounds }
Reset-DisplayChangeCount
$add = Invoke-ArgusCommand 'ADD'
Start-Sleep -Milliseconds 800   # let any change events fire
$dscPerAdd = Get-DisplayChangeCount
$after = [System.Windows.Forms.Screen]::AllScreens | ForEach-Object { "{0}|{1}" -f $_.DeviceName, $_.Bounds }
$preserved = @($before | Where-Object { $after -contains $_ }).Count
$results['no_reload'] = [pscustomobject]@{
    Metric='no adapter reload'; PreExistingMonitors=$before.Count; PreservedAfterAdd=$preserved
    AllPreExistingUntouched = ($preserved -eq $before.Count); DisplayChangeEventsPerAdd=$dscPerAdd
}
if ($add.Reply -match '^\d+$') { Invoke-ArgusCommand "REMOVE $($add.Reply)" | Out-Null }

# ---------------------------------------------------------------------------------------------
# Scenario 3 — EDGE CASES (look at what we'd rather not). None may crash the UMDF host.
# ---------------------------------------------------------------------------------------------
Reset-ToIdle
function Test-Edge { param($Name,$Cmd,$ExpectRegex,[switch]$NoReply)
    try {
        $r = Invoke-ArgusCommand -Command $Cmd -NoReply:$NoReply
        $ok = $NoReply -or ($r.Reply -match $ExpectRegex)
        $script:edgeFindings.Add([pscustomobject]@{ Case=$Name; Sent=$Cmd; Reply=$r.Reply; Expected=$ExpectRegex; Pass=$ok })
        return $r
    } catch {
        $script:edgeFindings.Add([pscustomobject]@{ Case=$Name; Sent=$Cmd; Reply="<exception: $_>"; Expected=$ExpectRegex; Pass=$false }); return $null
    }
}
# remove with nothing live
Test-Edge 'remove-not-live'      'REMOVE 0'        '^ERR$' | Out-Null
# invalid indices
Test-Edge 'remove-negative'      'REMOVE -1'       '^ERR$' | Out-Null
Test-Edge 'remove-huge'          'REMOVE 99999'    '^ERR$' | Out-Null
Test-Edge 'remove-noindex'       'REMOVE'          '^ERR$' | Out-Null
Test-Edge 'remove-nonnumeric'    'REMOVE abc'      '^ERR$' | Out-Null
# garbage / unknown commands MUST answer ERR and NOT take down live monitors
$g = Invoke-ArgusCommand 'ADD'; $gidx = if ($g.Reply -match '^\d+$') { [int]$g.Reply } else { -1 }
Start-Sleep -Milliseconds 400
$liveBefore = Get-ActiveMonitors
Test-Edge 'garbage-FOObar'       'FOObar'          '^ERR$' | Out-Null
Test-Edge 'garbage-empty-ish'    ' '               '^ERR$' | Out-Null
Test-Edge 'garbage-binary'       ([char]1+[char]2) '^ERR$' | Out-Null
$liveAfter = Get-ActiveMonitors
$edgeFindings.Add([pscustomobject]@{ Case='garbage-does-not-unplug'; Sent='(3 bad cmds)'; Reply="live $liveBefore -> $liveAfter"; Expected='unchanged'; Pass=($liveAfter -eq $liveBefore) })
# double remove
if ($gidx -ge 0) {
    Test-Edge 'double-remove-1st' "REMOVE $gidx"  '^OK$'  | Out-Null
    Test-Edge 'double-remove-2nd' "REMOVE $gidx"  '^ERR$' | Out-Null
}
# legacy SETDISPLAYCOUNT must not hang (no reply)
Test-Edge 'setdisplaycount-legacy' 'SETDISPLAYCOUNT 2' '' -NoReply | Out-Null
# saturation: ADD until ERR, record MAX_MONITORS and whether it degrades gracefully
Reset-ToIdle
$added = @(); $satReply = $null
for ($i = 0; $i -lt 64; $i++) {
    $r = Invoke-ArgusCommand 'ADD'
    if ($r.Reply -match '^\d+$') { $added += [int]$r.Reply } else { $satReply = $r.Reply; break }
}
$edgeFindings.Add([pscustomobject]@{ Case='saturation'; Sent="ADD x N"; Reply="max live=$($added.Count), overflow reply=$satReply"; Expected='ERR at cap, no crash'; Pass=($satReply -eq 'ERR') })
$results['max_monitors'] = $added.Count
Reset-ToIdle
# burst: rapid add/remove churn, verify it survives and stays responsive
$burstErrors = 0
for ($i = 0; $i -lt 50; $i++) {
    $a = Invoke-ArgusCommand 'ADD'
    if ($a.Reply -match '^\d+$') { $r = Invoke-ArgusCommand "REMOVE $($a.Reply)"; if ($r.Reply -ne 'OK') { $burstErrors++ } } else { $burstErrors++ }
}
$pingAfterBurst = Invoke-ArgusCommand 'PING'
$edgeFindings.Add([pscustomobject]@{ Case='burst-50x-add-remove'; Sent='churn'; Reply="errors=$burstErrors, ping-after=$($pingAfterBurst.Reply)"; Expected='0 errors, PONG'; Pass=(($burstErrors -eq 0) -and ($pingAfterBurst.Reply -eq 'PONG')) })

# ---------------------------------------------------------------------------------------------
# Scenario 4 — watchdog self-heal (opt-in). After a PING arms it, stopping PINGs must retire
# every monitor within ~timeout. WITHOUT any PING, monitors must persist.
# ---------------------------------------------------------------------------------------------
if (-not $SkipWatchdog) {
    # (a) persistence without PING: ADD, never PING, wait > assumed timeout, must still be live
    Reset-ToIdle
    $a = Invoke-ArgusCommand 'ADD'
    Start-Sleep -Seconds 6
    $persistLive = Get-ActiveMonitors
    $results['watchdog_persist_no_ping'] = [pscustomobject]@{ Metric='persist without PING (6s)'; StillLive=($persistLive -ge 1); ActiveMonitors=$persistLive }
    Reset-ToIdle
    # (b) self-heal: ADD, PING once (arms), stop pinging, measure time to auto-retire
    $a = Invoke-ArgusCommand 'ADD'
    Invoke-ArgusCommand 'PING' | Out-Null
    $base = Get-ActiveMonitors
    $heal = Measure-Until -Predicate { (Get-ActiveMonitors) -lt $base } -TimeoutMs 15000 -PollMs 50
    $results['watchdog_selfheal'] = [pscustomobject]@{ Metric='self-heal after PINGs stop'; Retired=$heal.Hit; SecondsToRetire=[math]::Round($heal.Ms/1000,2) }
    Reset-ToIdle
}

# ---------------------------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------------------------
Write-Host "`n===== TIMING (ms) =====" -ForegroundColor Green
$timing = $results.GetEnumerator() | Where-Object { $_.Value.PSObject.Properties.Name -contains 'P50' } | ForEach-Object { $_.Value }
$timing | Format-Table Metric,N,Min,P50,P95,P99,Max,Mean -Auto | Out-String | Write-Host
Write-Host "===== STRUCTURE / WATCHDOG =====" -ForegroundColor Green
foreach ($k in 'cold_add','no_reload','max_monitors','watchdog_persist_no_ping','watchdog_selfheal') {
    if ($results.Contains($k)) { Write-Host ("{0}: {1}" -f $k, ($results[$k] | ConvertTo-Json -Compress -Depth 4)) }
}
Write-Host "`n===== EDGE CASES =====" -ForegroundColor Green
$edgeFindings | Format-Table Case,Sent,Reply,Pass -Auto | Out-String | Write-Host
$failed = @($edgeFindings | Where-Object { $_.Pass -eq $false })
if ($failed.Count) { Write-Host "EDGE FAILURES: $($failed.Count)" -ForegroundColor Red } else { Write-Host "All edge cases passed." -ForegroundColor Green }

$payload = [pscustomobject]@{
    timestamp = $stamp; iterations = $Iterations; warmup = $Warmup
    baseline = @{ activeMonitors = (Get-ActiveMonitors); totalPaths = (Get-TotalPaths) }
    results = $results; edgeCases = $edgeFindings
}
$jsonPath = Join-Path $OutDir "argus-bench-$stamp.json"
$payload | ConvertTo-Json -Depth 8 | Set-Content -Path $jsonPath -Encoding UTF8
$timing | Export-Csv -Path (Join-Path $OutDir "argus-bench-$stamp.csv") -NoTypeInformation -Encoding UTF8
Write-Host "`nWrote $jsonPath" -ForegroundColor Cyan

Reset-ToIdle  # leave the box at idle-0
