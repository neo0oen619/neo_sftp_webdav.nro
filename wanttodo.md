## neo_sftp – download pipeline ideas

Goal: make large remote downloads faster and more reliable by fetching multiple chunks of a file concurrently, then stitching them back together on the Switch.

### High-level idea

- Keep the existing sequential download path as a safe fallback.
- Add an optional "parallel chunked download" mode:
  - Split a file into N chunks of size `chunk_size_mb` (configurable).
  - For each chunk, issue a ranged HTTP/SFTP/WebDAV request:
    - WebDAV/HTTP: use `Range: bytes=start-end`.
    - SFTP: use multiple read requests in flight if feasible.
  - Download several chunks at once (limited concurrency, e.g. 2–4 workers).
  - Write chunks into the final file at the correct offsets.

### Configuration sketch (INI)

- `[Global]`
  - `download_parallel_enabled` (bool, default `false`):
    - `false`: current sequential behaviour.
    - `true`: enable multi-chunk downloads where supported.
  - `download_parallel_workers` (int, default 2, clamp 1–4):
    - Number of concurrent chunk workers per file.
  - `download_chunk_mb` (int, default 8, clamp 1–32):
    - Target chunk size per request.

### Implementation notes (future work)

- WebDAV/HTTP:
  - Reuse the existing `HTTPClient` but add a "ranged GET" helper that:
    - Accepts `(path, offset, length, sink)` where `sink` writes into a preallocated buffer or directly into the file at an offset.
  - Manage workers via a small thread pool (or libcurl multi if we want pure event-driven).
  - Ensure:
    - No more than `download_parallel_workers` chunks in flight per file.
    - Per-connection timeouts and retry backoff (especially on Wi-Fi hiccups).

- Local file writes:
  - Open the destination file once, pre-size it to the full length (when size is known).
  - Use `fseek/ftello` (or equivalent) to write each chunk at its offset.
  - Guard writes with a mutex if multiple threads share the same `FILE*`, or give each worker its own descriptor.

- Progress reporting:
  - Keep a shared `bytes_downloaded` counter updated atomically as chunks complete.
  - UI can continue to read aggregate progress the same way it does today.

### Safety and constraints

- Switch has limited CPU/RAM; defaults must stay conservative:
  - Low default worker count.
  - Moderate chunk size to avoid huge allocations.
- Network stacks (VPN/WebDAV/proxies) may rate-limit or dislike too much parallelism:
  - Implementation should detect repeated failures and fall back to sequential mode.

This file is only a design/intent note for now; no behaviour changes are implemented yet.

