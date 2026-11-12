#Requires -Version 7
# Assembles SyncD's native dependency prefixes for one architecture at exactly what build-lock.json records.
# Inputs: -Architecture ARM64|x64 (the target); -Prefix, the directory to fill (default .dev/deps).
# Returns: writes <Prefix>/env-<arch>.json naming QtPrefix, QtHostPrefix, DependencyPrefix and PKG_CONFIG
#   for tools/build-upstream.ps1 and tools/build-syncd.ps1, and prints its path last.
# Side effects: downloads Qt archives from download.qt.io (SHA-1 checked), clones vcpkg and the source
#   dependencies at their recorded commits, builds and installs them under <Prefix>; installs nothing
#   system-wide and changes no PATH outside its own child processes. See doc/en-us/ci.md.
# Notes: a completed architecture is skipped while its stamp matches the record and this script; a recorded
#   commit, version or port version that the fetched source does not match is a failure, never a fallback.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('ARM64', 'x64')][string]$Architecture,
    [string]$Prefix = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Prefix) { $Prefix = Join-Path $root '.dev/deps' }
$Prefix = [IO.Path]::GetFullPath($Prefix)
$lock = (Get-Content (Join-Path $root 'build-lock.json') -Raw | ConvertFrom-Json).preparedDependencies
$arch = $Architecture.ToLowerInvariant()
$triplet = "$arch-windows"
$hostArch = if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'x64' }
$qtVersion = $lock.qt.version
$qtRoot = Join-Path $Prefix "Qt/$qtVersion"
$qtTarget = Join-Path $qtRoot $(if ($arch -eq 'arm64') { 'msvc2022_arm64' } else { 'msvc2022_64' })
$qtHost = Join-Path $qtRoot 'msvc2022_64'
$native = Join-Path $Prefix "native/$arch"
$vcpkg = Join-Path $Prefix 'vcpkg'
$watch = [Diagnostics.Stopwatch]::StartNew()

# Sources: build-lock.json records version and commit; the repository and tag spelling are fixed here.
$sources = [ordered]@{
    ecm                 = @{ Dir = 'extra-cmake-modules'; Url = 'https://invent.kde.org/frameworks/extra-cmake-modules.git'; Tag = 'v{0}' }
    qtKeychain          = @{ Dir = 'qtkeychain'; Url = 'https://github.com/frankosterfeld/qtkeychain.git'; Tag = 'v{0}' }
    kdSingleApplication = @{ Dir = 'kdsingleapplication'; Url = 'https://github.com/KDAB/KDSingleApplication.git'; Tag = 'v{0}' }
    karchive            = @{ Dir = 'karchive'; Url = 'https://invent.kde.org/frameworks/karchive.git'; Tag = 'v{0}' }
    libp11              = @{ Dir = 'libp11'; Url = 'https://github.com/OpenSC/libp11.git'; Tag = 'libp11-{0}' }
}

# Step prints one timed line per stage so CI logs show where cold time goes.
function Step([string]$Name, [scriptblock]$Body) {
    $w = [Diagnostics.Stopwatch]::StartNew()
    & $Body
    Write-Host ('[bootstrap {0}] {1} {2:n1}s' -f $Architecture, $Name, $w.Elapsed.TotalSeconds)
}

# Run runs a native program and fails on a nonzero exit.
function Run([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$(Split-Path $File -Leaf) $($Arguments[0]) failed with exit code $LASTEXITCODE" }
}

# Get-Source fetches one tag shallowly into <Prefix>/src/<dir> and requires the recorded commit.
function Get-Source([string]$Key) {
    $s = $sources[$Key]
    $dir = Join-Path $Prefix "src/$($s.Dir)-$arch"
    $want = $lock.$Key.commit
    if ((Test-Path (Join-Path $dir '.git')) -and (& git -C $dir rev-parse HEAD) -eq $want) {
        Run git @('-C', $dir, 'clean', '-fdxq'); Run git @('-C', $dir, 'checkout', '-q', '--', '.')
        return $dir
    }
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    Run git @('-c', 'init.defaultBranch=main', 'init', '-q', $dir)
    Run git @('-C', $dir, 'fetch', '-q', '--depth', '1', $s.Url, "refs/tags/$($s.Tag -f $lock.$Key.version)")
    $got = & git -C $dir rev-parse 'FETCH_HEAD^{commit}'
    if ($got -ne $want) { throw "$Key tag resolves to $got, build-lock.json records $want" }
    Run git @('-C', $dir, 'checkout', '-q', '--detach', $got)
    return $dir
}

# Get-VisualStudio returns the Visual Studio whose CMake tools/build-upstream.ps1 uses too.
function Get-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Visual Studio CMake tools are required.' }
    return $vs
}
$vs = Get-VisualStudio
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'

