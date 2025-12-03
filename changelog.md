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
  - Some versions of Tinfoil may show the split folder as a game but list the first part as `00000000` and fail to install from this layout; in that case, either use DBI or join the parts into a single NSP on a PC for Tinfoil.
