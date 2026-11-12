# Runs the real on-demand (Cloud Files) acceptance against a disposable local Nextcloud server.
# Inputs: -Architecture of a tools/build-syncd tree (default build/syncd-<arch>, or -Build); -State reuses a
#   server started with tools/test-server.ps1 -Start instead of starting and removing one.
# Side effects: the fixture registers one sync root of its own under a fresh account/folder ID, creates one
#   remote collection, one vault entry and one local folder, and removes all of them. Afterwards this
#   script fails if any Nextcloud-MyGo sync root that did not exist before is still registered. It never
#   touches another container, the Windows trust store or TLS checks.
[CmdletBinding()]
param(
    [ValidateSet('ARM64', 'x64')][string]$Architecture = 'ARM64',
    [string]$Build = '',
    [string]$State = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-job.ps1')
if (-not (Enter-OwnedJob)) { Invoke-InOwnedJob $PSCommandPath $PSBoundParameters; if ($LASTEXITCODE) { throw 'On-demand acceptance failed.' }; return }
$root = Split-Path $PSScriptRoot -Parent
if (-not $Build) { $Build = Join-Path $root "build/syncd-$Architecture" }
$cache = Get-Content -LiteralPath (Join-Path $Build 'CMakeCache.txt')
$qt = (($cache | Select-String '^CMAKE_PREFIX_PATH:[A-Z]+=(.+)$').Matches[0].Groups[1].Value) -replace '/', '\'
$upstream = (($cache | Select-String '^SYNCD_UPSTREAM_BUILD:[A-Z]+=(.+)$').Matches[0].Groups[1].Value) -replace '/', '\'
$native = if ($Architecture -eq 'ARM64') { 'arm64' } else { 'x64' }
$exe = Join-Path $Build 'Release/syncd_ondemand_acceptance.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "No on-demand fixture in $Build; build SyncD first." }

# SyncRoots lists this product's registrations, so a leak is visible without naming other providers' roots.
function SyncRoots {
    @(Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager' -ErrorAction SilentlyContinue |
        Where-Object PSChildName -Like 'Nextcloud-MyGo!*' | ForEach-Object PSChildName)
}
$before = SyncRoots
$clock = [Diagnostics.Stopwatch]::StartNew()
$owned = -not $State
$failure = $null
try {
    if ($owned) {
        $State = (& (Join-Path $PSScriptRoot 'test-server.ps1') -Start | Select-Object -Last 1)
        Write-Output ("PASS server start {0:n1}s" -f $clock.Elapsed.TotalSeconds)
    }
    $dir = Split-Path $State -Parent
    $s = Get-Content -LiteralPath $State -Raw | ConvertFrom-Json
    $password = ((Get-Content -LiteralPath (Join-Path $dir 'bootstrap.env')) | Where-Object { $_ -like 'NEXTCLOUD_ADMIN_PASSWORD=*' }) -replace '^NEXTCLOUD_ADMIN_PASSWORD=', ''
    $info = [Diagnostics.ProcessStartInfo]::new($exe)
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    # Credentials and the fixture root reach only the child's environment.
    # The container listens on IPv4 loopback only; 'localhost' may resolve to ::1 first.
    $info.Environment['NEXTCLOUD_TEST_URL'] = "https://127.0.0.1:$($s.Port)"
    $info.Environment['NEXTCLOUD_TEST_USER'] = 'fixture'
    $info.Environment['NEXTCLOUD_TEST_PASSWORD'] = $password
    $info.Environment['SYNCD_TEST_CA_CERT'] = Join-Path $dir 'root.pem'
    $info.Environment['SYNCD_TEST_CA_SHA1'] = $s.RootThumbprint
    $info.Environment['PATH'] = "$qt\bin;$upstream\bin\Release;$root\.dev\native\$native\bin;$root\.dev\vcpkg\installed\$native-windows\bin;" + $env:PATH
    $info.Environment['QT_PLUGIN_PATH'] = "$qt\plugins"
    $child = [Diagnostics.Process]::Start($info)
    $out = $child.StandardOutput.ReadToEndAsync(); $err = $child.StandardError.ReadToEndAsync()
    $budget = 150000
    $code = if ($child.WaitForExit($budget)) { $child.ExitCode } else { $child.Kill($true); $child.WaitForExit(); -1 }
    $null = $out.GetAwaiter().GetResult()
    $lines = $err.GetAwaiter().GetResult() -split "`r?`n" | Where-Object { $_ -match '^(STEP|PASS|FAIL|CLEANUP|STATE)' }
    $lines | ForEach-Object { Write-Output $_ }
    if ($code -ne 0) { $failure = if ($code -eq -1) { "fixture exceeded its $($budget / 1000) s budget" } else { "fixture exit $code" } }
} catch {
    $failure = $_.Exception.Message
} finally {
    if ($owned -and $State) {
        try { & (Join-Path $PSScriptRoot 'test-server.ps1') -Stop -State $State; Write-Output 'PASS server removed' }
        catch { $failure = "server cleanup: $($_.Exception.Message)" }
    }
}
$leaked = @(SyncRoots | Where-Object { $_ -notin $before })
if ($leaked.Count) { $failure = "$($leaked.Count) sync root registration(s) left behind" }
Write-Output ("TOTAL {0:n1}s" -f $clock.Elapsed.TotalSeconds)
if ($failure) { throw $failure }
