## neo_sftp WebDAV downloads – implementation notes

This file is for future agents working on the WebDAV / HTTP download path. It explains the current design, what was changed to support large files + resume, and where it’s easy (and dangerous) to tweak things.

### High‑level behaviour

- WebDAV client is implemented in `source/clients/webdav.cpp` (`WebDAVClient`).
- All HTTP I/O goes through `source/httpclient/HTTPClient.{h,cpp}` (`CHTTPClient`), which wraps libcurl.
- The UI polls global transfer stats from `source/windows.cpp`:
  - `bytes_transfered` – total bytes written locally for the active transfer.
  - `bytes_to_download` – expected total bytes (remote size when known, or 0 for WebDAV “size unknown” cases).
- Downloads are initiated from `Actions::DownloadFile` in `source/actions.cpp` and dispatch to `RemoteClient::Get`, which is overridden by `WebDAVClient::Get` for WebDAV sites.

### WebDAV download pipeline

`WebDAVClient::Get` is the main entry point:

1. Resolves the remote size via `Size(path, &size)` using a PROPFIND request.
2. Decides chunk size from INI:
   - `[Global] webdav_chunk_mb` (clamped 1–32 MiB, default 8).
3. Decides whether to use **split** mode:
   - `need_split = force_fat32 || (webdav_split_large && size > 0xFFFFFFFFLL);`
   - `force_fat32` is `[Global] force_fat32` (0/1).
4. If `need_split`:
   - Computes a **sanitized split base**:
     - Takes the user’s output path (e.g. `/games/...` or `/Download/<file>.nsp`).
     - Splits into `parentDir` and `fileName` by the final `/`.
     - Calls `SanitizePathComponent(fileName)` to strip/replace any characters that might upset the SD filesystem (non‑ASCII punctuation, fancy quotes, etc.) and to cap length.
       - Keeps alnum, space, `- _ [ ] ( ) +`, everything else becomes `_`.
       - Preserves the extension (e.g. `.nsp`).
     - Ensures `parentDir` exists; if not, falls back to `/Download` on the SD card.
     - Final split base path is: `parentDir + "/" + safeName`.
   - Computes existing split size via `GetSplitLocalSize(splitBase, partSize)` to support resume.
   - Chooses between **sequential** and **parallel** split download:
     - If `webdav_parallel > 1` and server supports `Range`, uses `GetRangedParallelSplit(...)`.
     - Otherwise, uses `GetRangedSequentialSplit(...)`.
5. If `need_split == false`:
   - Tries single‑file ranged download:
     - Uses `GetRangedSequential` with optional resume if a local partial file exists.
     - If `webdav_parallel > 1` and ranges are supported, uses `GetRangedParallel` instead.

### Split file layout (DBI‑style)

- File is stored as a folder named `<safe_name>.nsp` where `safe_name` is a sanitized version of the remote filename.
- Inside that folder are numbered parts:
  - `00`, `01`, `02`, … each up to `kSplitPartSize = 4 GiB – 64 KiB`.
- This is the layout DBI’s MTP mode expects and can install from directly.
- The split writer utility is `SplitFileWriter` inside `webdav.cpp`:
  - `SplitFileWriter::open()`:
    - Deletes a flat file with the same base path if it exists.
    - Creates parent directories + the base folder via `EnsureDirectoryTree(basePath)`.
  - `SplitFileWriter::write(offset, data, size)`:
    - Maps a global byte offset into a `(partIndex, offsetInPart)` pair.
    - Opens/creates the part file `basePath/XX` (two‑digit index).
    - Seeks and writes the data chunk.

**Tinfoil / DBI compatibility notes**

- After a split download completes, NeoSFTP calls `fsdevSetConcatenationFileAttribute` on the `<safe_name>.nsp` folder.
- This marks the directory as a **concatenation file** so Horizon OS treats it as a single large file whose contents are the concatenation of `00`, `01`, … parts.
- DBI and Tinfoil (and any other tools that use the normal FS APIs) can then see and install the split NSP as if it were a single file, even on FAT32.
- If you ever see a split folder that still appears as a bunch of raw parts, check that:
  - The download completed (look for `WEBDAV PERF split-*` logs with the full size).
  - You’re running a build that sets the concatenation attribute (v1.1.1 or newer).

### Parallel vs sequential modes

#### Sequential split (`GetRangedSequentialSplit`)

