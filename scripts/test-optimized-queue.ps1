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
& 'C:/Program Files/LLVM/bin/clang-cl.exe' /nologo /std:c++20 /EHsc /MD /I src /I lib/googletest/googletest/include /I lib/googletest/googletest /I lib/googletest/googlemock/include /I C:/Users/powww/AppData/Local/leafyrino-dev/conan/p/boost9ac358a81f73a/p/include tests/src/LimitedQueue.cpp lib/googletest/googletest/src/gtest-all.cc lib/googletest/googletest/src/gtest_main.cc /Fe:build-dev/queue-tests.exe /Fo:build-dev/
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ./build-dev/queue-tests.exe
exit $LASTEXITCODE
