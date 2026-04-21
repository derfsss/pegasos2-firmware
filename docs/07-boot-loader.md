# 07 — Boot loader

The firmware's `boot` command loads an OS kernel (or boot loader)
image from a device and transfers control to it. This chapter
covers the supported image formats, the boot-argument parsing, the
device-path resolution, and the register convention at transfer.

## `boot` command syntax

Per IEEE-1275 and the SmartFirmware manual:

```
boot [device-specifier] [filename] [arguments...]
```

All fields are optional:

- `device-specifier` — a device-tree path (`/pci@.../ide@0/cd@0`),
  an alias from `/aliases` (`cd`, `hd`, `net`, etc.), or an alias
  with a partition suffix (`hd:0`, `cd:2`). If omitted, use
  `boot-device` from env.
- `filename` — the file to load from the device. If omitted, use
  `boot-file`. If both are empty, look for a platform-default
  (`amigaboot.of`, `yaboot`, `zImage`, `boot.img`) in order.
- `arguments` — everything after the filename. Passed to the
  image via `/chosen/bootargs`.

Examples:

| Command | Meaning |
|---------|---------|
| `boot cd amigaboot.of` | Load amigaboot.of from CD alias |
| `boot hd:0 amigaboot.of bootdevice=DHY` | Load from HD partition 0, pass bootdevice= |
| `boot net` | Network boot using default file |
| `boot` | Use env defaults entirely |

## Device-path resolution

The firmware's path resolver handles:

1. Absolute paths: `/pci@.../ide@0/disk@0`.
2. Aliases: `cd`, `hd`, `net`, etc. The alias is looked up in
   `/aliases`; its value is used as the device path.
3. Partition suffix: `:N` after a device path selects partition
   N. The firmware applies the `disk-label` package on the
   device to find the partition.
4. Path:partition:file combined form: `cd:0,\boot\amigaboot.of`
   — partition 0, path `\boot\amigaboot.of`. Rarely used on
   Pegasos2; support is nice-to-have.

## Supported image formats

The firmware probes the first block of the loaded file and picks a
format handler.

### ELF (PowerPC, 32-bit, big-endian)

Magic: `\x7FELF\x01\x02\x01` (7 bytes: 0x7F, 'E', 'L', 'F',
class=32, data=BE, version=1).

Parse the ELF header:
- `e_type`: must be ET_EXEC (2) or ET_DYN (3; statically bound).
- `e_machine`: must be EM_PPC (20).
- `e_entry`: entry point virtual address.

Walk program headers:
- For each PT_LOAD segment: read from file offset `p_offset`,
  `p_filesz` bytes; write to memory at `p_vaddr`. Zero-fill from
  `p_vaddr + p_filesz` to `p_vaddr + p_memsz`.

Transfer: jump to `e_entry` with CI register convention.

### a.out (NetBSD/OpenBSD PowerPC zmagic)

Magic: first 4 bytes = `\x01\x0B\x00\x00` (OMAGIC) or
`\x07\x0B\x00\x00` (ZMAGIC) or similar.

Parse header:
- Text size, data size, bss size.
- Entry point.
- Load addresses (typically 0x10000000 for OpenBSD macppc kernel).

Load text + data contiguously; zero bss.

Transfer: jump to entry point.

### CHRP boot script

First 4 bytes = `<?` (ASCII) followed by XML-like boot-script
syntax. The firmware's CHRP-script parser locates the boot-file
embedded or referenced in the script, loads it, and recurses.

CHRP scripts are used by MorphOS and some Linux distributions
for their CD boot.

### Forth script (`.fs`)

First byte = `\\` (Forth comment) or first word is `:` or
`fload`. The firmware loads the file into the Forth input stream
and evaluates it. Used for development and diagnostic images.

### Raw binary / boot-sector

Fallback: load the entire file at a default address
(`0x00200000` historically on Pegasos2) and jump to that address.
Rarely used; included for compatibility with very old boot
loaders.

## AmigaOS 4 amigaboot.of protocol

`amigaboot.of` is an ELF binary; it loads via the standard ELF
path. However, AOS's boot-time contract has some firmware-side
expectations:

