# A2 Build Verification Protocol

- Repository: `ironwail`
- Verified commit SHA: `84af6b31e1fd51018663af4c27ceb635fc64658e`
- Workflow references:
  - `.github/workflows/linux_ci.yml`
  - `.github/workflows/windows_ci.yml`
- Verification date (UTC): 2026-02-27

## Environment

- Host OS: Linux `x86_64` (`6.12.47` kernel)
- Installed Linux CI dependencies (analog to workflow):
  - `libcurl4-openssl-dev`
  - `libmpg123-dev`
  - `libsdl2-dev`
  - `libvorbis-dev`
- Compiler/toolchain versions:
  - GCC: `gcc (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0`
  - Clang: `clang version 17.0.0`
  - CMake: `cmake version 3.28.x` (system cmake)
- Windows CI runner tools (`pwsh`, `vswhere`, `MSBuild`) are **not available** in this Linux environment.

## Commands Executed

### Linux (workflow-like)

```bash
make --jobs=3 --keep-going --directory=Quake CC=gcc
make --jobs=3 --keep-going --directory=Quake CC=gcc DEBUG=1
make --jobs=3 --keep-going --directory=Quake CC=clang
make --jobs=3 --keep-going --directory=Quake CC=clang DEBUG=1
```

### Linux fallback cross-check (CMake)

```bash
cmake -S . -B /tmp/a2logs/cmake-linux-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/a2logs/cmake-linux-release -j3
cmake -S . -B /tmp/a2logs/cmake-linux-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/a2logs/cmake-linux-debug -j3
```

### Windows (matrix analog attempts)

```bash
# for each platform in {x64, Win32}, configuration in {Release, Debug}
pwsh -NoProfile -Command '<workflow-like msbuild bootstrap and build>'
```

## PASS/FAIL Summary

| Build class | Variant(s) | Commit SHA | Config flags | Toolchain/compiler | Result | Relevant failure detail |
|---|---|---|---|---|---|---|
| Linux Debug | gcc + clang attempts | `84af6b31e1fd51018663af4c27ceb635fc64658e` | `DEBUG=1` (`make` workflow analog) / `-DCMAKE_BUILD_TYPE=Debug` | GCC 13.3.0, Clang 17.0.0 | **FAIL** | `../common/lightgrid.h:3:10: fatal error: quakedef.h: No such file or directory` |
| Linux Release | gcc + clang attempts | `84af6b31e1fd51018663af4c27ceb635fc64658e` | default release (`make`) / `-DCMAKE_BUILD_TYPE=Release` | GCC 13.3.0, Clang 17.0.0 | **FAIL** | `../common/lightgrid.h:3:10: fatal error: quakedef.h: No such file or directory` |
| Windows Debug | x64 + Win32 matrix analog | `84af6b31e1fd51018663af4c27ceb635fc64658e` | `-property:Configuration=Debug -property:Platform={x64|Win32} -maxcpucount -verbosity:minimal` | N/A (no Windows runner/MSBuild) | **FAIL** | `bash: command not found: pwsh` |
| Windows Release | x64 + Win32 matrix analog | `84af6b31e1fd51018663af4c27ceb635fc64658e` | `-property:Configuration=Release -property:Platform={x64|Win32} -maxcpucount -verbosity:minimal` | N/A (no Windows runner/MSBuild) | **FAIL** | `bash: command not found: pwsh` |

## Detailed Notes

- Linux failures are reproducible with both the Make-based workflow command and CMake builds, always failing on missing include `quakedef.h` while processing `common/lightgrid.h`.
- Windows workflow-equivalent execution cannot be validated on this host due to missing `pwsh`/MSBuild stack.
- Done criterion status: **NOT MET** (all four build classes are on the same commit, but none succeeded; Windows workflow toolchain unavailable in this environment).

## Log files

- `/tmp/a2logs/linux_gcc_release.log`
- `/tmp/a2logs/linux_gcc_debug.log`
- `/tmp/a2logs/linux_clang_release.log`
- `/tmp/a2logs/cmake_linux_release_build.log`
- `/tmp/a2logs/cmake_linux_debug_build.log`
- `/tmp/a2logs/windows_x64_Release.log`
- `/tmp/a2logs/windows_x64_Debug.log`
- `/tmp/a2logs/windows_Win32_Release.log`
- `/tmp/a2logs/windows_Win32_Debug.log`
