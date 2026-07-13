# VGPU-Mon

[![CI](https://github.com/xptea/VGPU-Mon/actions/workflows/ci.yml/badge.svg)](https://github.com/xptea/VGPU-Mon/actions/workflows/ci.yml)
[![CodeQL](https://github.com/xptea/VGPU-Mon/actions/workflows/codeql.yml/badge.svg)](https://github.com/xptea/VGPU-Mon/actions/workflows/codeql.yml)
[![MIT License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

VGPU-Mon is a native Windows GPU task manager written in C. It combines Windows WDDM performance counters with DXGI and NVIDIA NVML to show board telemetry and the processes consuming each GPU.

It is a real full-screen terminal application, not a wrapper around `nvidia-smi`. NVML is loaded dynamically, so the same executable still runs on AMD and Intel systems with the vendor-neutral data that Windows exposes.

## Features

- Live GPU and physical VRAM utilization with driver-reserved memory separated
- Full-screen scrolling charts for GPU, VRAM, engine, temperature, and power metrics
- WDDM dedicated/shared GPU-memory commitments per process
- Per-process 3D, compute, copy, encode, and decode engine activity
- Temperature, board power, power limit, fan, P-state, and clocks through NVML
- Encoder, decoder, and PCIe link/throughput telemetry when the driver exposes it
- Multiple-GPU switching
- Mouse-clickable sort headers and process rows, plus mouse-wheel navigation
- Responsive layouts with bounded full-frame repainting after every terminal resize
- Interactive sorting, process-name filtering, and process details
- Confirmed process termination with protection for system and self PIDs
- Pause/resume and adjustable 250-5000 ms refresh interval
- CSV logging
- One-shot human-readable and JSON output for scripts
- Automatic, checksum-verified updates for installed interactive launches
- Statically linked MSVC runtime; no NVML SDK or third-party runtime required

## Runtime requirements and compatibility

- 64-bit Windows 10 version 1809 or newer, or Windows 11
- A WDDM 2.x display driver for per-process and per-engine Windows counters
- Windows Terminal is recommended for the interactive interface
- A current NVIDIA display driver for temperature, power, clocks, fan, encoder/decoder, and NVML memory accounting

No CUDA Toolkit, NVML SDK, Visual C++ Redistributable, Python, or administrator access is required at runtime. The release executable statically links the C runtime and dynamically discovers NVML in both supported Windows driver locations: `System32` for DCH drivers and `Program Files\NVIDIA Corporation\NVSMI` for standard drivers.

Modern GeForce/RTX, Quadro/RTX professional, and data-center NVIDIA GPUs generally receive the full feature set when the installed driver exposes those calls. NVIDIA documents only limited NVML support for non-data-center GPUs, so **not every NVIDIA GPU/driver combination exposes every sensor**. Unsupported values appear as `N/A` or `Unavailable`, not as fabricated readings.

Under NVIDIA TCC mode, old pre-WDDM 2.x drivers, some VMs, or remote sessions, NVML board telemetry may work while Windows per-process and engine counters do not. AMD and Intel adapters receive DXGI/WDDM data but not NVIDIA-only sensors.

Visual Studio 2022 C++ Build Tools are needed only to build from source.

Some protected processes cannot be inspected or terminated without elevation. VGPU-Mon reports their GPU accounting normally when Windows exposes it, but does not bypass Windows security.

## Install

Open PowerShell or Windows Terminal and paste this one command:

```powershell
curl.exe -fsSL https://raw.githubusercontent.com/xptea/VGPU-Mon/main/install.ps1 | Out-String | Invoke-Expression
```

The bootstrap script finds the latest stable [GitHub Release](https://github.com/xptea/VGPU-Mon/releases), downloads its per-user installer and `SHA256SUMS.txt`, verifies the installer SHA-256, and only then runs it silently. It does not require administrator access. Open a new terminal tab after installation, then run:

```powershell
vgpu
vgpu --chart vram
vgpu --chart 3d
vgpu --json
```

Existing terminal processes keep their old environment. Uninstalling VGPU-Mon from Windows Settings removes the PATH entry only when the installer originally added it.

Starting with VGPU-Mon 1.2.0, an installed interactive launch checks the latest stable release's small `version.txt` manifest. If a newer version exists, VGPU-Mon downloads that exact versioned installer, verifies its SHA-256 with Windows cryptography, installs it without elevation, and reopens with the same interactive options. Network or GitHub failures do not prevent the monitor from opening. Redirected output and script commands (`--json`, `--once`, `--version`, and `--demo`) never perform an automatic update check.

Use `vgpu --update` to check immediately. Use `--no-update` for one launch or set `VGPU_MON_NO_UPDATE=1` to disable automatic checks. Portable ZIP copies do not update silently; `vgpu --update` converts a portable copy into the normal per-user installation.

Prefer to inspect scripts before running them? [Read `install.ps1`](install.ps1), or download the versioned installer/portable ZIP manually from Releases. Release assets include `SHA256SUMS.txt`. Windows SmartScreen may warn about community-built releases until the project has a trusted code-signing certificate; always verify the download came from this repository.

## Build and run

Install Visual Studio 2022 with **Desktop development with C++**. Include the optional **C++ AddressSanitizer** component if you want to run the sanitizer gate. From PowerShell:

```powershell
.\build.ps1 -Configuration Release -Test
.\run.ps1
```

The build script discovers Visual Studio through `vswhere`, initializes the correct x64 native toolchain, and writes `build\vgpu-mon.exe`.

A CMake target is also provided for IDEs and static-analysis tooling:

```powershell
cmake -S . -B build\cmake -A x64
cmake --build build\cmake --config Release --parallel
ctest --test-dir build\cmake -C Release --output-on-failure
```

The full local quality gates are:

```powershell
.\build.ps1 -Configuration Debug -Test
.\build.ps1 -Configuration Release -Test
.\build.ps1 -Configuration Sanitize -Test -OutputName vgpu-mon-asan
.\analyze.ps1
.\tools\verify-pe.ps1
```

Debug tests enable the MSVC CRT leak detector. The sanitizer build runs the same native, ConPTY, and CLI tests under MSVC AddressSanitizer. `/analyze` warnings fail the build. The PE check verifies ASLR, DEP, CFG, CET compatibility, and the absence of a dynamic MSVC runtime dependency. GitHub Actions repeats these checks and runs CodeQL on every change to `main`.

To create the portable release ZIP:

```powershell
.\package.ps1
```

To build the Windows installer, install Inno Setup 6 and run:

```powershell
winget install --id JRSoftware.InnoSetup --exact --scope user
.\installer.ps1
```

## Interactive controls

| Key | Action |
| --- | --- |
| `Up` / `Down` | Select a process |
| `PgUp` / `PgDn` | Move by ten rows |
| `u` | Sort by busiest GPU engine |
| `m` | Sort by dedicated VRAM |
| `s` | Sort by shared memory |
| `p`, `n`, `e` | Sort by PID, name, or engine |
| `o` | Reverse the current sort |
| Mouse click | Sort a header or select a process row |
| Mouse wheel | Move through process rows |
| `f` | Edit the process-name filter |
| `d` | Toggle details for the selected process |
| `i` | Toggle the full GPU information panel |
| `c` | Toggle full-screen chart/table view |
| `1`-`0` | Select GPU, VRAM, engine, temperature, or power chart |
| `k` | Ask to terminate the selected process |
| `l` | Toggle CSV logging to `vgpu-mon.csv` |
| `[` / `]` | Switch GPU |
| `+` / `-` | Refresh faster or slower |
| `Space` | Pause or resume sampling |
| `h` | Toggle help |
| `q` | Quit |

## Script-friendly output

```powershell
.\build\vgpu-mon.exe --once
.\build\vgpu-mon.exe --json
.\build\vgpu-mon.exe --gpu 0 --interval 500 --json |
    ConvertFrom-Json
```

## Demo mode

`--demo` feeds deterministic sample telemetry through the real renderer, input loop, charts, and JSON code without querying GPU hardware. It is useful for evaluating the UI, reproducing terminal bugs, and automated testing:

```powershell
.\build\vgpu-mon.exe --demo
.\build\vgpu-mon.exe --demo --chart vram
.\build\vgpu-mon.exe --demo --json
```

Demo values are clearly labeled and must not be treated as machine telemetry.

## Full-screen charts

```powershell
.\build\vgpu-mon.exe --chart
.\build\vgpu-mon.exe --chart vram
.\build\vgpu-mon.exe --chart 3d
.\build\vgpu-mon.exe --chart copy
.\build\vgpu-mon.exe --chart decode
.\build\vgpu-mon.exe --chart encode
.\build\vgpu-mon.exe --chart compute
.\build\vgpu-mon.exe --chart memory-controller
.\build\vgpu-mon.exe --chart temperature
.\build\vgpu-mon.exe --chart power
```

The flag form requested by the UI is also supported, such as `--chart --vram`, `--chart --3d`, or simply `--vram`. In chart view, number keys switch metrics without restarting and `c` returns to the process table.

Options:

```text
--once             Print one snapshot and exit
--json             Print one snapshot as JSON
--chart [METRIC]   Open a full-screen chart
--vram, --percent  Open a VRAM or overall-utilization chart
--3d, --copy, --decode, --encode, --compute
                   Open a WDDM engine chart
--gpu INDEX        Select a physical GPU
--interval MS      Sampling interval from 250 to 5000
--log PATH         Start CSV logging when interactive mode opens
--demo             Use deterministic sample data for previews and tests
--update           Check for and install an update now
--no-update        Skip the automatic update check for this run
--help             Show help
--version          Show the version
```

## Data sources and accounting

- **Windows GPU performance counters** provide per-process engine utilization and dedicated/shared memory commitments. The displayed process percentage is that process's busiest engine, which follows Windows Task Manager's accounting model.
- **DXGI 1.4** enumerates hardware adapters and reports local video-memory usage and the current OS memory budget.
- **NVML** supplies NVIDIA board telemetry. VGPU-Mon uses the versioned memory API to separate driver/firmware-reserved VRAM from application allocation. `nvml.dll` is discovered and loaded at runtime; the project does not ship NVIDIA libraries.

VRAM figures from NVML, DXGI, and WDDM can differ because they describe different layers: physical board usage, the operating-system budget, and per-process commitments. A WDDM process commitment can exceed physical VRAM because it is not a resident-byte measurement. VGPU-Mon marks process columns with `*`, labels JSON/CSV fields as commitments, and keeps the physical total and OS budget separate.

Windows also has a documented GPU Process Memory counter issue that can produce implausibly large per-process values. VGPU-Mon appends `!`, raises `wddm_process_memory_warning` in JSON, and preserves the raw value for diagnosis instead of silently clamping it. Physical board usage at the top remains sourced from NVML/DXGI.

Terminating a process is not the same as resetting a GPU. VGPU-Mon deliberately does not expose driver resets or unsafe attempts to cancel individual command queues.

JSON, CSV, screenshots, and bug reports can expose process names, executable activity, paths, or GPU identifiers. Review and redact telemetry before sharing it.

## Contributing and security

- [Contributing guide](CONTRIBUTING.md)
- [Architecture and ownership model](docs/architecture.md)
- [Security policy and private reporting](SECURITY.md)
- [Support guide](SUPPORT.md)
- [Changelog](CHANGELOG.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)

VGPU-Mon is released under the [MIT License](LICENSE).

## Technical references

- [NVIDIA NVML API reference](https://docs.nvidia.com/deploy/nvml-api/nvml-api-reference.html)
- [Microsoft console input modes](https://learn.microsoft.com/en-us/windows/console/low-level-console-modes)
- [Microsoft GPU Process Memory counter known issue](https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/gpu-process-memory-counters-report-wrong-value)
- [GPU virtual memory in WDDM 2.0](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)
