#Requires -Version 7
# Installs a built package for the current user, launches it, reinstalls it over itself, optionally
# upgrades it to a higher version, uninstalls it twice (keeping, then removing user data) and proves
# nothing is left behind (PLAN.md §16, R19/R20).
# Inputs: -Setup, the installer tools/package.ps1 built for this machine's architecture; -UpgradeTo, a
# higher-version installer of the same architecture (tools/package.ps1 -TestVersion), installed over
# the running -Setup version through daemon.prepareUpdate. -UpdateDir, with -UpgradeTo, makes that upgrade
# the in-app one instead: the signed update tools/package.ps1 wrote for -UpgradeTo (build/package/
# update-<Arch>-testversion), served over loopback HTTP, checked against -UpdateKey (its public key) and
# handed to its installer as the interface does (internal/release TestInstalledUpdate).
# Side effects: a temporary per-user install, Start menu shortcut, Apps entry and startup entry, all
# removed again; runs under a throwaway interface/SyncD profile, never the default one, and never
# touches the official Nextcloud client. Refuses to run over an existing Nextcloud-MyGo install.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Setup, [string]$UpgradeTo = '', [string]$UpdateDir = '', [string]$UpdateKey = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$Setup = (Resolve-Path $Setup).Path
if ($UpgradeTo) { $UpgradeTo = (Resolve-Path $UpgradeTo).Path }
if ($UpdateDir) {
    if (-not $UpgradeTo -or -not $UpdateKey) { throw '-UpdateDir needs -UpgradeTo and -UpdateKey' }
    $UpdateDir = (Resolve-Path $UpdateDir).Path
    $updateTest = Join-Path $repo 'build/package/update.test.exe'
    & go test -c -o $updateTest ./internal/release
    if ($LASTEXITCODE -ne 0) { throw 'building the update test failed' }
}
$programs = [Environment]::GetFolderPath('LocalApplicationData') + '\Programs'
$root = Join-Path $programs 'Nextcloud-MyGo'
$appsKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Nextcloud-MyGo'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runName = 'com.yuanzhe.nextcloud-mygo'
$link = Join-Path ([Environment]::GetFolderPath('Programs')) 'Nextcloud MyGo.lnk'
$profileName = 'pkgsmoke-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$hash = -join ([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($profileName))[0..11] | ForEach-Object { $_.ToString('x2') })
$data = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) "Nextcloud-MyGo\syncd\$hash"
$settings = Join-Path ([Environment]::GetFolderPath('ApplicationData')) "Nextcloud-MyGo-$profileName"
$tool = Join-Path $repo 'build/package/pkg.exe'
$total = [Diagnostics.Stopwatch]::StartNew()

if (Test-Path $root) { throw "$root exists; uninstall Nextcloud-MyGo first. This test never runs over a real install." }
if (-not (Test-Path $tool)) { throw "Run tools/package.ps1 first ($tool is missing)." }

Add-Type -Namespace Smoke -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct MachineInfo { public ushort ProcessMachine; public ushort Reserved; public uint Attributes; }
[DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool GetProcessInformation(IntPtr process, int infoClass, out MachineInfo info, int size);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
'@

# Invoke-Bounded runs a program under the throwaway profile with a deadline; its output is shown and
# kept in $script:output.
function Invoke-Bounded([string]$Name, [int]$Seconds, [string]$File, [string[]]$Arguments = @()) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $info = [Diagnostics.ProcessStartInfo]::new($File)
    foreach ($a in $Arguments) { $info.ArgumentList.Add($a) }
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.WorkingDirectory = $env:TEMP
    $info.Environment['NEXTCLOUD_MYGO_PROFILE'] = $profileName
    $p = [Diagnostics.Process]::Start($info)
    $text = $p.StandardOutput.ReadToEndAsync()
    $exited = $p.WaitForExit($Seconds * 1000)
    if (-not $exited) { $p.Kill($true) }
    $script:output = $text.Result
    if ($script:output) { Write-Host $script:output.TrimEnd() }
    if (-not $exited) { throw "$Name timed out after $Seconds s" }
    if ($p.ExitCode -ne 0) { throw "$Name failed with exit code $($p.ExitCode)" }
    Write-Host ("[{0}] {1:n1}s" -f $Name, $watch.Elapsed.TotalSeconds)
}

