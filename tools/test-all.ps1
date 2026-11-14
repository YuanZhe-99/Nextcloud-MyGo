# Runs one test tier for one architecture with per-scenario durations and a hard total budget.
# Inputs: -Tier fast (no server, <= 2 min) or server (disposable local container, <= 10 min);
#   -Architecture ARM64 (everything) or x64 (fast tier plus one server smoke scenario).
#   -ServerVersion picks the disposable server's Nextcloud release (default the latest tested one;
#   run the previous supported major too, PLAN.md §15.3).
#   -Large adds the opt-in above-4-GiB chunk scenario (about 30 min, outside the budget).
#   -Extended adds the eight switching and data-safety cases outside the release-critical set in three
#   more lanes after the default ones (about 2.5 min, outside the budget).
#   -SharedServer adds the paired-daemon login fixture against the already approved server in
#   NEXTCLOUD_TEST_* (outside the budget: the paired daemon trusts only a publicly trusted chain).
#   -Build selects the SyncD tree (default build/syncd-<arch>).
# Side effects: builds nothing; runs the built SyncD of that tree, owned profiles and, for the
#   server tier, one disposable container that it removes afterwards. Everything runs inside one
#   kill-on-close Job Object, so no daemon, probe or test executable outlives this script.
# Notes: Stops at the first failure; a failed scenario is never rerun, on this or another architecture.
[CmdletBinding()]
param(
    [ValidateSet('fast', 'server')][string]$Tier = 'fast',
    [ValidateSet('ARM64', 'x64')][string]$Architecture = 'ARM64',
    [ValidateSet('wslc', 'docker')][string]$Runtime = 'wslc',
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$ServerVersion = '35.0.2',
    [switch]$Large,
    [switch]$Extended,
    [switch]$SharedServer,
    [string]$Build = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-job.ps1')
if (-not (Enter-OwnedJob)) { Invoke-InOwnedJob $PSCommandPath $PSBoundParameters; if ($LASTEXITCODE) { throw 'Test tier failed.' }; return }
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root # the Go steps test the module this script belongs to, wherever it was started from
$budget = if ($Tier -eq 'fast') { 120 } else { 600 }
$scenarioBudget = 150

# The build records which Qt and upstream build it used, and the upstream build its dependency
# prefixes after Qt; the tests use exactly those DLLs.
$build = if ($Build) { $Build } else { Join-Path $root "build/syncd-$Architecture" }
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
$qt = (($cache | Select-String '^CMAKE_PREFIX_PATH:[A-Z]+=(.+)$').Matches[0].Groups[1].Value) -replace '/', '\'
$upstream = (($cache | Select-String '^SYNCD_UPSTREAM_BUILD:[A-Z]+=(.+)$').Matches[0].Groups[1].Value) -replace '/', '\'
$deps = (Get-Content -LiteralPath (Join-Path $upstream 'CMakeCache.txt') | Select-String '^CMAKE_PREFIX_PATH:[A-Z]+=(.+)$').Matches[0].Groups[1].Value -split ';' |
    Select-Object -Skip 1 | ForEach-Object { (Join-Path $_ 'bin') -replace '/', '\' }
$env:PATH = "$qt\bin;$upstream\bin\Release;$($deps -join ';');$env:PATH"
$env:QT_PLUGIN_PATH = "$qt\plugins"
$release = Join-Path $build 'Release'
$env:SYNCD_TEST_EXE = Join-Path $release 'nextcloud-mygo-syncd.exe'
$vs = & (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe') -latest -products '*' -property installationPath
$ctest = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe'

$results = [Collections.Generic.List[object]]::new()
$clock = [Diagnostics.Stopwatch]::StartNew()

# Step runs one scenario, prints its line with the duration, and stops the tier on the first failure.
function Step([string]$Name, [scriptblock]$Body, [switch]$OutsideBudget) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $global:LASTEXITCODE = 0
    $output = [Collections.Generic.List[string]]::new()
    try {
        & $Body 2>&1 | ForEach-Object { $output.Add("$_") }
        if ($LASTEXITCODE) { throw "exit $LASTEXITCODE" }
        $seconds = $watch.Elapsed.TotalSeconds
        $note = if ($OutsideBudget) { ' (opt-in, outside budget)' } elseif ($seconds -gt $scenarioBudget) { " (above the $scenarioBudget s scenario budget)" } else { '' }
        Write-Output ('PASS {0,-34} {1,7:N1} s{2}' -f $Name, $seconds, $note)
        $results.Add([pscustomobject]@{ Name = $Name; Seconds = $seconds; Outside = [bool]$OutsideBudget })
    } catch {
        $lines = @($output | Where-Object { $_ -match '^(\[[a-z]+\] )?(PASS|FAIL|--- FAIL|Owned|step:|panic:|Daemon|The following tests FAILED|\s+\d+ - |\s+step at |\s+at |\s+\S+_test\.go:\d+:)' } | Select-Object -Last 12)
        foreach ($line in $lines) { Write-Output "  $line" }
        Write-Output ('FAIL {0,-34} {1,7:N1} s: {2}' -f $Name, $watch.Elapsed.TotalSeconds, $_.Exception.Message)
        throw "Tier $Tier on $Architecture stopped at $Name."
    }
}

# Go runs Go tests built for this architecture (x64 runs emulated on ARM64 hosts).
function GoArch([string[]]$Arguments) {
    $saved = $env:GOARCH
    $env:GOARCH = if ($Architecture -eq 'ARM64') { 'arm64' } else { 'amd64' }
    try { & go.exe @Arguments } finally { $env:GOARCH = $saved }
}

# Acceptance runs one owned C++ scenario through its wrapper (which owns cleanup) on this build.
function Acceptance([string]$Scenario, [string[]]$Stage) {
    $exe = Join-Path $release 'syncd_reauth_tests.exe'
    $p = @{ Executable = $exe; Scenario = $Scenario }
    if ($Stage) { $p.Stage = $Stage }
    & (Join-Path $PSScriptRoot 'test-reauth-lifetime.ps1') @p
}

# Lanes runs groups of acceptance cases in parallel child processes (inside this script's job),
# prints one line per case with its duration, and stops everything at the first failure.
function Lanes([object[]]$Groups) {
    $wrapper = Join-Path $PSScriptRoot 'test-reauth-lifetime.ps1'
    $started = Get-Date
    $exe = Join-Path $release 'syncd_reauth_tests.exe'
    $children = @()
    foreach ($group in $Groups) {
        # Lanes start 3 s apart: all sign-ins and first probes at once outran the 4 s public probe
        # deadline on a fresh server (NETWORK_UNAVAILABLE at the first endpoints.stage).
        if ($children) { Start-Sleep -Seconds 3 }
        $lines = foreach ($case in $group) {
            $extra = if (-not $case[2]) { '' } elseif ($case[1] -eq 'switch-kill') { " -Stage '$($case[2])'" } else { " -Case '$($case[2])'" }
            # The lifetime case addresses the fixture server directly; the others go through the relay names,
            # so the lane's address comes back after it (a later case once ran unrelayed and unheld).
            $url, $restore = if ($case[1] -eq 'lifetime') { '$relayed = $env:NEXTCLOUD_TEST_URL; $env:NEXTCLOUD_TEST_URL = $env:NEXTCLOUD_TEST_CLEANUP_URL; ', '$env:NEXTCLOUD_TEST_URL = $relayed; ' } else { '', '' }
            "`$w = [Diagnostics.Stopwatch]::StartNew(); try { $url& '$wrapper' -Executable '$exe' -Scenario '$($case[1])'$extra | Where-Object { `$_ -match '^\s+step at ' }; $restore'PASS {0,-34} {1,7:N1} s' -f '$($case[0])', `$w.Elapsed.TotalSeconds } catch { 'FAIL {0,-34} {1,7:N1} s: {2}' -f '$($case[0])', `$w.Elapsed.TotalSeconds, `$_.Exception.Message; exit 1 }"
        }
        $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSHOME 'pwsh'))
        foreach ($a in '-NoProfile', '-NonInteractive', '-Command', (($lines -join '; ') + "; exit 0")) { $info.ArgumentList.Add($a) }
        $info.UseShellExecute = $false
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $p = [Diagnostics.Process]::Start($info)
        $children += [pscustomobject]@{ Process = $p; Output = $p.StandardOutput.ReadToEndAsync(); Errors = $p.StandardError.ReadToEndAsync() }
    }
    $failed = $null
    while ($children | Where-Object { -not $_.Process.HasExited }) {
        $failed = $children | Where-Object { $_.Process.HasExited -and $_.Process.ExitCode -ne 0 } | Select-Object -First 1
        if ($failed) { break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $failed) { $failed = $children | Where-Object { $_.Process.ExitCode -ne 0 } | Select-Object -First 1 }
    $killed = $false
    foreach ($child in $children) { if (-not $child.Process.HasExited) { $child.Process.Kill($true); $child.Process.WaitForExit(); $killed = $true } }
    if ($killed) {
        # A killed lane never ran its wrapper's cleanup: run it now, while the server still exists, for
        # every profile this tier created and left behind (remote content first, then native credentials).
        foreach ($dir in Get-ChildItem -LiteralPath (Join-Path $root 'build') -Directory -Filter 'reauth-lifetime-*' | Where-Object CreationTime -ge $started) {
            $env:SYNCD_REAUTH_LIFETIME_ROOT = $dir.FullName
            $p = Start-Process -FilePath $exe -ArgumentList '--cleanup' -NoNewWindow -PassThru -RedirectStandardOutput 'NUL'
            if ($p.WaitForExit(60000) -and $p.ExitCode -eq 0) { Remove-Item -LiteralPath $dir.FullName -Recurse -Force }
            else { if (-not $p.HasExited) { $p.Kill($true) }; Write-Output "  owned profile $($dir.Name) kept: its cleanup did not finish" }
            $env:SYNCD_REAUTH_LIFETIME_ROOT = $null
        }
    }
    foreach ($child in $children) {
        foreach ($line in ($child.Output.GetAwaiter().GetResult() -split "`r?`n" | Where-Object { $_ -match '^(PASS|FAIL) |^\s+step at ' })) {
            Write-Output $line
            if ($line -match '^PASS (.+?)\s+([\d.,]+) s$') { $results.Add([pscustomobject]@{ Name = $Matches[1]; Seconds = [double]($Matches[2] -replace ',', ''); Outside = $false }) }
        }
        $child.Process.Dispose()
    }
    if ($failed) { throw "Tier $Tier on $Architecture stopped at a failed server case." }
}
$server = $null
$fixtureNames = 'NEXTCLOUD_TEST_URL', 'NEXTCLOUD_TEST_ALTERNATE_URL', 'NEXTCLOUD_TEST_CLEANUP_URL', 'NEXTCLOUD_TEST_USER', 'NEXTCLOUD_TEST_PASSWORD', 'SYNCD_TEST_CA_CERT', 'SYNCD_TEST_CA_SHA1'
$saved = @{}
foreach ($name in $fixtureNames) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    if ($Tier -eq 'fast') {
        if ($Architecture -eq 'ARM64') {
            Step 'go format and vet' { $bad = gofmt -l cmd internal tools; if ($bad) { throw 'gofmt reports files' }; GoArch @('vet', './...') }
        }
        Step 'go unit and real-pipe tests' { GoArch @('test', '-count=1', './...') }
        Step 'syncd ctest (all targets)' { & $ctest --test-dir $build -C Release --no-tests=error --timeout 60 -j 4 --output-on-failure }
    } else {
        Step 'disposable server start' {
            $script:server = & (Join-Path $PSScriptRoot 'test-server.ps1') -Start -Runtime $Runtime -Version $ServerVersion | Select-Object -Last 1
        }
        $state = Get-Content -LiteralPath $server -Raw | ConvertFrom-Json
        $values = @{}
        foreach ($line in [IO.File]::ReadAllLines((Join-Path (Split-Path $server -Parent) 'bootstrap.env'))) {
            $pair = $line.Split('=', 2); if ($pair.Length -eq 2) { $values[$pair[0]] = $pair[1] }
        }
        $env:NEXTCLOUD_TEST_URL = "https://primary.syncd.test:$($state.Port)"
        $env:NEXTCLOUD_TEST_ALTERNATE_URL = "https://alternate.syncd.test:$($state.Port)"
        $env:NEXTCLOUD_TEST_CLEANUP_URL = "https://127.0.0.1:$($state.Port)"
        $env:NEXTCLOUD_TEST_USER = $values['NEXTCLOUD_ADMIN_USER']
        $env:NEXTCLOUD_TEST_PASSWORD = $values['NEXTCLOUD_ADMIN_PASSWORD']
        $values.Clear()
        $env:SYNCD_TEST_CA_CERT = Join-Path (Split-Path $server -Parent) 'root.pem'
        $env:SYNCD_TEST_CA_SHA1 = $state.RootThumbprint
        if ($Architecture -eq 'x64') {
            # Emulated x64 gets one real-server smoke scenario; ARM64 runs the full set.
            Step 'smoke: switch-kill committed' { Acceptance 'switch-kill' @('committed') }
        } else {
            # Every case below uses its own process, profile, relay and remote collection, so the
            # cases run in parallel lanes; each lane stops at its first failure and a failure stops
            # every lane. Most of a case's time is the daemon's own failover and recovery timing.
            # Six lanes, as before the switching and data-safety cases joined: a seventh made the host slow
            # enough for a lost final MOVE to miss its failover window once, and nine made an overflow audit
            # miss its 30 s window. -Extended adds the cases outside the release-critical set in three lanes.
            Lanes @(
                , @(@('kill at prepared', 'switch-kill', 'prepared'), @('kill at quiescing', 'switch-kill', 'quiescing'), @('switch: automatic', 'switching', 'auto-switch'), @('file safety', 'file-safety', ''), @('switch: TLS trust', 'switching', 'tls-trust'))
                , @(@('kill at verifying', 'switch-kill', 'verifying'), @('kill at committed', 'switch-kill', 'committed'), @('switch: redirect', 'switching', 'redirect'), @('switch: revoked app password', 'switching', 'revoke-switch'), @('sleep and resume, simulated', 'sleep-resume', ''))
                , @(@('kill at reconciling', 'switch-kill', 'reconciling'), @('chunk session resume', 'network-chunked', ''), @('switch: TLS and sign-in', 'switching', 'tls-auth'), @('data: rename MOVE cut', 'data-safety', 'rename-cut'))
                , @(@('active upload', 'network-active', 'upload'), @('active download', 'network-active', 'download'), @('active lost PUT reply', 'network-active', 'unacknowledged-upload'), @('switch: automatic fallback', 'switching', 'auto-fallback'), @('data: journal kept', 'data-safety', 'journal'), @('data: root replaced', 'data-safety', 'root-replaced'), @('data: partial listing', 'data-safety', 'partial-listing'), @('held revocation', 'held-revocation', ''), @('slow server stays healthy', 'slow-server', ''))
                , @(@('lost MOVE reply', 'network-uncertain', 'move'), @('lost DELETE reply', 'network-uncertain', 'delete'), @('lost final MOVE reply', 'network-uncertain', 'final-move'), @('switch held past its window', 'switch-quota', ''), @('switch: late epoch', 'switching', 'late-epoch'))
                , @(@('network failover and fixed target', 'network', ''), @('renewal lifetime', 'lifetime', ''), @('two accounts, two folders each', 'multi-account', ''), @('data: interrupted change', 'data-safety', 'interrupted-change'), @('data: discovery killed', 'data-safety', 'discovery-kill'), @('data: revocation', 'data-safety', 'revocation'), @('file safety, 100 deletions', 'file-safety-bulk', ''))
            )
            if ($Extended) {
                $watch = [Diagnostics.Stopwatch]::StartNew()
                Lanes @(
                    , @(@('switch: TLS circuit', 'switching', 'tls-circuit'), @('switch: subpath', 'switching', 'subpath'))
                    , @(@('switch: new folder', 'switching', 'new-folder'), @('switch: root changed', 'switching', 'root-change'), @('data: names', 'data-safety', 'names'))
                    , @(@('data: watcher boundary', 'data-safety', 'watcher'), @('data: root denied', 'data-safety', 'root-denied'), @('data: excluded trees', 'data-safety', 'excluded'))
                )
                $results.Add([pscustomobject]@{ Name = 'extended lanes'; Seconds = $watch.Elapsed.TotalSeconds; Outside = $true })
                Write-Output ('PASS {0,-34} {1,7:N1} s (opt-in, outside budget)' -f 'extended lanes', $watch.Elapsed.TotalSeconds)
            }
            # Maintenance mode is instance-wide, so it runs after every lane, alone.
            Lanes @(, @(, @('data: maintenance mode', 'data-safety', 'maintenance')))
            if ($Large) { Step 'above-4-GiB chunk resume' { Acceptance 'network-large' } -OutsideBudget }
        }
    }
} finally {
    if ($server) {
        $watch = [Diagnostics.Stopwatch]::StartNew()
        try {
            & (Join-Path $PSScriptRoot 'test-server.ps1') -Stop -State $server
            Write-Output ('PASS {0,-34} {1,7:N1} s' -f 'disposable server removal', $watch.Elapsed.TotalSeconds)
        } catch { Write-Output ('FAIL {0,-34} {1,7:N1} s: {2}' -f 'disposable server removal', $watch.Elapsed.TotalSeconds, $_.Exception.Message) }
        # The shared server's approved environment, if any, comes back for -SharedServer.
        foreach ($name in $fixtureNames) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    }
}
if ($SharedServer) {
    if (-not $env:NEXTCLOUD_TEST_URL) { throw 'Set the approved NEXTCLOUD_TEST_* environment for -SharedServer.' }
    Step 'shared server: paired-daemon login' { & (Join-Path $PSScriptRoot 'test-login.ps1') -Executable $env:SYNCD_TEST_EXE -SyncFolders -Reauthenticate } -OutsideBudget
}
$total = $clock.Elapsed.TotalSeconds - (($results | Where-Object Outside | Measure-Object Seconds -Sum).Sum)
Write-Output ('TOTAL {0} {1}: {2:N1} s of {3} s budget' -f $Tier, $Architecture, $total, $budget)
if ($total -gt $budget) { throw "Tier $Tier on $Architecture took $([int]$total) s, above its $budget s budget." }
