# One-time setup for local Windows builds (see scripts/dev-build.ps1).
#
# Installs Qt 6.9.3 (the version CI uses) with the modules Leafyrino needs to
# C:\Qt via aqtinstall. Visual Studio 2022 with the C++ workload, Python and
# Conan are expected to be installed already. Nothing outside C:\Qt and your
# Python user packages is changed; your Conan profile stays as it is.
#
#   powershell -ExecutionPolicy Bypass -File scripts\dev-setup.ps1

param(
    [string]$QtVersion = '6.9.3',
    [string]$QtRoot = 'C:\Qt'
)

$ErrorActionPreference = 'Stop'

$qtDir = Join-Path $QtRoot "$QtVersion\msvc2022_64"
if (Test-Path (Join-Path $qtDir 'bin\qmake.exe')) {
    Write-Host "Qt $QtVersion is already installed in $qtDir"
} else {
    Write-Host "Installing aqtinstall (Python user package)..."
    python -m pip install --user --upgrade aqtinstall
    Write-Host "Downloading Qt $QtVersion to $QtRoot (about 1.5 GB)..."
    python -m aqt install-qt windows desktop $QtVersion win64_msvc2022_64 `
        -m qtimageformats qtcharts qtmultimedia -O $QtRoot
}

python -c "import conans" 2>$null
if ($LASTEXITCODE) {
    throw 'Conan is missing: python -m pip install --user conan'
}
# dev-build.ps1 uses a Conan home of its own; nothing to set up here.

Write-Host ''
Write-Host 'Done. Build and start with:'
Write-Host '  powershell -ExecutionPolicy Bypass -File scripts\dev-build.ps1 -Run'
