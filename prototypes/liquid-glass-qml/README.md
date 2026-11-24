# Liquid Glass in Qt Quick (prototype)

A Chatterino split rewritten in Qt Quick/QML, with GPU Liquid Glass. The glass
is a port of `jiltv-frontend/src/lib/glass/liquid-glass.ts` (Snell refraction
on a squircle rim, frost, specular rim, optional dispersion) as one fragment
shader. Chat is live anonymous Twitch IRC with Twitch and 7TV emotes.

## Build & run (Windows)

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.9.3/msvc2022_64
& $cmake --build build --config Release
$env:PATH = "C:\Qt\6.9.3\msvc2022_64\bin;$env:PATH"
.\build\Release\liquidglass.exe            # or: --stress, --picker, --prism, -c xqc,forsen
```

Shaders are checked in as `.qsb`. After editing `shaders/glass.*`, run
`.\compile-shaders.ps1 -Qsb <path to qsb.exe>`.

## Controls

| Key          |                                          |
| ------------ | ---------------------------------------- |
| Ctrl+G       | glass on/off (flat comparison)           |
| Ctrl+F       | frost 0–4                                |
| Ctrl+D       | prism (chromatic dispersion)             |
| Ctrl+S       | stress: 40 synthetic msg/s + moving lens |
| Ctrl+L       | draggable lens                           |
| Ctrl+W       | background on/off                        |
| Ctrl+H       | HUD                                      |
| `/join name` | new tab                                  |

## Benchmark

`liquidglass.exe --stress --bench 8 [--noglass] [--picker --prism] [--screenshot out.png]`
prints frame interval percentiles, render-thread CPU time and GPU time.
