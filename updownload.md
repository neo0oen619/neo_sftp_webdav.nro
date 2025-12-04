# neo_sftp – upload/download speed notes

This file is meant as a performance notebook: what limits throughput on the Switch, what neo_sftp already does to go fast, and where there is still headroom if your network is faster than what the app is currently using.

The goal is to get as close as possible to **“max out Switch Wi‑Fi + SD card”** without turning the console into a space heater or making the UI unusable.

---

## 1. Hard limits to keep in mind

- **Switch Wi‑Fi**  
  - Real‑world sustained throughput on 5 GHz is typically **30–80 Mbit/s** (≈ 4–10 MiB/s). Exceptional setups can push higher but you will almost never see 250 Mbit/s on the console itself, even if your PC ↔ server link can do that.
  - USB‑C / MTP (like DBI) can go faster because it skips Wi‑Fi entirely.

- **SD card writes**
  - Many microSD cards can write >50 MiB/s in a PC, but on Switch through libnx you often top out **well below that**.
  - Every write is also encrypted by the OS, so CPU becomes part of the cost.

- **CPU + TLS**
  - WebDAV over HTTPS means TLS + HTTP parsing + splitting/merging chunks in userland.
  - Pushing lots of parallel range requests (12–16) and writing to SD at the same time will peg at least one CPU core.

**Bottom line:** if you’re already seeing ~6–8 MiB/s sustained over WebDAV, you are in the “realistic” range for Switch Wi‑Fi. We can often squeeze a bit more, but we won’t hit your 250 Mbit/s PC uplink on the console.

---

## 2. What neo_sftp already does for speed

High‑level pipeline (for WebDAV downloads):

- Uses **HTTP `Range`** to download big files in pieces instead of one long stream.
- Supports **parallel range workers per file**:
  - `[Global] webdav_parallel` controls workers (1–32 in config, clamped to 1–16 in code for safety).
  - Each worker holds a separate HTTP connection and TLS session.
- Uses a **tunable chunk size**:
  - `[Global] webdav_chunk_mb` (1–32 MiB) controls how big each range is.
  - There’s a safety cap: `webdav_chunk_mb * webdav_parallel` ≤ **256 MiB** in flight per file. If you go too high, we shrink chunks automatically.
- Has a **multi‑file scheduler**:
  - `[Global] download_parallel_files` (1–3) controls how many WebDAV files download at once.
  - Each file still uses its own `webdav_parallel` range workers, so total HTTP connections is roughly:
    - `download_parallel_files × webdav_parallel`
- WebDAV client (`HTTPClient.cpp`) is tuned for high‑latency links:
  - HTTP/2 preferred, `TCP_NODELAY`, 1 MiB `CURLOPT_BUFFERSIZE`, keep‑alives, no low‑speed aborts.
- On the system side:
  - CPU boost + Wi‑Fi priority while the app runs.
  - Auto‑sleep disabled so long transfers don’t get killed.

All of this is already why you can see ~6–7 MiB/s on large NSPs instead of ~2–3 MiB/s from a single sequential HTTP stream.

---

## 3. Current default tuning vs “aggressive” tuning

**Defaults baked into 1.1.1:**

```ini
[Global]
webdav_chunk_mb=8
webdav_parallel=12
download_parallel_files=2
webdav_split_large=1
force_fat32=0
logging_enabled=1
```

This is chosen as a **safe sweet spot**:

- In‑flight window per file: `8 MiB × 12 ≈ 96 MiB` (well below 256 MiB cap).
- Two files at once means up to 24 concurrent HTTP connections, which most WebDAV servers and VPN tunnels can still handle.
- Enough parallelism to hide latency but not so much that the Switch spends all of its time context‑switching or thrashing the SD card.

### Aggressive profile for fast, stable networks

If your WebDAV server and network are solid (no frequent timeouts) and you want to chase every extra MiB/s, you can try:

```ini
[Global]
webdav_chunk_mb=16
webdav_parallel=16
download_parallel_files=2
```

Why this matters:

- `16 MiB × 16 = 256 MiB` in‑flight per file – this hits the internal window cap, so each worker gets a big chunk and you reduce HTTP overhead.
- Two files at once gives you up to **32 concurrent HTTP connections**. That’s a lot, so:
  - Watch your server CPU and memory.
  - Watch for more “Couldn’t connect to server” or timeout errors; if they spike, dial the numbers back.

If this is unstable, back off in this order:

