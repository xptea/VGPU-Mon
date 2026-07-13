# AGENTS.md

This file is the working guide for coding agents and contributors changing VGPU-Mon.

## Project shape

VGPU-Mon is a native 64-bit Windows GPU process monitor written in C11. The supported build toolchain is MSVC from Visual Studio 2022. The interactive UI uses Win32 console input plus VT output; it is not a curses application.

Important areas:

- `src/main.c`: application state, sampling loop, terminal rendering, keyboard/mouse input, CLI output
- `src/nvml_dyn.c`: dynamically loaded NVIDIA NVML telemetry
- `src/dxgi_gpu.c`: DXGI adapter and memory telemetry
- `src/d3dkmt_gpu.c`: direct per-process video-memory queries using DXGI adapter LUIDs
- `src/pdh_gpu.c`: WDDM/PDH engine counters, process discovery, and protected-process memory fallback
- `src/updater.c`: signed-release metadata checks, download verification, and update handoff
- `tests/test_core.c`: native unit and CLI behavior tests
- `tests/test_conpty.c`: full terminal lifecycle, input, resize, and frame-bound tests
- `tests/test_installer.ps1`: install, PATH, upgrade, and uninstall lifecycle
- `tools/`: release metadata, PE-hardening, dependency, and packaging checks

Read `docs/architecture.md`, `CONTRIBUTING.md`, and `SECURITY.md` before changing provider ownership, process termination, installation, or update behavior.

## Build and verification

Run commands from the repository root in PowerShell. The PowerShell/MSVC path is release-authoritative.

```powershell
.\build.ps1 -Configuration Debug -Test
.\build.ps1 -Configuration Release -Test
.\build.ps1 -Configuration Sanitize -Test -OutputName vgpu-mon-asan
.\analyze.ps1
.\tools\verify-pe.ps1
```

For a release candidate, also run:

```powershell
.\package.ps1 -SkipBuild
.\installer.ps1 -SkipBuild
.\tests\test_installer.ps1 -InstallerPath (Get-ChildItem .\dist\VGPU-Mon-*-setup.exe -File).FullName
```

Use `--demo` for UI work that must not depend on the local GPU:

```powershell
.\build\vgpu-mon.exe --demo
.\build\vgpu-mon.exe --demo --chart vram
```

Do not commit `build/`, `dist/`, telemetry logs, installers, or machine-specific output.

## Coding rules

- Keep C warnings clean under `/W4 /WX /sdl` and static analysis.
- Use four-space indentation, bounded buffers, explicit ownership, and one cleanup path per acquired resource.
- Preserve graceful behavior when NVML, DXGI, D3DKMT, PDH, a sensor, or an individual process query is unavailable.
- Do not add a mandatory runtime, SDK, administrator requirement, service, or driver.
- Keep optional vendor APIs dynamically discovered and keep AMD/Intel fallback behavior intact.
- Add or update automated tests for terminal input, resize, CLI parsing, cleanup, updater, or installer behavior changes.
- Update `README.md` and `CHANGELOG.md` for user-visible changes. Bump `src/version.h` only for a release-ready user-facing change.

## Terminal rendering invariants

- Every repaint must compose a complete frame for the current viewport.
- Never write the terminal's final column; a pending wrap can scroll or flash the viewport.
- Keep emitted rows within the current visible height and clear stale line/tail content.
- Mouse coordinates are console-buffer coordinates; normalize them with `viewport_left` and `viewport_top`.
- Resize, mouse, and chart changes must continue to pass the ConPTY bounds test.
- Chart history is a timestamped ring. Time labels and hover values must use stored timestamps, not assume the current refresh interval applied to older samples.
- A historical chart view should remain anchored while new samples arrive. `End` returns it to live data.

## Safety and release rules

- Treat process names, executable paths, UUIDs, and logs as potentially sensitive.
- Keep termination confirmation and protected/self-process checks intact.
- Never weaken HTTPS-only downloads, version parsing, release filename validation, SHA-256 verification, or current-user installer behavior.
- Do not publish a release unless Debug, Release, AddressSanitizer, static analysis, PE hardening, packaging, and installer lifecycle checks pass.
- Release tags must exactly match `VGPU_VERSION`, and release assets must include the installer, portable ZIP, bootstrap script, `version.txt`, and `SHA256SUMS.txt`.

When a check cannot run, report that explicitly; do not describe unverified work as production-ready.
