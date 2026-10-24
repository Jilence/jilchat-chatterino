# Local Windows build without GitHub Actions. Run scripts/dev-setup.ps1 once
# first. The first run installs the Conan dependencies and configures CMake
# (takes a while); after that only changed files are compiled. Needs Visual
# Studio 2022 (C++), Python with Conan and LLVM (winget install LLVM.LLVM).
#
#   powershell -ExecutionPolicy Bypass -File scripts\dev-build.ps1         # build
#   powershell -ExecutionPolicy Bypass -File scripts\dev-build.ps1 -Run    # build and start
#   powershell -ExecutionPolicy Bypass -File scripts\dev-build.ps1 -Clean  # start over
#
# Everything lands in build-dev\ (ignored by git); the app is
# build-dev\bin\Leafyrino.exe. It runs portable: settings and logins live in
# build-dev\bin, your installed Chatterino's settings are never touched.
# Pass -SharedSettings to use the normal settings folder instead.

param(
    [switch]$Run,
    [switch]$Clean,
    [switch]$SharedSettings,
    [string]$Config = 'RelWithDebInfo',
    [string]$QtDir = 'C:\Qt\6.9.3\msvc2022_64'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build-dev'
$exe = Join-Path $build 'bin\Leafyrino.exe'

if (-not (Test-Path (Join-Path $QtDir 'bin\qmake.exe'))) {
    throw "Qt not found in $QtDir - run scripts\dev-setup.ps1 first."
}

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}
New-Item -ItemType Directory -Force $build | Out-Null

# VsDevCmd is a batch file: a PATH entry with & or % in it (like a mistyped
# "%JAVA_HOME&/bin") breaks it and cuts PATH off. Only this script's own
# session is cleaned up; the system PATH stays as it is.
$env:Path = ($env:Path -split ';' | Where-Object { $_ -and $_ -notmatch '[&%]' }) -join ';'

# Conan lives in Python's scripts folder, which isn't always on PATH.
$conanDir = $null
$found = Get-Command conan -ErrorAction SilentlyContinue
if ($found) {
    $conanDir = Split-Path -Parent $found.Source
} else {
    $dirs = @(
        (python -c "import sysconfig; print(sysconfig.get_path('scripts'))"),
        (python -c "import sysconfig; print(sysconfig.get_path('scripts', 'nt_user'))")
    )
    $conanDir = $dirs | Where-Object { $_ -and (Test-Path (Join-Path $_ 'conan.exe')) } | Select-Object -First 1
}
if (-not $conanDir) { throw 'Conan not found: python -m pip install --user conan' }

# The Visual Studio developer environment (compiler, CMake, Ninja), once per shell.
if (-not $env:VSCMD_VER) {
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
    $env:Path = "$installer;$env:Path"  # VsDevCmd calls vswhere itself
    $vswhere = Join-Path $installer 'vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ workload was not found.' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
$env:Path = "$conanDir;$env:Path"

# Like CI, the code is compiled with clang-cl (it uses C++23 that MSVC's own
# compiler doesn't take yet): winget install LLVM.LLVM
$llvm = Join-Path $env:ProgramFiles 'LLVM\bin'
if (-not (Test-Path (Join-Path $llvm 'clang-cl.exe'))) { throw 'clang-cl not found: winget install LLVM.LLVM' }
$env:Path = "$llvm;$env:Path"

# A Conan home of its own, so your global Conan config and profiles are
# never changed; it starts with the current ConanCenter remote.
$env:CONAN_HOME = Join-Path $env:LOCALAPPDATA 'leafyrino-dev\conan'
if (-not (Test-Path (Join-Path $env:CONAN_HOME 'profiles\default'))) {
    conan profile detect | Out-Null
}

Push-Location $build
try {
    if (-not (Test-Path 'conan_toolchain.cmake')) {
        Write-Host '== Conan dependencies (first run only)'
        conan install $root -s build_type=$Config -s compiler.cppstd=20 `
            -c tools.cmake.cmaketoolchain:generator=Ninja `
            -b missing --output-folder=. -o with_openssl3=True
        if ($LASTEXITCODE) { throw 'conan install failed' }
    }

    if (-not (Test-Path 'build.ninja')) {
        Write-Host '== CMake configure (first run only)'
        cmake -S $root -B . -G Ninja `
            "-DCMAKE_BUILD_TYPE=$Config" `
            -DCMAKE_TOOLCHAIN_FILE="conan_toolchain.cmake" `
            -DCMAKE_CXX_COMPILER=clang-cl `
            -DCMAKE_C_COMPILER=clang-cl `
            -DCMAKE_RC_COMPILER=llvm-rc `
            -DCMAKE_LINKER_TYPE=LLD `
            "-DCMAKE_PREFIX_PATH=$QtDir" `
            -DUSE_PRECOMPILED_HEADERS=ON `
            -DBUILD_WITH_CRASHPAD=OFF `
            -DCHATTERINO_LTO=OFF
        if ($LASTEXITCODE) { throw 'cmake configure failed' }
    }

    # Windows locks a running exe, so the linker can't replace it: close the
    # dev build if it's open (only this one, never an installed Leafyrino).
    $running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
    foreach ($p in $running) {
        Write-Host "== Closing the running dev build (PID $($p.Id))"
        $p.CloseMainWindow() | Out-Null
        if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force }
    }

    Write-Host '== Build'
    cmake --build . --parallel
    if ($LASTEXITCODE) { throw 'build failed' }

    # Qt's DLLs next to the exe, once (and again after -Clean).
    if (-not (Test-Path (Join-Path $build 'bin\Qt6Core.dll'))) {
        Write-Host '== Copying Qt runtime next to the exe'
        & (Join-Path $QtDir 'bin\windeployqt.exe') $exe --release --no-translations --no-opengl-sw --force-openssl
        if ($LASTEXITCODE) { throw 'windeployqt failed' }
    }
    # Conan's shared libraries (OpenSSL); without them Windows refuses to start the exe.
    if (-not (Test-Path (Join-Path $build 'bin\libssl-3-x64.dll'))) {
        Write-Host '== Copying Conan runtime next to the exe'
        conan install $root -s build_type=$Config -s compiler.cppstd=20 `
            -c tools.cmake.cmaketoolchain:generator=Ninja `
            -b missing --output-folder=. -o with_openssl3=True `
            --deployer=runtime_deploy --deployer-folder=bin
        if ($LASTEXITCODE) { throw 'conan runtime deploy failed' }
    }
} finally {
    Pop-Location
}

$modes = Join-Path $build 'bin\modes'
if ($SharedSettings) {
    Remove-Item -Force $modes -ErrorAction SilentlyContinue
} else {
    Set-Content -Path $modes -Value 'portable' -Encoding ascii
}

Write-Host "Built $exe"
if ($Run) {
    Start-Process $exe
}
