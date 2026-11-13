#Requires -Version 7
# Builds one per-user test installer: the UI, an existing SyncD build and only the DLLs they load.
# Inputs: -Arch ARM64|x64; -SyncDBuild, the tools/build-syncd.ps1 tree (default build/syncd-<Arch>);
# -TestVersion X.Y.Z, for upgrade tests only: stamps that version instead of mygo.json's into the UI,
# a SyncD rebuilt in build/syncd-<Arch>-testversion from -SyncDBuild's cache settings, and the manifest
# (marked testVersion), without editing any committed file. SyncD's dependencies must come from
# tools/bootstrap-deps.ps1: the license bundle (tools/licenses.ps1) needs the texts it stages.
# With MYGO_UPDATER_PRIVATE_KEY set (the release secret, or a throwaway `mygo keygen` key for tests) the
# UI gets the update channel of mygo.json's `updates` and the installer a signed update; without it the
# package offers and publishes no updates. -UpdatePublicKey, with -TestVersion only, trusts another key
# than mygo.json's, for update tests with a throwaway key.
# Side effects: writes build/package/<Arch> (build/package/<Arch>-testversion),
# build/package/Nextcloud-MyGo-<version>[-testversion]-<arch>-test-setup.exe and, signed, the archive and
# update-windows-<goarch>.json in build/package/update-<Arch>[-testversion]; never writes into -SyncDBuild,
# never installs, registers or Authenticode-signs anything, never prints the key. Every step is bounded;
# see doc/en-us/packaging.md.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('ARM64', 'x64')][string]$Arch,
    [string]$SyncDBuild = '',
    [ValidatePattern('^\d+\.\d+\.\d+$')][string]$TestVersion = '',
    [string]$UpdatePublicKey = ''
)
if ($UpdatePublicKey -and -not $TestVersion) { throw '-UpdatePublicKey is for -TestVersion packages only; releases trust mygo.json''s key' }
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$goarch = @{ ARM64 = 'arm64'; x64 = 'amd64' }[$Arch]
if (-not $SyncDBuild) { $SyncDBuild = Join-Path $root "build/syncd-$Arch" }
$out = Join-Path $root 'build/package'
$work = Join-Path $out $(if ($TestVersion) { "$Arch-testversion" } else { $Arch })
$total = [Diagnostics.Stopwatch]::StartNew()

# Invoke-Step runs one program with a deadline and fails on a nonzero exit or a timeout.
function Invoke-Step([string]$Name, [int]$Seconds, [string]$File, [string[]]$Arguments, [hashtable]$Environment = @{}) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $info = [Diagnostics.ProcessStartInfo]::new($File)
    foreach ($a in $Arguments) { $info.ArgumentList.Add($a) }
    $info.UseShellExecute = $false
    $info.WorkingDirectory = $root
    foreach ($k in $Environment.Keys) {
        if ($null -eq $Environment[$k]) { [void]$info.Environment.Remove($k) } else { $info.Environment[$k] = $Environment[$k] }
    }
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

# A test version needs a SyncD that reports it: the same sources configured like -SyncDBuild (Qt,
# host Qt, upstream build), in a tree of its own, building only SyncD and the plugin it loads.
$syncd = $SyncDBuild
if ($TestVersion) {
    $syncd = Join-Path $root "build/syncd-$Arch-testversion"
    $cmake = Read-Cache $SyncDBuild 'CMAKE_COMMAND'
    Invoke-Step 'syncd configure' 600 $cmake @('-S', (Join-Path $root 'backend'), '-B', $syncd,
        '-G', (Read-Cache $SyncDBuild 'CMAKE_GENERATOR'), '-A', (Read-Cache $SyncDBuild 'CMAKE_GENERATOR_PLATFORM'),
        "-DCMAKE_PREFIX_PATH=$(Read-Cache $SyncDBuild 'CMAKE_PREFIX_PATH')", "-DQT_HOST_PATH=$(Read-Cache $SyncDBuild 'QT_HOST_PATH')",
        "-DSYNCD_UPSTREAM_BUILD=$(Read-Cache $SyncDBuild 'SYNCD_UPSTREAM_BUILD')", '-DBUILD_TESTING=OFF', "-DSYNCD_TEST_VERSION=$TestVersion")
    Invoke-Step 'syncd build' 3600 $cmake @('--build', $syncd, '--config', 'Release', '--parallel', '--target', 'nextcloud-mygo-syncd', 'syncd_cfapi_plugin')
}