# Get-Report reads the running SyncD's version, pause state, counts and startup report through its pipe.
# SyncD's pipe serves one client at a time, so the interface is closed first; SyncD keeps running.
function Get-Report([string[]]$Options = @()) {
    Get-Ours | Where-Object Name -EQ 'Nextcloud-MyGo' | Stop-Process -Force
    Wait-For 'the interface to close' 20 { -not (Get-Ours | Where-Object Name -EQ 'Nextcloud-MyGo') }
    $d = Get-Ours | Where-Object Name -EQ 'nextcloud-mygo-syncd' | Select-Object -First 1
    Invoke-Bounded 'report' 90 $tool (@('report', '-profile', $profileName, '-syncd', $d.Path) + $Options)
    return $script:output | ConvertFrom-Json
}

# Get-Manifest returns the manifest of an installed version directory.
function Get-Manifest([string]$Dir) { Get-Content (Join-Path $Dir 'manifest.json') -Raw | ConvertFrom-Json }

# Wait-For polls a condition until it holds or the deadline passes.
function Wait-For([string]$What, [int]$Seconds, [scriptblock]$Condition) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while (-not (& $Condition)) {
        if ([DateTime]::UtcNow -gt $deadline) { throw "timed out after $Seconds s waiting for $What" }
        Start-Sleep -Milliseconds 250
    }
}