# CMakeProject configures, builds and installs one source tree into the native prefix.
function CMakeProject([string]$Source, [string]$Name, [string[]]$Prefixes, [string[]]$Options) {
    $build = Join-Path $Prefix "b/$Name-$arch"
    Remove-Item -Recurse -Force $build -ErrorAction SilentlyContinue
    Run $cmake (@('-S', $Source, '-B', $build, '-G', 'Visual Studio 18 2026', '-A', $Architecture,
            "-DCMAKE_PREFIX_PATH=$($Prefixes -join ';')", "-DQT_HOST_PATH=$qtHost", "-DCMAKE_INSTALL_PREFIX=$native") + $Options)
    Run $cmake @('--build', $build, '--config', 'Release', '--parallel')
    Run $cmake @('--install', $build, '--config', 'Release')
    Remove-Item -Recurse -Force $build
}

$scriptHash = (Get-FileHash $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$record = [ordered]@{ script = $scriptHash; architecture = $Architecture; dependencies = $lock }
$stampFile = Join-Path $native 'bootstrap.json'
$envFile = Join-Path $Prefix "env-$arch.json"
$stampWanted = $record | ConvertTo-Json -Depth 8 -Compress

# Qt: the target SDK and the x64 host SDK, extracted unmodified (the layout .dev/Qt was made with).
Step 'qt' {
    $short = $qtVersion -replace '\.', ''
    $downloads = Join-Path $Prefix 'downloads'
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $flavours = @('msvc2022_64') + $(if ($arch -eq 'arm64') { @('msvc2022_arm64_cross_compiled') } else { @() })
    foreach ($flavour in $flavours) {
        $dir = Join-Path $qtRoot ($flavour -replace '_cross_compiled$', '')
        $done = Join-Path $dir '.bootstrap-archives'
        $base = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_$short/qt6_${short}_$flavour"
        [xml]$xml = (Invoke-WebRequest "$base/Updates.xml" -MaximumRetryCount 4 -RetryIntervalSec 5).Content
        $packages = $xml.Updates.PackageUpdate | Where-Object { $_.Name -match "(qt5compat|qtwebsockets|qtshadertools)\.win64_$flavour`$|^qt\.qt6\.$short\.win64_$flavour`$" }
        $archives = foreach ($package in $packages) {
            foreach ($archive in ($package.DownloadableArchives -split ',')) {
                $archive = $archive.Trim()
                if ($archive -and $archive -notmatch '^(qtdoc|qttranslations)-') { [pscustomobject]@{ Url = "$base/$($package.Name)/$($package.Version)$archive"; Name = $archive } }
            }
        }
        if (-not $archives) { throw "Qt $qtVersion $flavour lists no archives" }
        $list = ($archives.Url | Sort-Object) -join "`n"
        if ((Test-Path $done) -and (Get-Content $done -Raw) -eq $list) { continue }
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $dir | Out-Null
        foreach ($a in $archives) {
            $file = Join-Path $downloads $a.Name
            $body = (Invoke-WebRequest "$($a.Url).sha1" -MaximumRetryCount 4 -RetryIntervalSec 5).Content
            if ($body -is [byte[]]) { $body = [Text.Encoding]::ASCII.GetString($body) }
            $expected = ($body -split '\s+')[0].Trim()
            if (-not (Test-Path $file) -or (Get-FileHash $file -Algorithm SHA1).Hash -ne $expected) {
                Invoke-WebRequest $a.Url -OutFile $file -MaximumRetryCount 4 -RetryIntervalSec 5
            }
            if ((Get-FileHash $file -Algorithm SHA1).Hash -ne $expected) { throw "Checksum mismatch: $($a.Name)" }
            Run 7z @('x', $file, "-o$dir", '-y', '-bso0', '-bsp0')
        }
        [IO.File]::WriteAllText($done, $list)
    }
    foreach ($dir in $qtTarget, $qtHost) { if (-not (Test-Path (Join-Path $dir 'lib/cmake/Qt6/Qt6Config.cmake'))) { throw "$dir is not a Qt SDK" } }
}

if ((Test-Path $stampFile) -and (Get-Content $stampFile -Raw) -eq $stampWanted -and (Test-Path $envFile)) {
    Write-Host "[bootstrap $Architecture] native prefix matches the record"
} else {
    Remove-Item -Recurse -Force $native -ErrorAction SilentlyContinue

    # vcpkg at the recorded commit: OpenSSL, SQLite and zlib with the default dynamic triplet.
    Step 'vcpkg' {
        if (-not (Test-Path (Join-Path $vcpkg '.git')) -or (& git -C $vcpkg rev-parse HEAD) -ne $lock.vcpkg.commit) {
            Remove-Item -Recurse -Force $vcpkg -ErrorAction SilentlyContinue
            Run git @('-c', 'init.defaultBranch=main', 'init', '-q', $vcpkg)
            Run git @('-C', $vcpkg, 'fetch', '-q', '--depth', '1', 'https://github.com/microsoft/vcpkg.git', $lock.vcpkg.commit)
            Run git @('-C', $vcpkg, 'checkout', '-q', '--detach', 'FETCH_HEAD')
        }
        $tool = Join-Path $vcpkg 'vcpkg.exe'
        if (-not (Test-Path $tool)) { Run (Join-Path $vcpkg 'bootstrap-vcpkg.bat') @('-disableMetrics') }
        $toolVersion = (& $tool version) -join "`n"
        if ($toolVersion -notmatch [regex]::Escape($lock.vcpkg.toolVersion)) { throw "vcpkg tool is not $($lock.vcpkg.toolVersion)" }
        $saved = @{}
        # Unset, not empty: vcpkg rejects an empty VCPKG_* path. PKG_CONFIG would replace its pinned pkgconf.
        foreach ($n in 'VCPKG_ROOT', 'VCPKG_DOWNLOADS', 'VCPKG_DEFAULT_TRIPLET', 'VCPKG_FEATURE_FLAGS', 'PKG_CONFIG') {
            $saved[$n] = [Environment]::GetEnvironmentVariable($n)
            Remove-Item "env:$n" -ErrorAction SilentlyContinue
        }
        $env:VCPKG_DISABLE_METRICS = '1'
        try {
            Run $tool @('install', '--classic', "--vcpkg-root=$vcpkg", "--triplet=$triplet", '--clean-after-build', 'openssl', 'sqlite3[json1]', 'zlib')
            $installed = (& $tool list --classic "--vcpkg-root=$vcpkg" --x-json | ConvertFrom-Json -AsHashtable)
        } finally { foreach ($n in $saved.Keys) { if ($null -ne $saved[$n]) { Set-Item "env:$n" $saved[$n] } } }
        foreach ($port in @(@('openssl', 'openssl'), @('sqlite3', 'sqlite'), @('zlib', 'zlib'))) {
            $got = $installed["$($port[0]):$triplet"]
            $want = $lock.($port[1])
            $wantPort = if ($want.portVersion) { [int]$want.portVersion } else { 0 }
            if (-not $got -or $got.version -ne $want.version -or [int]$got.port_version -ne $wantPort) {
                throw "vcpkg installed $($port[0]) $($got.version)#$($got.port_version), build-lock.json records $($want.version)#$wantPort"
            }
        }
    }
    $vcpkgInstalled = Join-Path $vcpkg "installed/$triplet"

    Step 'ecm' { CMakeProject (Get-Source 'ecm') 'ecm' @($qtTarget) @('-DBUILD_TESTING=OFF', '-DBUILD_DOC=OFF') }
    Step 'qtkeychain' {
        CMakeProject (Get-Source 'qtKeychain') 'qtkeychain' @($qtTarget) @('-DBUILD_WITH_QT6=ON', '-DBUILD_SHARED_LIBS=ON',
            '-DBUILD_TEST_APPLICATION=OFF', '-DBUILD_TRANSLATIONS=ON', '-DBUILD_TRANSLATIONS_AS_RESOURCES=OFF', '-DUSE_CREDENTIAL_STORE=ON')
    }
    Step 'kdsingleapplication' {
        CMakeProject (Get-Source 'kdSingleApplication') 'kdsa' @($qtTarget) @('-DKDSingleApplication_QT6=ON', '-DKDSingleApplication_STATIC=OFF',
            '-DKDSingleApplication_TESTS=OFF', '-DKDSingleApplication_EXAMPLES=OFF', '-DKDSingleApplication_DOCS=OFF')
    }
    Step 'karchive' {
        # BUILD_SHARED_LIBS=TRUE as originally: KF6ArchiveConfig.cmake embeds the spelling.
        CMakeProject (Get-Source 'karchive') 'karchive' @($qtTarget, $native, $vcpkgInstalled) @('-DBUILD_WITH_QT6=ON', '-DBUILD_SHARED_LIBS=TRUE',
            '-DBUILD_TESTING=OFF', '-DWITH_BZIP2=OFF', '-DWITH_LIBLZMA=OFF', '-DWITH_LIBZSTD=OFF', '-DWITH_OPENSSL=ON')
    }

    # libp11 has no CMake build on Windows: its nmake makefile, against vcpkg's OpenSSL, libp11.dll only.
    Step 'libp11' {
        $src = Get-Source 'libp11'
        $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
        $spec = @{ 'arm64-arm64' = 'arm64'; 'arm64-x64' = 'arm64_amd64'; 'x64-x64' = 'amd64'; 'x64-arm64' = 'amd64_arm64' }["$hostArch-$arch"]
        $machine = if ($arch -eq 'arm64') { '/MACHINE:ARM64' } else { '/MACHINE:X64' }
        Run cmd.exe @('/d', '/c', "call `"$vcvars`" $spec >nul && cd /d `"$src\src`" && nmake /nologo /f Makefile.mak BUILD_FOR=WIN64 MACHINE=$machine OPENSSL_DIR=`"$vcpkgInstalled`" libp11.dll")
        foreach ($d in 'bin', 'lib/pkgconfig', 'include') { New-Item -ItemType Directory -Force (Join-Path $native $d) | Out-Null }
        Copy-Item (Join-Path $src 'src/libp11.dll') (Join-Path $native 'bin')
        Copy-Item (Join-Path $src 'src/libp11.lib') (Join-Path $native 'lib')
        foreach ($h in 'libp11.h', 'p11_err.h', 'p11_ver.h') { Copy-Item (Join-Path $src "src/$h") (Join-Path $native 'include') }
        $pc = "prefix=`${pcfiledir}/../..`nexec_prefix=`${prefix}`nlibdir=`${exec_prefix}/lib`nincludedir=`${prefix}/include`n`n" +
            "Name: libp11`nDescription: PKCS11 interface library`nVersion: $($lock.libp11.version)`nLibs: -L`${libdir} -lp11`nCflags: -I`${includedir}`n"
        [IO.File]::WriteAllText((Join-Path $native 'lib/pkgconfig/libp11.pc'), $pc)
    }

    # License texts of every library a package ships, into share/licenses/<component> for tools/licenses.ps1.
    # Qt's binary archives carry none: they come from each shipped module's source tag (LICENSES/ only).
    Step 'licenses' {
        $out = Join-Path $native 'share/licenses'
        foreach ($module in 'qtbase', 'qt5compat', 'qtsvg', 'qtwebsockets') {
            $dir = Join-Path $Prefix "src/qt-licenses/$module"
            if (-not (Test-Path (Join-Path $dir 'LICENSES'))) {
                Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
                Run git @('-c', 'init.defaultBranch=main', 'init', '-q', $dir)
                Run git @('-C', $dir, 'sparse-checkout', 'set', '--no-cone', '/LICENSES/')
                Run git @('-C', $dir, 'fetch', '-q', '--depth', '1', '--filter=blob:none', "https://github.com/qt/$module.git", "refs/tags/v$qtVersion")
                Run git @('-C', $dir, 'checkout', '-q', '--detach', 'FETCH_HEAD')
            }
            New-Item -ItemType Directory -Force (Join-Path $out "Qt/$module") | Out-Null
            Copy-Item (Join-Path $dir 'LICENSES/*') (Join-Path $out "Qt/$module")
        }
        foreach ($port in @(@('openssl', 'OpenSSL'), @('sqlite3', 'SQLite'), @('zlib', 'zlib'))) {
            New-Item -ItemType Directory -Force (Join-Path $out $port[1]) | Out-Null
            Copy-Item (Join-Path $vcpkgInstalled "share/$($port[0])/copyright") (Join-Path $out "$($port[1])/copyright.txt")
        }
        foreach ($c in @(@('qtKeychain', 'QtKeychain', 'COPYING'), @('karchive', 'KArchive', 'LICENSES/*'), @('libp11', 'libp11', 'COPYING'))) {
            New-Item -ItemType Directory -Force (Join-Path $out $c[1]) | Out-Null
            Copy-Item (Join-Path $Prefix "src/$($sources[$c[0]].Dir)-$arch/$($c[2])") (Join-Path $out $c[1])
        }
    }

    # Upstream's CMake finds libp11 through pkg-config: the msys2 pkgconf package for this host that vcpkg's
    # recorded commit pins (URL and SHA-512 from its own script; vcpkg acquires it only when it builds a port).
    Step 'pkg-config' {
        $flavour = if ($hostArch -eq 'arm64') { 'clangarm64' } else { 'mingw64' }
        $pins = Get-Content (Join-Path $vcpkg 'scripts/cmake/vcpkg_find_acquire_program(PKGCONFIG).cmake') -Raw
        $m = [regex]::Match($pins, "URL `"(https://[^`"]+/$flavour/[^`"/]+pkgconf[^`"/]+)`"\s+SHA512 ([0-9a-f]{128})")
        if (-not $m.Success) { throw "vcpkg pins no $flavour pkgconf" }
        $file = Join-Path $Prefix "downloads/$([IO.Path]::GetFileName($m.Groups[1].Value))"
        # vcpkg_acquire_msys.cmake's mirrors in its order; the SHA-512 decides, so a mirror that answers with
        # anything else (an error or a browser check page) is simply the next one's turn.
        $mirrors = [regex]::Matches((Get-Content (Join-Path $vcpkg 'scripts/cmake/vcpkg_acquire_msys.cmake') -Raw), '(?m)^\s*"(https://[^"]+/)"\s*$') | ForEach-Object { $_.Groups[1].Value }
        foreach ($url in @($m.Groups[1].Value) + @($mirrors | ForEach-Object { $m.Groups[1].Value -replace '^https://mirror\.msys2\.org/', $_ })) {
            if ((Test-Path $file) -and (Get-FileHash $file -Algorithm SHA512).Hash -eq $m.Groups[2].Value) { break }
            try { Invoke-WebRequest $url -OutFile $file -MaximumRetryCount 2 -RetryIntervalSec 3 } catch { Write-Host "pkgconf: $url failed" }
        }
        if (-not (Test-Path $file) -or (Get-FileHash $file -Algorithm SHA512).Hash -ne $m.Groups[2].Value) { throw 'No mirror served the pinned pkgconf package' }
        $dir = Join-Path $Prefix "tools/pkgconf-$hostArch"
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $dir | Out-Null
        Run (Join-Path $env:SystemRoot 'System32/tar.exe') @('-xf', $file, '-C', $dir)
        $script:pkgConfig = Join-Path $dir "$flavour/bin/pkg-config.exe"
        if ((& $pkgConfig --version) -notmatch '^\d') { throw "$pkgConfig does not run" }
    }

    # Every installed PE file must be the target architecture (AA64 / 8664).
    $machineWanted = if ($arch -eq 'arm64') { 0xAA64 } else { 0x8664 }
    foreach ($f in Get-ChildItem $native, (Join-Path $vcpkgInstalled 'bin') -Recurse -Include '*.dll', '*.exe') {
        $b = [IO.File]::ReadAllBytes($f.FullName)
        $pe = [BitConverter]::ToInt32($b, 0x3c)
        if ([BitConverter]::ToUInt16($b, $pe + 4) -ne $machineWanted) { throw "$($f.FullName) is not $Architecture" }
    }

    Remove-Item -Recurse -Force (Join-Path $Prefix "b"), (Join-Path $vcpkg 'buildtrees'), (Join-Path $vcpkg 'packages') -ErrorAction SilentlyContinue
    [ordered]@{
        Architecture     = $Architecture
        QtPrefix         = $qtTarget
        QtHostPrefix     = $qtHost
        DependencyPrefix = @($native, $vcpkgInstalled)
        PkgConfig        = $pkgConfig
    } | ConvertTo-Json | Set-Content $envFile
    [IO.File]::WriteAllText($stampFile, $stampWanted)
}
Write-Host ('[bootstrap {0}] total {1:n1}s' -f $Architecture, $watch.Elapsed.TotalSeconds)
Write-Output $envFile
