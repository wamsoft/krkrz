<!-- source_hash: a25a290e4fb2 -->
# krkrz — Kirikiri Z engine (multi-platform)

[日本語](README.md) | English

This is the engine of a derivative of Kirikiri Z (吉里吉里Z) — a TJS2 scripting engine and novel-game
runtime — extended from Windows-only to multiple platforms.

- From the same sources it builds either a variant that uses **SDL3** for the basic system layer,
  or the traditional **native Win32** variant
- Rendering has an **OpenGL ES** based mechanism (Canvas / Texture / Shader / Offscreen); the
  native Windows build uses a **Direct3D 11** path by default (D3D9 has been removed)
- External libraries are referenced as-is wherever possible, and dependencies are resolved with
  **vcpkg** (SDL3 is fetched with `FetchContent` to track the latest version)

Usage, the class reference and guides are on the [documentation site](https://wamsoft.github.io/krkrz_dev/en/).

## Where this repository fits

This repository contains **only the engine**. Plugins, TJS2 script libraries (KAG3 etc.) and the
integrated build are bundled as submodules by the umbrella repository **`krkrz_dev`**. Use that
repository if you want to build everything including plugins.

Builds for the browser (Emscripten/wasm), Android, iOS and Linux distribution packages live in the
wrapper repositories that reference this repository as the engine source
([krkrz_web](https://github.com/wamsoft/krkrz_web) /
[krkrz_android](https://github.com/wamsoft/krkrz_android) /
[krkrz_ios](https://github.com/wamsoft/krkrz_ios) /
[krkrz_linux](https://github.com/wamsoft/krkrz_linux)).

## Build variants

| `KRKRZ_VARIANT` | Description |
|---|---|
| `SDL` (default) | SDL3-based generic build. Windows / Linux / macOS / Android / wasm |
| `WIN` | The traditional native Windows build (Win32 API + D3D11). Called WINVER |
| `LIB` | Static library version of the generic build (`libkrkrz`) |

`SDL` / `LIB` produce the **GENERIC version** without Windows-specific features, and the
`__GENERIC__` macro is defined.

> When building a plugin for the GENERIC version, define `__GENERIC__` before including
> `tp_stub.h`. If you use `tp_stub/krkrz.cmake`, it is added automatically when `KRKRZ_VARIANT`
> is defined. Without it, `tp_stub.h` behaves in compatibility mode for the old Windows version,
> with `__WINVER__` defined.

---

## Preparing the development environment

### Windows

Install Visual Studio 2022 so that the C++ compiler is available. The bundled CMake / Ninja are
used.

> Builds are verified with **Visual Studio 2022 v17.14 and its bundled CMake / Ninja**. Other
> versions, or a separately installed CMake etc., are not tested.

If you want to use `make`, install msys2 and the basic development tools.

```bash
pacman -S base-devel
```

> **Start the build from a Visual Studio Developer Command Prompt.** The 32-bit build
> (`x86-windows`) needs the x86 Developer Command Prompt (vcpkg misbehaves when the architectures
> do not match).

### Linux

The reference environment for build verification is the Docker build environment set up by
`deckbuild/` in the [steamdev](https://github.com/wamsoft/steamdev) repository (Valve's official
Steam Linux Runtime 3.0 "sniper" SDK = based on Debian 11 / glibc 2.31). For the compatibility
approach, usage and the acceptance criteria for binaries, see
[doc/LinuxBuild.md](doc/LinuxBuild.md) (in Japanese).

A direct build on any distribution (the `x64-linux` preset) is also possible, but binaries built
with a newer glibc do not run on older systems, so distributions are verified in the reference
environment above.

### macOS

Work in progress.

### vcpkg

Install vcpkg in each environment and set its folder in the environment variable `VCPKG_ROOT`.

Note: Visual Studio 2022 and later bundle vcpkg. It conflicts with one you install yourself, so
settle on one of them.

https://learn.microsoft.com/en-us/vcpkg/get_started/overview

```bash
# dos
set VCPKG_ROOT="c:\work\vcpkg"

# msys/cygwin
export VCPKG_ROOT='c:\work\vcpkg'
```

---

## Building

### Getting the sources

After cloning, update the submodules.

```bash
git submodule update --init
```

### Presets

Build with the presets in `CMakePresets.json`. The required libraries are provided through
`vcpkg.json`. The build folder is `build/<preset name>` by default, and the generator is Ninja
Multi-Config.

| Preset | Variant | Notes |
|---|---|---|
| `x64-windows` / `x86-windows` / `arm64-windows` | SDL | **SDL3 build** (the ones without `-win` in the name) |
| `x64-windows-win` / `x86-windows-win` / `arm64-windows-win` | WIN | **Native Windows (WINVER)** |
| `x64-linux` / `arm64-linux` | SDL | Linux |
| `x64-osx` / `arm64-osx` | SDL | macOS |
| `x64-android` / `arm64-android` | SDL | Android |

> Beware of the confusing names: `x64-windows` is the **SDL build**; for WINVER use
> `x64-windows-win`.

```bash
cmake --preset x64-windows
cmake --build build/x64-windows --config Release
```

### Via the Makefile

There is a Makefile that wraps the same steps.

```bash
# Target preset (detected from the OS when not set)
export PRESET=x64-windows
# Build type (Release when not set)
export BUILD_TYPE=Release

# cmake options
export CMAKEOPT="-DKRKRZ_USE_SJIS=ON"

make prebuild            # cmake configure (vcpkg runs here)
make build               # build
make run                 # run with data/ as the argument
make test                # parity tests (image SIMD / sound SIMD)
INSTALL_PREFIX=install make install
```

### Main build options

Specify them with `-D<name>=<value>` or through `CMAKEOPT`. For all definitions, see
[PreprocessorDefinitions.md](PreprocessorDefinitions.md) (in Japanese).

| Option | Default | Description |
|---|---|---|
| `KRKRZ_VARIANT` | `SDL` | Build variant (table above) |
| `KRKRZ_USE_SJIS` | OFF | Make SJIS (MBCS) the default for reading text |
| `KRKRZ_USE_OPENGL` | ON | OpenGL ES rendering mechanism (Canvas/Texture/Shader etc.) |
| `KRKRZ_CANVAS_GPU` | OFF | SDL3: run the Canvas classes on SDL_GPU (combine with `KRKRZ_USE_OPENGL=OFF`. [CanvasGPU.md](doc/CanvasGPU.md)) |
| `KRKRZ_USE_ELEMENTS` | ON | Elements dialog UI (SDL/WIN). OFF removes the `ElementsDialog` class entirely |
| `KRKRZ_USE_GLYPHWARE` | ON | Unified font engine glyphware |
| `KRKRZ_USE_MOVIE` | ON | Movie playback (external/movie-player) |
| `KRKRZ_REPL` | ON on desktop | Interactive TJS REPL (`-repl` / `-replfile`) |
| `KRKRZ_REPL_WEB` | Follows `KRKRZ_REPL` | Browser REPL / WebServer class (`-replweb`) |
| `KRKRZ_USERCONF_WEB` | ON on desktop | Open the engine configuration screen (`-userconf`) in a browser (appserve. Included even with MASTER. [UserConfig.md](doc/UserConfig.md)) |
| `KRKRZ_ENABLE_DAP` | ON | VSCode debug adapter (DAP) server |
| `KRKRZ_BUILD_TESTS` | ON | Build the parity tests (image / sound SIMD) |
| `KRKRZ_DRAW_STATS` | OFF | Measure DrawThreadPool utilization ([DrawStats.md](doc/DrawStats.md)) |
| `KRKRZ_RESOURCE_DIR` | `resource/` | Embedded resource folder (can be replaced per title) |
| `KRKRZ_WIN_ICON` | The default Kirikiri icon | Windows (SDL version): icon embedded in the exe (absolute path to an `.ico`) |
| `KRKRZ_VERSION_BUILD` | `0` | Fourth digit of the version ([Versioning.md](doc/Versioning.md)) |
| `MASTER` | OFF | Fix the log level at WARNING (no INFO output) |

Without `MASTER`, the log level at startup is INFO for Release and DEBUG for Debug, and it can be
changed with the command-line option `-loglevel=ERROR,WARNING,INFO,DEBUG,VERBOSE`. For details,
see [Logging.md](doc/Logging.md).

When you pass the absolute path of an `.ico` to `KRKRZ_WIN_ICON`, `MAINICON ICON <path>` is added
to the generated `.rc`, and the exe has the icon right after the build (when not specified, the
same default Kirikiri icon as the WINVER version, `win32/vcproj/tvpwin32.ico`, is used). The
resource is named `MAINICON` because the tools that replace the icon afterwards (IconReset /
`iconreset.tjs`) assume that name. A dependency is also set up so that the `.res` is rebuilt when
the contents of the `.ico` change.

---

## Running

```bash
make run                 # start with data/ as the argument
DATAPATH=path/to/game make run
```

`data/` contains the **core demos** for checking engine features. `data/gallery` is a gallery that
switches between a menu and 21 demos in a single process; specifying `data/` itself starts a
launcher that also includes the cross-plugin demos.

With `-demotest`, each demo runs headless for a few dozen frames, prints `@demotest:` lines and
exits, so it can be used for CI-style startup checks.

```bash
krkrz64.exe <repo>/data/gallery -demotest
```

### Draw device at startup

The rendering path is chosen with the command-line option `-drawdevice=`.

| Build | Values | Default |
|---|---|---|
| SDL3 | `sdl` (SDL_Renderer) / `sdlogl` (OpenGL ES directly) / `ogl` (OpenGL ES + full features such as Canvas) / `gpu` (SDL_GPU + Canvas, `KRKRZ_CANVAS_GPU` build) | `sdlogl` (`sdl` in builds with OpenGL disabled, `gpu` in `KRKRZ_CANVAS_GPU` builds) |
| WINVER | `basic` (Direct3D 11) / `ogl` / `null` (no rendering, for testing) | `basic` |

You can also switch at run time by assigning to `Window.drawDevice`, but **an SDL3 build started
with `-drawdevice=sdl` cannot switch to an OpenGL device at run time** (OpenGL initialization does
not succeed). To use OpenGL features, start with `sdlogl` / `ogl` from the beginning.

The Canvas in `KRKRZ_CANVAS_GPU` builds (the GPU version) does not use OpenGL; it uses shaders
converted in advance. For the build, shader preparation and command-line options (`-gpudriver` /
`-gpushaderpack` / `-gpushadercollect`), see [CanvasGPU.md](doc/CanvasGPU.md).

### OpenGL ES runtime environment

The engine's OpenGL features (Canvas / Texture / Shader, `-drawdevice=ogl` / `sdlogl`) run on
OpenGL ES. The required runtime differs by build.

**The WINVER build** always creates the GLES context through EGL (ANGLE), so when using OpenGL
features the ANGLE DLLs are needed next to the executable (`make run` copies them automatically):

```
plugin/     libEGL.dll / libGLESv2.dll        (32bit)
plugin64/   libEGL.dll / libGLESv2.dll        (64bit)
```

**The SDL3 build** declares at SDL initialization that it expects a GLES context, so to use OpenGL
the environment must support GLES from the start. On Windows the GLES context is first created
from the OS OpenGL driver (the WGL ES profile = `WGL_EXT_create_context_es2_profile`), and falls
back to ANGLE (the same `libEGL.dll` / `libGLESv2.dll` as above) when that is not available. If the
driver's ES profile implementation has problems (garbled rendering, initialization failures, etc.),
the command-line option `-forceegl=yes` makes it use EGL (ANGLE) from the start, to isolate or
work around the problem. Linux / Android etc. use the OS-native EGL / GLES.

The requested context version is ES 3.2; if it cannot be created, it retries with 3.1 → 3.0 → 2.0
(ANGLE's D3D11 backend only goes up to ES 3.0/3.1, so on the EGL fallback this retry is what makes
it succeed). The version actually obtained can be checked in the startup log line
`Loaded GLES x.y`.

#### Getting the ANGLE DLLs

The ANGLE DLLs are not included in the repository. They used to be included as standalone DLLs in
Chromium installations, but are no longer distributed that way, so using a prebuilt is the easy
route:

- [mmozeiko/build-angle](https://github.com/mmozeiko/build-angle) — builds upstream ANGLE daily
  and puts it in Releases (x64 / arm64. The zip contains `libEGL.dll` / `libGLESv2.dll` /
  `d3dcompiler_47.dll`). Verified: the 2026-07-25 build (upstream
  [cc226c81](https://github.com/google/angle/commit/cc226c81fda884e3b85aad1e00e581c12f8aee04)).
- No prebuilt is provided for 32-bit (x86), so if you need it you have to build it yourself
  (depot_tools + gn).

Place them next to the executable. In the umbrella repository, putting them in `plugin/` (32bit) /
`plugin64/` (64bit) at the root makes `make run` copy them into the build output.

> **Note**: even without the DLLs next to the exe, it may pick up an ANGLE bundled with another
> application on the PATH (e.g. the Oculus runtime's `libEGL.dll`) and run with it. That leads to
> confusing symptoms such as initialization failures due to an old version, so always ship the DLLs
> with distributions that use OpenGL features (the ANGLE license notice is already included in the
> engine's license collection mechanism).

### Display language

Engine messages (error texts etc.) and the option descriptions (the `-userconf` settings UI) are
multilingual (ja / en / chs / cht). By default they follow the OS language setting
(`System.systemLanguage`), and can be set explicitly with the command-line option
`-language=<tag>` (BCP-47 or the short forms ja / en / chs / cht). WINVER switches through PE
resource language resolution (the thread UI language), and SDL3 through the choice of
`resource/messages*.json` / `optiondesc*.json` (for message strings, only a direct command-line
setting takes effect).

---

## Tests

`make test` runs two CTest parity tests (image SIMD / sound SIMD).

### Image SIMD parity test

`tests/simd_parity_test.cpp` is a CTest test that compares the output of the image-processing SIMD
code (SSE2 / AVX2 / NEON) with the C reference implementation byte by byte
(`krkrz_simd_parity_test` / test name `simd_parity`).

It is a standalone target that links only the SIMD core directly (`tvpgl.c` /
`blend_function.cpp` / each `*_sse2.cpp` / `*_avx2.cpp` / `*_neon.cpp` / `detect_cpu.cpp` etc.),
so it has no runtime dependency on SDL3 / OpenGL / vcpkg. It is enabled when
`KRKRZ_BUILD_TESTS=ON` (the default) and the target is x86 or ARM.

```bash
make test          # both image + sound (-R parity)
# or individually
ctest --test-dir build/<preset> -C Release -R simd_parity --output-on-failure
```

Expected output:

- x86: the two sections `[SSE2 vs C reference]` and `[AVX2 vs C reference]` pass all items
- ARM / ARM64: the `[NEON vs C reference]` section

Only the PsBlend family uses a tolerance of `tol_alpha=-1, tol_rgb=2` (`tol_rgb=8` for ColorDodge5
only), because the SSE2 side quantizes to 7 bits. Everything else is compared byte-exact. **A new
mismatch is either a new bug or an intentional change of the reference** — not "known noise".

### Sound SIMD parity test

`tests/sound_parity_test.cpp` compares the SSE implementations in `common/sound/` (Ooura Real DFT /
window-function deinterleave and interleave, etc.) with the C reference
(`krkrz_sound_parity_test` / test name `sound_parity`). Because rounding differences in float
arithmetic are unavoidable, it judges with a combined relative + absolute error tolerance instead
of byte-exact comparison. It also does not depend on SDL3 / rendering / audio devices.

### DAP smoke test

```bash
python tests/dap_smoke.py build/x64-windows/Release/krkrz64.exe data
```

It starts the engine with `-dap=<port>` and verifies the round trips of initialize / attach /
evaluate / scopes / variables / the step commands / disconnect without the VSCode extension. It is
OK when the last line shows `[smoke] PASS: all phases verified`.

---

## Debugging

### Visual Studio (C++)

- Drop the executable onto a Visual Studio window with no project open
- In the debug properties, set the working folder to the project folder (it is where the plugin
  folder is looked up)
- Give the data folder as the argument with a full path (only exe-relative or absolute paths are
  supported)

### VSCode (native C++)

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "WINデバッグ起動",
            "type": "cppvsdbg",
            "request": "launch",
            "program": "build/x86-windows/Debug/krkrz.exe",
            "args": ["${workspaceFolder}/data"],
            "stopAtEntry": false,
            "console": "externalTerminal",
            "cwd": "${workspaceFolder}",
            "environment": []
        }
    ]
}
```

### VSCode + DAP (TJS script debugging)

The engine has a built-in DAP server, so with the
[krkrz-vscode](https://github.com/wamsoft/krkrz-vscode) extension you can debug TJS2 like any other
language (breakpoints / stepping / call stack / variable inspection / conditional breakpoints /
log points / Watch).

```bash
krkrz64.exe -dap=6635 ${workspaceFolder}/data
```

With `KRKRZ_ENABLE_DAP=OFF`, all DAP-related code is excluded by `#ifdef`. Syntax highlighting for
TJS2 / KAG (.ks) is also bundled with the same extension (breakpoints on KAG lines are not possible
by design; they can be set in TJS inside `[iscript]...[endscript]`).

### REPL / agent-driven operation

The engine has a built-in interactive TJS shell. For details, see [REPL.md](doc/REPL.md).

```bash
krkrz64.exe data -repl              # console REPL
krkrz64.exe data -replfile=<dir>    # file channel (for external tools / agents)
krkrz64.exe data -replweb=8899      # browser REPL (HTTP + SSE)
```

Combined with the `Agent` class (input injection / screen capture / dialog operation), it allows
automated verification while looking at the actual screen.

---

## Version number

`PROJECT_VERSION` in `CMakeLists.txt` is the single source, from which `krkrz_version.h` (C++ side)
and `krkrz_version.rc` (Windows VERSIONINFO) are generated. To bump the number, rewrite only that
one line.

For the criteria for bumping, how the product name is assembled and how to check it, see
[Versioning.md](doc/Versioning.md).

---

## Generated files

Some files are committed but are **generated**. Do not edit them directly; edit the source and
regenerate. Generation needs **python** and, in some cases, **perl**.

### bison path (perl + bison)

Generated by `common/tjs2/syntax/compile.bat`:

| Output | Source |
|---|---|
| `tjs.tab.cpp` / `tjs.tab.hpp` | `tjs.y` |
| `tjsdate.tab.cpp` / `tjsdate.tab.hpp` | `tjsdate.y` |
| `tjspp.tab.cpp` / `tjspp.tab.hpp` | `tjspp.y` |
| `tjsDateWordMap.cc` | `gen_wordtable.bat` |

bison needs `libiconv2.dll` / `libintl3.dll` / `regex2.dll`.

- http://gnuwin32.sourceforge.net/packages/bison.htm
- http://gnuwin32.sourceforge.net/packages/libintl.htm
- http://gnuwin32.sourceforge.net/packages/libiconv.htm
- http://gnuwin32.sourceforge.net/packages/regex.htm

### tvpgl (perl)

`common/visual/glgen/gengl.bat` generates `tvpgl.c` / `tvpgl.h` from `maketab.c` / `tvpps.c`.

### tp_stub (python only)

Generated by `common/base/makestub.bat` (which runs `python gen_tpstub.py`). It works with only the
Python standard library (zlib / hashlib); the old Perl version (`makestub.pl` + Compress::Zlib +
Digest::MD5) is not needed. The output is byte-identical to the old version.

| Output | Source |
|---|---|
| `FuncStubs.cpp` / `FuncStubs.h` | The `TJS_EXP_FUNC_DEF` / `TVP_GL_FUNC_PTR_EXTERN_DECL` / `TJS_*_METHOD_DEF*` macros in each header |
| `tp_stub.cpp` / `tp_stub.h` | Same as above |

After generation, copy `tp_stub.{h,cpp}` to the tp_stub submodule on the plugins side, as before.

### Message definitions (python only)

Generated by `common/msg/text/gen_messages.py`. The source is a CSV
(`common/msg/text/messages.csv`); edit it and run `python gen_messages.py`. For details, see
`common/msg/text/README.md`.

Outputs: `tjsErrorInc.h` / `MsgIntfInc.h` / `MsgImpl.h` /
`resource/messages{,-en,-chs,-cht}.json` / `win32/vcproj/string_table_*.rc` +
`string_table_resource.h` / `MsgLoad.cpp` for generic and win32

The language columns of the CSV are ja / en / chs / cht. **Rows without cht (Traditional Chinese)
fall back to chs.** The WINVER Win32 STRINGTABLE has only jp / en / chs, and Traditional Chinese
engine messages fall back through PE resource language resolution (SDL3 reads
`messages-cht.json` directly, so Traditional Chinese is shown).

### Version (CMake)

`${CMAKE_BINARY_DIR}/krkrz_version.{h,rc}` are generated from `cmake/krkrz_version.h.in` /
`cmake/krkrz_version.rc.in` (see above).

---

## Documentation index (`doc/`)

The documents under `doc/` are in Japanese.

### Rendering

| Document | Description |
|---|---|
| [ScreenTransfer.md](doc/ScreenTransfer.md) | The cost of transferring composited frames to the GPU. Measurement (`System.renderStats`) and how to read the numbers |
| [D3D11Migration.md](doc/D3D11Migration.md) | WINVER's migration from D3D9 to D3D11, and later additions (vblank / partial transfer) |
| [GLCompositor.md](doc/GLCompositor.md) | Mechanism that writes offscreen GLES compositing back to a layer |
| [CanvasEffect.md](doc/CanvasEffect.md) / [CanvasTransition.md](doc/CanvasTransition.md) | Canvas post effects and transitions |
| [Viewport.md](doc/Viewport.md) | Controlling the displayed area of the game screen |
| [DrawStats.md](doc/DrawStats.md) | Measuring DrawThreadPool utilization |
| [OpaqueExcludeNotPropagating.md](doc/OpaqueExcludeNotPropagating.md) | The issue of opaque-region exclusion not propagating |

### UI / input

| Document | Description |
|---|---|
| [ElementsDialog.md](doc/ElementsDialog.md) | The Elements dialog mechanism (JSON layout / input routing / overlay rendering) |
| [HotKey.md](doc/HotKey.md) | Top-level hotkeys (`System.registerHotKey`) — keys that take effect before any dispatch |
| [ModalWindow.md](doc/ModalWindow.md) | `Window.showModal` and the pitfalls of multiple windows |
| [Gamepad.md](doc/Gamepad.md) | Gamepad input (logical index / axes / vibration) |
| [PadOverlay.md](doc/PadOverlay.md) | Pad state overlay |
| [FontEngine.md](doc/FontEngine.md) | Unified font engine glyphware |

### Sound / movie

| Document | Description |
|---|---|
| [MovieMFMigration.md](doc/MovieMFMigration.md) | Movie playback (DirectShow removal / Media Foundation / presenter) |
| [Sound3D.md](doc/Sound3D.md) | 3D sound |

### Memory / diagnostics

| Document | Description |
|---|---|
| [MemoryDesign.md](doc/MemoryDesign.md) | Allocator design (pools / tags / hooks for sound) |
| [MemoryGuide.md](doc/MemoryGuide.md) | Guide to observing and investigating memory |
| [LeakAudit.md](doc/LeakAudit.md) | Leak audit |
| [TtstrDataRetention.md](doc/TtstrDataRetention.md) | Notes on how `ttstr` retains data |
| [Logging.md](doc/Logging.md) | Logging mechanism and API |

### Running / operation

| Document | Description |
|---|---|
| [REPL.md](doc/REPL.md) | Interactive TJS shell / file channel / browser REPL / agent-driven operation |
| [CommandLinePresets.md](doc/CommandLinePresets.md) | Command-line presets |
| [PlatformPorting.md](doc/PlatformPorting.md) | Hooks for embedding the engine from wrapper builds (platform ports) (CMake variables / detection macros / platform-specific Application) |
| [UserStorage.md](doc/UserStorage.md) | Initialization of the `user://` storage media and the platform replacement hook |
| [LocalFileOps.md](doc/LocalFileOps.md) | Local file operations and environment variables in Storages (moving the old fstat / systemEx into the engine, division of work with the plugins) |
| [WindowState.md](doc/WindowState.md) | Window maximize / rectangle / extended events and monitor information (moving the old windowEx into the engine) |
| [AppEvent.md](doc/AppEvent.md) | Sending application events |
| [LicenseSystem.md](doc/LicenseSystem.md) | License notice collection mechanism |
| [Versioning.md](doc/Versioning.md) | The source of the version number and how to bump it |
| [ModernizationRoadmap.md](doc/ModernizationRoadmap.md) | Plan and progress of the WINVER modernization |

---

## License

The license of the Kirikiri Z engine is in [LICENSE](LICENSE) (BSD-3-Clause-equivalent terms,
written in Japanese, including a clause that forbids using the names to endorse or promote
products). The same file also contains the licenses of the third-party code included in the source
tree (MT19937 / glad / PL_MPEG).

The license texts of the separately obtained external libraries that are linked (SDL3 / FreeType /
HarfBuzz / zlib / libpng / ANGLE and others) and of bundled data assets such as fonts are kept as
individual files in [licenses/](licenses/) (the list is
[licenses/manifest.json](licenses/manifest.json); the texts are in `licenses/texts/` and in each
library's source tree). They are built into the engine and can be read at run time with
`System.getLicenseList` / `System.getLicenseText`. For the mechanism, see
[LicenseSystem.md](doc/LicenseSystem.md).
