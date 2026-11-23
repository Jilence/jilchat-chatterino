$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskCmake = 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$taskMsvc = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.40.33807'
$taskSdk = 'C:/Program Files (x86)/Windows Kits/10'
$taskSdkVersion = '10.0.22621.0'
$env:PATH = "C:/Program Files/LLVM/bin;$taskMsvc/bin/Hostx64/x64;$taskSdk/bin/$taskSdkVersion/x64;$env:PATH"
$env:INCLUDE = "$taskMsvc/include;$taskSdk/Include/$taskSdkVersion/ucrt;$taskSdk/Include/$taskSdkVersion/shared;$taskSdk/Include/$taskSdkVersion/um;$taskSdk/Include/$taskSdkVersion/winrt"
$env:LIB = "$taskMsvc/lib/x64;$taskSdk/Lib/$taskSdkVersion/ucrt/x64;$taskSdk/Lib/$taskSdkVersion/um/x64"

Set-Location $taskRoot
$taskSource = "$taskRoot/build-dev/qtcanvaspainter-src"
if (-not (Test-Path "$taskSource/CMakeLists.txt")) {
    & git clone --depth 1 --branch v6.11.2 https://github.com/qt/qtcanvaspainter.git $taskSource
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
foreach ($taskPatchName in @('qtcanvaspainter-texture-lifetime.patch', 'qtcanvaspainter-widgetonly.patch')) {
    $taskPatch = "$taskRoot/.patches/$taskPatchName"
    & git -C $taskSource apply --reverse --check $taskPatch 2>$null
    if ($LASTEXITCODE -ne 0) {
        & git -C $taskSource apply --check $taskPatch
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & git -C $taskSource apply $taskPatch
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}
$taskQt = "$taskRoot/build-dev/qt-sdk/6.11.2/msvc2022_64"
& $taskCmake -S "$taskRoot/build-dev/qtcanvaspainter-src" -B "$taskRoot/build-dev/qtcanvaspainter-build" -G Ninja '-DCMAKE_CXX_COMPILER=C:/Program Files/LLVM/bin/clang-cl.exe' '-DCMAKE_C_COMPILER=C:/Program Files/LLVM/bin/clang-cl.exe' '-DCMAKE_LINKER=C:/Program Files/LLVM/bin/lld-link.exe' '-DCMAKE_RC_COMPILER=C:/Program Files/LLVM/bin/llvm-rc.exe' '-DCMAKE_MAKE_PROGRAM=C:/Users/powww/AppData/Local/Python/pythoncore-3.14-64/Scripts/ninja.exe' -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$taskQt" "-DQt6_DIR=$taskQt/lib/cmake/Qt6" -DCMAKE_DISABLE_FIND_PACKAGE_Qt6Quick=TRUE -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DQT_BUILD_DOCS=OFF -DQT_BUILD_BENCHMARKS=OFF
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# Use the official matching host tool: the local clang-cl/lld tool executable
# receives an invalid UAC manifest from Qt's tool-build configuration.
& $taskCmake --build "$taskRoot/build-dev/qtcanvaspainter-build" --target qcshadergen -j 12
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$taskHostTool = "$taskRoot/build-dev/qtcanvaspainter-build/bin/qcshadergen.exe"
if ((Get-FileHash "$taskQt/bin/qcshadergen.exe").Hash -ne (Get-FileHash $taskHostTool).Hash) {
    Copy-Item -LiteralPath "$taskQt/bin/qcshadergen.exe" -Destination $taskHostTool -Force
    (Get-Item $taskHostTool).LastWriteTimeUtc = [DateTime]::UtcNow
}
& $taskCmake --build "$taskRoot/build-dev/qtcanvaspainter-build" --target CanvasPainter -j 12
exit $LASTEXITCODE


