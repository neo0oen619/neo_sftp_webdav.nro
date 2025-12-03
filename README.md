# neo_sftp_webdav.nro

neo_sftp – screaming at WebDAV so you don’t have to

> A Nintendo Switch file manager that speaks WebDAV, SFTP, FTP & SMB — with 1.1.1 speed boosts, split‑file wizardry, and enough sarcasm to make your SD card blush.

## Why Does This Exist?

Because *switch-ezremote-client* was a bit of a drama queen. It stalled, crashed and generally made transferring games feel like waiting for Animal Crossing load screens. This fork rewired the guts and turned it into **neo_sftp** — a two-panel file manager that can slurp games off WebDAV (`webdavs://` via Tailscale + Funnel + SFTPGo) as well as SFTP/FTP/SMB. Now it just grumbles and goes faster.

Think of it as Cyberduck/FileZilla, but trapped in a handheld console and fuelled by your impatience.

---

## What we actually fixed (besides our sanity)

### WebDAV over `webdavs://`

- Fixed crashes from servers doing weird things with chunked transfers / keep-alives.
- Relaxed TLS checks so Funnel/SFTPGo cert chains stop throwing tantrums.
- Fixed path handling so WebDAV hrefs like `F:/2TB/...` and `/games/...` actually match what the UI shows.
- Switched to ranged downloads so long HTTPS streams don’t die mid‑transfer.

### Large files & DBI‑style splits

- NSPs larger than 4 GiB are saved in DBI‑style split folders (`<name>.nsp/00`, `01`, …) so they work on FAT32 and install fine.
- After a split download completes, the `<name>.nsp` folder is marked as a **concatenation file** via libnx (`fsdevSetConcatenationFileAttribute`):
  - Horizon OS, DBI and Tinfoil see that folder as a **single logical NSP**, with `00`, `01`, … concatenated under the hood.
  - This effectively matches DBI’s own “archived folder” trick when you drop >4 GiB files via MTP.
- On exFAT you can turn splitting off with `webdav_split_large=0` to get one “normal” NSP instead of parts.
- On FAT32 you should keep splitting enabled (`webdav_split_large=1` or `force_fat32=1`) so you don’t slam into the 4 GiB limit.

### Speed hacks (a bit cursed, but effective)

- Chunked downloads via HTTP `Range`; `webdav_chunk_mb` controls chunk size (1–32 MiB).
- Optional **parallel range workers per file** via `webdav_parallel` (1–32), with a 256 MiB in‑flight cap to avoid out‑of‑memory fun.
- Parallel mode works for both single‑file and split downloads.
- Multi‑file scheduling for WebDAV:
  - `download_parallel_files` (1–3) keeps several WebDAV files in flight at once.
  - Each file still uses its own `webdav_parallel` workers, so total connections ≈ `download_parallel_files × webdav_parallel`.
- Tuned libcurl (HTTP/2 preferred, bigger buffers, `TCP_NODELAY`, keep‑alives).
- CPU boost + Wi‑Fi priority on Switch so your downloads get VIP treatment while your battery quietly plots revenge.
- Auto‑sleep is disabled while the app runs so long transfers don’t get murdered by the system sleep timer.

### Safer failures & resume

- WebDAV range workers retry transient errors (disconnects, DNS issues) a few times per chunk.
- If a WebDAV download fails on the main connection, the app now auto‑retries the whole file up to **6 times**, spreading about **20 seconds of total backoff** across those attempts before it even shows a popup.
- Once automatic retries are exhausted, you get a **Confirm** dialog with the real error and can choose to retry/resume instead of the app silently giving up.
- Partial data is kept, so re‑starting the same file will resume instead of starting over from 0%.

### More reliable long transfers

- The app bumps CPU and Wi‑Fi priority while running and disables auto‑sleep so long downloads aren’t interrupted.
- HTTP/curl settings are tuned for better throughput over VPN / reverse proxy setups.

### Docs & config cleanup

- README, `webdav_downloads.md` and `wanttodo.md` describe:
  - The WebDAV pipeline,
  - Split vs full NSP behaviour,
  - FAT32 vs exFAT limits,
  - New settings and safe tuning ranges.
- `config.example.ini` was updated with:
  - `webdav_chunk_mb`, `webdav_parallel`,
  - `download_parallel_files`,
  - `webdav_split_large`, `force_fat32`,
  - and comments for each.

---

## What Does It Actually Do?

- Presents a split-view UI: **left panel** is your Switch’s SD card; **right panel** is the remote (WebDAV/SFTP/FTP/SMB).
- Lets you navigate with D‑Pad/sticks and perform actions with A/X/Y/etc., similar to the original EZ Remote client.
- Supports logging to `/switch/neo_sftp/log.txt` if you enjoy CSI: Download Edition.
- Resumes interrupted downloads and provides a friendly popup when automatic retries run out.
- Keeps partial files so you can restart later instead of screaming into the void.
- Splits large NSPs, sanitizes folder names, and marks split folders as concatenation files so DBI/Tinfoil can install them on FAT32.
- Can download multiple WebDAV files at once while still handling per‑file parallel ranges under the hood.
- Uses SFTP as a “calmer, faster” default where possible; WebDAV is there for Tailscale/Funnel setups and when paths matter.

---

## How to Use It (Switch)

1. **Download the latest build.**  
   Grab `neo_sftp.nro` from your build or release.

2. **Drop it onto your Switch.**  
   Copy the `.nro` to `/switch/neo_sftp` on your SD card.

