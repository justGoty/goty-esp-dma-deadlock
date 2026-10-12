# GOTY ESP · Deadlock

![C++](https://img.shields.io/badge/C%2B%2B-23-58a6ff?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-30363d?style=flat-square)
![Snapshot](https://img.shields.io/badge/snapshot-0.1.12-f0883e?style=flat-square)
![Checks](https://github.com/justGoty/goty-esp-dma-deadlock/actions/workflows/checks.yml/badge.svg)
![License](https://img.shields.io/badge/core%20license-MIT-3fb950?style=flat-square)

A public, source-only snapshot of **GOTY Deadlock DMA** — a DMA reader and
DirectX 11 fuser renderer for a Windows receiver.

## Inside

- **Player boxes first:** bones, NPCs, camps, labels, and radar are disabled by default.
- **Fullscreen output:** 2560×1440 with automatic monitor selection and frame pacing.
- **Freshness checks:** process-liveness validation and automatic hiding of stale player boxes.
- **Diagnostics:** connection stages, latency metrics, error reporting, and collection tools.
- **Clean lifecycle:** cooperative shutdown, DMA cleanup, and temporary display/system sleep prevention.

## Refactored from upstream

| Area | Technical changes |
|---|---|
| DMA reads | RAII scatter batches, returned-byte validation, and staged player updates published only after successful reads. |
| Rendering | Player, camera, and map reads moved outside GUI locks; one camera copy per frame; optional read lanes skipped in the default box mode. |
| Worker lifecycle | Managed refresh, direct process-identity checks, a reusable high-resolution timer, and orderly worker joins on exit. |

## Start here

Run the hardware-independent checks on Windows with Visual Studio C++ Build Tools:

```powershell
./run-public-checks.ps1
```

Publication checks passed **38,001 assertions**, plus crash-recording and
monitoring integration fixtures, on **12 October 2026**. The checks cover read
validation, discovery bounds, projection, timing, process liveness, shutdown,
and diagnostic collection. They require no DMA board or game process.
See [build and dependencies](docs/BUILD.md) for the full application.

## Controls

Press these keys **on the computer running the EXE**.

| Key | Action |
|---|---|
| **Insert** | Show / hide the control panel |
| **Delete** | Exit and release DMA resources |

Use one DMA client per board. Restart the application after restarting the
game; attachment to a new game process is not automatic. Inherited MAKCU/input
features are disabled by default and have not been validated for this snapshot.

## Snapshot and provenance

Published from version **0.1.12**, source revision
`2477f85ae2f12b5542a6d42aaba687e538aa06e0`.
This repository starts with a new public snapshot commit. Private history,
internal reports, user state, logs, binaries, SDK libraries, databases, images,
and embedded font data are excluded. The publication uses ImGui's built-in font.

Based on [CyN1ckal/Deadlock_DMA](https://github.com/CyN1ckal/Deadlock_DMA).
The original [MIT notice](LICENSE), source notices, and
[upstream README](UPSTREAM_README.md) are retained. Dependencies keep their own
terms; see [third-party notices](THIRD_PARTY.md).

Offsets and layouts can change with game updates. Offline checks establish
regression coverage; they do not establish current hardware or game compatibility.
