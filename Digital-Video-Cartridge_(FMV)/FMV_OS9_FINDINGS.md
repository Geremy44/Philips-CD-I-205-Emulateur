# Philips CD-i — Digital Video Cartridge (FMV) — Reverse Engineering Findings

## Overview

- **Target:** Philips CD-i FMV (Full Motion Video / MPEG) cartridge ROM, used with CD-i 200/205 players.
- **Files analyzed:**
  - `FMV_combined_7308hi_7307lo.bin` (131072 bytes / 128 KB) — the FMV cartridge ROM (deinterleaved hi/lo).
  - `cdi200.rom` (524288 bytes / 512 KB) — the CD-i 205 host player firmware (reference).
- **Architecture confirmed:** Motorola 68000 + CD-RTOS (OS-9 / OS-9000).

## Key Confirmations

- The `0x4AFC` markers ARE OS-9 module sync IDs (`M$ID`), not inter-function guards.
- Evidence: OS-9 kernel strings found in `cdi200.rom`:
  - `"OS-9 Boot failed; can't find 'init'"`
  - `"WARNING - kernel has bad CRC"`
  - `"MPU incompatible with OS-9 kernel"`
  - `"Bad pseudo-vector table"`
  - `"can't open console terminal"`
  - `"OS9P2 module aborted"`
- **73 valid OS-9 modules** parsed in `cdi200.rom`; **15 valid OS-9 modules** parsed in the FMV ROM.

## OS-9 Module Header Format (68000)

| Offset | Size | Field           | Notes                                              |
|--------|------|-----------------|----------------------------------------------------|
| +0x00  | 2    | `M$ID`          | Sync word = `0x4AFC`                               |
| +0x02  | 2    | `M$SysRev`      | System revision level                              |
| +0x04  | 4    | `M$Size`        | Module total size (32-bit)                         |
| +0x08  | 4    | `M$Owner`       | Owner ID                                           |
| +0x0C  | 4    | `M$Name`        | Offset to module name string (32-bit)              |
| +0x10  | 2    | `M$Accs`        | Access permissions                                 |
| +0x12  | 2    | `M$Type/Lang`   | Module type + language                             |
| +0x16  | 4    | `M$Exec`        | Entry point offset                                 |
| +0x18  | 3    | `M$HdrChk`      | 24-bit header parity (XOR of header words)         |

**Notes:**
- Header parity = XOR of header words yields even parity (bit7=0).
- Module body CRC = OS-9 24-bit polynomial `0x800063`.
- Module name is a null-terminated ASCII string at `header_base + M$Name`.
- Modules are **all packed together** in ROM with no alignment padding — scan sequentially for `0x4AFC`.

## FMV Cartridge Modules (15)

| Offset   | Name          | Type        | Role                                                          |
|----------|---------------|-------------|---------------------------------------------------------------|
| 0x000000 | `sysgo`       | prog 0x01   | Boot/init of the card                                         |
| 0x0001E6 | `csd_fmvvm`   | csd 0x05    | Configuration Status Descriptor — declares the card to system |
| 0x000276 | `fmvconf`     | prog 0x01   | FMV configuration                                             |
| 0x001E82 | `vmpeg`       | data 0x04   | MPEG video decoder data/firmware                              |
| 0x002D82 | `vcd`         | prog 0x01   | Video CD handler                                              |
| 0x0042E4 | `fmvll`       | prog 0x01   | FMV Low-Level (hardware/MMIO routines)                        |
| 0x009CE6 | `dspcode`     | data 0x04   | Audio DSP microcode (MPEG audio)                              |
| 0x00DD66 | `MoviMan`     | sysmgr 0x0D | Movie Manager — playback manager                              |
| 0x00FBB8 | `ma`          | descr 0x0F  | Device descriptor "ma" (MPEG audio)                           |
| 0x00FC20 | `madriv`      | driver 0x0E | MPEG audio driver                                             |
| 0x011A14 | `mv`          | descr 0x0F  | Device descriptor "mv" (MPEG video)                           |
| 0x011A8C | `fmvdrv`      | driver 0x0E | Main FMV driver (core MPEG decoder control — MMIO registers)  |
| 0x014C60 | `fmvvolset`   | subr 0x02   | Volume setting                                                |
| 0x014D3A | `ramtest4`    | prog 0x01   | Card RAM test                                                 |
| 0x0157B6 | `dummy`       | data 0x04   | Padding                                                       |

