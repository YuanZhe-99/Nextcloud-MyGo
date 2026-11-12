# Dot-sourced by the fixture wrappers: runs them inside a kill-on-close Windows Job Object.
# Side effects: a direct call relaunches the calling script in one child pwsh that owns the job, so every
# daemon, probe and test executable it starts dies when that child exits, fails or is killed. The caller's
# own shell is never put in the job.

if (-not ('NextcloudMyGoTestJob' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NextcloudMyGoTestJob {
    [StructLayout(LayoutKind.Sequential)] struct Basic {
        public long UserTime, JobUserTime; public uint LimitFlags; public UIntPtr MinWorkingSet, MaxWorkingSet;
        public uint ActiveProcesses; public UIntPtr Affinity; public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] struct Extended {
        public Basic Basic; public ulong R, W, O, RB, WB, OB; public UIntPtr ProcessMemory, JobMemory, PeakProcess, PeakJob;
    }
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetInformationJobObject(IntPtr job, int kind, ref Extended info, int size);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern uint SetErrorMode(uint mode);
    static IntPtr job = IntPtr.Zero;
    // Enter assigns this process to a kill-on-close job; the handle stays open until the process ends.
    public static void Enter() {
        if (job != IntPtr.Zero) return;
        var handle = CreateJobObjectW(IntPtr.Zero, null);
        var info = new Extended();
        info.Basic.LimitFlags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if (handle == IntPtr.Zero || !SetInformationJobObject(handle, 9, ref info, Marshal.SizeOf(info)) ||
            !AssignProcessToJobObject(handle, GetCurrentProcess()))
            throw new InvalidOperationException("Fixture job object unavailable.");
        job = handle;
        // A child that cannot load a DLL fails with its exit code instead of a modal
        // system error box; every process started from here inherits this mode.
        SetErrorMode(0x0001 | 0x8000); // SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX
    }
}
'@
}

# Enter-OwnedJob returns $true when this process runs inside an owned job (entering it for a nested
# owned process), or $false when the caller must relaunch itself with Invoke-InOwnedJob.
function Enter-OwnedJob {
    if ($env:NEXTCLOUD_MYGO_TEST_JOB -eq [string]$PID) { return $true }
    if (-not $env:NEXTCLOUD_MYGO_TEST_JOB) { return $false }
    $env:NEXTCLOUD_MYGO_TEST_JOB = [string]$PID
    [NextcloudMyGoTestJob]::Enter()
    return $true
}

# Invoke-InOwnedJob runs the script with its parameters in one child pwsh that owns the job; the
# child's output streams through, and $LASTEXITCODE is its result.
function Invoke-InOwnedJob([string]$Script, [hashtable]$Parameters) {
    $plain = @{}
    foreach ($key in $Parameters.Keys) {
        $value = $Parameters[$key]
        $plain[$key] = if ($value -is [switch]) { [bool]$value } else { $value }
    }
    $env:NEXTCLOUD_MYGO_TEST_ARGS = $plain | ConvertTo-Json -Compress -Depth 3
    $command = '$env:NEXTCLOUD_MYGO_TEST_JOB = [string]$PID; . ''' + (Join-Path $PSScriptRoot 'test-job.ps1') +
        '''; [NextcloudMyGoTestJob]::Enter(); $p = @{}; if ($env:NEXTCLOUD_MYGO_TEST_ARGS) { $p = $env:NEXTCLOUD_MYGO_TEST_ARGS | ConvertFrom-Json -AsHashtable }; ' +
        '$env:NEXTCLOUD_MYGO_TEST_ARGS = $null; try { & ''' + $Script + ''' @p; exit 0 } catch { Write-Output (''FAIL '' + $_.Exception.Message); exit 1 }'
    try {
        & (Join-Path $PSHOME 'pwsh') -NoProfile -NonInteractive -Command $command
    } finally {
        $env:NEXTCLOUD_MYGO_TEST_ARGS = $null
    }
}