# Changelog

All notable user-facing changes are documented here. This project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.3.2] - 2026-07-13

### Fixed

- Preserve valid per-process dedicated-memory values when another WDDM row is corrupt instead of replacing the entire dedicated column with `N/A`.
- Reject individual stale rows against NVIDIA's current board allocation with bounded sampling slack, catching both the impossible Zen value and the inflated DWM value from the reported Windows counter failure.

## [1.3.1] - 2026-07-13

### Fixed

- Quarantine the complete WDDM dedicated-memory snapshot when Windows returns an impossible per-process value, preventing smaller stale values from being presented as trustworthy VRAM usage.
- Detach the installer helper from the terminal and stop relaunching a competing foreground monitor after the invoking shell regains keyboard input.
- Remove the persistent `Click headers to sort` hint from the interactive footer.

### Security and quality

- Return `null`/blank dedicated-memory values in JSON and CSV whenever the affected WDDM snapshot is quarantined.
- Exercise snapshot-wide WDDM quarantine and verify that live update handoff never relaunches an input-competing monitor.

## [1.3.0] - 2026-07-13

### Added

- Zoom the chart's visible time span with the mouse wheel, from a tight live view through the retained history.
- Pan older/newer with `Shift`+wheel, arrow keys, or page keys, with `Home` for the oldest complete window and `End` for live data.
- Show an in-chart crosshair tooltip with the nearest real sample's exact value and age.
- Retain 14,400 timestamped samples per metric in a bounded ring and preserve historical view position while new samples arrive.
- Add a repository-level `AGENTS.md` with architecture, testing, terminal-rendering, security, and release guidance.

### Security and quality

- Keep chart time calculations correct across live refresh-interval changes by recording monotonic timestamps per sample.
- Exercise chart wheel, pan, hover, resize, and complete-frame bounds through the ConPTY integration test.

## [1.2.0] - 2026-07-13

### Added

- Check a small version manifest on each installed interactive launch and automatically install newer stable releases.
- Verify the exact versioned setup executable with Windows SHA-256 APIs before starting an update.
- Relaunch the monitor with its original interactive options after an update handoff.
- Add `--update`, `--no-update`, and the `VGPU_MON_NO_UPDATE` environment opt-out.

### Security and quality

- Keep JSON, one-shot, version, help, and demo commands network-free and deterministic.
- Reject malformed manifests, path-like installer names, HTTPS downgrades, oversized downloads, and checksum mismatches.
- Publish `version.txt` and checksum it alongside every release asset.

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

[Unreleased]: https://github.com/xptea/VGPU-Mon/compare/v1.3.0...HEAD
[1.3.0]: https://github.com/xptea/VGPU-Mon/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/xptea/VGPU-Mon/compare/v1.1.4...v1.2.0
[1.1.4]: https://github.com/xptea/VGPU-Mon/compare/v1.1.3...v1.1.4
[1.1.3]: https://github.com/xptea/VGPU-Mon/compare/v1.1.2...v1.1.3
[1.1.2]: https://github.com/xptea/VGPU-Mon/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/xptea/VGPU-Mon/releases/tag/v1.1.1
