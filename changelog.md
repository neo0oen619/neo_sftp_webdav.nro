# Changelog

## 2025-12-03 – WebDAV large-file & speed work

- Added ranged WebDAV downloads with DBI-style split layout for files larger than 4 GiB so they can be stored safely on FAT32 and installed directly by DBI.
- Implemented automatic split folder name sanitisation and a fallback path under `/switch/neo_sftp/downloads` to avoid SD filesystem issues with long/Unicode-heavy filenames or unwritable roots (e.g. `/Download`).
- Introduced parallel ranged downloads for WebDAV:
  - Parallel workers per file are controlled by `[Global] webdav_parallel`.
  - Both split and non-split downloads use parallel HTTP `Range` requests, with a 256 MiB cap on the total in-flight window (`webdav_chunk_mb * webdav_parallel`).
- Improved WebDAV resume behaviour:
  - Split downloads resume based on existing parts (`<safe_name>.nsp/00`, `01`, …).
  - Non-split downloads resume from the existing local file size when possible.
- Hardened failure handling:
  - WebDAV workers retry transient network errors a limited number of times per range (disconnects, DNS issues, etc.).
  - When a WebDAV download still fails, the UI now shows a **Confirm** dialog with the real error and lets the user choose to retry/resume or abort instead of immediately failing.
  - Failure messages now include the actual amount downloaded (e.g. `downloaded X/Y MiB`) so partial progress is visible.
- Prevented long downloads from being interrupted by console sleep:
  - Enabled CPU boost and Wi-Fi priority.
  - Disabled auto-sleep via `appletSetAutoSleepDisabled(true)` while the app is running.
- Updated naming and UI text:
  - Homebrew title name changed to `neo_sftp_webdav` (NRO file remains `neo_sftp.nro`).
  - Main settings header text changed to `neo_sftp_webdav settings`.
  - Documentation updated to use `/switch/neo_sftp/config.ini` as the runtime config path.
- Known installer behaviour:
  - DBI understands the `<game>.nsp/00, 01, …` split layout and can install large NSPs directly from those folders.

## 2025-12-03 – Multi-file WebDAV & auto-retry

- Added a small WebDAV-only download scheduler so multiple files can be downloaded in parallel:
  - The number of concurrent WebDAV files is controlled by `[Global] download_parallel_files` (clamped 1–3).
  - Each file still uses the existing per-file WebDAV range pipeline (`webdav_chunk_mb` and `webdav_parallel`), with one “leader” job using the main connection for UI and background jobs using their own `WebDAVClient` instances.
  - Background jobs log failures but don’t show confirm popups; the leader keeps the existing UI prompts and progress bar semantics.
- Refactored the download helpers so they can work with an explicit `RemoteClient` instance, enabling per-job WebDAV clients in worker threads while preserving the existing behaviour for the main connection.
- Improved WebDAV failure behaviour on the main connection:
  - When a WebDAV download fails, the app now auto-retries it up to 6 times, spreading a ~20s total wait across those attempts before showing any confirm dialog.
  - This smooths over transient “Couldn’t connect to server” errors (e.g. VPN/proxy hiccups) on long queues without changing resume semantics.

## 1.1.1 – Concatenation splits & installer compatibility

- Marked WebDAV split NSP folders (`<name>.nsp`) as **concatenation files** using `fsdevSetConcatenationFileAttribute`, so Horizon OS sees them as single logical NSPs whose contents are the concatenation of `00`, `01`, … parts.
- This makes large, split WebDAV downloads installable on FAT32 SD cards by tools like DBI and Tinfoil, matching DBI’s own “archived folder” behaviour for >4 GiB files.
- Fixed WebDAV `href` decoding to stop converting `+` into spaces in path segments, so filenames like `[B+U393216+16DLC].nsp` resolve correctly.
- Tweaked WebDAV auto-retry semantics so failed downloads are retried up to 6 times with a ~20s total backoff before showing a confirm prompt, keeping long queues moving through transient network issues.
- Changed `[Global] webdav_split_large` to default to `1` (DBI-style split layout) so large NSPs are safe on FAT32 and still installable by DBI/Tinfoil; exFAT users who prefer a single full NSP can set this back to `0`.
- Updated documentation:
  - `README.md` documents `download_parallel_files` alongside `webdav_chunk_mb` and `webdav_parallel`.
  - `webdav_downloads.md` and `wanttodo.md` have been refreshed to describe the implemented WebDAV pipeline and to focus TODOs on cross-protocol support and UI polish.
