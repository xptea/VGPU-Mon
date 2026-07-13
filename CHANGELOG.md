# Changelog

All notable user-facing changes are documented here. This project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.1.4] - 2026-07-13

### Fixed

- Execute the curl bootstrap through PowerShell's text pipeline instead of an unreliable native-to-native stdin pipe.
- Allow the optional version selector to remain omitted when the downloaded script text is evaluated directly.
- Exercise the README's downloaded-text execution behavior in the installer lifecycle test.

## [1.1.3] - 2026-07-13

### Added

- Add a one-line `curl.exe` installation command for PowerShell and Windows Terminal.
- Resolve the latest stable GitHub Release automatically and install `vgpu` for the current user.

### Security and quality

- Verify the downloaded setup executable against the release's `SHA256SUMS.txt` before execution.
- Exercise the bootstrap install, PATH command, and exact uninstall restoration in CI.

## [1.1.2] - 2026-07-13

### Security and quality

- Instrument C compilation as well as linking for Windows Control Flow Guard.
- Add a tested CMake/MSBuild path for IDEs and accurate compiler-traced CodeQL analysis.
- Create CSV logs with explicit read/write permissions and non-inheritable file descriptors.
- Extend CLI tests to cover secure CSV creation and output.

## [1.1.1] - 2026-07-13

### Added

- Full-screen charts for GPU, VRAM, WDDM engines, temperature, power, and memory-controller activity.
- Mouse-clickable sortable headers and mouse-wheel process navigation.
- JSON snapshots, CSV logging, multiple-GPU switching, and process details.
- Deterministic `--demo` mode for previews and GPU-independent automated tests.
- Per-user Windows installer that installs the `vgpu` command and safely restores PATH on uninstall.

### Fixed

- Repaint the complete terminal frame after resize without leaving stale columns or duplicate headers.
- Prevent visible frame flashing and vertical bounce during periodic refresh.
- Release partially initialized WDDM/PDH providers and close every provider on all shutdown paths.
- Detect CSV write failures and reject malformed numeric command-line options.
- Preserve a pre-existing trailing user-PATH separator across uninstall.

### Security and quality

- Added `/W4 /WX /sdl`, control-flow protection, ASLR/DEP/CET-compatible linker flags, reproducible Release builds, MSVC AddressSanitizer tests, CRT leak checks, static analysis, CodeQL, and pinned CI dependencies.

[Unreleased]: https://github.com/xptea/VGPU-Mon/compare/v1.1.4...HEAD
[1.1.4]: https://github.com/xptea/VGPU-Mon/compare/v1.1.3...v1.1.4
[1.1.3]: https://github.com/xptea/VGPU-Mon/compare/v1.1.2...v1.1.3
[1.1.2]: https://github.com/xptea/VGPU-Mon/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/xptea/VGPU-Mon/releases/tag/v1.1.1
