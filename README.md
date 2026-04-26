# Pegasos II Firmware

A clean-room reimplementation of the BIOS / OpenFirmware-style boot
ROM for the **Pegasos II** PowerPC motherboard. It boots **AmigaOS 4**,
is ready to extend to **MorphOS** and **Linux**, and runs on the QEMU 
`pegasos2` machine with real hardware to follow.

The original firmware (bPlan/CodeGen SmartFirmware 1.2, 2004) is
nineteen years out of date and has known defects with modern PCI
expansion cards. This rewrite is built fresh from public specs +
the original SmartFirmware source license; it is not a binary patch
of the stock ROM.

## ⚠️  Important — what this release has actually been tested on

**This is a v0.5 pre-release.** The firmware has been validated in
exactly one configuration:

- **QEMU `-M pegasos2`** (Windows host, qemu-system-ppc 10.2.2)
- booting **AmigaOS 4.1 Final Edition Update 3**
- from a **single IDE hard disk** (FFS2 RDB partition layout)

That's it. **Anything outside this combination is unverified.** In
particular:

- **Real Pegasos II hardware: not yet flashed.** Do not write this
  firmware to a board's flash chip unless you have recovery
  hardware (a flash programmer + a known-good backup of the
  original ROM) and you understand the risk of bricking the
  board.
- **MorphOS, Linux, AmigaOS 3.x: not yet bootable.** The
  framework recognises their partition types and falls through
  to a "loader not implemented" message; nothing will load.
- **CD-ROM / USB / network / floppy boot: not implemented.**
- **Multi-disk / slave-drive / secondary IDE channel: untested.**
- **Other QEMU versions / host OSes: untested.** The build is
  deterministic but the QEMU runtime path has only been
  exercised on the version above.

If you're a developer, hobbyist, or researcher comfortable
with a bare-metal PowerPC boot environment, the codebase is
ready to extend. If you're looking for a drop-in BIOS
replacement for daily use on a Pegasos II machine, **wait for
a v1.0 release that has been validated on real hardware.**

## Status

| | works |
|--|--|
| Boot to `ok` prompt on QEMU `pegasos2` | ✅ |
| Boot AmigaOS 4 from a hard drive (FFS2 / SFS) | ✅ |
| PCI bus + PCI-to-PCI bridge enumeration | ✅ |
| VT8231 IDE (HD + CD-ROM) | ✅ |
| Real hardware bring-up | 🚧 (not yet validated on metal) |
| Linux loader | 📋 (framework in place, kernel loader not written) |
| MorphOS loader | 📋 (framework in place, loader not written) |

A 512 KiB binary that drops into the flash socket; 1 GiB of DRAM
on QEMU is sufficient (`-m 1024`); real Pegasos II boards ship
with 256 MiB–2 GiB.

## Quick start

### Build

You need a PowerPC 32-bit cross-toolchain. On Ubuntu:

```bash
sudo apt install gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu
make                       # qemu-tuned firmware (default)
make CONFIG_TARGET=hw      # real-hardware-tuned firmware
```

The output is `build/firmware-raw.bin`, exactly 512 KiB, ready to
flash or hand to QEMU.

### Boot in QEMU

```bash
qemu-system-ppc -M pegasos2 -m 1024 \
    -bios build/firmware-raw.bin \
    -drive file=YOUR_DISK.qcow2,format=qcow2,if=none,id=disk \
    -device ide-hd,drive=disk,bus=ide.0 \
    -serial mon:stdio
```

You'll see the firmware self-test, the SmartFirmware banner, and
then either an auto-boot countdown (default on the QEMU build) or
an `ok` prompt (default on the HW build).

### What happens by default

|                  | QEMU build (`make`) | HW build (`make CONFIG_TARGET=hw`) |
|------------------|---------------------|------------------------------------|
| Auto-boot        | enabled, 3-second countdown | disabled |
| What it boots    | first AmigaOS-family partition (DH0 by default) | nothing — drops to `ok` |
| Persistence      | none — defaults reload every boot (QEMU has no real NVRAM) | yes — `setenv` changes survive reboots (M48T59 battery-backed RAM) |

If you don't press a key during the QEMU countdown, the firmware
runs `smart-boot`, picks the highest-priority bootable partition,
and hands off to the matching OS loader.

### Booting AmigaOS 4 manually

If auto-boot is disabled or you want to override:

```
ok boot hd:0 amigaboot.of
```

That's it — `amigaboot.of` lives at the root of any AOS4 install
disk and handles the rest (kicklayout, kernel modules, etc.).

If you have multiple bootable partitions and want a specific one:

```
ok boot hd:0 amigaboot.of bootdevice=DH1
```

### Booting MorphOS manually

```
ok boot hd:0 morphos.of
```

(MorphOS' equivalent of amigaboot.of; place it at the partition
root.)

### Booting Linux manually

For now, load a kernel ELF directly:

