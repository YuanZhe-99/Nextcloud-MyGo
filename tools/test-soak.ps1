# Runs the accelerated soak (acceptance R21, PLAN.md §23.3-4): one SyncD with two accounts and two
# folders each against a disposable server through the fixture relay, the daemon's timers 24 times
# faster, a repeating mix of changes, faults, renewals and UI pipe connections, and a restart after the
# first fifth; it samples the daemon every 30 s and checks for growth and drift.
# Inputs: -Minutes (default 60); -Architecture and -Build select the SyncD tree as in test-all.ps1;
#   -Runtime and -ServerVersion pick the disposable server.
# Side effects: one disposable container, one owned profile under build/, an owned server user; all
#   removed afterwards. Writes build/soak/samples.csv, summary.json, summary.txt and the daemon's
#   output. Everything runs in one kill-on-close Job Object (tools/test-job.ps1).
# Notes: opt-in, outside the test tiers and CI. Every step has a deadline. An accelerated run is not a
#   wall-clock day: see acceptance.md R21.
[CmdletBinding()]
param(
    [ValidateRange(10, 240)][int]$Minutes = 60,
    [ValidateSet('ARM64', 'x64')][string]$Architecture = 'ARM64',
    [ValidateSet('wslc', 'docker')][string]$Runtime = 'wslc',
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$ServerVersion = '35.0.2',
    [string]$Build = '',
    # -State reuses a disposable server that is already running (its state.json); it is left running.
    [string]$State = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-job.ps1')
if (-not (Enter-OwnedJob)) { Invoke-InOwnedJob $PSCommandPath $PSBoundParameters; if ($LASTEXITCODE) { throw 'Soak failed.' }; return }
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

# Thresholds over the slope window (the last 40 minutes, or the last two thirds of a shorter run),
# fixed before the 60-minute run from the 10-minute dry run.
$limits = [ordered]@{
    PrivateMB = @{ Slope = 10; Unit = 'MB/h' }   # private bytes: 240 MB a day would fail
    Handles   = @{ Slope = 60; Band = 150; Unit = '/h' }
    Threads   = @{ Slope = 4; Band = 12; Unit = '/h' }
    Tcp       = @{ Slope = 20; Band = 60; Unit = '/h' }   # bursty: probes and transfers
    ControlKB = @{ Slope = 1024; Unit = 'KB/h' }  # the database file; its WAL is reported
    JournalKB = @{ Slope = 1024; Unit = 'KB/h' }
}

$build = if ($Build) { $Build } else { Join-Path $root "build/syncd-$Architecture" }
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
$qt = (($cache | Select-String '^CMAKE_PREFIX_PATH:[A-Z]+=(.+)$').Matches[0].Groups[1].Value) -replace '/', '\'
$upstream = (($cache | Select-String '^SYNCD_UPSTREAM_BUILD:[A-Z]+=(.+)$').Matches[0].Groups[1].Value) -replace '/', '\'
$deps = (Get-Content -LiteralPath (Join-Path $upstream 'CMakeCache.txt') | Select-String '^CMAKE_PREFIX_PATH:[A-Z]+=(.+)$').Matches[0].Groups[1].Value -split ';' |
    Select-Object -Skip 1 | ForEach-Object { (Join-Path $_ 'bin') -replace '/', '\' }
$env:PATH = "$qt\bin;$upstream\bin\Release;$($deps -join ';');$env:PATH"
$env:QT_PLUGIN_PATH = "$qt\plugins"
$exe = Join-Path $build 'Release/syncd_reauth_tests.exe'
$out = Join-Path $root 'build/soak'
$null = New-Item -ItemType Directory -Force -Path $out
Get-ChildItem -LiteralPath $out -File | Remove-Item -Force
$saved = $env:GOARCH
$env:GOARCH = if ($Architecture -eq 'ARM64') { 'arm64' } else { 'amd64' }
try { & go.exe build -o (Join-Path $out 'test-soak.exe') ./tools/test-soak } finally { $env:GOARCH = $saved }
if ($LASTEXITCODE) { throw 'Soak UI client build failed.' }

$clock = [Diagnostics.Stopwatch]::StartNew()
$samples = [Collections.Generic.List[object]]::new()
$server = $null; $owned = $null; $ui = $null; $child = $null

# Launch runs one owned process with its output in build/soak.
function Launch([string]$File, [string[]]$Arguments, [string]$Name) {
    Start-Process -FilePath $File -ArgumentList $Arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $out "$Name.out") -RedirectStandardError (Join-Path $out "$Name.log")
}

# Size sums the sizes of the files matching a pattern in a directory, in KB.
function Size([string]$Directory, [string]$Pattern) {
    if (-not (Test-Path -LiteralPath $Directory)) { return 0 }
    $sum = (Get-ChildItem -LiteralPath $Directory -File -Filter $Pattern | Measure-Object Length -Sum).Sum
    [math]::Round([double]$sum / 1KB, 1)
}

# Journals sums the sizes of the folders' journals (with their WAL files), in KB, from the list the
# daemon writes at its start.
function Journals {
    $list = Join-Path $owned 'soak-journals.txt'
    if (-not (Test-Path -LiteralPath $list)) { return 0 }
    $sum = 0.0
    foreach ($path in Get-Content -LiteralPath $list) {
        # A journal and its WAL come and go with the cycles: absent counts as empty.
        foreach ($file in $path, "$path-wal") { $info = [IO.FileInfo]::new($file); $info.Refresh(); if ($info.Exists) { try { $sum += $info.Length } catch { } } }
    }
    [math]::Round($sum / 1KB, 1)
}

# Sample records one row of the daemon's resources.
function Sample([Diagnostics.Process]$Process, [string]$Half) {
    $p = Get-Process -Id $Process.Id -ErrorAction SilentlyContinue
    if (-not $p) { return }
    $io = Get-CimInstance Win32_Process -Filter "ProcessId=$($Process.Id)"
    $tcp = @(Get-NetTCPConnection -OwningProcess $Process.Id -ErrorAction SilentlyContinue)
    $logs = (Get-ChildItem -LiteralPath $out -File -Filter 'soak-*.log' | Measure-Object Length -Sum).Sum
    $samples.Add([pscustomobject][ordered]@{
        Seconds     = [math]::Round($clock.Elapsed.TotalSeconds, 1)
        Half        = $Half
        PrivateMB   = [math]::Round($p.PrivateMemorySize64 / 1MB, 2)
        WorkingMB   = [math]::Round($p.WorkingSet64 / 1MB, 2)
        Handles     = $p.HandleCount
        Threads     = $p.Threads.Count
        Tcp         = $tcp.Count
        Established = @($tcp | Where-Object State -eq 'Established').Count
        CpuSeconds  = [math]::Round($p.TotalProcessorTime.TotalSeconds, 2)
        ReadMB      = [math]::Round($io.ReadTransferCount / 1MB, 1)
        WriteMB     = [math]::Round($io.WriteTransferCount / 1MB, 1)
        ControlKB   = Size $owned 'control.sqlite'
        ControlWalKB = Size $owned 'control.sqlite-wal'
        JournalKB   = Journals
        LogKB       = [math]::Round([double]$logs / 1KB, 1)
    })
}

# Half runs one daemon process of the soak to its end, sampling every 30 s.
function Half([string]$Name, [int]$Seconds) {
    $env:SYNCD_SOAK_SECONDS = [string]$Seconds
    $script:child = Launch $exe @("--soak=$Name") "soak-$Name"
    # Setup, a final convergence and shutdown each have their own step deadlines inside.
    $deadline = $clock.Elapsed.TotalSeconds + $Seconds + 900
    $next = 0
    while (-not $child.HasExited) {
        if ($clock.Elapsed.TotalSeconds -ge $next) { Sample $child $Name; $next = $clock.Elapsed.TotalSeconds + 30 }
        if ($clock.Elapsed.TotalSeconds -gt $deadline) { $child.Kill($true); throw "soak $Name half exceeded its $($Seconds + 900) s budget" }
        Start-Sleep -Milliseconds 500
    }
    $child.WaitForExit()
    $output = Get-Content -LiteralPath (Join-Path $out "soak-$Name.out")
    $output | Where-Object { $_ -match '^PASS ' } | ForEach-Object { Write-Output $_ }
    # The second half ends with exit 1 when a soak check failed after the run completed; its result
    # file then exists and the resource analysis still runs.
    if ($child.ExitCode -ne 0 -and $Name -eq 'second' -and (Test-Path -LiteralPath (Join-Path $owned 'soak-result.json'))) { return }
    if ($child.ExitCode -ne 0) {
        $output | Where-Object { $_ -match '^STEP ' } | Select-Object -Last 4 | ForEach-Object { Write-Output "  $_" }
        Get-Content -LiteralPath (Join-Path $out "soak-$Name.log") | Where-Object { $_ -match '^owned|^[A-Za-z].{3,160}$' } | Select-Object -Last 3 | ForEach-Object { Write-Output "  $_" }
        throw "soak $Name half failed (exit $($child.ExitCode))"
    }
}

# Fit returns the least-squares slope per hour of one column over the rows.
function Fit([object[]]$Rows, [string]$Column) {
    $n = $Rows.Count; $mx = ($Rows | Measure-Object Seconds -Average).Average; $my = ($Rows | Measure-Object $Column -Average).Average
    $sxy = 0.0; $sxx = 0.0
    foreach ($r in $Rows) { $dx = ($r.Seconds - $mx) / 3600; $sxy += $dx * ($r.$Column - $my); $sxx += $dx * $dx }
    if ($n -lt 3 -or $sxx -eq 0) { return 0 }
    $sxy / $sxx
}

$fixtureNames = 'NEXTCLOUD_TEST_URL', 'NEXTCLOUD_TEST_ALTERNATE_URL', 'NEXTCLOUD_TEST_CLEANUP_URL', 'NEXTCLOUD_TEST_USER', 'NEXTCLOUD_TEST_PASSWORD', 'SYNCD_TEST_CA_CERT', 'SYNCD_TEST_CA_SHA1'
$failure = $null
try {
    $server = if ($State) { $State } else { & (Join-Path $PSScriptRoot 'test-server.ps1') -Start -Runtime $Runtime -Version $ServerVersion | Select-Object -Last 1 }
    $info = Get-Content -LiteralPath $server -Raw | ConvertFrom-Json
    $values = @{}
    foreach ($line in [IO.File]::ReadAllLines((Join-Path (Split-Path $server -Parent) 'bootstrap.env'))) {
        $pair = $line.Split('=', 2); if ($pair.Length -eq 2) { $values[$pair[0]] = $pair[1] }
    }
    $env:NEXTCLOUD_TEST_URL = "https://primary.syncd.test:$($info.Port)"
    $env:NEXTCLOUD_TEST_ALTERNATE_URL = "https://alternate.syncd.test:$($info.Port)"
    $env:NEXTCLOUD_TEST_CLEANUP_URL = "https://127.0.0.1:$($info.Port)"
    $env:NEXTCLOUD_TEST_USER = $values['NEXTCLOUD_ADMIN_USER']
    $env:NEXTCLOUD_TEST_PASSWORD = $values['NEXTCLOUD_ADMIN_PASSWORD']
    $values.Clear()
    $env:SYNCD_TEST_CA_CERT = Join-Path (Split-Path $server -Parent) 'root.pem'
    $env:SYNCD_TEST_CA_SHA1 = $info.RootThumbprint
    Write-Output ('PASS {0,-34} {1,7:N1} s' -f 'disposable server start', $clock.Elapsed.TotalSeconds)

    $id = [Guid]::NewGuid().ToString('N')
    $owned = Join-Path $root "build/reauth-lifetime-$id"
    $null = New-Item -ItemType Directory -Path $owned
    $env:SYNCD_REAUTH_LIFETIME_ROOT = $owned
    $env:SYNCD_SOAK_PROFILE = "soak-$($id.Substring(0, 12))"
    $ui = Launch (Join-Path $out 'test-soak.exe') @('-profile', $env:SYNCD_SOAK_PROFILE, '-executable', $exe, '-for', "$($Minutes)m") 'ui'
    $start = $clock.Elapsed.TotalSeconds
    # The restart comes after the first fifth, so the slope window holds one daemon process past its
    # warm-up.
    Half 'first' ([int]($Minutes * 60 / 5))
    $restartAt = $clock.Elapsed.TotalSeconds - $start
    Half 'second' ([int][math]::Max(60, $Minutes * 60 - $restartAt - 90))
    if (-not $ui.WaitForExit(120000)) { $ui.Kill($true); throw 'soak UI client exceeded its budget' }
    $result = Get-Content -LiteralPath (Join-Path $owned 'soak-result.json') -Raw | ConvertFrom-Json
    Copy-Item -LiteralPath (Join-Path $owned 'soak-result.json') -Destination $out
} catch {
    $failure = $_.Exception.Message
} finally {
    foreach ($p in @($child, $ui)) { if ($p -and -not $p.HasExited) { $p.Kill($true); $p.WaitForExit() } }
    if ($owned -and (Test-Path -LiteralPath $owned)) {
        if (Test-Path -LiteralPath (Join-Path $owned 'soak-result.json')) { Copy-Item -LiteralPath (Join-Path $owned 'soak-result.json') -Destination $out -Force }
        $cleanup = Start-Process -FilePath $exe -ArgumentList '--cleanup' -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $out 'cleanup.out') -RedirectStandardError (Join-Path $out 'cleanup.log')
        if ($cleanup.WaitForExit(60000) -and $cleanup.ExitCode -eq 0) { Remove-Item -LiteralPath $owned -Recurse -Force }
        else { if (-not $cleanup.HasExited) { $cleanup.Kill($true) }; Write-Output "  owned profile $(Split-Path $owned -Leaf) kept: its cleanup did not finish" }
        $env:SYNCD_REAUTH_LIFETIME_ROOT = $null
    }
    if ($server -and -not $State) {
        try { & (Join-Path $PSScriptRoot 'test-server.ps1') -Stop -State $server; Write-Output 'PASS disposable server removal' }
        catch { Write-Output "FAIL disposable server removal: $($_.Exception.Message)" }
    }
    foreach ($name in $fixtureNames + 'SYNCD_SOAK_PROFILE', 'SYNCD_SOAK_SECONDS') { [Environment]::SetEnvironmentVariable($name, $null) }
}
$samples | Export-Csv -LiteralPath (Join-Path $out 'samples.csv') -NoTypeInformation
if ($failure) { throw $failure }

