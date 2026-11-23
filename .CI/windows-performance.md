# Windows release performance configuration

The Windows x64 and ARM64 jobs in `build.yml` use Qt 6.12.0, like macOS. The
installer workflow uses the same artifact version.

`.CI/prepare-windows-performance.ps1` checks out pinned Qt Canvas Painter
(v6.12.0) and mimalloc revisions under the ignored build directory. It applies
the tracked texture lifetime patch (deferred texture deletion, bindings released
before textures) and builds a widgets-only Canvas runtime. A failed patch or
build stops the job rather than packaging the original runtime. Qt 6.12 already
contains the binding cleanup and the Qt Quick source split that the 6.11 patches
added, so only the deferred deletion remains patched.

`CHATTERINO_CANVAS_PAINTER` is on by default (compiled in, used on request)
and turns itself off when the Qt Canvas Painter module is missing (macOS and
Linux builds). Packaging replaces
the SDK Canvas DLL after `windeployqt` and includes mimalloc and the
architecture-specific allocation redirect DLL. Release LTO stays enabled. PGO
stays off: there is no representative, trained application profile yet.

The chat renders on the CPU by default. Launch with `JILCHAT_ENABLE_CANVAS=1`
(and without `JILCHAT_DISABLE_CANVAS=1`) to render it with Canvas;
`JILCHAT_CANVAS_BACKEND=opengl` uses OpenGL instead of Direct3D 11. Canvas
stays opt-in: a GPU surface makes Qt composite the whole window on the GPU and
upload the rest of the raster UI, which measured at about three times the CPU
(2.3 % vs 0.8 %) and 1.6 times the memory of the CPU renderer. Canvas adds
retained dirty-region rendering and releases its resources when hidden. GPU
texture handles are renderer-local; sharing them across independent renderers
is deliberately avoided.

Animated JilTV paints are drawn as two pictures, the glow and the letters. Every
frame of a paint's loop is rasterized once on the CPU (exactly as before, with
its CSS blend modes) and then kept, along with its texture, so a running loop
costs neither drawing nor uploads.
