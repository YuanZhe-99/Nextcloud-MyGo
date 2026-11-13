# Runs the renewal cancellation/replacement lifetime regression (default) or one real network acceptance scenario.
# Inputs: compiled contract executable and approved NEXTCLOUD_TEST_* environment; matching native DLL paths.
# Side effects: one disposable profile per run and borrowed native-vault copies; the transfer scenarios also reserve one remote content collection; held-revocation, network-chunked, network-uncertain and network-large need an isolated server and SYNCD_TEST_CA_*. switch-kill ends its own child at each named stage and restarts it on the same profile. Parent cleanup runs after a child crash.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [ValidateSet('lifetime','held-revocation','network','network-lifetime','network-active','network-chunked','network-uncertain','network-large','file-safety','file-safety-bulk','multi-account','switch-quota','slow-server','sleep-resume','switch-kill','wrong-instance','data-safety','switching')][string]$Scenario = 'lifetime',
    [ValidateSet('prepared','quiescing','verifying','committed','reconciling')][string[]]$Stage = @('prepared','quiescing','verifying','committed','reconciling'),
    # network-active and network-uncertain run each case in its own process and profile, so every case
    # starts on healthy endpoints instead of waiting out the circuit history of the case before it.
    # data-safety runs its cases the same way; journal, root-replaced and discovery-kill end their
    # first process (exit 75) and check the restarted one on the same profile.
    [ValidateSet('upload','download','unacknowledged-upload','move','delete','final-move',
        'interrupted-change','watcher','root-denied','maintenance','partial-listing','names','excluded','revocation',
        'journal','root-replaced','discovery-kill',
        'auto-switch','auto-fallback','tls-trust','tls-auth','tls-circuit','redirect','subpath','revoke-switch','new-folder','root-change','late-epoch')][string[]]$Case = @()
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-job.ps1')
if (-not (Enter-OwnedJob)) { Invoke-InOwnedJob $PSCommandPath $PSBoundParameters; if ($LASTEXITCODE) { throw 'Owned acceptance failed.' }; return }
if (-not [IO.Path]::IsPathFullyQualified($Executable) -or -not (Test-Path -LiteralPath $Executable)) { throw 'Existing absolute contract path required.' }
if (-not $env:NEXTCLOUD_TEST_URL -or -not $env:NEXTCLOUD_TEST_USER -or -not $env:NEXTCLOUD_TEST_PASSWORD) { throw 'Approved test environment required.' }
$root = Split-Path $PSScriptRoot -Parent
$saved = $env:SYNCD_REAUTH_LIFETIME_ROOT

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
        # Each child has a short budget of its own and every step inside it a shorter one; only the opt-in
        # above-4-GiB case is long. Cleanup is direct and bounded.
        # network waits out the daemon's own circuit-open and stability windows twice, so it gets more;
        # sleep-resume holds itself suspended for 35 s of its two minutes.
        $timeout = if ($Argument -eq '--cleanup') { 60000 } elseif ($Scenario -eq 'network-large') { 2400000 } elseif ($Scenario -eq 'network') { 240000 } elseif ($Scenario -eq 'sleep-resume') { 120000 } else { 150000 }
        $code = if ($child.WaitForExit($timeout)) { $child.ExitCode } else { $child.Kill($true); $child.WaitForExit(); -1 }
        return @{ Code = $code; Budget = [int]($timeout / 1000); Output = $output.GetAwaiter().GetResult(); Error = $errors.GetAwaiter().GetResult() }
    } finally { $child.Dispose() }
}