# Slopes over the window, against the thresholds.
$total = $samples[-1].Seconds
$window = [math]::Min(2400, ($total - $start) * 2 / 3)
$rows = @($samples | Where-Object { $_.Seconds -ge $total - $window -and $_.Half -eq 'second' })
$resources = [ordered]@{}
$passed = $true
foreach ($name in $limits.Keys) {
    $limit = $limits[$name]
    $slope = Fit $rows $name
    $values = $rows | ForEach-Object { $_.$name } | Sort-Object
    $band = $values[[int](($values.Count - 1) * 0.95)] - $values[[int](($values.Count - 1) * 0.05)]
    $ok = [math]::Abs($slope) -le $limit.Slope -and (-not $limit.Band -or $band -le $limit.Band)
    $passed = $passed -and $ok
    $resources[$name] = [ordered]@{ Slope = [math]::Round($slope, 2); Limit = $limit.Slope; Band = [math]::Round($band, 2); BandLimit = $limit.Band; Unit = $limit.Unit; Passed = $ok }
}
$resources['WorkingMB'] = [ordered]@{ Slope = [math]::Round((Fit $rows 'WorkingMB'), 2); Unit = 'MB/h'; Note = 'reported' }
$resources['ControlWalKB'] = [ordered]@{ Slope = [math]::Round((Fit $rows 'ControlWalKB'), 1); Unit = 'KB/h'; Note = 'checkpointed at about 4 MB; reported' }
$resources['LogKB'] = [ordered]@{ Slope = [math]::Round((Fit $rows 'LogKB'), 1); Unit = 'KB/h'; Note = 'grows with events; reported' }
# CPU use (the slope of CPU seconds) must not climb from the window's first half to its second.
$mid = $rows[[int]($rows.Count / 2)].Seconds
$early = @($rows | Where-Object Seconds -le $mid); $late = @($rows | Where-Object Seconds -ge $mid)
$cpuEarly = (Fit $early 'CpuSeconds') / 36; $cpuLate = (Fit $late 'CpuSeconds') / 36
$cpuOk = $cpuLate -le [math]::Max(1.5 * $cpuEarly, $cpuEarly + 5)
$passed = $passed -and $cpuOk -and $result.passed
$resources['CpuPercent'] = [ordered]@{ Early = [math]::Round($cpuEarly, 1); Late = [math]::Round($cpuLate, 1); Passed = $cpuOk; Note = 'late <= max(1.5 x early, early + 5 points)' }
$uiSummary = Get-Content -LiteralPath (Join-Path $out 'ui.out') | Select-Object -Last 1 | ConvertFrom-Json
$summary = [ordered]@{
    Minutes = $Minutes; Architecture = $Architecture; ServerVersion = $ServerVersion
    RunSeconds = [math]::Round($total - $start); RestartAtSeconds = [math]::Round($restartAt)
    WindowSeconds = [math]::Round($window); Samples = $samples.Count; WindowSamples = $rows.Count
    Resources = $resources; Checks = $result.checks; Events = $result.tally; SwitchReasons = $result.reasons; Ui = $uiSummary
    Passed = $passed
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'summary.json')
$lines = @("Accelerated soak, $Minutes min on $Architecture (server $ServerVersion), restart at $([math]::Round($restartAt / 60, 1)) min; slopes over the last $([math]::Round($window / 60, 1)) min ($($rows.Count) samples)")
foreach ($name in $resources.Keys) { $r = $resources[$name]; $lines += ('  {0,-10} {1}' -f $name, (($r.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')) }
foreach ($c in $result.checks.PSObject.Properties) { $lines += ('  check {0,-28} {1}' -f $c.Name, $c.Value) }
$lines += '  events ' + (($result.tally.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' ')
$lines += '  switch reasons ' + (($result.reasons.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' ')
$lines += '  ui ' + (($uiSummary.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' ')
$lines | Set-Content -LiteralPath (Join-Path $out 'summary.txt')
$lines | ForEach-Object { Write-Output $_ }
if (-not $passed) { throw 'Soak thresholds or checks failed (build/soak/summary.txt).' }
Write-Output ('PASS {0,-34} {1,7:N1} s' -f "accelerated soak, $Minutes min", $clock.Elapsed.TotalSeconds)