# Get-Ours lists processes running from the install root.
function Get-Ours { Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith("$root\", 'OrdinalIgnoreCase') } }

# Get-Machine returns the machine a process actually runs as (GetProcessInformation, ProcessMachineTypeInfo).
function Get-Machine([int]$Id) {
    $h = [Smoke.Native]::OpenProcess(0x1000, $false, $Id)
    try {
        $info = [Smoke.Native+MachineInfo]::new()
        if (-not [Smoke.Native]::GetProcessInformation($h, 9, [ref]$info, 8)) { throw "GetProcessInformation failed for $Id" }
        return '0x{0:X4}' -f $info.ProcessMachine
    } finally { [void][Smoke.Native]::CloseHandle($h) }
}

# Assert-Running checks that the UI and the SyncD it started run natively from the current version directory.
function Assert-Running([string]$Dir) {
    Wait-For 'the interface and SyncD' 30 { $ps = @(Get-Ours); ($ps.Name -contains 'Nextcloud-MyGo') -and ($ps.Name -contains 'nextcloud-mygo-syncd') }
    $want = @{ ARM64 = '0xAA64'; x64 = '0x8664' }[$arch]
    foreach ($p in Get-Ours) {
        if (-not $p.Path.StartsWith("$Dir\", 'OrdinalIgnoreCase')) { throw "$($p.Name) runs from $($p.Path), not the current version" }
        $m = Get-Machine $p.Id
        if ($m -ne $want) { throw "$($p.Name) runs as machine $m, not $want" }
        Write-Host "  $($p.Name) pid $($p.Id) machine $m"
    }
    $d = Get-Ours | Where-Object Name -EQ 'nextcloud-mygo-syncd' | Select-Object -First 1
    $cmd = (Get-CimInstance Win32_Process -Filter "ProcessId=$($d.Id)").CommandLine
    if ($cmd -notmatch [regex]::Escape("--profile $profileName")) { throw "SyncD was not started by the interface's profile: $cmd" }
    Start-Sleep -Seconds 2 # let SyncD load its TLS and SQL plugins
    $foreign = $d.Modules | Where-Object { -not $_.FileName.StartsWith("$Dir\", 'OrdinalIgnoreCase') -and -not $_.FileName.StartsWith("$env:SystemRoot\", 'OrdinalIgnoreCase') }
    if ($foreign) { throw "SyncD loaded modules from outside its version directory: $($foreign.FileName -join ', ')" }
    $shadowed = $d.Modules | Where-Object { (Test-Path (Join-Path $Dir $_.ModuleName)) -and -not $_.FileName.StartsWith("$Dir\", 'OrdinalIgnoreCase') }
    if ($shadowed) { throw "SyncD loaded installed copies instead of its own: $($shadowed.FileName -join ', ')" }
    $plugins = $d.Modules | Where-Object { $_.FileName.StartsWith("$Dir\plugins\", 'OrdinalIgnoreCase') } | ForEach-Object { $_.FileName.Substring($Dir.Length + 1) }
    Write-Host "  SyncD modules: $(@($d.Modules).Count), all from the version directory or Windows; plugins loaded: $($plugins -join ', ')"
}

# Assert-Installed checks install.json, the shortcut, the Apps entry and the installed files' architecture.
function Assert-Installed {
    $state = Get-Content (Join-Path $root 'install.json') -Raw | ConvertFrom-Json
    $dir = Join-Path $root ($state.current -replace '/', '\')
    $exe = Join-Path $dir 'Nextcloud-MyGo.exe'
    $target = (New-Object -ComObject WScript.Shell).CreateShortcut($link).TargetPath
    if ($target -ne $exe) { throw "shortcut points to $target, not $exe" }
    $apps = Get-ItemProperty $appsKey
    if ($apps.DisplayName -ne 'Nextcloud MyGo (test build)' -or $apps.DisplayIcon -ne "$exe,0" -or $apps.UninstallString -notmatch 'uninstall\.exe" -uninstall$') { throw 'Apps entry is wrong' }
    & $tool check -arch $arch $dir (Join-Path $root 'uninstall.exe') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'installed files failed the architecture check' }
    Write-Host "  installed $($apps.DisplayVersion) in $dir; shortcut and Apps entry point there"
    return $dir
}

# Assert-Gone checks that no process, startup entry, shortcut, Apps entry or program file is left.
function Assert-Gone {
    Wait-For 'the install root to disappear' 20 { -not (Test-Path $root) }
    if (Get-Ours) { throw 'processes from the install root are still running' }
    if ((Get-ItemProperty $runKey).PSObject.Properties.Name -contains $runName) { throw 'startup entry left' }
    if (Test-Path $link) { throw 'shortcut left' }
    if (Test-Path $appsKey) { throw 'Apps entry left' }
    Write-Host '  no process, startup entry, shortcut, Apps entry or program file left'
}

# The official client's registrations and configuration must not change.
function Get-Official {
    $nc = if (Test-Path 'HKCU:\Software\Classes\nc') { (Get-ItemProperty 'HKCU:\Software\Classes\nc').'(default)' } else { '<none>' }
    $cfg = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'Nextcloud'
    $stamp = if (Test-Path $cfg) { (Get-Item $cfg).LastWriteTimeUtc.Ticks } else { 0 }
    $keys = (Get-ChildItem 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall').PSChildName | Where-Object { $_ -ne 'Nextcloud-MyGo' }
    return "nc=$nc;config=$stamp;apps=$($keys -join ',')"
}

# Get-Arch returns the architecture a tools/package.ps1 installer's name states.
function Get-Arch([string]$Path) { switch -Regex ([IO.Path]::GetFileName($Path)) { '-arm64-test-setup\.exe$' { 'ARM64' } '-x64-test-setup\.exe$' { 'x64' } default { throw "$Path is not a tools/package.ps1 installer" } } }
$arch = Get-Arch $Setup
if ($UpgradeTo -and (Get-Arch $UpgradeTo) -ne $arch) { throw '-UpgradeTo is for another architecture' }
$official = Get-Official
$dirs = { (Get-ChildItem ([Environment]::GetFolderPath('ApplicationData')), ([Environment]::GetFolderPath('LocalApplicationData'))).FullName }
$before = & $dirs
Write-Host "profile $profileName; package $arch"
try {
    Write-Host '1. install and launch'
    Invoke-Bounded 'install' 120 $Setup
    $first = Assert-Installed
    Assert-Running $first

    Write-Host '2. reinstall the same version over the running one'
    $oldExe = Join-Path $first 'Nextcloud-MyGo.exe'
    New-ItemProperty $runKey -Name $runName -Value "`"$oldExe`" --mygo-opened-at-login" -Force | Out-Null
    Invoke-Bounded 'reinstall' 120 $Setup
    if ($script:output -notmatch 'SyncD is ready') { throw 'the reinstall did not bring SyncD to the update safe point (daemon.prepareUpdate)' }
    $second = Assert-Installed
    if ($second -eq $first -or (Test-Path $first)) { throw 'the reinstall did not move to a fresh version directory' }
    $run = (Get-ItemProperty $runKey).$runName
    if ($run -ne "`"$(Join-Path $second 'Nextcloud-MyGo.exe')`" --mygo-opened-at-login") { throw "startup entry not moved: $run" }
    Write-Host '  startup entry moved to the new version'
    Assert-Running $second
    $last = $Setup

    if ($UpgradeTo) {
        Write-Host '2b. upgrade the running version to the higher test version'
        $old = Get-Manifest $second
        $pre = Get-Report @('-pause-all')
        if (-not $pre.allPaused -or $pre.version -ne $old.version) { throw "unexpected state before the upgrade: $($pre | ConvertTo-Json -Compress)" }
        Write-Host "  SyncD $($pre.version) running, every folder paused (kept across the upgrade)"
        $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $second 'Nextcloud-MyGo.exe'))
        $info.UseShellExecute = $false
        $info.Environment['NEXTCLOUD_MYGO_PROFILE'] = $profileName
        [void][Diagnostics.Process]::Start($info)
        Assert-Running $second
        $copies = @((Get-ChildItem $data -Filter 'control.pre-update.*.sqlite').Name)
        if ($UpdateDir) {
            $env:NEXTCLOUD_MYGO_UPDATE_DIR, $env:NEXTCLOUD_MYGO_UPDATE_KEY, $env:NEXTCLOUD_MYGO_UPDATE_CURRENT = $UpdateDir, $UpdateKey, $old.version
            try { Invoke-Bounded 'in-app update' 300 $updateTest @('-test.run', '^TestInstalledUpdate$', '-test.v', '-test.count', '1') }
            finally { Remove-Item env:NEXTCLOUD_MYGO_UPDATE_DIR, env:NEXTCLOUD_MYGO_UPDATE_KEY, env:NEXTCLOUD_MYGO_UPDATE_CURRENT -ErrorAction SilentlyContinue }
            if ($script:output -notmatch 'verified the signed') { throw 'the in-app update did not verify the signed update' }
        } else {
            Invoke-Bounded 'upgrade' 300 $UpgradeTo
        }
        if ($script:output -notmatch 'SyncD is ready') { throw 'the upgrade did not bring SyncD to the update safe point (daemon.prepareUpdate)' }
        if (-not (Get-ChildItem $data -Filter 'control.pre-update.*.sqlite' | Where-Object Name -NotIn $copies)) { throw "the upgrade left no new pre-update Control copy in $data" }
        $third = Assert-Installed
        if ($third -eq $second -or (Test-Path $second)) { throw 'the upgrade did not move to a fresh version directory' }
        $next = Get-Manifest $third
        if (-not $next.testVersion -or [version]$next.version -le [version]$old.version) { throw "installed $($next.version) is not a higher test version than $($old.version)" }
        Assert-Running $third
        $post = Get-Report
        $r = $post.startupReport
        if ($post.version -ne $next.version -or $r.previousVersion -ne $old.version -or $r.previousSchema -ne $old.syncd.controlSchema -or
            $r.schema -ne $next.syncd.controlSchema -or $r.selfCheck -ne 'passed' -or -not $post.allPaused -or $post.accounts -ne $pre.accounts) {
            throw "startup report after the upgrade is wrong: $($post | ConvertTo-Json -Compress -Depth 4)"
        }
        Write-Host "  SyncD $($post.version) reports previous version $($r.previousVersion), schema $($r.previousSchema) -> $($r.schema), self-check $($r.selfCheck); profile state kept"
        $last = $UpgradeTo
    }

    Write-Host '3. uninstall, keeping user data'
    Invoke-Bounded 'uninstall' 120 (Join-Path $root 'uninstall.exe') @('-uninstall')
    Assert-Gone
    if (-not (Test-Path $data)) { throw "SyncD data $data was not kept" }
    Write-Host "  kept $data"

    Write-Host '4. install again, then uninstall removing user data'
    Invoke-Bounded 'install' 120 $last
    Assert-Running (Assert-Installed)
    Invoke-Bounded 'uninstall -remove-user-data' 180 (Join-Path $root 'uninstall.exe') @('-uninstall', '-remove-user-data')
    Assert-Gone
    if ((Test-Path $data) -or (Test-Path $settings)) { throw 'user data left after -remove-user-data' }
    Write-Host '  profile data and settings removed'

    if ((Get-Official) -ne $official) { throw 'the official client''s registration or configuration changed' }
    Write-Host '  official client registration and configuration unchanged'
} finally {
    # Leave nothing behind, whatever failed above.
    if (Test-Path (Join-Path $root 'uninstall.exe')) {
        try { Invoke-Bounded 'cleanup uninstall' 180 (Join-Path $root 'uninstall.exe') @('-uninstall', '-remove-user-data') } catch { Write-Warning $_ }
    }
    Get-Ours | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 4
    $left = @($root, $data, $settings, $link) | Where-Object { Test-Path $_ }
    $left | ForEach-Object { Remove-Item -Recurse -Force $_ -ErrorAction SilentlyContinue }
    if (Test-Path $appsKey) { Remove-Item $appsKey -Force }
    Remove-ItemProperty $runKey -Name $runName -ErrorAction SilentlyContinue
    $stray = Get-ChildItem ([Environment]::GetFolderPath('ApplicationData')), ([Environment]::GetFolderPath('LocalApplicationData')) -Filter "*$profileName*" -ErrorAction SilentlyContinue
    if ($stray) { Write-Warning "profile state outside the known directories: $($stray.FullName -join ', ')"; $stray | Remove-Item -Recurse -Force }
    if ($left) { Write-Warning "cleanup removed: $($left -join ', ')" }
    if ($UpdateDir) { Remove-Item $updateTest -ErrorAction SilentlyContinue }
    $new = & $dirs | Where-Object { $_ -notin $before }
    if ($new) { Write-Warning "created during the run and still present: $($new -join ', ')" }
    Write-Host ("total {0:n1}s" -f $total.Elapsed.TotalSeconds)
}
