# Contributing to VGPU-Mon

Thanks for helping improve VGPU-Mon. Bug reports, compatibility results, documentation fixes, and focused pull requests are welcome.

## Before opening an issue

- Search existing issues first.
- Use `vgpu --version` and include the result.
- Include your Windows version, GPU model, display-driver version, terminal, and the exact command used.
- Try `vgpu --demo` when reporting a rendering problem. If demo mode reproduces it, the issue is probably in the terminal UI rather than a telemetry provider.
- Redact process names, paths, GPU UUIDs, and logs if they contain sensitive information.

Security vulnerabilities must be reported privately as described in [SECURITY.md](SECURITY.md).

## Build prerequisites

- 64-bit Windows 10 1809 or newer, or Windows 11
- Visual Studio 2022 with **Desktop development with C++**
- The **C++ AddressSanitizer** optional component for sanitizer builds
- Inno Setup 6 only when building the installer

No NVIDIA SDK is needed. NVML is loaded dynamically at runtime.

## Development checks

Run these from PowerShell before opening a pull request:

```powershell
.\build.ps1 -Configuration Debug -Test
.\build.ps1 -Configuration Release -Test
.\build.ps1 -Configuration Sanitize -Test -OutputName vgpu-mon-asan
.\analyze.ps1
.\tools\verify-pe.ps1
```

To exercise the terminal UI without a supported GPU:

```powershell
.\build\vgpu-mon.exe --demo
.\build\vgpu-mon.exe --demo --chart vram
.\build\vgpu-mon.exe --demo --json
```

## Pull requests

- Keep each pull request focused on one change.
- Add or update tests for behavior changes, especially terminal input, resize handling, CLI validation, and provider cleanup.
- Preserve graceful fallback behavior when NVML, a sensor, or a WDDM counter is unavailable.
- Do not commit `build/`, `dist/`, logs, generated installers, or personal telemetry.
- Update `README.md` and `CHANGELOG.md` when user-visible behavior changes.
- Keep warnings clean under `/W4 /WX /sdl` and MSVC `/analyze`.

The project uses four-space indentation in C, explicit ownership/cleanup, bounded buffers, and Windows APIs directly. New optional APIs should be discovered dynamically or guarded so older supported Windows and driver configurations continue to fail gracefully.

By contributing, you agree that your contribution is licensed under the project’s [MIT License](LICENSE).
