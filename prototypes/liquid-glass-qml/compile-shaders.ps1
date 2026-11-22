# Compiles shaders/*.vert|frag to .qsb (the build embeds the .qsb files).
# Needs qsb from Qt Shader Tools, e.g. C:\Qt\6.9.3\msvc2022_64\bin\qsb.exe.
param([string]$Qsb = "qsb")

$dir = Join-Path $PSScriptRoot "shaders"
foreach ($f in Get-ChildItem $dir -Include *.vert, *.frag -Recurse) {
    & $Qsb --glsl "330,300es" --hlsl 50 --msl 12 -o "$($f.FullName).qsb" $f.FullName
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "compiled $($f.Name)"
}