1. Drop `download_parallel_files` from 2 → 1 (keep multi‑chunk, use fewer simultaneous files).
2. Drop `webdav_parallel` (e.g. 16 → 12 → 8).
3. Only then shrink `webdav_chunk_mb` (16 → 12 → 8).

---

## 4. Why two simultaneous downloads can be faster than one

You noticed that downloading **two files at once** seems to use more of your 250 Mbit/s uplink than a single file. That’s expected:

- Every HTTP request has **latency overhead**: TLS handshake, headers, waiting for the first byte, etc.
- Even with `webdav_parallel`, one file may not have enough in‑flight requests to *fully* fill the pipe, especially if your:
  - WebDAV server limits connection pool or per‑connection throughput.
  - VPN / tunnel has its own per‑flow shaping.
- When you add a second file, you effectively add another set of flows, which can:
  - Occupy otherwise idle bandwidth on the route.
  - Hide jitter on one connection with progress on the other.

In neo_sftp that is controlled by `[Global] download_parallel_files`. Setting it to `2` (or `3` if your server is really strong) is how you tell the app to do this on purpose.

---

## 5. Things that would need deeper code changes

These are ideas that could squeeze more speed but require non‑trivial rework and a lot of testing on real hardware.

1. **Dynamic auto‑tuning per connection**
   - Measure effective throughput for the first N chunks of a big file and automatically adjust:
     - `webdav_chunk_mb` (larger on low‑latency, smaller on flaky links).
     - `webdav_parallel` up/down to keep a target in‑flight window (e.g. always aim for ~128–256 MiB).
   - Needs care to avoid oscillation and to respect Switch memory constraints.

2. **libcurl multi interface instead of many easy handles**
   - Today we run several independent `CHTTPClient` instances in multiple threads.
   - A libcurl multi‑handle could drive many parallel ranges in a single thread with better connection reuse and less thread overhead.
   - Trade‑off: more complex state machine, harder debugging on Switch.

3. **Async SD writes / larger write buffers**
   - Right now each worker takes the HTTP body and writes it to SD in the same thread.
   - A background writer (e.g. a queue that batches 1–4 MiB writes) could smooth out latency spikes in SD I/O.
   - Needs careful backpressure so we don’t buffer hundreds of MiB in RAM.

4. **Protocol‑level changes (beyond WebDAV)**
   - SFTP/FTP pipelines could get similar range/parallel treatment, but each protocol has its own quirks.
   - For ultimate speed, a dedicated USB/MTP pipeline (like DBI) would bypass Wi‑Fi entirely, but that’s effectively a different app.

None of this is coded yet; this file is intentionally documenting where the next 10–20 % might come from if we decide it’s worth the complexity.

---

## 6. Practical tuning checklist for your setup

If your PC ↔ server can do ~250 Mbit/s and you want neo_sftp as fast as possible:

1. **Network side**
   - Use **5 GHz Wi‑Fi** on the Switch, close to the access point.
   - Keep the Switch in the dock or a stand where antennas aren’t blocked.
   - Avoid heavy traffic on the same Wi‑Fi channel during big transfers.

2. **neo_sftp config**
   - Start with:
     ```ini
     [Global]
     webdav_split_large=1
     webdav_chunk_mb=8
     webdav_parallel=12
     download_parallel_files=2
     ```
   - If stable, try the **aggressive profile**:
     ```ini
     [Global]
     webdav_split_large=1
     webdav_chunk_mb=16
     webdav_parallel=16
     download_parallel_files=2
     ```
   - Watch the log (`/switch/neo_sftp/log.txt`) for:
     - `WEBDAV GET range error` / `Couldn't connect to server` spikes → parallelism too high.
     - `WEBDAV GET parallel write failed ... written=0` at ~4 GiB on FAT32 → need split (`webdav_split_large=1` or `force_fat32=1`).

3. **Behaviour expectations**
   - If you see sustained **8–10 MiB/s** on large files, you’re already near the practical ceiling for Switch Wi‑Fi + SD.
   - If single‑file downloads plateau at ~5–6 MiB/s but **two files** together push ~10 MiB/s combined, that’s normal and exactly what `download_parallel_files` is for.

If you capture logs showing long, stable runs that are still far below what we’d expect (e.g. <2 MiB/s with aggressive settings and a clean Wi‑Fi environment), that’s a good signal to revisit the pipeline and consider some of the deeper changes in section 5.

