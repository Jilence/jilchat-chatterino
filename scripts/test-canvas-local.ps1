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
$taskQt = "$taskRoot/build-dev/qt-sdk/6.12.0/msvc2022_64"
$env:PATH = "$taskQt/bin;$env:PATH"
& 'C:/Program Files/LLVM/bin/clang-cl.exe' /nologo /std:c++20 /Zc:__cplusplus /MD /EHsc /DQT_NO_DEBUG /I src "/I$taskQt/include" "/I$taskQt/include/QtCore" "/I$taskQt/include/QtGui" "/I$taskQt/include/QtWidgets" "/I$taskQt/include/QtCanvasPainter" src/widgets/helper/CanvasChatSurface.cpp prototypes/canvas-chat-smoke/main.cpp /Fe:build-dev/canvas-smoke/canvas-smoke.exe /Fo:build-dev/canvas-smoke/ /link "$taskQt/lib/Qt6CanvasPainter.lib" "$taskQt/lib/Qt6Widgets.lib" "$taskQt/lib/Qt6Gui.lib" "$taskQt/lib/Qt6Core.lib"
exit $LASTEXITCODE