1. **bootdevice argument.** Everything after `amigaboot.of` on
   the boot command line MUST be passed via `/chosen/bootargs`.
   `amigaboot.of` reads this to find the OS partition
   (`bootdevice=DHY` etc.).
2. **peg2ide.device support.** `amigaboot.of` uses
   `/pci@.../ide@*/disk@*` via the standard `open`/`read`/`seek`
   services. No extra AOS-specific device package is required.
3. **`bootloader_prepare` (two-stage boot).** Older AOS install
   CDs want a `boot cd bootloader_prepare` pass that loads a
   preparation stage, then `boot cd amigaboot.of` proper. The
   firmware must support loading any ELF file by name; the naming
   `bootloader_prepare` is not special to the firmware — it's
   just a file on the CD.
4. **SFS-01 support not required.** `amigaboot.of` handles the
   filesystem; the firmware only needs block-device `read`/`seek`
   on the CD.

## Boot-time register state for the OS

At jump to the entry point:

- r1 = a valid small stack in the loaded image's memory region.
- r3 = 0 (per IEEE-1275 client-program entry).
- r4 = 0.
- r5 = CI handler entry pointer.
- r6 = 0 (IEEE-1275 reserved).
- r7 = `e_entry` of this image (some OSes use it).
- MSR = supervisor, translation on, interrupts disabled.
- MMU = firmware-established BATs still covering DRAM and MV64361
  registers. The OS may re-map as it sees fit.
- L1 I-cache enabled. L1 D-cache enabled. L2 enabled.
- Decrementer disabled (the OS manages its own tick).
- External-interrupt line state: all masks inherited; the OS must
  mask/unmask as it sets up its own interrupt handlers.

## Load-address contract

By convention the firmware's default load address is `0x400000`
(4 MiB mark) for AOS-style kernels. The stock firmware's malloc
pool clashed with this on some builds (forum thread 10090); the
new firmware avoids the conflict by placing its heap above
`0x200000` but below `0x400000`, leaving the 4 MiB mark free for
kernel load.

If the ELF's PT_LOAD segments specify a different address, the
firmware honours that (writes to the specified virtual address).
If the address is outside installed DRAM, `boot` fails with a
diagnostic.

## Error handling

`boot` can fail at many points:

- Device not found: log "no such device: <name>" and return to
  the `ok` prompt.
- File not found: log "file not found: <filename>" and return.
- Format unknown: log "unknown image format" and return.
- Image too large for DRAM: log "image does not fit" and return.
- Image load error (disk read failure): log the disk error and
  return.
- Image entry invalid (NULL, outside PT_LOAD): log and return.

None of these should crash the firmware. The user must be able to
issue another `boot` attempt or inspect the device tree after a
failed boot.

---

# 10 — Test plan

## Overview

The test plan has three tiers:

1. **Unit tests** — Forth-level tests for individual words and
   primitives. Run as part of the build, in a host-side QEMU.
2. **Boot tests (automated)** — QEMU scripted runs that boot
   various OS images and verify observable outcomes (serial
   output markers, guest-side scripts).
3. **Real-hardware acceptance tests** — Manual checklists against
   a real Pegasos II with a battery of OS installation media.

Each fail in tiers 1-2 is a CI-blocker; tier 3 is done per
release candidate.

## Tier 1 — unit tests

Run via a test harness that loads the Forth runtime into a host
binary (x86-64 Linux) with stubbed hardware. Sources: per-chapter
"Tests" sections of this spec tree.

Per-chapter test count (minimum):

