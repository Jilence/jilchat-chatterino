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
'int main(int argc, char**) { volatile int value = argc; return value == 0; }' | Set-Content build-dev/pgo-smoke.cpp
& 'C:/Program Files/LLVM/bin/clang-cl.exe' /nologo /MD /O2 /clang:-fprofile-instr-generate build-dev/pgo-smoke.cpp /Fe:build-dev/pgo-smoke.exe /Fo:build-dev/pgo-smoke.obj /link '/LIBPATH:C:/Program Files/LLVM/lib/clang/23/lib/windows'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$env:LLVM_PROFILE_FILE = "$taskRoot/build-dev/pgo-smoke.profraw"
& ./build-dev/pgo-smoke.exe
& 'C:/Program Files/LLVM/bin/llvm-profdata.exe' merge -o build-dev/pgo-smoke.profdata build-dev/pgo-smoke.profraw
& 'C:/Program Files/LLVM/bin/clang-cl.exe' /nologo /MD /O2 "/clang:-fprofile-instr-use=$taskRoot/build-dev/pgo-smoke.profdata" build-dev/pgo-smoke.cpp /Fe:build-dev/pgo-smoke-use.exe /Fo:build-dev/pgo-smoke-use.obj
exit $LASTEXITCODE
