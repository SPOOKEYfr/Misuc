# Building Misuc on Windows

Misuc runs its audio through WASAPI shared mode on Windows (the PipeWire
backend is used on Linux; both implement the same `AudioBackend` contract so
the engine and UI are unchanged).

Status: compiles and tests through GitHub Actions (`windows` workflow, MSVC
2022 + Qt 6.8 + vcpkg). Device behaviour on real hardware should be verified
like the Linux build was — see honest bounds in
`MISUC_CURRENT_STATUS.txt` (Windows port section).

## Prerequisites

- Visual Studio 2022 with the "Desktop development with C++" workload
  (MSVC 2022 toolset + Windows 10/11 SDK; Windows 10 1703+ at runtime so
  WASAPI stream conversion flags exist).
- Qt 6.4 or newer, MSVC variant (the online installer's "Qt 6.8 MSVC 2022"
  component is what CI uses).
- CMake 3.21+, Ninja or the Visual Studio generator.
- [vcpkg](https://github.com/microsoft/vcpkg) — the repo ships a manifest
  (`vcpkg.json`) that pulls `zlib` and `sndfile` automatically; no manual
  installs needed.

## Configure & build (Visual Studio generator)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
```

The binary lands in `build\bin\Release\misuc_app.exe`.

Ninja works too — point `CMAKE_CXX_COMPILER`/Qt at the MSVC environment
(e.g. from a "x64 Native Tools Command Prompt for VS 2022").

## Tests

```bat
ctest --test-dir build -C Release --output-on-failure
:: or directly:
build\bin\Release\misuc_tests.exe
```

The WASAPI live tests (`wasapi_device_enumeration`,
`wasapi_stream_lifecycle`) skip themselves politely when the machine has no
active audio endpoints — they never hard-fail a release build.

## Deploying (running outside the build tree)

```bat
windeployqt build\bin\Release\misuc_app.exe
```

CI does exactly this and zips the result (`misuc-windows.zip` artifact); the
runner builds the same tree with `cmake --build build --target package`
(CPack ZIP generator) if you prefer the staged install layout.

## Runtime notes (honest bounds)

- WASAPI shared mode always floor-routes through the Windows audio engine:
  requested rates/buffer sizes are converted by WASAPI itself
  (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`, Windows 10 1703+). Effective
  per-callback frames appear as "negotiated buffer" on the Audio page.
- Exclusive/low-latency modes (WDM-KS, ASIO) are NOT claimed by any lead.
- LV2 hosting is currently disabled on Windows (lilv is not wired there);
  sessions still round-trip the fx schema and the UI marks the feature
  unsupported, same as a Linux build without lilv.
- No Windows installer yet — the artifact is a portable ZIP.
