## Agent Instructions for neo_sftp

Scope: this file applies to the entire repository.

### Build & output naming

- Always keep the primary CMake target named `neo_sftp` (see `CMakeLists.txt`).
- The canonical build outputs must be:
  - `build/neo_sftp.elf`
  - `build/neo_sftp.nacp`
  - `build/neo_sftp.nro`
- Do not rename the target or NRO back to `ezremote-client`; external tooling and docs assume `neo_sftp.nro`.
- When updating build steps in docs or scripts, prefer the devkitPro helper:
  - Configure: `/opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -S . -B build`
  - Build: `/opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake --build build`

### Repo hygiene

- Avoid introducing additional build systems; keep CMake + devkitPro as the source of truth.
- Do not check in local build artefacts other than the canonical NRO/ELF/NACP used for releases.

### Future work hints

- There is a design note in `wanttodo.md` about improving download speed via concurrent, chunked transfers.
- If you implement that work:
  - Prefer using libcurl’s multi interface or carefully bounded worker threads.
  - Keep configuration surface small and driven by INI options rather than hard-coded constants.

