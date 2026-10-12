# Build and dependencies

## Offline checks

Requirements: Windows x64, Visual Studio 2022 / C++ Build Tools with MSVC v143
or a compatible newer compiler, and Windows SDK. The scripts locate the compiler
through `vswhere` and use `/std:c++latest` for C++23 features.

```powershell
./run-public-checks.ps1
```

This runs the standalone regression suites and the synthetic monitoring fixture.
No DMA board, game process, external binary SDK, or MemProcFS runtime is needed.
Generated test executables and reports stay under the ignored `diagnostics/` folder.
The crash-recording fixture deliberately triggers an access violation and checks
that a crash record is written; that child-process failure is expected.

Individual suites remain available as `run-*-checks.ps1` scripts.

## Full application

This source-only publication includes the project definitions and dependency
headers/source, but excludes binary SDK libraries and runtime assets.
Restore these before running `./build.ps1`:

| Destination | Required files |
|---|---|
| `Dependencies/MemProcFS/` | Matching x64 `vmm.lib`, `leechcore.lib`, and the coherent MemProcFS/LeechCore runtime distribution. The development source used MemProcFS 5.15.9. |
| `Dependencies/MemProcFS/` | `dbghelp.dll`, `symsrv.dll`, and `info.db`, with their distribution notices, for managed process-liveness checks. |
| `Dependencies/Makcu/lib/` | Compatible `makcu-cpp.lib` for the existing project link configuration; bundled headers are from 1.3.3. |
| `Resources/Map.png` | Optional radar map asset, if using the inherited radar path. It is not needed for the default player-box presentation. |

Upstreams: [MemProcFS](https://github.com/ufrisk/MemProcFS),
[LeechCore](https://github.com/ufrisk/LeechCore),
[makcu-cpp](https://github.com/K4HVH/makcu-cpp).
Obtain binaries and optional assets under their upstream terms.

```powershell
./build.ps1
```

Target: **Release | x64**, MSVC v143, C++23, Windows SDK.
Output: `x64/Release/GOTY-Deadlock-DMA.exe`.
Keep the compatible runtime DLLs, `info.db`, and distribution notices alongside
the executable. Managed liveness checks also require matching symbols for the
target Windows kernel. MAKCU/input is opt-in; its runtime DLL is delay-loaded.

The embedded IBM Plex font was replaced with ImGui's built-in font for this
publication. Missing optional `Resources/` assets are skipped by the build script.
The full application has not been rebuilt or tested against hardware from this
source-only distribution.
