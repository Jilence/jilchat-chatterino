param(
    [Parameter(Mandatory)][string]$QtRoot,
    [string]$BuildRoot = 'build/performance',
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
Set-Location $taskRoot
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$BuildRoot = [IO.Path]::GetFullPath((Join-Path $taskRoot $BuildRoot))
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
# Keep llvm-rc unwrapped, just as in the application build step.
$env:Path = ($env:Path -split ';' | Where-Object {
    $_ -and -not (Test-Path (Join-Path $_ 'ccache.exe'))
}) -join ';'
Remove-Item Env:CMAKE_RC_COMPILER_LAUNCHER -ErrorAction SilentlyContinue
Remove-Item Env:CMAKE_CXX_COMPILER_LAUNCHER -ErrorAction SilentlyContinue
Remove-Item Env:CMAKE_C_COMPILER_LAUNCHER -ErrorAction SilentlyContinue

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Get-PinnedSource([string]$Name, [string]$Url, [string]$Revision) {
    $destination = Join-Path $BuildRoot $Name
    if (-not (Test-Path "$destination/.git")) {
        Invoke-Checked git @('init', $destination)
        Invoke-Checked git @('-C', $destination, 'remote', 'add', 'origin', $Url)
        Invoke-Checked git @('-C', $destination, 'fetch', '--depth', '1', 'origin', $Revision)
        Invoke-Checked git @('-C', $destination, 'checkout', '--detach', 'FETCH_HEAD')
    }
    $actual = & git -C $destination rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $actual -ne $Revision) { throw "Unexpected $Name revision: $actual" }
    return $destination
}

# Match the locally tested SDK and patch contexts exactly. Do not silently apply
# these renderer fixes to another Qt version.
$taskQtConfig = Join-Path $QtRoot 'lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake'
if ((Get-Content $taskQtConfig -Raw) -notmatch 'set\(PACKAGE_VERSION "6\.11\.2"\)') {
    throw 'The patched Canvas runtime requires Qt 6.11.2.'
}
$taskCanvas = Get-PinnedSource qtcanvaspainter https://github.com/qt/qtcanvaspainter.git ec6199e71187c76debcc0a68aa37e3e54f19c25e
$taskMimalloc = Get-PinnedSource mimalloc https://github.com/microsoft/mimalloc.git dfa50c37d951128b1e77167dd9291081aa88eea4
foreach ($name in @('qtcanvaspainter-texture-lifetime.patch', 'qtcanvaspainter-widgetonly.patch')) {
    $patch = Join-Path $taskRoot ".patches/$name"
    & git -C $taskCanvas apply --reverse --check $patch 2>$null
    if ($LASTEXITCODE -ne 0) {
        Invoke-Checked git @('-C', $taskCanvas, 'apply', '--check', $patch)
        Invoke-Checked git @('-C', $taskCanvas, 'apply', $patch)
    }
}
$taskCanvasBuild = Join-Path $BuildRoot 'canvas-build'
Invoke-Checked cmake @('-S', $taskCanvas, '-B', $taskCanvasBuild, '-G', 'Ninja',
    '-DCMAKE_CXX_COMPILER=clang-cl', '-DCMAKE_C_COMPILER=clang-cl',
    '-DCMAKE_RC_COMPILER=llvm-rc', '-DCMAKE_LINKER_TYPE=LLD',
    '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$QtRoot", "-DQt6_DIR=$QtRoot/lib/cmake/Qt6",
    '-DCMAKE_DISABLE_FIND_PACKAGE_Qt6Quick=TRUE', '-DQT_BUILD_EXAMPLES=OFF',
    '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_DOCS=OFF', '-DQT_BUILD_BENCHMARKS=OFF')
Invoke-Checked cmake @('--build', $taskCanvasBuild, '--target', 'qcshadergen', '-j', "$Jobs")
# Use Qt's matching official host tool, as in the local runtime build.
$taskHostTool = Join-Path $taskCanvasBuild 'bin/qcshadergen.exe'
Copy-Item "$QtRoot/bin/qcshadergen.exe" $taskHostTool -Force
(Get-Item $taskHostTool).LastWriteTimeUtc = [DateTime]::UtcNow
Invoke-Checked cmake @('--build', $taskCanvasBuild, '--target', 'CanvasPainter', '-j', "$Jobs")

if ($env:GITHUB_ENV) {
    "C2_MIMALLOC_SOURCE=$taskMimalloc" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
    "C2_CANVAS_RUNTIME=$taskCanvasBuild/bin/Qt6CanvasPainter.dll" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
}