# The DLLs come from exactly the prefixes this SyncD and its upstream build were configured with.
$release = Join-Path $syncd 'Release'
$upstream = Read-Cache $syncd 'SYNCD_UPSTREAM_BUILD'
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
$config = Get-Content (Join-Path $root 'mygo.json') -Raw | ConvertFrom-Json
$version = $config.version
$name = $version
$ldflags = @()
$testFlags = @()
if ($TestVersion) {
    if ($TestVersion -eq $version) { throw "-TestVersion must differ from mygo.json's $version" }
    $version = $TestVersion
    $name = "$version-testversion"
    # Only this build links the test version (internal/version.Version is a variable for this);
    # MYGO_LDFLAGS come after mygo build's own -X flags, so they win.
    $ldflags += "-X github.com/YuanZhe-99/Nextcloud-MyGo/internal/version.Version=$version -X github.com/egoist/mygo.packageVersion=$version"
    $testFlags = @('-test-version', $version)
}
# The update channel exists only in a package whose update is signed (doc/en-us/packaging.md, Updates).
$signing = [bool]$env:MYGO_UPDATER_PRIVATE_KEY
$updateKey = if ($UpdatePublicKey) { $UpdatePublicKey.Trim() } else { $config.updates.publicKey }
$updates = Join-Path $out "update-$Arch$(if ($TestVersion) { '-testversion' })"
if ($signing) {
    if (-not $updateKey -or -not $config.updates.github) { throw 'mygo.json has no updates.publicKey and updates.github' }
    $ldflags += "-X github.com/YuanZhe-99/Nextcloud-MyGo/internal/release.UpdateFeed=https://github.com/$($config.updates.github)/releases/latest/download/ -X github.com/YuanZhe-99/Nextcloud-MyGo/internal/release.UpdateKey=$updateKey"
}
# mygo build never gets the key: its own update archive would hold the UI alone, which must not update
# without SyncD (PLAN.md §16.2).
$stamp = @{ MYGO_UPDATER_PRIVATE_KEY = $null }
if ($ldflags) { $stamp.MYGO_LDFLAGS = $ldflags -join ' ' }
$setup = Join-Path $out "Nextcloud-MyGo-$name-$($Arch.ToLower())-test-setup.exe"
$target = @{ GOOS = 'windows'; GOARCH = $goarch; CGO_ENABLED = '0' }
try {
    Invoke-Step 'checker' 120 'go' @('build', '-o', $tool, './tools/pkg')
    Invoke-Step 'ui' 300 'go' @('tool', 'mygo', 'build', '-platform', "windows/$goarch", '-skip-installer', '-o', (Join-Path $work 'ui')) $stamp
    Invoke-Step 'uninstaller' 180 'go' @('build', '-trimpath', '-o', (Join-Path $work 'uninstall.exe'), './packaging/windows/setup') $target
    $ui = Join-Path $work "ui/windows-$goarch"
    # License texts of exactly what this SyncD and its upstream build linked; the stager refuses without them.
    $notices = Join-Path $work 'notices'
    Invoke-Step 'licenses' 120 (Join-Path $PSHOME 'pwsh') @('-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'licenses.ps1'),
        '-SyncDBuild', $syncd, '-Arch', $Arch, '-Version', $version, '-Out', $notices)
    $staged = Join-Path $work "Nextcloud-MyGo-$version-$Arch"
    Invoke-Step 'stage' 120 $tool (@('stage', '-arch', $Arch, '-out', $staged, '-zip', $payload) + $testFlags + @(
        '-search', ($search -join ';'), '-plugins', (Join-Path $qt 'plugins'),
        '-plugin', 'tls/qopensslbackend.dll', '-plugin', 'sqldrivers/qsqlite.dll',
        # Qt's OpenSSL backend opens these by name at run time; they are not imports.
        '-runtime', "libssl-3-$($Arch.ToLower()).dll", '-runtime', "libcrypto-3-$($Arch.ToLower()).dll",
        '-file', "Nextcloud-MyGo.exe=$(Join-Path $ui 'Nextcloud-MyGo.exe')",
        '-file', "nextcloud-mygo-syncd.exe=$(Join-Path $release 'nextcloud-mygo-syncd.exe')",
        '-file', "sync-exclude.lst=$(Join-Path $release 'sync-exclude.lst')",
        '-file', "uninstall.exe=$(Join-Path $work 'uninstall.exe')",
        '-notices', $notices))
    # The halves and the manifest must state one version: SyncD's own --version (run from the staged
    # directory, beside its DLLs) and the version linked into the UI.
    $manifest = Get-Content (Join-Path $staged 'manifest.json') -Raw | ConvertFrom-Json
    $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $staged 'nextcloud-mygo-syncd.exe'), '--version')
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $p = [Diagnostics.Process]::Start($info)
    $text = $p.StandardOutput.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { $p.Kill($true); throw 'SyncD --version timed out after 30 s' }
    $self = $text.Result | ConvertFrom-Json
    $reported = $self.version
    # -trimpath leaves -ldflags out of the build info, so look for the stamped string itself.
    $linked = -not $TestVersion -or [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $ui 'Nextcloud-MyGo.exe'))).Contains($version)
    if ($manifest.version -ne $version -or $reported -ne $version -or -not $linked -or [bool]$manifest.testVersion -ne [bool]$TestVersion) {
        throw "versions disagree: manifest $($manifest.version) (testVersion $($manifest.testVersion)), SyncD $reported, UI linked $linked, want $version"
    }
    if ($self.controlSchema -ne $manifest.syncd.controlSchema -or $self.protocol.minor -ne $manifest.syncd.protocol.minor -or $self.protocol.major -ne $manifest.syncd.protocol.major) {
        throw "SyncD reports schema $($self.controlSchema) and protocol $($self.protocol.major).$($self.protocol.minor), the manifest $($manifest.syncd.controlSchema) and $($manifest.syncd.protocol.major).$($manifest.syncd.protocol.minor): rebuild SyncD from these sources"
    }
    Write-Host "version ${version}: manifest, SyncD --version and UI agree$(if ($TestVersion) { ' (test version)' })"
    Invoke-Step 'installer' 300 'go' @('build', '-trimpath', '-tags', 'payload', '-o', $setup, './packaging/windows/setup') $target
    Invoke-Step 'check installer' 60 $tool @('check', '-arch', $Arch, $setup)
    Remove-Item -Recurse -Force $updates -ErrorAction SilentlyContinue
    if ($signing) {
        Invoke-Step 'sign update' 120 $tool @('sign-update', '-arch', $Arch, '-version', $version, '-setup', $setup, '-key', $updateKey, '-out', $updates)
    } else {
        Write-Host 'MYGO_UPDATER_PRIVATE_KEY is not set: this package offers and publishes no updates'
    }
} finally {
    Remove-Item -Force $payload -ErrorAction SilentlyContinue
}
$hash = (Get-FileHash -Algorithm SHA256 $setup).Hash.ToLower()
Write-Host ("{0} ({1:n1} MB, sha256 {2}) in {3:n0}s" -f $setup, ((Get-Item $setup).Length / 1MB), $hash, $total.Elapsed.TotalSeconds)