- Uses a single `CHTTPClient` and loops from `start_offset` to `size` by `chunk_size`.
- For each chunk:
  - Sends `Range: bytes=start-end` GET.
  - Writes body via `SplitFileWriter::write` at the global offset.
  - Updates `bytes_transfered`.
- Logs a `WEBDAV PERF split-ranged` line with total bytes and average MiB/s.

#### Parallel split (`GetRangedParallelSplit`)

- New in this patch – this is how you get >0.6 MiB/s on huge files.
- Creates `parallel` `CHTTPClient` instances and a shared `WebDAVParallelSplitContext`:
  - `url`, `sink`, `size`, `chunk_size`, `nextOffset`, `hadError`, `lastHttpCode`, `errorMessage`.
  - `stateMutex` to coordinate work distribution and error state.
  - `sinkMutex` to serialize writes to `SplitFileWriter` (which is not thread‑safe by itself).
- Spawns `parallel` worker threads, each running `WebDAVParallelSplitWorkerThread`:
  - Claims a `(start, end)` byte range from `ctx` under `stateMutex`.
  - Performs a ranged GET with retries (up to 10 attempts with backoff).
  - On success, acquires `sinkMutex` and calls `sink->write(start, body, size)`.
  - Updates `bytes_transfered` and `ctx->lastHttpCode`.
  - On unrecoverable errors, sets `ctx->hadError` and `ctx->errorMessage` under `stateMutex`.
- Caller joins all threads, then:
  - If `ctx->hadError`, propagates `ctx->errorMessage` to `this->response`.
  - Otherwise, logs `WEBDAV PERF split-parallel` and `WEBDAV GET split-parallel ranged done`.

**Important:** `webdav_parallel` is clamped to 16 for split downloads to avoid extreme memory usage:

- Effective in‑flight window = `chunk_size * parallel`.
- We cap it at 256 MiB (see `max_window` logic).

### Speed tuning knobs

Current tunables (in `config.ini`, section `[Global]`):

- `webdav_chunk_mb` (int, 1–32, default 8):
  - Size of each HTTP range chunk per request.
  - Larger ⇒ fewer requests, more RAM, better throughput on high latency links.
- `webdav_parallel` (int, 1–32, **but split mode clamps to 16**):
  - Number of parallel HTTP workers per file.
  - Higher ⇒ more connections and better speed until CPU/SD/network becomes the bottleneck.
- `force_fat32` (0/1):
  - When `1`, forces split layout even for files ≤4 GiB (useful for FAT32 SD cards).
  - Large files (>4 GiB) always use split, regardless of this flag.

**Practical recommendations (WebDAV over Tailscale + SFTPGo):**

- Start with:
  - `webdav_chunk_mb=8` or `16`.
  - `webdav_parallel=4` or `6`.
- Only push beyond that if you’re sure about SD and network stability.
- Remember the code enforces a 256 MiB max in‑flight window (`webdav_chunk_mb * webdav_parallel`). If you crank both, the chunk size will be automatically reduced.

#### Additional speed ideas (future work)

- **Adaptive chunk sizing**  
  Dynamically adjust `chunk_size` at runtime based on measured throughput and latency instead of relying purely on the INI value. For example:
  - Start at 4–8 MiB.
  - If average throughput is stable and memory headroom is good, step up to 16–32 MiB.
  - If retries or timeouts spike, step back down.

- **Per‑server presets**  
  Some endpoints (local LAN SFTPGo vs Internet WebDAV behind Cloudflare) behave very differently. Consider storing per‑site performance presets (e.g. `webdav_chunk_mb_siteN`, `webdav_parallel_siteN`) so switching to a “slow Internet” site doesn’t accidentally reuse aggressive LAN settings.

- **HTTP/2 vs HTTP/1.1 tuning**  
  Right now we request HTTP/2 (`CURL_HTTP_VERSION_2TLS`) and let libcurl fall back as needed. If you see pathological behaviour on a specific server (e.g. sluggish HTTP/2), you could:
  - Add a per‑site toggle to force HTTP/1.1 for that server only.
  - Or detect repeated retries / low speeds and temporarily downgrade to HTTP/1.1 for that connection.

- **Smarter parallel ramp‑up**  
  Instead of always using the configured `webdav_parallel`, consider:
  - Start with 2 workers.
  - Measure throughput and error rate over the first few chunks.
  - Gradually ramp up to the configured maximum (e.g. 8) if things are stable.
  - Back off if error rates climb or if the SD write path becomes a bottleneck.

