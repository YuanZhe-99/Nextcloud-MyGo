# Repeats upstream timeout recovery followed by another discovery fixture (M0 lifetime audit).
# Inputs: a built native RemoteDiscoveryTest executable and its dependency PATH.
# Side effects: temporary test fixtures and diagnostic output under ignored build/.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$executablePath = (Resolve-Path $Executable).Path
$output = Join-Path $root ('build/discovery-lifetime-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $output | Out-Null
for ($iteration = 1; $iteration -le 20; $iteration++) {
    $process = Start-Process -FilePath $executablePath -WorkingDirectory (Split-Path $executablePath) `
        -ArgumentList @('testRemoteDiscoveryError:Timeout', 'testMissingData') `
        -RedirectStandardOutput (Join-Path $output "$iteration.stdout") `
        -RedirectStandardError (Join-Path $output "$iteration.stderr") -PassThru
    try {
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Discovery lifetime regression timed out at iteration $iteration."
        }
        if ($process.ExitCode -ne 0) {
            throw "Discovery lifetime regression failed at iteration ${iteration}: exit $($process.ExitCode)."
        }
    } finally { $process.Dispose() }
}
Write-Output 'Discovery timeout/next-fixture regression: 20/20 processes exited successfully.'
