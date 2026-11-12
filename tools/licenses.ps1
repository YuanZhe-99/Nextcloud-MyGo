#Requires -Version 7
# Assembles the license and notice bundle of one package from what it was actually built with.
# Inputs: -SyncDBuild, a tools/build-syncd.ps1 tree; -Arch ARM64|x64 (the Go target); -Out, an empty or new directory;
#   -Version, the package version SOURCE.txt names (default mygo.json's).
# Returns: <Out>/LICENSE.txt (GPL-3.0, the product), THIRD_PARTY_NOTICES.md, SOURCE.txt (where the corresponding
#   source is) and licenses/: Nextcloud Desktop's COPYING and file-level LICENSES, the native libraries' texts that
#   tools/bootstrap-deps.ps1 staged in the upstream build's dependency prefixes, and the Go toolchain's and every
#   linked Go module's license files.
# Side effects: writes only <Out>; runs `go list` and `git`.
# Notes: fails when a dependency prefix carries no share/licenses (a prefix not made by tools/bootstrap-deps.ps1)
#   or a linked Go module has no license file, so a bundle is never silently incomplete.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SyncDBuild,
    [Parameter(Mandatory)][ValidateSet('ARM64', 'x64')][string]$Arch,
    [Parameter(Mandatory)][string]$Out,
    [string]$Version = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Version) { $Version = (Get-Content (Join-Path $root 'mygo.json') -Raw | ConvertFrom-Json).version }

# Read-Cache returns one CMake cache value of a build tree.
function Read-Cache([string]$Tree, [string]$Key) {
    $line = Select-String -LiteralPath (Join-Path $Tree 'CMakeCache.txt') -Pattern "^$Key(:[A-Z]+)?=" | Select-Object -First 1
    if (-not $line) { throw "$Tree has no $Key" }
    return ($line.Line -split '=', 2)[1]
}

# Copy-Into copies files into <Out>/<Relative>, creating the directory.
function Copy-Into([string]$Relative, [object[]]$Files) {
    $dir = Join-Path $Out $Relative
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($f in $Files) { Copy-Item -LiteralPath $f -Destination $dir -Recurse -Force }
}

if ((Test-Path $Out) -and (Get-ChildItem $Out -Force | Select-Object -First 1)) { throw "$Out is not empty" }
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $Out 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') $Out

$upstream = Read-Cache $SyncDBuild 'SYNCD_UPSTREAM_BUILD'
Copy-Into 'licenses/Nextcloud-Desktop' @((Join-Path $root 'third_party/nextcloud/COPYING'), (Join-Path $root 'third_party/nextcloud/LICENSES'))

# Qt comes first in the upstream build's prefixes; bootstrap-deps put the Qt texts into the native prefix.
$prefixes = (Read-Cache $upstream 'CMAKE_PREFIX_PATH') -split ';' | Where-Object { $_ } | Select-Object -Skip 1
$staged = $prefixes | ForEach-Object { Join-Path $_ 'share/licenses' } | Where-Object { Test-Path $_ }
if (-not $staged) { throw "No dependency prefix of $upstream has share/licenses: rebuild the dependencies with tools/bootstrap-deps.ps1 and SyncD against them (hand-built prefixes are not a source for packages)" }
foreach ($dir in $staged) { Copy-Into 'licenses' (Get-ChildItem $dir) }

