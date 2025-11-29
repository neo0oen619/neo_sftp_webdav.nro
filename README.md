## neo_sftp – screaming at WebDAV so you don’t have to

This is a fork of **switch-ezremote-client**, rewired into **neo_sftp**: a two‑panel file manager for Nintendo Switch that talks SFTP, FTP, SMB and *especially* WebDAV over `webdavs://` (Tailscale + Funnel + SFTPGo).  
We took the part that used to crash, stall and cry, and turned it into something that mostly just grumbles and goes faster.

Think of it as Cyberduck/FileZilla, but trapped in a tiny plastic console and motivated purely by your disappointment.

---

### What we actually fixed (besides our sanity)

- **WebDAV over `webdavs://`**:
  - Fixed crashes from reusing a single `CURL*` with stale callbacks.
  - Relaxed TLS checks so Funnel/SFTPGo cert chains stop throwing tantrums.
  - Fixed path handling so SFTPGo’s `/F:2TB/...` hrefs actually match what the UI shows.
  - Switched to ranged downloads so long HTTPS streams don’t die mid‑transfer.
- **Speed hacks (a bit cursed, but effective)**:
  - Chunked downloads via HTTP `Range` (config: `webdav_chunk_mb`, default 8, clamped 1–16).
  - Optional **parallel range workers** (config: `webdav_parallel`, default 2, clamped 1–4).
  - Tuned libcurl (HTTP/2 preferred, bigger buffers, TCP_NODELAY, keep‑alives).
  - CPU boost + Wi‑Fi priority on Switch so your downloads get VIP treatment while your battery quietly plots revenge.

It’s still limited by Switch Wi‑Fi + VPN + reverse proxy + your ISP. We’re fast *for that*, not for physics.

---

### Building (short version, no ritual sacrifice required)

Prereqs:
- devkitPro with `devkitA64` and Switch toolchain.
- Switch libs via `pacman` (names may vary slightly by setup), e.g.:
  - `switch-libnx`, `switch-curl`, `switch-mbedtls`, `switch-libarchive`,
    `switch-zlib`, `switch-libjpeg-turbo`, `switch-libpng`, `switch-webp`,
    `switch-freetype`, `switch-glad`, etc.

Build:
```bash
mkdir -p build
cd build
/opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake ..
make -j4
```

You’ll get `ezremote-client.nro` in `build/` – rename or ship it as you like  
(we use `neo_sftp.nro` in our own builds, but the Switch doesn’t judge names, only crashes).

If you just want to run it right now, there’s also a prebuilt `neo_sftp.nro` in `build/neo_sftp.nro`.

---

### Configuration & usage (aka “how not to brick your downloads”)

Config file path on SD (runtime):  
`/switch/ezremote-client/config.ini`

Key bits:
- `[Global]`
  - `last_site=Site 1` – which site to auto‑connect.
  - `webdav_chunk_mb=8` – WebDAV range chunk size in MiB (1–16).  
    Bigger = fewer requests, more RAM, more “hold my beer”.
  - `webdav_parallel=2` – parallel WebDAV workers (1–4).  
    More = more connections, more speed *until* your network says “nope”.
- `[Site N]`
  - `remote_server=` – e.g. `webdavs://unk1server.../dav` or `sftp://host:port`.
  - `remote_server_user=`, `remote_server_password=` – basic auth creds.

UI basics:
- Left panel = local SD, right panel = remote.
- Use d‑pad / sticks to navigate, A to select, X/Y/etc. for actions (similar to the original EZ Remote client).
- Enable logging via `LOG_DIR` (`/switch/neo_sftp/log.txt`) if you enjoy reading your download history like a slow-motion crime scene.

Pro tip: For raw speed, SFTP is usually faster and calmer than WebDAV over Tailscale/Funnel. WebDAV is here for when paths matter and you like pain.

---

### Contributing / hacking on it

- WebDAV logic lives in `source/clients/webdav.cpp`.
- HTTP client / curl setup is in `source/httpclient/HTTPClient.{h,cpp}`.
- Global transfer stats (`bytes_transfered`, `bytes_to_download`) are in `source/windows.cpp`.

If you touch WebDAV:
- Reset curl options before each request (method, callbacks, uploads).
- Don’t point curl callbacks at stack memory unless you enjoy random crashes.
- Treat `webdav_parallel` and `webdav_chunk_mb` as loaded guns: powerful, but pointed squarely at your own performance.

If you add something clever and it doesn’t explode, please update this README so future you knows which demon you already exorcised.

---

### Dark-ish FAQ

**Q: Why is my download still “only” ~1 MB/s?**  
A: Because Switch Wi‑Fi + VPN + reverse proxy + WebDAV. This app can’t turn your network into fiber any more than a new pair of shoes makes you a car.

**Q: Can we hit 10 MB/s?**  
A: Maybe in the next life, where the Switch has a 10G NIC and your ISP isn’t throttling you harder than your boss throttles your deadlines.

**Q: Is it safe?**  
A: As safe as multithreaded C++ on an embedded console can be. So… let’s call it “exciting”. Backups are cheaper than therapy.
