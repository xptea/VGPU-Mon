# Architecture

VGPU-Mon is a native Windows application. It has no service, kernel component, injected DLL, persistent background updater, or vendor SDK dependency. An installed interactive launch performs a bounded HTTPS update check before provider initialization; a verified update uses a short-lived PowerShell handoff only after the monitor exits.

## Data flow

1. `DXGI` enumerates physical hardware adapters and provides Windows video-memory usage and budget data.
2. `NVML` is loaded dynamically from NVIDIA’s driver locations. When present, it provides board sensors, clocks, power, physical VRAM, and driver identity.
3. `PDH` reads Windows GPU-engine and GPU-process-memory performance counters. Instances are grouped by PID and physical adapter.
4. The sampler normalizes those sources into one `GpuTelemetry` snapshot plus bounded process rows and chart histories.
5. The renderer builds one bounded frame in memory and repaints the visible terminal region in a single write.

The updater reads `version.txt` from the latest stable GitHub Release, validates its strict version, installer, and hash fields, and compares semantic versions. It downloads an exact tag-specific installer to the user temporary directory and hashes it with Windows CNG before starting a transient installer helper. Offline, malformed, oversized, downgraded, or hash-mismatched responses fail closed for the update and fail open for monitoring. Non-interactive output paths do not access the network.

`--demo` replaces steps 1–3 with deterministic values while keeping the real sampling, layout, JSON, input, and rendering paths. It exists for testing and UI previews; it is never presented as hardware telemetry.

## Ownership and cleanup

Each provider owns its query/library/COM handles and exposes an idempotent close function. Application cleanup calls all close functions even after partial initialization. Dynamic arrays, command-line conversion buffers, terminal buffers, process handles, tokens, pseudo-console resources, and test capture buffers have explicit paired cleanup.

Updater HTTP, file, registry, hashing, and child-process handles are closed on every path. Partial downloads and helper scripts are deleted after failure or handoff. The updater runs before GPU providers open, so no telemetry handles are inherited by the installer helper.

Debug builds use the MSVC debug heap to report leaks. Sanitizer builds compile the application and native tests with MSVC AddressSanitizer. CI also runs `/analyze` and CodeQL. These checks reduce risk but do not prove the absence of every defect.

## Terminal model

The interactive UI uses Windows console input records and VT output. Every update computes layout from the current visible viewport, bounds content to that viewport, erases each rendered line, and emits a complete frame. Resize events trigger immediate layout recomputation. ConPTY integration tests repeatedly resize the viewport and assert bounded frames, chart transitions, mouse sorting, and clean shutdown.

## Accounting caveats

NVML physical allocation, DXGI OS budget/usage, and WDDM per-process commitments describe different layers and do not necessarily sum. The UI labels commitments explicitly and preserves implausible WDDM values with a warning instead of disguising a Windows counter anomaly.

The displayed per-process GPU percentage is the busiest engine for that process, matching the accounting style used by Windows Task Manager. It is not additive across dissimilar engines.

## Trust boundaries

VGPU-Mon runs with the caller’s privileges. Process termination is opt-in, confirmed, and rejects self/system targets. Windows remains the authority for process access. The app does not elevate itself or bypass protected processes.

Automatic updates trust HTTPS certificate validation and the assets attached to this repository's stable GitHub Release. The downloaded setup executable must match the release manifest SHA-256 before execution. Updates remain per-user and do not request elevation.

Process names, GPU UUIDs, CSV files, and JSON snapshots can disclose local system activity. Users should review telemetry before sharing it.
