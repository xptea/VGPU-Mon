# Security policy

## Supported versions

Security fixes are made on the latest released version. Older binaries and development snapshots are not supported.

| Version | Supported |
| --- | --- |
| Latest release | Yes |
| Older releases | No |

## Reporting a vulnerability

Do not open a public issue for a vulnerability or publish exploit details before a fix is available. Use GitHub’s private vulnerability reporting for this repository:

https://github.com/xptea/VGPU-Mon/security/advisories/new

Include the affected version, Windows version, reproduction steps, impact, and any proof of concept. Redact unrelated process names, paths, GPU UUIDs, and telemetry.

Maintainers will acknowledge a complete report as time permits, investigate it, and coordinate disclosure when a fix is ready. Please allow a reasonable remediation window before public disclosure.

## Scope

Examples that are in scope include unsafe memory handling, argument or path injection, privilege-boundary mistakes, installer PATH corruption, and process-termination safety bypasses. General driver problems, inaccurate vendor counters, and unsupported hardware are compatibility issues unless they create a security impact.

VGPU-Mon does not require elevation and must never attempt to bypass Windows process security. The project does not bundle or download NVIDIA libraries at runtime.
