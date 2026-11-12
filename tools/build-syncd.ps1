# Builds and tests the connected SyncD unit without installation or Shell registration.
# Inputs: architecture, existing Qt target/host prefixes, the upstream build to link, and -Build, the
#   output tree (default build/syncd-<architecture>).
# Side effects: writes the build tree and runs disposable core contract tests.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('ARM64','x64')][string]$Architecture,
    [Parameter(Mandatory)][string]$QtPrefix,
    [Parameter(Mandatory)][string]$QtHostPrefix,
    [string]$UpstreamBuild = '',
    [string]$Build = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Visual Studio CMake tools are required.' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
$build = if ($Build) { $Build } else { Join-Path $root "build/syncd-$Architecture" }
& $cmake -S (Join-Path $root 'backend') -B $build -G 'Visual Studio 18 2026' -A $Architecture "-DCMAKE_PREFIX_PATH=$QtPrefix" "-DQT_HOST_PATH=$QtHostPrefix" "-DSYNCD_UPSTREAM_BUILD=$UpstreamBuild" -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'SyncD configure failed.' }
& $cmake --build $build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'SyncD build failed.' }
$oldPath = $env:PATH
$oldPlugins = $env:QT_PLUGIN_PATH
try {
    $env:PATH = (Join-Path $QtPrefix 'bin') + ';' + $oldPath
    $env:QT_PLUGIN_PATH = Join-Path $QtPrefix 'plugins'
    & $ctest --test-dir $build -C Release --output-on-failure --no-tests=error
    if ($LASTEXITCODE -ne 0) { throw 'SyncD contracts failed.' }
} finally {
    $env:PATH = $oldPath
    $env:QT_PLUGIN_PATH = $oldPlugins
}
