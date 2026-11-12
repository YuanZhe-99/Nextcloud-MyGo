# Runs upstream's SyncCfApiTest from a tools/build-upstream.ps1 tests tree.
# Inputs: -Build, the upstream tests build; target Qt and dependency bin directories on PATH.
# Side effects: the test registers and removes temporary "OC-TEST" sync roots under %TEMP%. Its one case
#   that opens Explorer windows (testServer_caseClash_createConflict) is left out. Fails if an OC-TEST
#   registration or a shell-extension class of upstream's IDs is left behind.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Build)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-job.ps1')
if (-not (Enter-OwnedJob)) { Invoke-InOwnedJob $PSCommandPath $PSBoundParameters; if ($LASTEXITCODE) { throw 'Upstream CFAPI tests failed.' }; return }
$exe = Join-Path $Build 'bin/Release/SyncCfApiTest.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "No SyncCfApiTest in $Build." }
$classes = '{1E62D59A-6EA4-476C-B707-4A32E88ED822}', '{6FF9B5B6-389F-444A-9FDD-A286C36EA079}'
# Registrations lists the state this run may change, to compare before and after.
function Registrations {
    @(Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager' -ErrorAction SilentlyContinue |
        Where-Object PSChildName -Like 'OC-TEST!*' | ForEach-Object PSChildName) +
    @($classes | Where-Object { Test-Path "HKCU:\Software\Classes\CLSID\$_" })
}
$before = Registrations
$functions = @(& $exe -functions | ForEach-Object { $_.Trim() -replace '\(\)$', '' } |
    Where-Object { $_ -and $_ -ne 'testServer_caseClash_createConflict' })
if ($functions.Count -lt 2) { throw 'Could not list upstream CFAPI test cases.' }
$report = Join-Path $Build 'Testing/SyncCfApiTest.txt'
$clock = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $exe -ArgumentList (@('-o', "$report,txt") + $functions) -WorkingDirectory (Join-Path $Build 'bin') -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $Build 'Testing/SyncCfApiTest.log')
if (-not $p.WaitForExit(300000)) { $p.Kill($true); $p.WaitForExit(); throw 'SyncCfApiTest exceeded 300 s.' }
$summary = Select-String -LiteralPath $report -Pattern '^Totals:' | Select-Object -Last 1
Write-Output ("{0} ({1} of {2} cases, {3:n1}s)" -f $summary.Line, $functions.Count, ($functions.Count + 1), $clock.Elapsed.TotalSeconds)
$left = @(Registrations | Where-Object { $_ -notin $before })
if ($left.Count) { throw "$($left.Count) registration(s) left behind by SyncCfApiTest." }
if ($p.ExitCode -ne 0) { throw "SyncCfApiTest failed (exit $($p.ExitCode)); see $report." }