3. **Configure it (optional but recommended).**  
   Copy `config.example.ini` to `/switch/neo_sftp/config.ini` and tweak:

   - Core WebDAV tuning:
     - `webdav_chunk_mb=8` — chunk size in MiB (1–32).
     - `webdav_parallel=8` — per‑file workers (1–32, capped by memory).
     - `download_parallel_files=2` — number of WebDAV files in flight (1–3).
   - Split behaviour:
     - `webdav_split_large=0` — keep big files as single NSP (good for exFAT + Tinfoil).
     - `force_fat32=1` or `webdav_split_large=1` — always split big files into `<name>.nsp/00, 01, …` (required on FAT32; works with DBI/Tinfoil thanks to concatenation files).
   - Logging:
     - `logging_enabled=1` — turn it off if you don’t want log spam.
   - Remote sites:
     - `[Site N]` sections for servers (`webdavs://unk1server.../dav`, `sftp://host:port`), username & password.

4. **Run it.**  
   Launch from the Homebrew Menu. Use the left/right panels to browse, press A to copy/move, and watch your downloads fly (or crawl, depending on your Wi‑Fi and VPN sins).

5. **Pro tips.**
   - If speed matters more than remote protocol, prefer **SFTP**.
   - Use WebDAV when Tailscale/Funnel is the only practical way to reach your storage.
   - On FAT32, leave splitting on for >4 GiB; on exFAT, turn it off if you want “normal” NSPs.

---

## Building from Source (for Masochists)

- You need devkitPro with `devkitA64` and the Switch toolchain.
- Install Switch libraries via `pacman` (names may vary):  
  `switch-libnx`, `switch-curl`, `switch-mbedtls`, `switch-libarchive`,  
  `switch-zlib`, `switch-libjpeg-turbo`, `switch-libpng`, `switch-webp`,  
  `switch-freetype`, `switch-glad`, etc.

Build:

```bash
mkdir -p build
cd build
/opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake ..
make -j4
```

- The `.nro` will land in `build/neo_sftp.nro`. You can also keep a copy there as your “prebuilt” for quick testing.

---

## Configuration & Usage (a.k.a. “how not to brick your downloads”)

Config file path on SD (runtime):  
`/switch/neo_sftp/config.ini`

Key settings:

- `[Global]`
  - `last_site=Site 3` — which site auto‑connects by default on a fresh config.
  - `webdav_chunk_mb=8` — WebDAV range chunk size in MiB (1–32).  
    Bigger = fewer requests, more RAM, more “hold my beer”.
  - `webdav_parallel=12` — parallel WebDAV workers per file (1–32, effectively clamped by a 256 MiB in‑flight window).  
    More = more connections, more speed *until* your server/tunnel says “nope”.
  - `download_parallel_files=2` — how many WebDAV files to download at once (1–3).  
    Each file still uses its own `webdav_parallel` workers, so total connections ≈ `download_parallel_files * webdav_parallel`.
  - `webdav_split_large=1` — `1` = split big files into `<name>.nsp/00, 01, …` (safe on FAT32 and works with DBI/Tinfoil); set to `0` on pure exFAT setups if you prefer single full NSPs.
  - `force_fat32=0` — when set to `1`, always treat SD as FAT32 for large downloads and force split layout even for smaller files.
  - `logging_enabled=1` — toggle log spam in `/switch/neo_sftp/log.txt`.

- `[Site N]`
  - `remote_server=` — e.g. `webdavs://unk1server.../dav` or `sftp://host:port`.
  - `remote_server_user=`, `remote_server_password=` — basic auth creds.

UI basics:

- Left panel = local SD, right panel = remote.
- Use D‑Pad / sticks to navigate, A to select, X/Y/etc. for actions.
- Enable logging via `logging_enabled` if you enjoy reading your download history like a slow-motion crime scene.

Pro tip: For raw speed, SFTP is usually faster and calmer than WebDAV over Tailscale/Funnel. WebDAV is here for when paths matter and you like pain.

---

## Contributing / Hacking on It

- WebDAV logic lives in `source/clients/webdav.cpp`.
- HTTP client / curl setup is in `source/httpclient/HTTPClient.{h,cpp}`.
- Global transfer stats (`bytes_transfered`, `bytes_to_download`) are in `source/windows.cpp`.

If you touch WebDAV:

- Reset curl options before each request (method, callbacks, uploads).
- Don’t point curl callbacks at stack memory unless you enjoy random crashes.
- Treat `webdav_parallel` and `webdav_chunk_mb` as loaded guns: powerful, but pointed squarely at your own performance.
- Respect the concatenation file / split logic so FAT32 + big games keep working.

If you add something clever and it doesn’t explode, please update this README so future you knows which demon you already exorcised.

---

## Dark-ish FAQ

**Q: Why is my download still “only” ~1 MB/s?**  
A: Because Switch Wi‑Fi + VPN + reverse proxy + WebDAV. This app can’t turn your network into fiber any more than a new pair of shoes makes you a car.

**Q: Can we hit 10 MB/s?**  
A: Maybe in the next life, where the Switch has a 10 G NIC and your ISP isn’t throttling you harder than your boss throttles your deadlines.

**Q: Is it safe?**  
A: As safe as multithreaded C++ on an embedded console can be. So… let’s call it “exciting”. Backups are cheaper than therapy.

**Q: It crashed, what do I do?**  
A: Check the logs, tweak `webdav_parallel` & `webdav_chunk_mb`, try again. If it still blows up, open an issue with what you did and which demon summoned the segfault. Bonus points for memes.