# RunProfile runs the given child arguments in order on one fresh profile, then cleans it.
# Inputs: each step is an argument and the exit code it must end with; 75 is the fixture's own kill.
function RunProfile([object[]]$Steps) {
    $profile = Join-Path $root ('build/reauth-lifetime-' + [Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $profile
    $env:SYNCD_REAUTH_LIFETIME_ROOT = $profile
    $failure = ''
    try {
        foreach ($step in $Steps) {
            $result = RunOwned $step.Argument
            if ($result.Code -ne $step.Code) {
                # Completed stages locate the failure; only fixed-vocabulary PASS lines are echoed.
                [regex]::Matches($result.Output, '(?m)^PASS [A-Za-z0-9 _,/.-]*\r?$') | ForEach-Object { Write-Output $_.Value.Trim() }
                # The last steps reached, with their start times, show where the time went.
                $steps = @([regex]::Matches($result.Output, '(?m)^STEP (\d+) ([A-Za-z0-9 _,/().-]{3,160})\r?$') | Select-Object -Last 6)
                foreach ($s in $steps) { Write-Output ('  step at {0} ms: {1}' -f $s.Groups[1].Value, $s.Groups[2].Value) }
                $lastStep = if ($steps) { $steps[-1].Groups[2].Value } else { 'setup' }
                $summary = if ($result.Code -eq -1) { "child exceeded its $($result.Budget) s budget in step: $lastStep" } else { (([regex]::Matches($result.Error, '(?m)^[A-Za-z][A-Za-z0-9 _,/().:-]{3,160}\r?$') | Select-Object -Last 3).Value.Trim() -join '; ') }
                throw ('Owned network/renewal acceptance failed (' + $step.Argument + ', exit 0x' + [Convert]::ToString([int]$result.Code,16) + '). ' + $summary)
            }
            $passed = [regex]::Matches($result.Output, '(?m)^PASS .*\r?$') | ForEach-Object { $_.Value.Trim() }
            if ($passed) { Write-Output $passed }
            # SYNCD_TEST_STEPS=1 shows each step's start time (ms since the child started) to find slow steps.
            if ($env:SYNCD_TEST_STEPS -eq '1') { [regex]::Matches($result.Output, '(?m)^STEP \d+ [A-Za-z0-9 _,/().-]{3,160}\r?$') | ForEach-Object { Write-Output $_.Value.Trim() } }
        }
    } catch {
        $failure = $_.Exception.Message
        throw
    } finally {
        $cleanup = RunOwned '--cleanup'
        $env:SYNCD_REAUTH_LIFETIME_ROOT = $saved
        # A failed cleanup keeps the case's own failure in its message.
        if ($cleanup.Code -ne 0) { throw ("Owned content/native credential cleanup needs attention; profile retained. " + $failure).Trim() }
        Remove-Item -LiteralPath $profile -Recurse -Force
    }
}

if ($Scenario -eq 'switch-kill') {
    foreach ($name in $Stage) {
        RunProfile @(@{ Argument = "--switch-kill=$name"; Code = 75 }, @{ Argument = "--switch-recover=$name"; Code = 0 })
    }
} elseif ($Scenario -eq 'data-safety') {
    $cases = if ($Case) { $Case } else { @('interrupted-change','watcher','root-denied','maintenance','partial-listing','names','excluded','revocation','journal','root-replaced','discovery-kill') }
    foreach ($name in $cases) {
        if ($name -in 'journal','root-replaced','discovery-kill') { RunProfile @(@{ Argument = "--data-safety=$name"; Code = 75 }, @{ Argument = "--data-recover=$name"; Code = 0 }) }
        else { RunProfile @(@{ Argument = "--data-safety=$name"; Code = 0 }) }
    }
} elseif ($Scenario -in 'network-active','network-uncertain') {
    $cases = if ($Case) { $Case } elseif ($Scenario -eq 'network-active') { @('upload','download','unacknowledged-upload') } else { @('move','delete','final-move') }
    foreach ($name in $cases) { RunProfile @(@{ Argument = "--$Scenario=$name"; Code = 0 }) }
} elseif ($Scenario -eq 'switching') {
    # Each switching case runs in a process and profile of its own (PLAN §20.3).
    $cases = if ($Case) { $Case } else { @('auto-switch','auto-fallback','tls-trust','tls-auth','tls-circuit','redirect','subpath','revoke-switch','new-folder','root-change','late-epoch') }
    foreach ($name in $cases) { RunProfile @(@{ Argument = "--switching=$name"; Code = 0 }) }
} else {
    RunProfile @(@{ Argument = '--' + $Scenario; Code = 0 })
}
