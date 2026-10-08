# PS2 Multi-Screen project handoff

Repository: https://github.com/khant735/PS2-MultiScreen-Network-Test

Current state: bootstrap only. Network, i.LINK and LAN support not yet implemented or tested.

Build: PS2DEV/PS2SDK/GSKit toolchain; compile with `make` and package with `python3 scripts/package.py --version 0.1.0`.

Filename convention: `PS2-MultiScreen-vVERSION-(DD-MM-YYYY)-[hh-mm-ssAM/PM]-XXXXXXXX.ELF` and matching `-SOURCE.zip` with independently computed CRC32 values.

Emulator: PCSX2 2.9.31 Linux AppImage; user-provided BIOS is not redistributable.

Layouts: 62 grids, 1–5 rows, max 32 screens; GT3 3x2 screen numbering top 5,4,6; bottom 2,1,3. Screen 1 standalone fallback after 10 seconds without peers.

Do not claim hardware or emulator validation until tested. Keep important builds as GitHub Releases/prereleases because Actions artifacts expire.