- `01-cpu-init.md` — 5 tests
- `02-memory-controller.md` — 7 tests
- `03-pci.md` — 6 tests (most important, bug #2 regression)
- `04-southbridge.md` — 6 tests
- `05-of-runtime.md` — 5 tests
- `06-client-interface.md` — 6 tests
- `07-boot-loader.md` — see below
- `08-nvram.md` — 7 tests
- `09-known-bugs.md` — 4 tests (the headline regression cases)

Target: ≥95% pass rate on every commit. Code coverage ≥80% of
non-architectural Forth words.

## Tier 2 — QEMU boot matrix

Each row is a fully automated boot from a clean
`firmware-raw.bin` build. Success criterion: a marker string
appears on serial within a timeout.

| Row | Config                                                         | Timeout | Marker               |
|-----|----------------------------------------------------------------|---------|----------------------|
| T2.1 | Stock `-M pegasos2` (no CD, no display)                       | 15 s    | `ok` prompt on serial |
| T2.2 | Stock + nested PCI bridge + e1000                             | 20 s    | `show-devs /pci` lists the e1000 under the bridge |
| T2.3 | AOS 4.1 install CD (`-cdrom` or `-drive ide-cd,bus=ide.1`)    | 60 s    | `amigaboot.of` banner |
| T2.4 | AOS 4.1 install CD + empty HD image; automated install script | 15 min  | `AmigaOS 4.1 installation complete` |
| T2.5 | Boot from installed HD                                        | 90 s    | AOS Workbench startup marker |
| T2.6 | MorphOS install CD                                            | 60 s    | MorphOS splash |
| T2.7 | Linux Debian PPC netinst                                      | 60 s    | "Debian GNU/Linux installer" |
| T2.8 | A VGA BIOS test ROM (synthetic) that exercises every INT 10 AH | 30 s | All INT 10 functions return expected values, no `UNHANDLED` log lines |
| T2.9 | Yaboot from ISO                                               | 60 s    | Yaboot prompt |
| T2.10 | Power-cycle test: boot → setenv auto-boot? true → reboot → auto-boot fires | 90 s | Shell boot sequence completes |

Scripts for each row live in `test/qemu-*.py` following the
model of `scripts/validate_vga.py` from the current tree.

## Tier 3 — real-hardware acceptance

Per release candidate, manually verified on a real Pegasos II:

| Row | Scenario |
|-----|----------|
| T3.1 | Cold boot to `ok` prompt on serial (no display) |
| T3.2 | Cold boot to `ok` prompt on VGA with PS/2 keyboard |
| T3.3 | Install AOS 4.1 from DVD to SATA-via-IDE drive |
| T3.4 | Boot AOS 4.1 with Radeon 9200 AGP — display initialises correctly via FCode (no x86 emu path) |
| T3.5 | Boot AOS 4.1 with Radeon 9550 AGP — display initialises via x86 emulator path (tests bug #1 fix) |
| T3.6 | Boot AOS 4.1 with Radeon RX 570 via PCI-to-PCIe riser — display initialises (tests bugs #1 + #2 in combination) |
| T3.7 | Boot Debian PPC installer |
| T3.8 | Boot MorphOS |
| T3.9 | USB keyboard boot (PS/2 absent) reaches `ok` |
| T3.10 | USB mass-storage boot (`boot usb`) reaches installed system |

## Regression-specific tests (must never regress)

From `09-known-bugs.md`, the following must remain passing on
every build:

1. No `UNHANDLED 32 BIT DATA PREFIX` in any boot log.
2. No `INTERNAL ERROR: 0000000A..0000000F` codes in any boot log.
3. No `Failed to emulate` lines in any boot log during the T2.8
   synthetic VGA BIOS test.
4. `show-devs /pci@*` lists every device visible in QEMU's
   `info pci`, including those behind bridges — in both T2.2 and
   a nested-bridge-squared variant.
5. `auto-boot?=true` + `boot-command=boot cd amigaboot.of` + CD
   ISO attached → AOS reaches installation screen within 2 min.
6. NVRAM `setenv` survives `quit`/power-cycle in QEMU (with the
   NVRAM backing file preserved).

## CI integration

Tier 1 tests run on every commit (target: under 60 s).
Tier 2 rows T2.1, T2.2, T2.3, T2.8 run on every commit (target:
under 5 min total).
Tier 2 rows T2.4-T2.7, T2.9, T2.10 run nightly.
Tier 3 runs per release candidate, manually signed off.

The CI harness consumes serial logs and exits non-zero on any
unexpected `INTERNAL ERROR`, `UNHANDLED`, `Failed to emulate`, or
`STUCK CS:IP` line in any test's serial output (except when the
test explicitly expects them, e.g. a synthetic test that
deliberately feeds a bad opcode).
