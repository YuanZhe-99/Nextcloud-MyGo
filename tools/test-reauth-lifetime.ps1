# Exercises renewal cancellation/replacement inside real read-only DAV verification.
# Inputs: compiled contract executable and approved NEXTCLOUD_TEST_* environment; matching native DLL paths.
# Side effects: one disposable profile and borrowed native-vault copies; network-active also reserves one remote content collection. Parent cleanup runs after a child crash.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable, [ValidateSet('lifetime','network','network-lifetime','network-active','wrong-instance')][string]$Scenario = 'lifetime')
$ErrorActionPreference = 'Stop'
if (-not [IO.Path]::IsPathFullyQualified($Executable) -or -not (Test-Path -LiteralPath $Executable)) { throw 'Existing absolute contract path required.' }
if (-not $env:NEXTCLOUD_TEST_URL -or -not $env:NEXTCLOUD_TEST_USER -or -not $env:NEXTCLOUD_TEST_PASSWORD) { throw 'Approved test environment required.' }
$root = Split-Path $PSScriptRoot -Parent
$profile = Join-Path $root ('build/reauth-lifetime-' + [Guid]::NewGuid().ToString('N'))
$saved = $env:SYNCD_REAUTH_LIFETIME_ROOT
$null = New-Item -ItemType Directory -Path $profile
$env:SYNCD_REAUTH_LIFETIME_ROOT = $profile

# RunOwned waits for the exact console/GUI-subsystem child and keeps all native output in memory.
function RunOwned([string]$Argument) {
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.ArgumentList.Add($Argument)
    $child = [Diagnostics.Process]::new()
    $child.StartInfo = $info
    try {
        if (-not $child.Start()) { throw 'Owned lifetime child launch failed.' }
        $output = $child.StandardOutput.ReadToEndAsync()
        $errors = $child.StandardError.ReadToEndAsync()
        $timeout = if ($Scenario -eq 'network-active' -and $Argument -ne '--cleanup') { 900000 } elseif ($Scenario -eq 'network' -and $Argument -ne '--cleanup') { 600000 } else { 150000 }
        if (-not $child.WaitForExit($timeout)) { $child.Kill(); $child.WaitForExit(); throw 'Owned lifetime child exceeded its deadline.' }
        return @{ Code = $child.ExitCode; Output = $output.GetAwaiter().GetResult(); Error = $errors.GetAwaiter().GetResult() }
    } finally { $child.Dispose() }
}

try {
    $result = RunOwned ('--' + $Scenario)
    if ($result.Code -ne 0) {
        # Completed stages locate the failure; only fixed-vocabulary PASS lines are echoed.
        [regex]::Matches($result.Output, '(?m)^PASS [A-Za-z0-9 _,/.-]*\r?$') | ForEach-Object { Write-Output $_.Value.Trim() }
        $summary = [regex]::Match($result.Error, '(?m)^(?:renewal|owned|lifetime|replacement|in-flight|cancelled|removed|endpoint)[A-Za-z0-9 _,/-]*\r?$').Value.Trim()
        throw ('Owned network/renewal acceptance failed (exit 0x' + [Convert]::ToString([int]$result.Code,16) + '). ' + $summary)
    }
    Write-Output $result.Output.Trim()
} finally {
    $cleanup = RunOwned '--cleanup'
    $env:SYNCD_REAUTH_LIFETIME_ROOT = $saved
    if ($cleanup.Code -ne 0) { throw 'Owned content/native credential cleanup needs attention; profile retained.' }
    Remove-Item -LiteralPath $profile -Recurse -Force
}