- **Shared connection pools across files**  
  Today each download builds its own set of `CHTTPClient` instances. In theory, a shared pool could reuse connections more aggressively across multiple concurrent downloads, but this adds complexity and thread‑safety concerns. Only attempt this if you really need multi‑file parallelism and are comfortable refactoring `WebDAVParallelContext` into a longer‑lived manager.

### Resume behaviour

#### Split files

- On start, `WebDAVClient::Get` calls `GetSplitLocalSize(splitBase, partSize)`:
  - Sums the sizes of `splitBase/00`, `01`, `02`, … until it hits a missing or shorter‑than‑part file.
  - Sets `bytes_transfered = local_split_size` and `bytes_to_download = size`.
  - If `local_split_size >= size`, the file is treated as already complete.
- Both `GetRangedSequentialSplit` and `GetRangedParallelSplit` accept `start_offset` (via `GetRangedSequentialSplit`) or implicitly start from `nextOffset = local_split_size` in the split context.

**Key property:** As long as the split base path for a given remote file is stable across runs, we can resume even after app restarts.

#### Single‑file downloads

- For non‑split downloads, `WebDAVClient::Get` checks `FS::GetSize(outputfile)`:
  - If `0 < local_size < remote_size`, it will call `GetRangedSequential` with `start_offset=local_size` and set `bytes_transfered = local_size`.
  - This provides resume for HTTP/S (non-WebDAV) and WebDAV small files.

#### What **not** to do if you care about resume

- Do **not** rename partial split folders or partial files on failure. Resume logic keys off the base path.
  - If you really want to mark failed downloads, add metadata (e.g. a `.failed` marker file or a log entry) instead of renaming the data directory.
  - If you must rename, you should also rename it back to the deterministic `splitBase` naming convention before retrying.

### Failure handling, auto‑retry & real size

Current behaviour:

- On failure, `DownloadFile` (`source/actions.cpp`) logs:
  - `"Download failed path=%s resp=%s"` using `remoteclient->LastResponse()`.
- `bytes_to_download` typically retains the remote size (if known).
- `bytes_transfered` reflects the actual local bytes written.

For WebDAV downloads dispatched through the main `remoteclient`, the app now:

- Automatically retries a failed download up to 6 times with a short delay between attempts (20 s total per attempt, split into smaller sleeps so cancel stays responsive).
- Only after those automatic retries are exhausted does it show the existing **Confirm** popup asking the user whether to resume or abort.
- Leaves partial data on disk so a later retry can resume (split: based on parts in `<safe_name>.nsp/00, 01, …`; non‑split: based on the existing flat file size).

This keeps long queues from stalling on a single transient “Couldn’t connect to server” without changing the resume semantics or data layout.

### FAT32 vs exFAT and >4 GiB files

Important filesystem notes:

- FAT32 cannot store single files larger than 4 GiB (4 294 967 295 bytes). Attempts to write past that limit will fail even if there is free space.
- exFAT can store large NSPs as a single file without this limit.

How this interacts with NeoSFTP:

- When `[Global] webdav_split_large=0` and `force_fat32=0`:
  - Large WebDAV downloads (>4 GiB) are written as a single flat NSP.
  - On **exFAT** this is fine and works well with Tinfoil.
  - On **FAT32** this will fail around ~3.9–4.0 GiB with log lines like:
    - `WEBDAV GET parallel write failed ... expected=8388608 written=0`
    - `WEBDAV GET range write failed ... expected=8388608 written=0`
- When either `webdav_split_large=1` or `force_fat32=1`:
  - Large WebDAV downloads are written using the DBI-style split layout (`<name>.nsp/00, 01, …`).
  - Each part stays below the FAT32 limit, so downloads succeed on FAT32 and DBI can install from the split folder.

Recommended combinations:

- **exFAT SD card + Tinfoil installs**
  - `force_fat32=0`
  - `webdav_split_large=0` (single full NSP per game)
- **FAT32 SD card + DBI/Tinfoil installs**
  - `force_fat32=1` or `webdav_split_large=1` (split layout + concatenation attribute).
  - The OS exports the `<name>.nsp` folder as a single logical NSP via the concatenation file mechanism, so DBI and Tinfoil can install it despite the 4 GiB FAT32 limit.

