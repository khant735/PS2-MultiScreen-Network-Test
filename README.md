# PS2 Multi-Screen Network Test

Homebrew PlayStation 2 project for exploring multi-screen arrangements and network diagnostics inspired by Gran Turismo 3 i.LINK and Gran Turismo 4 LAN.

## Build and preservation policy

- Stable source lives on `main`; integration on `develop`; experiments on `feature/*`.
- Each build produces separate `.ELF` and `-SOURCE.zip` files, with individual CRC32 checksums.
- Filenames: `PS2-MultiScreen-vVERSION-(DD-MM-YYYY)-[hh-mm-ssAM/PM]-XXXXXXXX.ELF` and `PS2-MultiScreen-vVERSION-(DD-MM-YYYY)-[hh-mm-ssAM/PM]-XXXXXXXX-SOURCE.zip`.
- Timestamp uses Europe/London local time. SHA-256 checksums are also generated.
- GitHub Actions artifacts expire; keep important binaries as GitHub Releases/prereleases.
- BIOS and proprietary files must never be committed.

**Status:** repository initialization; no tested network diagnostic ELF has been published yet.

<h2><strong><ins>Concept:</ins></strong></h2>

<img width="810" height="540" alt="PS2-MultiScreen-Network-Test1" src="17cc67c9-02e3-49d8-80e4-2e161e502b6f.png" />

<img width="810" height="540" alt="PS2-MultiScreen-Network-Test2" src="7d8183a6-27a7-4170-a8cd-f65cd74c73d9.png" />

<h2><strong><ins>Screenshots:</ins></strong></h2>

Screenshots captured from the PS2 multi-screen network test interface.

### Main menu

![Main menu](./screen-00.png)

### Network adapter

![Network adapter](./screen-10.png)

### Diagnostics and counters

![Diagnostics and counters](./screen-12.png)

### About and status

![About and status](./screen-44.png)

### LAN diagnostics — page 1

![LAN diagnostics — page 1](./screen-47.png)

### LAN diagnostics — page 2

![LAN diagnostics — page 2](./screen-48.png)

### Input Diagnostics layout preview

![Input Diagnostics layout preview](./screen-50.png)

### Input Diagnostics layout preview

![Input Diagnostics layout preview](./screen-51.png)