```
ok boot hd:0 /boot/vmlinux
```

A more capable Linux loader (kernel + initrd + cmdline, yaboot-style)
is on the roadmap.

### Auto-booting on real hardware

By default the HW build sits at the `ok` prompt. To make it
auto-boot AmigaOS 4 on every power-up:

```
ok setenv auto-boot? true
ok setenv auto-boot-timeout 5000
ok reset-all
```

Those settings are saved in the M48T59 battery-backed NVRAM and
survive power cycles. The default `boot-command` is `smart-boot`,
which handles partition selection automatically.

To prefer Linux over AmigaOS when both are installed:

```
ok setenv boot-os-priority linux,amigaos,morphos
```

### Aborting auto-boot

Press any key during the countdown:

- **ENTER** → boot immediately, skip the rest of the countdown.
- **ESC** (or any other key) → abort to the `ok` prompt.

## Where to look next

- **[FEATURES.md](FEATURES.md)** — full feature reference (every
  Forth word, every NVRAM variable, every build flag, debugging
  hooks, filesystem support, limitations).
- **[BOOT.md](BOOT.md)** — internal documentation of the boot path
  (reset → OS entry), for anyone hacking on the firmware itself.
- **[PROGRESS.md](PROGRESS.md)** — running implementation log /
  bring-up history; what's done, what's next.
- **[docs/](docs/)** — the original specification this firmware
  was written from (chapter-numbered, written before the impl
  started).

## License

The firmware is released under the same source license as the
upstream SmartFirmware tree it was built atop:

- Our newly written code (everything under `machdep/pegasos2/`)
  carries a CodeGen-style BSD-3-clause-ish header (see any
  source file for the exact wording, or
  `LICENSES/CodeGen-smartfirmware.txt` for the full license).
- Vendored upstream code in `upstream/smartfirmware/` keeps its
  CodeGen copyright + license headers verbatim.
- Vendored x86emu code in `upstream/x86emu/` keeps its SciTech /
  Freescale / Mosberger-Tang / Eich notices verbatim (see
  `LICENSES/SciTech-x86emu.txt`).

Source for the released binary is published with the binary, per
clause 3 of the CodeGen license.

## Sources used (clean-room references)

This is a from-scratch implementation; no bytes of the original
Pegasos II ROM were inspected, decompiled, or copied. The
specifications and references that informed it are all public:

- **[IEEE-1275-1994](https://standards.ieee.org/standard/1275-1994.html)**
  ("Open Firmware") — boot interface and device-tree contract.
- **PCI Local Bus Specification 2.3** — config-space, BAR, bridge.
- **MV64361 / Marvell Discovery II** datasheet — MV64361 northbridge.
- **VIA VT8231** datasheet — VT8231 southbridge (IDE, USB,
  SuperIO, RTC).
- **Freescale MPC7447A / 7450** Reference Manual — CPU registers,
  BATs, exceptions, MSR.
- **Amiga Guru Book (Babel, 1996), ch. 15** — RDB / partition
  table format.
- **Amiga ROM Kernel Reference Manual: Devices** — DosType
  encoding, FFS structures.
- **Clevy's ADF format FAQ** — FFS file/directory header
  layout, hash function.
- **Olaf Barthel's LNFS writeup**
  (AmigaOS Documentation Wiki) — DOS\6/\7 long-name extensions.
- **OpenBIOS for Pegasos II** (`github.com/openbios/smartfirmware`)
  — the source-license-compatible CodeGen SmartFirmware tree
  this build sits atop.
- **U-Boot `drivers/bios_emulator/`** (BSD-licensed SciTech
  derivative) — x86 real-mode emulator for option-ROM execution.

## Contributing

The firmware is a research project. Patches, bug reports, and
hardware-validation runs are welcome. See
[PROGRESS.md](PROGRESS.md) for the current open-questions list and
[SPEC-QUESTIONS.md](SPEC-QUESTIONS.md) for divergences from the
original spec that need clarification.

## Roadmap to full BIOS replacement

What's still needed to fully replace the original Pegasos II BIOS:

- Test on a real Pegasos II board and fix anything QEMU got wrong.
- Linux loader — load a kernel + initrd from a Linux partition.
- MorphOS loader — load MorphOS from a MorphOS partition.
- Boot from CD-ROM — pick up an AmigaOS 4 install CD and run it.
- Boot from USB stick.
- Network boot — load a kernel over Ethernet (PXE / TFTP).
- Boot from floppy disk.
- SCSI boot for boards with a SCSI card fitted.
- Use both IDE channels and slave drives, not just the first master.
- Show a graphical console on a connected monitor (today it's serial only).
- Show a setup screen at boot for editing settings without typing Forth.
- Power management — let the OS suspend and resume the machine.
- Sound chip (AC'97) initialisation.
- Faster IDE transfers (DMA instead of programmed I/O).
