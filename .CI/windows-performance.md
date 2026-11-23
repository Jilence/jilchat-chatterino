# Windows release performance configuration

The Windows x64 and ARM64 jobs in `build.yml` use Qt 6.11.2, the version
against which the Canvas renderer patches were developed. The installer workflow
uses the same artifact version. macOS and Linux retain their existing SDKs.

`.CI/prepare-windows-performance.ps1` checks out pinned Qt Canvas Painter and
mimalloc revisions under the ignored build directory. It applies both tracked
Canvas patches and builds a widgets-only Canvas runtime. A failed patch or build
stops the job rather than packaging the original runtime.

The application enables Canvas support and mimalloc at compile time. Packaging
replaces the SDK Canvas DLL after `windeployqt` and includes mimalloc and the
architecture-specific allocation redirect DLL. Release LTO stays enabled.
PGO stays off: there is no representative, trained application profile yet.

The normal executable uses the lower-memory raster renderer. To opt into Canvas,
launch with `JILCHAT_ENABLE_CANVAS=1` and without `JILCHAT_DISABLE_CANVAS=1`.
The channel bootstrap, shared decoded image pool, unchanged-layout reuse,
hidden-view guards, cache budgets and animation-timer changes also apply to the
normal renderer. Canvas adds retained dirty-region rendering and releases its
resources when hidden. GPU texture handles are renderer-local; sharing them
across independent renderers is deliberately avoided.

Local checks validate script/YAML syntax and patch applicability. The full
Windows x64/ARM64 CI jobs must still run on GitHub after the changes are pushed.
