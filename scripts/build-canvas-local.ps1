param([switch]$ConfigureOnly, [switch]$WithoutLto, [int]$Jobs = 12)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskCmake = 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$taskMsvc = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.40.33807'
$taskSdk = 'C:/Program Files (x86)/Windows Kits/10'
$taskSdkVersion = '10.0.22621.0'
$env:PATH = "C:/Program Files/LLVM/bin;$taskMsvc/bin/Hostx64/x64;$taskSdk/bin/$taskSdkVersion/x64;$env:PATH"
$env:INCLUDE = "$taskMsvc/include;$taskSdk/Include/$taskSdkVersion/ucrt;$taskSdk/Include/$taskSdkVersion/shared;$taskSdk/Include/$taskSdkVersion/um;$taskSdk/Include/$taskSdkVersion/winrt"
$env:LIB = "$taskMsvc/lib/x64;$taskSdk/Lib/$taskSdkVersion/ucrt/x64;$taskSdk/Lib/$taskSdkVersion/um/x64"


$taskQt = "$taskRoot/build-dev/qt-sdk/6.11.2/msvc2022_64"
$taskBuild = "$taskRoot/build-canvas"
if (-not (Test-Path "$taskQt/lib/cmake/Qt6CanvasPainter/Qt6CanvasPainterConfig.cmake")) {
    throw 'Run scripts/install-qt-canvas-local.py to install the local Qt SDK first.'
}
$taskLto = if ($WithoutLto) { 'OFF' } else { 'ON' }
# Configure explicitly; do not inherit Qt 6.9 paths or overwrite the existing executable.
& $taskCmake -S $taskRoot -B $taskBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=C:/Users/powww/AppData/Local/Python/pythoncore-3.14-64/Scripts/ninja.exe" '-DCMAKE_CXX_COMPILER=C:/Program Files/LLVM/bin/clang-cl.exe' '-DCMAKE_C_COMPILER=C:/Program Files/LLVM/bin/clang-cl.exe' '-DCMAKE_LINKER=C:/Program Files/LLVM/bin/lld-link.exe' '-DCMAKE_RC_COMPILER=C:/Program Files/LLVM/bin/llvm-rc.exe' "-DCMAKE_TOOLCHAIN_FILE=$taskRoot/build-dev/conan_toolchain.cmake" -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_PREFIX_PATH=$taskQt" "-DQt6_DIR=$taskQt/lib/cmake/Qt6" -DCHATTERINO_CANVAS_PAINTER=ON -DCHATTERINO_ALLOW_PRIVATE_QT_API=ON -DBUILD_TESTS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE -DCHATTERINO_UPDATER=OFF -DCHATTERINO_THEME_VIDEO=OFF "-DCHATTERINO_LTO=$taskLto" "-DCHATTERINO_FORCE_LTO=$taskLto" -DCHATTERINO_MIMALLOC=ON "-DCHATTERINO_MIMALLOC_SOURCE=$taskRoot/build-dev/mimalloc-src"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($ConfigureOnly) { exit 0 }
& $taskCmake --build $taskBuild --target chatterino -j $Jobs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Get-ChildItem "$taskRoot/build-dev/bin/*.dll" | Where-Object { $_.Name -notlike 'Qt6*' } | Copy-Item -Destination "$taskBuild/bin"
& "$taskQt/bin/windeployqt.exe" --no-translations --no-compiler-runtime "$taskBuild/bin/JilChatChatterino.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$PSScriptRoot/build-canvas-runtime.ps1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item -LiteralPath "$taskRoot/build-dev/qtcanvaspainter-build/bin/Qt6CanvasPainter.dll" -Destination "$taskBuild/bin/Qt6CanvasPainter.dll" -Force
exit 0

