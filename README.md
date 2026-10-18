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

Concept:

<img width="810" height="540" alt="PS2-MultiScreen-Network-Test1" src="[https://raw.githubusercontent.com/khant735/PS2-MultiScreen-Network-Test/refs/heads/main/17cc67c9-02e3-49d8-80e4-2e161e502b6f.png](https://raw.githubusercontent.com/khant735/PS2-MultiScreen-Network-Test/refs/heads/main/17cc67c9-02e3-49d8-80e4-2e161e502b6f.png)" />

<img width="810" height="540" alt="PS2-MultiScreen-Network-Test2" src="[https://raw.githubusercontent.com/khant735/PS2-MultiScreen-Network-Test/refs/heads/main/17cc67c9-02e3-49d8-80e4-2e161e502b6f.png](https://raw.githubusercontent.com/khant735/PS2-MultiScreen-Network-Test/refs/heads/main/7d8183a6-27a7-4170-a8cd-f65cd74c73d9.png))" />
