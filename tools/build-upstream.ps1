# Builds the recorded upstream engine or its real test suite without installing it.
# Inputs: target architecture, build mode, and existing native dependency prefixes.
# Side effects: writes only the build tree; tests execute only in tests mode.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('ARM64', 'x64')][string]$Architecture,
    [Parameter(Mandatory)][ValidateSet('engine', 'tests')][string]$Mode,
    [Parameter(Mandatory)][string]$QtPrefix,
    [Parameter(Mandatory)][string]$QtHostPrefix,
    [Parameter(Mandatory)][string[]]$DependencyPrefix
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $root 'third_party/nextcloud'
$record = Get-Content (Join-Path $root 'build-lock.json') -Raw | ConvertFrom-Json
$commit = & git -C $source rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $commit -ne $record.nextcloudDesktop.commit) {
    throw 'Initialize the recorded upstream submodule before building.'
}
$changes = & git -C $source status --porcelain --untracked-files=normal
if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Upstream snapshot must be unmodified.' }
$patchFiles = @($record.nextcloudDesktop.patches | ForEach-Object {
    $file = Join-Path $root $_.path
    if ((Get-FileHash $file -Algorithm SHA256).Hash -ne $_.sha256) { throw 'Upstream patch hash mismatch.' }
    $file
})
$sourceId = $commit.Substring(0, 12)
if ($patchFiles.Count) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($record.nextcloudDesktop.patches.sha256 -join ':'))
    $sourceId += '-' + [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).Substring(0, 12)
    $patched = Join-Path $root "build/src-$sourceId"
    if (-not (Test-Path $patched)) {
        & git -c core.longpaths=true clone --quiet --no-hardlinks --no-checkout $source $patched
        if ($LASTEXITCODE -ne 0) { throw 'Could not create isolated upstream build source.' }
        & git -c core.longpaths=true -C $patched checkout --quiet --detach $commit
        if ($LASTEXITCODE -ne 0) { throw 'Could not check out recorded upstream commit.' }
        foreach ($file in $patchFiles) {
            & git -C $patched apply --check $file
            if ($LASTEXITCODE -ne 0) { throw 'Upstream patch does not apply.' }
            & git -C $patched apply $file
            if ($LASTEXITCODE -ne 0) { throw 'Could not apply upstream patch.' }
        }
    } else {
        # Validate overlapping patches sequentially in a private index, without editing sources.
        $savedIndex = $env:GIT_INDEX_FILE
        $index = Join-Path $root ('build/patch-check-' + [Guid]::NewGuid().ToString('N'))
        try {
            $env:GIT_INDEX_FILE = $index
            & git -C $patched read-tree HEAD
            if ($LASTEXITCODE -ne 0) { throw 'Could not initialize patch verification index.' }
            & git -C $patched add --update
            if ($LASTEXITCODE -ne 0) { throw 'Could not index existing build sources.' }
            for ($i = $patchFiles.Count - 1; $i -ge 0; $i--) {
                & git -C $patched apply --cached --reverse $patchFiles[$i]
                if ($LASTEXITCODE -ne 0) { throw 'Existing build source differs from the recorded patch set; inspect it before rebuilding.' }
            }
            & git -C $patched diff --cached --quiet $commit
            if ($LASTEXITCODE -ne 0) { throw 'Existing build source contains unrecorded tracked changes.' }
        } finally {
            $env:GIT_INDEX_FILE = $savedIndex
            Remove-Item -LiteralPath $index -Force -ErrorAction SilentlyContinue
        }
    }
    $source = $patched
}
foreach ($prefix in @($QtPrefix, $QtHostPrefix) + $DependencyPrefix) {
    if (-not [IO.Path]::IsPathFullyQualified($prefix) -or -not (Test-Path $prefix -PathType Container)) {
        throw 'SDK and dependency prefixes must be existing absolute directories.'
    }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Visual Studio CMake tools are required.' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
$fingerprint = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($sourceId))).Substring(0, 12)
$build = Join-Path $root "build/$($Architecture.Substring(0, 1).ToLowerInvariant())$($Mode.Substring(0, 1))-$fingerprint"
$gui = if ($Mode -eq 'tests') { 'ON' } else { 'OFF' }
$prefixes = (@($QtPrefix) + $DependencyPrefix) -join ';'
& $cmake -S $source -B $build -G 'Visual Studio 18 2026' -A $Architecture `
    "-DCMAKE_PREFIX_PATH=$prefixes" "-DQT_HOST_PATH=$QtHostPrefix" "-DCMAKE_INSTALL_PREFIX=$build/staging" `
    "-DBUILD_GUI=$gui" "-DBUILD_TESTING=$gui" -DBUILD_SHELL_INTEGRATION=OFF -DBUILD_CFAPI_VFS=ON `
    -DBUILD_UPDATER=OFF -DWITH_CRASHREPORTER=OFF `
    -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF -DKDE_SKIP_UNINSTALL_TARGET=ON
if ($LASTEXITCODE -ne 0) { throw 'Upstream configure failed; no build or test success is claimed.' }
$target = if ($Mode -eq 'engine') { @('nextcloudcmd', 'nextcloudsync_vfs_cfapi') } else { @('ALL_BUILD') }
& $cmake --build $build --config Release --target $target --parallel
if ($LASTEXITCODE -ne 0) { throw 'Upstream build failed.' }
if ($Mode -eq 'tests') {
    $savedPath = $env:PATH
    try {
        $bins = @((Join-Path $QtPrefix 'bin')) + @($DependencyPrefix | ForEach-Object { Join-Path $_ 'bin' })
        $env:PATH = ($bins -join ';') + ';' + $savedPath
        # UtilityTest creates a real startup entry; SyncCfApiTest registers sync roots and one case opens
        # Explorer windows, so tools/test-cfapi-upstream.ps1 runs it separately with that case left out.
        & $ctest --test-dir $build -C Release --output-on-failure --no-tests=error -E '^(UtilityTest|SyncCfApiTest)$'
        if ($LASTEXITCODE -ne 0) { throw 'Upstream tests failed or no tests were executed.' }
    } finally { $env:PATH = $savedPath }
}
