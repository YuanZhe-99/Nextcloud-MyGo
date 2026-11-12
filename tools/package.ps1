#Requires -Version 7
# Builds one per-user test installer: the UI, an existing SyncD build and only the DLLs they load.
# Inputs: -Arch ARM64|x64; -SyncDBuild, the tools/build-syncd.ps1 tree (default build/syncd-<Arch>).
# Side effects: writes build/package/<Arch> and build/package/Nextcloud-MyGo-<version>-<arch>-test-setup.exe;
# never installs, registers or signs anything. Every step is bounded; see doc/en-us/packaging.md.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('ARM64', 'x64')][string]$Arch,
    [string]$SyncDBuild = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$goarch = @{ ARM64 = 'arm64'; x64 = 'amd64' }[$Arch]
if (-not $SyncDBuild) { $SyncDBuild = Join-Path $root "build/syncd-$Arch" }
$out = Join-Path $root 'build/package'
$work = Join-Path $out $Arch
$total = [Diagnostics.Stopwatch]::StartNew()

# Invoke-Step runs one program with a deadline and fails on a nonzero exit or a timeout.
function Invoke-Step([string]$Name, [int]$Seconds, [string]$File, [string[]]$Arguments, [hashtable]$Environment = @{}) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $info = [Diagnostics.ProcessStartInfo]::new($File)
    foreach ($a in $Arguments) { $info.ArgumentList.Add($a) }
    $info.UseShellExecute = $false
    $info.WorkingDirectory = $root
    foreach ($k in $Environment.Keys) { $info.Environment[$k] = $Environment[$k] }
    $p = [Diagnostics.Process]::Start($info)
    if (-not $p.WaitForExit($Seconds * 1000)) {
        $p.Kill($true)
        throw "$Name timed out after $Seconds s"
    }
    if ($p.ExitCode -ne 0) { throw "$Name failed with exit code $($p.ExitCode)" }
    Write-Host ("[{0}] {1:n1}s" -f $Name, $watch.Elapsed.TotalSeconds)
}

# Read-Cache returns one CMake cache value of a build tree.
function Read-Cache([string]$Tree, [string]$Key) {
    $line = Select-String -LiteralPath (Join-Path $Tree 'CMakeCache.txt') -Pattern "^$Key(:[A-Z]+)?=" | Select-Object -First 1
    if (-not $line) { throw "$Tree has no $Key" }
    return ($line.Line -split '=', 2)[1]
}

# The DLLs come from exactly the prefixes this SyncD and its upstream build were configured with.
$release = Join-Path $SyncDBuild 'Release'
$upstream = Read-Cache $SyncDBuild 'SYNCD_UPSTREAM_BUILD'
$prefixes = (Read-Cache $upstream 'CMAKE_PREFIX_PATH') -split ';' | Where-Object { $_ }
$qt = $prefixes[0]
$vs = & (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe') -latest -products '*' -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC/Redist/MSVC') -Directory | Where-Object Name -Match '^\d+\.' | Sort-Object { [version]$_.Name } | Select-Object -Last 1 |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName $Arch.ToLower()) -Directory -Filter 'Microsoft.VC*.CRT' } | Select-Object -First 1
if (-not $crt) { throw "No Visual C++ $Arch runtime under $vs" }
$search = @($release, (Join-Path $upstream 'bin/Release')) + ($prefixes | ForEach-Object { Join-Path $_ 'bin' }) + $crt.FullName
foreach ($f in 'nextcloud-mygo-syncd.exe', 'sync-exclude.lst') {
    if (-not (Test-Path (Join-Path $release $f))) { throw "$release has no $f; run tools/build-syncd.ps1 first" }
}
Write-Host "SyncD $release`nupstream $upstream`nQt $qt`nCRT $($crt.FullName)"

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
$tool = Join-Path $out 'pkg.exe'
$payload = Join-Path $root 'packaging/windows/setup/payload.zip'
$version = (Get-Content (Join-Path $root 'mygo.json') -Raw | ConvertFrom-Json).version
$setup = Join-Path $out "Nextcloud-MyGo-$version-$($Arch.ToLower())-test-setup.exe"
$target = @{ GOOS = 'windows'; GOARCH = $goarch; CGO_ENABLED = '0' }
try {
    Invoke-Step 'checker' 120 'go' @('build', '-o', $tool, './tools/pkg')
    Invoke-Step 'ui' 300 'go' @('tool', 'mygo', 'build', '-platform', "windows/$goarch", '-skip-installer', '-o', (Join-Path $work 'ui'))
    Invoke-Step 'uninstaller' 180 'go' @('build', '-trimpath', '-o', (Join-Path $work 'uninstall.exe'), './packaging/windows/setup') $target
    $ui = Join-Path $work "ui/windows-$goarch"
    Invoke-Step 'stage' 120 $tool @('stage', '-arch', $Arch, '-out', (Join-Path $work "Nextcloud-MyGo-$version-$Arch"), '-zip', $payload,
        '-search', ($search -join ';'), '-plugins', (Join-Path $qt 'plugins'),
        '-plugin', 'tls/qopensslbackend.dll', '-plugin', 'sqldrivers/qsqlite.dll',
        # Qt's OpenSSL backend opens these by name at run time; they are not imports.
        '-runtime', "libssl-3-$($Arch.ToLower()).dll", '-runtime', "libcrypto-3-$($Arch.ToLower()).dll",
        '-file', "Nextcloud-MyGo.exe=$(Join-Path $ui 'Nextcloud-MyGo.exe')",
        '-file', "nextcloud-mygo-syncd.exe=$(Join-Path $release 'nextcloud-mygo-syncd.exe')",
        '-file', "sync-exclude.lst=$(Join-Path $release 'sync-exclude.lst')",
        '-file', "uninstall.exe=$(Join-Path $work 'uninstall.exe')",
        '-file', "LICENSE.txt=$(Join-Path $root 'LICENSE')",
        '-file', "licenses/Nextcloud-Desktop-COPYING.txt=$(Join-Path $root 'third_party/nextcloud/COPYING')")
    Invoke-Step 'installer' 300 'go' @('build', '-trimpath', '-tags', 'payload', '-o', $setup, './packaging/windows/setup') $target
    Invoke-Step 'check installer' 60 $tool @('check', '-arch', $Arch, $setup)
} finally {
    Remove-Item -Force $payload -ErrorAction SilentlyContinue
}
$hash = (Get-FileHash -Algorithm SHA256 $setup).Hash.ToLower()
Write-Host ("{0} ({1:n1} MB, sha256 {2}) in {3:n0}s" -f $setup, ((Get-Item $setup).Length / 1MB), $hash, $total.Elapsed.TotalSeconds)