## Key Modules for Emulation

| Priority | Module        | Offset   | Size     | Reason                                                    |
|----------|---------------|----------|----------|-----------------------------------------------------------|
| **1st**  | `csd_fmvvm`   | 0x0001E6 | ~144 B   | First to emulate — the system DETECTS the card via this   |
| **2nd**  | `fmvdrv`      | 0x011A8C | ~12 KB   | Core MPEG decoder control — **disassemble for MMIO map**  |
| **3rd**  | `fmvll`       | 0x0042E4 | ~23 KB   | Low-level hardware communication (MMIO routines)          |
| **4th**  | `madriv`      | 0x00FC20 | ~7.5 KB  | MPEG audio driver                                         |
| **5th**  | `MoviMan`     | 0x00DD66 | ~7 KB    | Playback manager (uses fmvdrv + madriv)                   |
| **6th**  | `vmpeg`       | 0x001E82 | ~15 KB   | Data loaded into the MPEG video chip                      |
| **7th**  | `dspcode`     | 0x009CE6 | ~16 KB   | Microcode loaded into the audio DSP                       |

## Host Player (cdi200.rom) — Notable Modules

The host firmware contains 73 OS-9 modules. Key modules include:

- `kernel` — OS-9 kernel core
- `init` — system initialization
- `sysgo` — primary boot loader
- `csd_220` — Configuration Status Descriptor for the main board (vs `csd_fmvvm` on cartridge)
- `csdinit` — CSD initialization
- `cdfm` — CD file manager
- `ucm` — unit control module
- `video` — video subsystem
- `cdapdriv` — CD audio player driver
- `config` — system configuration
- `play` — playback control

> **Note:** The FMV cartridge uses `csd_fmvvm`; the host uses `csd_220`. The cartridge CSD must match the host's expectation for the system to enumerate the FMV device.

## Notes

- `0x4E400006` sequences flagged as ❌ during scan are **TRAP instructions** (OS-9 system call entry points), not OS-9 modules — **ignore them**.
- The earlier `.HEX` file was incomplete; the full 512 KB `cdi200.rom` is the authoritative host firmware.
- The FMV ROM was originally interleaved (odd/even bytes from two ROM chips, 7308hi / 7307lo). The combined file `FMV_combined_7308hi_7307lo.bin` was reconstructed by merging hi/lo bytes.
- OS-9 module types used in the FMV cartridge:
  - `0x01` — Program module
  - `0x02` — Subroutine module
  - `0x04` — Data module
  - `0x05` — CSD (Configuration Status Descriptor)
  - `0x0D` — System Manager
  - `0x0E` — Device Driver
  - `0x0F` — Device Descriptor

## Status / Next Steps

| Task                                          | Status |
|-----------------------------------------------|--------|
| Deinterleave hi/lo ROM                        | ✅ Done |
| Confirm 68000 + OS-9 / CD-RTOS architecture   | ✅ Done |
| Working OS-9 module parser (88 modules total) | ✅ Done |
| FMV cartridge module map (15 modules named)   | ✅ Done |
| Extract & disassemble `fmvdrv` for MMIO map   | 🔲 Todo |
| Emulate `csd_fmvvm` so system detects card    | 🔲 Todo |
| Implement FMV device in emulator              | 🔲 Todo |
| Load `dspcode` into audio DSP                 | 🔲 Todo |
| Load `vmpeg` into MPEG video decoder          | 🔲 Todo |

## References

- OS-9 Module Header Reference: Microware System Architecture II (M68000/OS-9)
- CD-i Player: Philips CD-i 200/205, Model No. 22/24
- FMV Cartridge: Digital Video Cartridge (22TC922/22TC923)