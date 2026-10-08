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
