param(
    [ValidateSet('OFF', 'GENERATE', 'USE')][string]$Pgo = 'OFF',
    [string]$Profile = '',
    [switch]$WithoutMimalloc,
    [switch]$WithoutLto,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskCmake = 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$taskMsvc = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.40.33807'
$taskSdk = 'C:/Program Files (x86)/Windows Kits/10'
$taskSdkVersion = '10.0.22621.0'
$env:PATH = "C:/Program Files/LLVM/bin;$taskMsvc/bin/Hostx64/x64;$taskSdk/bin/$taskSdkVersion/x64;$env:PATH"
$env:INCLUDE = "$taskMsvc/include;$taskSdk/Include/$taskSdkVersion/ucrt;$taskSdk/Include/$taskSdkVersion/shared;$taskSdk/Include/$taskSdkVersion/um;$taskSdk/Include/$taskSdkVersion/winrt"
$env:LIB = "$taskMsvc/lib/x64;$taskSdk/Lib/$taskSdkVersion/ucrt/x64;$taskSdk/Lib/$taskSdkVersion/um/x64"
if (-not (Test-Path "$taskRoot/build-dev/mimalloc-src/CMakeLists.txt") -and -not $WithoutMimalloc) {
    & git clone --depth 1 --branch v3.1.5 https://github.com/microsoft/mimalloc.git "$taskRoot/build-dev/mimalloc-src"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
$taskLto = if ($WithoutLto) { 'OFF' } else { 'ON' }
$taskMimalloc = if ($WithoutMimalloc) { 'OFF' } else { 'ON' }
$taskTests = if ($Test) { 'ON' } else { 'OFF' }
& $taskCmake -S $taskRoot -B "$taskRoot/build-dev" "-DCHATTERINO_LTO=$taskLto" "-DCHATTERINO_FORCE_LTO=$taskLto" "-DCHATTERINO_MIMALLOC=$taskMimalloc" "-DCHATTERINO_PGO=$Pgo" "-DCHATTERINO_PGO_PROFILE=$Profile" "-DBUILD_TESTS=$taskTests" -DCHATTERINO_FOCUSED_PERF_TESTS=ON "-DCHATTERINO_MIMALLOC_SOURCE=$taskRoot/build-dev/mimalloc-src" '-DCMAKE_RC_COMPILER=C:/Program Files/LLVM/bin/llvm-rc.exe'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $taskCmake --build "$taskRoot/build-dev" -j 6
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Test) {
    & "$taskRoot/build-dev/bin/chatterino-test.exe" '--gtest_filter=Image.*:LimitedQueue.*'
    exit $LASTEXITCODE
}
exit 0
