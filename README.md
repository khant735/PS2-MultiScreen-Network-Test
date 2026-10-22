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

## Compiling from source

The current application source and build system are maintained on the **`develop`** branch. The default `main` branch README describes the project, but for the current networking build you should compile **`develop`**, not assume both branches contain the same files.

### Option 1 — GitHub Actions (no local SDK installation)

1. Open the [GitHub Actions page](https://github.com/khant735/PS2-MultiScreen-Network-Test/actions/workflows/build.yml).
2. Select **PS2 development build** → **Run workflow** and choose **`develop`**.
3. Wait for the workflow to finish successfully. A green check means the workflow completed; it does not prove real-console or two-instance networking works.
4. Download the build artifact from the workflow run, or get the individually downloadable ELF and source ZIP from [Releases](https://github.com/khant735/PS2-MultiScreen-Network-Test/releases). Development releases are named `dev-develop-<run number>`.
5. Use the **`.ELF`** file to launch the app. The `-SOURCE.zip` contains source code; it is not the executable. Check `SHA256SUMS.txt` or `manifest.json` if you want to verify the downloaded files.

The workflow builds in the `ps2dev/ps2dev:latest` Docker image, clones and compiles GSKit, runs `make`, and packages the result. GitHub Actions artifacts have limited retention, so prefer Releases for long-term preservation.

### Option 2 — Compile locally (Linux or WSL2)

**Prerequisites:** Git, GNU Make, Python 3 with the `zoneinfo` module and Europe/London timezone data, a working PS2DEV/PS2SDK toolchain, and GSKit built for that toolchain. The build needs the EE compiler, IOP compiler, `bin2c`, PS2SDK headers/libraries and the PS2SDK `ps2dev9.irx`, `netman.irx` and `smap.irx` modules.

For a build environment matching GitHub Actions, use Linux with Docker:

```bash
git clone --branch develop https://github.com/khant735/PS2-MultiScreen-Network-Test.git
cd PS2-MultiScreen-Network-Test

docker run --rm -v "$PWD:/src" -w /src ps2dev/ps2dev:latest sh -ec '
  # Ensure make, git and GSKit build dependencies are installed
  # (the GitHub Actions workflow installs missing packages).
  export GSKIT=/tmp/gsKit
  git clone --depth 1 https://github.com/ps2dev/gsKit.git "$GSKIT"
  make -C "$GSKIT"
  make
'
```

**Important:** The short Docker example assumes the image already contains `git`, `make`, and the GSKit build dependencies. If it does not, use the package-install steps in [`.github/workflows/build.yml`](https://github.com/khant735/PS2-MultiScreen-Network-Test/blob/develop/.github/workflows/build.yml), which is the authoritative reproducible build recipe.

Alternatively, if PS2DEV and GSKit are already installed and configured in your Linux/WSL2 shell:

```bash
git clone --branch develop https://github.com/khant735/PS2-MultiScreen-Network-Test.git
cd PS2-MultiScreen-Network-Test
# Ensure PS2DEV, PS2SDK and GSKIT point to your installed toolchain.
make
```

The Makefile first builds the read-only IOP `macprobe.irx`, embeds it and the PS2SDK DEV9/NETMAN/SMAP modules, then links the EE executable. The un-packaged ELF is:

```text
build/PS2-MultiScreen.ELF
```

### Package a compiled build

After `make` succeeds, run:

```bash
python3 scripts/package.py --version 0.1.0
```

This writes to `dist/`:

- `PS2-MultiScreen-vVERSION-(DD-MM-YYYY)-[hh-mm-ssAM/PM]-XXXXXXXX.ELF`
- `PS2-MultiScreen-vVERSION-(DD-MM-YYYY)-[hh-mm-ssAM/PM]-XXXXXXXX-SOURCE.zip`
- `manifest.json` and `SHA256SUMS.txt`

Each `XXXXXXXX` is that file's own CRC32; the timestamp uses UK local time. To rebuild from scratch, run `make clean` followed by `make`. Do not commit or distribute proprietary PS2 BIOS files.

### Troubleshooting compilation

- **`PS2SDK` / `GSKIT` not found:** verify the toolchain environment variables and that GSKit has been compiled.
- **Missing `ps2dev9.irx`, `netman.irx` or `smap.irx`:** check the installed PS2SDK IOP module directory.
- **Missing GSKit headers or linker libraries:** build GSKit with the same PS2DEV toolchain used for the app.
- **`bin2c` or compiler not found:** use a correctly configured PS2DEV environment, or the GitHub Actions workflow.
- **Packaging fails:** first confirm that `build/PS2-MultiScreen.ELF` exists, and that Python's UK timezone data is available.
- **Windows:** use WSL2 or Docker with a Linux environment rather than trying to run the GNU/PS2DEV build from a plain Windows Command Prompt.

## How to use the app

### Getting started

1. Build the PS2 ELF with the PS2DEV/PS2SDK and GSKit toolchain, or use a suitable ELF build from the repository's [Actions](../../actions) or [Releases](../../releases) when available.
2. Run the ELF on a PS2 with a compatible homebrew ELF launcher, or in PCSX2. For Ethernet testing, enable the emulator's DEV9 network adapter or connect a supported PS2 Ethernet adapter to a network.
3. The program starts at the **main menu**. Network activity does **not** start automatically: select **Diagnostic Counters** to choose a profile and start LAN diagnostics.
4. To test communication, run the application on two consoles or emulator instances connected to the same reachable network. An assigned IP address alone does not mean a peer connection has succeeded.

### Current controller controls

These controls are taken from the current `develop` implementation in [`src/main.c`](./src/main.c). They describe implemented input handling rather than planned controls.

| Screen / context | Control | Action |
| --- | --- | --- |
| Main menu | **D-pad Up / Down** | Move between the five menu entries |
| Main menu | **Cross (X)** | Open the selected page |
| Any page | **Triangle** | Return to the main menu |
| Screen Layout | **D-pad Left / Right** | Cycle backward / forward through the 62 available layout presets; this is a preview, not an automatic physical display setup |
| Diagnostic Counters (page 1) | **D-pad Up / Down** | Change LAN profile while networking is stopped or cancelled and no worker is active |
| Diagnostic Counters (page 1) | **Cross (X)** | Start the selected LAN profile when stopped or cancelled |
| Diagnostic Counters (page 1) | **Circle (O)** | Request cancellation of the current LAN session |
| Diagnostic Counters (page 1) | **D-pad Right** | Open diagnostics page 2 (module and TCP initialization details) |
| Diagnostic Counters (page 2) | **D-pad Left** | Return to diagnostics page 1 |
| Input Diagnostics | **D-pad Left / Right** | Switch between Host, Clients 01–16 and Clients 17–31 |
| Network Adapter / About & Status | **Triangle** | Return to the main menu; these are primarily information pages |

**Note:** Input Diagnostics *displays* digital button states, analogue-stick values and pressure readings. The displayed buttons are diagnostic data, not additional menu shortcuts. There are no separately implemented Start, Select, shoulder-button or analogue-stick menu-navigation actions in the current main input dispatch.

### Main menu pages

| Page | What it shows |
| --- | --- |
| **Network Adapter** | BIOS module probe results, ROM information, active NETMAN adapter MAC, independently read DEV9 EEPROM MAC, and MAC comparison |
| **Screen Layout** | Preview of multi-screen arrangements and screen numbering |
| **Diagnostic Counters** | LAN initialization, IP and peer addresses, traffic counters, network profile and status; page 2 shows lower-level module and TCP diagnostics |
| **About / Status** | Additional connection details including TCP target, gateway, subnet mask, socket results and packet counters |
| **Input Diagnostics** | Local controller data and up to 31 client-controller status entries across three tabs |

### LAN profiles

Choose a profile with **Up / Down** on Diagnostic Counters page 1, then press **Cross** to start. The current build starts with **PS2 #1 / Host** selected.

| Profile | Behaviour |
| --- | --- |
| **DHCP** | Requests an address from a DHCP server |
| **PS2 #1 / Host** | Static `192.168.50.101/24` |
| **PS2 #2 / Client** | Static `192.168.50.102/24` |
| **AUTO DHCP / Link-local** | Tries DHCP, then attempts a provisional `169.254.x.x/16` fallback derived from the active interface MAC after a polling timeout |

For the two static profiles, use PS2 #1 on one instance and PS2 #2 on the other. The network must support communication between their addresses. **AUTO is experimental:** its link-local fallback currently lacks address-conflict detection, and successful peer communication has not been established merely by selecting it. The EEPROM MAC is diagnostic information, not a substitute for the active Ethernet interface's MAC.

### Connection status colours

On the LAN diagnostics screen, **Local IP** and **Peer** text changes colour based on the current connection state:

- **Red:** no active connection, or connection failed/cancelled.
- **Orange:** network initialization or connection/peer discovery is underway.
- **Green:** TCP connection established with a peer and recent peer traffic observed.

The application may show an IP address while still orange or red; an address assignment does not itself prove a successful connection. If no peer is detected, inspect the two consoles' network configuration, link status, subnet, and diagnostic counters.

### Testing notes

PCSX2 tests established that the DEV9 EEPROM MAC is readable when either its HDD or Ethernet emulation is enabled; NETMAN's adapter MAC was available when Ethernet emulation was enabled. The addresses matched across the tested configurations. These are **emulator observations**, not guarantees for every physical PS2 model. The MAC probes are read-only and do not modify the adapter EEPROM.

<h2><strong><ins>Concept:</ins></strong></h2>

<img width="810" height="540" alt="PS2-MultiScreen-Network-Test1" src="17cc67c9-02e3-49d8-80e4-2e161e502b6f.png" />

<img width="810" height="540" alt="PS2-MultiScreen-Network-Test2" src="7d8183a6-27a7-4170-a8cd-f65cd74c73d9.png" />

<h2><strong><ins>Screenshots:</ins></strong></h2>

Screenshots captured from the PS2 multi-screen network test interface.

### Main menu

![Main menu](./screen-00.png)

### Network adapter

![Network adapter](./screen-10.png)

### Diagnostic Counters

![Diagnostic Counters](./screen-12.png)

### About & Status

![About & Status](./screen-44.png)

### LAN diagnostics — page 1

![LAN diagnostics — page 1](./screen-48.png)

### LAN diagnostics — page 2

![LAN diagnostics — page 2](./screen-47.png)

### Input Diagnostics

#### Host

![Input Diagnostics — Host](./screen-50.png)

#### Clients Page 1 (01–16)

![Input Diagnostics — Clients 01–16](./input-diagnostics-clients-01-16.png)

#### Clients Page 2 (17–31)

![Input Diagnostics — Clients 17–31](./input-diagnostics-clients-17-31.png)

### Screen Layout

![Screen Layout preview](./screen-51.png)

## Credits, source references and acknowledgements

This is an independent homebrew diagnostic project built with open-source PS2 development tools. Credit is due to the developers who documented PS2 hardware interfaces and made their implementations available for study. The categories below distinguish **adapted implementation techniques**, **libraries/toolchain dependencies**, and **design or research inspiration**; they do not imply that whole applications were copied or that upstream developers endorse this project.

### Source-code techniques adapted for this project

| Project | Contribution and relevance |
| --- | --- |
| [BIOS Drain — F0bes](https://github.com/F0bes/biosdrain) | Reference for low-level IOP loadcore library/export-table lookup and DEV9 EEPROM access. The read-only `iop/macprobe/main.c` adapts the lookup/access approach, including a local export lookup rather than relying on a nonexistent PS2SDK `FindLibrary` API. |
| [PS2Ident — ps2homebrew](https://github.com/ps2homebrew/PS2Ident) | Reference for PS2 hardware identification and reading DEV9 Ethernet identity / EEPROM MAC information. Used alongside BIOS Drain to cross-check the MAC-probe approach. |

The project's `macprobe` is a separate, read-only IOP module with an EE-side SIF RPC client. It does **not** write EEPROM or Mechacon NVRAM. The DEV9 EEPROM MAC and the active NETMAN interface MAC are deliberately reported separately.

### Toolchain and runtime dependencies

| Project | Role |
| --- | --- |
| [PS2DEV / PS2SDK](https://github.com/ps2dev/ps2sdk) | PlayStation 2 EE/IOP development interfaces, module loading, SIF RPC, controller support, DEV9 / NETMAN / SMAP network modules and PS2IP sockets and configuration. The build embeds PS2SDK-supplied IRX modules. |
| [GSKit](https://github.com/ps2dev/gsKit) | Graphics rendering and on-screen diagnostic interface. |
| [PCSX2](https://github.com/PCSX2/pcsx2) | Emulator used for development and testing of DEV9, HDD, Ethernet, networking and controller input. PCSX2 behavior is not necessarily identical to every physical PS2 revision. |

Please refer to the upstream repositories for their authors, contributors, copyright notices and license conditions. Acknowledging these dependencies does not replace compliance with the licenses of any redistributed source or binary components.

### Design inspiration and additional engineering references

| Resource | Inspiration or research context |
| --- | --- |
| **Gran Turismo 3: A-Spec** (Polyphony Digital / Sony) | PS2 i.LINK multi-screen concepts and the six-display screen-numbering arrangement used as a visual reference. |
| **Gran Turismo 4** (Polyphony Digital / Sony) | Ethernet LAN and multi-monitor concepts that inspired networked display arrangements. |
| [Open PS2 Loader](https://github.com/ps2homebrew/Open-PS2-Loader) | Reference project studied for practical PS2 networking and DEV9/SMAP architecture. |
| [PS2Link](https://github.com/ps2dev/ps2link) | Reference project for homebrew communication with a PS2 over Ethernet. |
| [HDLGameInstaller](https://github.com/ps2homebrew/HDLGameInstaller) | Reference project investigated for PS2 Ethernet / network-adapter initialization and related IOP module interactions. |
| [PCSX2 documentation and source](https://github.com/PCSX2/pcsx2) | Emulator behavior, configuration, and diagnostic cross-checking during development. |

The game titles above are credited **for inspiration only**: no proprietary Gran Turismo source, assets, BIOS files or game code are included or claimed as dependencies.

### How the sources are used

- **Directly adapted technique:** the IOP-side DEV9 EEPROM export lookup and read path, informed by BIOS Drain and PS2Ident. See [`iop/macprobe/main.c`](./iop/macprobe/main.c).
- **Library/API usage:** the EE program uses PS2SDK, PS2IP, NETMAN, SMAP and GSKit interfaces. See [`src/main.c`](./src/main.c) and [`Makefile`](./Makefile).
- **Research and conceptual inspiration:** other PS2 homebrew applications, emulator documentation and commercial multi-screen games helped inform testing and layout choices; listing them does not imply verbatim copied code.

If any upstream attribution or licensing notice is incomplete, please open an issue with the specific file and original source so it can be corrected.