If you see a large NSP repeatedly failing around 4 GiB with write errors, check your SD card format and the `webdav_split_large` / `force_fat32` settings.

### Program naming & UI text

- NRO build target and filenames (per `AGENTS.md` and `CMakeLists.txt`):
  - Target: `neo_sftp`.
  - Outputs:
    - `build/neo_sftp.elf`
    - `build/neo_sftp.nacp`
    - `build/neo_sftp.nro`
- Homebrew menu display name is controlled by `TITLE_NAME` in `CMakeLists.txt`:
  - Currently set to `"neo_sftp_webdav"` so the HB menu shows that name.
- Connection settings header text is `STR_CONNECTION_SETTINGS`:
  - Default string and `lang/English.ini` entry have been updated to `"neo_sftp_webdav settings"`.

**Do not** change the CMake target name or output names away from `neo_sftp`/`neo_sftp.nro` without also updating external tooling and docs. If the user wants a differently named NRO on their SD card, they can copy/rename it after the build.

### Things still on the TODO list

These are tasks that would improve behaviour but are more invasive, so they’re noted here for future work instead of being implemented now:

1. **Cross‑protocol parallelism and resume**
   - SFTP and FTP paths currently use simpler, mostly sequential reads.
   - Implementing multi‑chunk, parallel reads for SFTP would require either libssh2 pipelining or multiple SFTP handles per file, plus careful coordination similar to `WebDAVParallelSplitContext`.
   - Any such change must respect Switch CPU/RAM limits and be disabled by default or guarded behind INI flags.

2. **Better failure labelling without breaking resume**
   - The user requested prefixing filenames with “failed ” when downloads fail.
   - This conflicts with path‑based resume, since rename breaks the deterministic base path used by `GetSplitLocalSize` and `FS::GetSize`.
   - A safer alternative is to:
     - Leave data paths untouched.
     - Track failures in a small metadata file under `/switch/neo_sftp` or mark entries in the UI only.

3. **Per‑protocol speed tuning**
   - Add per‑protocol buffer/parallel settings (e.g. `[SFTP]`, `[FTP]`, `[HTTP]`) instead of only WebDAV‑specific knobs.
   - For HTTP (non‑WebDAV), consider reusing `GetRangedParallel` logic for large downloads initiated via `BaseClient::Get`, with conservative defaults.

4. **More detailed progress & ETA**
   - Extend `windows.cpp` to show:
     - Remaining bytes / estimated time for current file.
     - Per‑file summary after completion (“Downloaded X MiB in Y seconds at Z MiB/s”).
   - Make sure to keep per‑frame work extremely light to avoid UI hitches.

5. **Multi-file parallelism / “torrent-like” behaviour**
   - Current behaviour:
     - `Actions::DownloadFilesThread` walks the selected remote entries and downloads them **one at a time**.
     - The global progress bar and speed display in `windows.cpp` reflect the current file only.
   - A safe next step (not yet implemented):
     - Introduce a small download scheduler that keeps N files in flight at once (e.g. 2–3) and treats each file as its own job:
       - Each job would reuse the existing single-file pipeline (WebDAV parallel ranges, SFTP reads, etc.) in its own worker thread.
       - A central queue would hold pending jobs; when one finishes or fails, the scheduler starts the next.
     - UI changes:
       - Add a “Transfers” pane showing per-file status, current speed, and progress (e.g. a simple table rendered in `windows.cpp`).
       - Keep the existing global bar for the currently focused job, plus aggregated totals if useful.
     - Constraints:
       - Limit concurrent jobs aggressively (2–3) to avoid saturating Switch CPU, SD write bandwidth, or the remote server.
       - Coordinate per-job counters (bytes downloaded, total size, ETA) so UI remains meaningful; prefer per-job structs over reusing the global `bytes_transfered` / `bytes_to_download` directly.
   - Torrent-style swarming (many peers per file, piece selection, DHT, etc.) is **out of scope** for this project and would require a different protocol stack; the realistic goal here is “multiple WebDAV/SFTP/FTP downloads in parallel with per-file speeds”, not a full torrent client.

If you touch the WebDAV path again, keep the following invariants in mind:

- `WebDAVClient::Get` is the only place that decides between split vs non‑split, sequential vs parallel.
- Split base path must remain deterministic for a given `(outputfile, config)` to keep resume working.
- Never block the main/UI thread with network calls; everything heavy should stay in the worker threads started from `Actions::DownloadFilesThread`.