# Go: the toolchain's standard library and every module linked into the UI and the installer.
$goarch = @{ ARM64 = 'arm64'; x64 = 'amd64' }[$Arch]
$saved = $env:GOOS, $env:GOARCH
$env:GOOS, $env:GOARCH = 'windows', $goarch
try {
    $goroot = (& go -C $root env GOROOT).Trim()
    $packages = & go -C $root list -deps -f '{{with .Module}}{{if not .Main}}{{.Path}}@{{.Version}}|{{.Dir}}{{end}}{{end}}|{{.Dir}}' ./cmd/nextcloud-mygo ./packaging/windows/setup
    if ($LASTEXITCODE -ne 0) { throw 'go list failed' }
} finally { $env:GOOS, $env:GOARCH = $saved }
Copy-Into 'licenses/Go' @(Join-Path $goroot 'LICENSE')
$pattern = '^(LICEN[CS]E|COPYING|NOTICE)(\..*)?$'
$modules = @{}
foreach ($line in $packages) {
    $module, $moduleDir, $packageDir = $line -split '\|'
    if (-not $module) { continue }
    if (-not $modules.ContainsKey($module)) { $modules[$module] = [Collections.Generic.HashSet[string]]::new() }
    foreach ($dir in $moduleDir, $packageDir) {
        Get-ChildItem -LiteralPath $dir -File | Where-Object Name -Match $pattern | ForEach-Object { [void]$modules[$module].Add($_.FullName) }
    }
}
foreach ($module in $modules.Keys | Sort-Object) {
    if (-not $modules[$module].Count) { throw "Go module $module has no license file" }
    $moduleDir = ($packages | Where-Object { $_.StartsWith("$module|") } | Select-Object -First 1).Split('|')[1]
    foreach ($file in $modules[$module]) {
        $rel = [IO.Path]::GetRelativePath($moduleDir, (Split-Path $file -Parent))
        Copy-Into (Join-Path "licenses/Go/$($module -replace '[\\/:@]', '_')" $(if ($rel -eq '.') { '' } else { $rel })) @($file)
    }
}

# The corresponding source: this commit of the public repository, its submodule and the recorded patches.
$lock = Get-Content (Join-Path $root 'build-lock.json') -Raw | ConvertFrom-Json
$commit = (& git -C $root rev-parse HEAD).Trim()
$tag = (& git -C $root describe --tags --exact-match 2>$null)
$global:LASTEXITCODE = 0 # an untagged commit is fine; its exit code must not fail the caller
$deps = $lock.preparedDependencies.PSObject.Properties | ForEach-Object {
    $v = $_.Value
    $shown = if ($v.version) { $v.version } else { "tool $($v.toolVersion)" }
    '  {0} {1}{2}' -f $_.Name, $shown, $(if ($v.commit) { " (commit $($v.commit))" } elseif ($v.portVersion) { " (vcpkg port version $($v.portVersion))" } else { '' })
}
@"
Nextcloud MyGo $Version for Windows ${Arch}: corresponding source

Nextcloud MyGo is free software under the GNU General Public License version 3 (LICENSE.txt). Its
complete corresponding source, including the scripts used to build this package, is the public
repository https://github.com/YuanZhe-99/Nextcloud-MyGo at $(if ($tag) { "tag $tag, " })commit $commit.

It links Nextcloud Desktop $($lock.nextcloudDesktop.tag) (commit $($lock.nextcloudDesktop.commit)), the Git
submodule third_party/nextcloud, unmodified, with the patches in patches/nextcloud/ applied in the order
build-lock.json lists them ($($lock.nextcloudDesktop.patches.Count) patches). Nextcloud Desktop is GPL-2.0-or-later with other
file-level licenses (licenses/Nextcloud-Desktop).

Native libraries, built or fetched by tools/bootstrap-deps.ps1 at exactly these versions:
  Qt $($lock.preparedDependencies.qt.version) (official binaries from download.qt.io; LGPL-3.0, source at https://download.qt.io/official_releases/qt/)
$($deps | Where-Object { $_ -notmatch '^  qt ' } | Out-String)
Their license texts are in licenses/. The Microsoft Visual C++ runtime DLLs are redistributed under the
Visual Studio license terms. Build steps: doc/en-us/development.md and doc/en-us/ci.md in the repository.
"@ | Set-Content (Join-Path $Out 'SOURCE.txt')
Write-Host "$((Get-ChildItem $Out -Recurse -File).Count) files in $Out"
