# 00 — Overview: hardware baseline, boot flow, and rewrite goals

This chapter sets the baseline for the rewrite. It describes the
target hardware, the memory map the firmware must produce for a
booted OS, the major boot phases, and the non-negotiable
conformance points (IEEE-1275, AmigaOS 4 boot loader contract).

## Target hardware

Pegasos II (bPlan, 2003-2005):

| Role | Part                       | Notes |
|------|-----------------------------|-------|
| CPU  | Motorola MPC7447 / 7447A   | 600-1500 MHz range shipped. PVR 0x8002xxxx. G4 core with AltiVec. |
| System controller (North) | Marvell Discovery II MV64361 | PPC system bus ↔ DDR SDRAM ↔ two PCI host bridges ↔ two Gigabit Ethernet. Integrated interrupt controller. |
| Southbridge | VIA VT8231                | PCI-to-ISA bridge, IDE (2 channels), USB UHCI ×2, AC97 audio, MC97 modem, SuperIO (serial/parallel/PS/2), power management. |
| Flash  | AMD Am29F040B (or similar 512 KiB boot flash) | Mapped at `0xFFF80000..0xFFFFFFFF` on real hardware (top 512 KiB of 32-bit address space). |
| NVRAM  | ST M48T59 TimeKeeper (8 KiB SRAM + RTC) | Battery-backed. Holds OF environment variables, device aliases, and `nvramrc`. Accessed via ISA I/O. |
| Clock gen | Winbond W83194 | PLL configuration IC. Programmed over the VT8231 SMBus by the bootstrap to set FSB. Failure is tolerated (default FSB is used). |
| Expansion | 3× 32-bit PCI, 1× AGP ×4 | AGP slot is routed as a PCI bridge port from PCI0 and carries a GPU. The AGP slot + on-board risers are where modern GPU installation paths live (motivation for bug #1). |

The rewrite must produce a firmware that:
- Works on real Pegasos II hardware with any shipped motherboard
  revision.
- Works on QEMU's `pegasos2` machine (QEMU 8.0+ recommended; see the
  test plan).
- Supports AmigaOS 4.x as the primary OS (motivating the rewrite),
  with MorphOS and Linux as nice-to-haves.

## System memory map

The firmware must programme a memory map that the booted OS can
rely on. The map below is the target; numeric ranges come from
Marvell MV64361 and Pegasos II conventions cross-checked against a
live QEMU `info mtree` for the pegasos2 machine.

| Range (physical)              | Contents |
|-------------------------------|----------|
| `0x00000000`..`0x???FFFFF`    | DDR SDRAM (size determined by SPD probe; typical 256 MB..2 GB) |
| `0x80000000`..`0xBFFFFFFF`    | PCI1 memory window |
| `0xC0000000`..`0xDFFFFFFF`    | PCI0 memory window |
| `0xF1000000`..`0xF100FFFF`    | MV64361 internal registers (64 KiB bank) |
| `0xF8000000`..`0xF8FFFFFF`    | PCI0 I/O space window (16 MiB) |
| `0xF9000000`..`0xF9FFFFFF`    | PCI0 mem window alias (16 MiB) |
| `0xFD000000`..`0xFDFFFFFF`    | PCI1 mem window alias (16 MiB) |
| `0xFE000000`..`0xFEFFFFFF`    | PCI1 I/O space window |
| `0xFF800000`..`0xFFFFFFFF`    | PCI1 mem3 window (top 8 MiB — QEMU convention) |
| `0xFFF80000`..`0xFFFFFFFF`    | Boot flash (512 KiB) — **real hardware only**. |
| `0xFFF00000`..`0xFFF7FFFF`    | Boot flash as mapped by QEMU's `pegasos2` machine — differs from real HW, be aware when interpreting `0xFFFxxxxx` addresses in disassembly. |

Notes:
- The new BIOS must expose this physical memory map to the booted OS
  via IEEE-1275 `reg` and `ranges` properties on the `/` and `/pci@*`
  nodes. See `03-pci.md`.
- The 0xF1000000 register bank is internal to the MV64361 and is NOT
  on any PCI bus.

## Boot flow (required, in order)

The rewrite's boot flow is four phases. Each phase ends by handing
control to the next with a well-defined state.

### Phase 1 — Reset vector and bootstrap

Executes directly from flash at the MPC7447 hard-reset vector
(`0xFFFxxxxx`, CPU setting dependent). This phase is pure PPC
assembly / minimal C, not Forth.

Responsibilities:

1. Set up a minimal MMU state sufficient to execute from flash with
   a sane data stack.
2. Detect CPU PVR, enable L1 I-cache, initialise BATs, enable
   branch-target address cache if available.
3. Programme the W83194 clock generator over the VT8231 SMBus to
   set the target FSB. Tolerate SMBus failure (stock firmware logs
   "Reading W83194: FAILED" and continues with the default FSB).
4. Probe DDR SDRAM via SPD on the VT8231 SMBus; programme the
   MV64361 SDRAM controller; zero the memory and run a short test
   pattern (stock firmware runs "fill random / fill linear / read
   back" passes with a progress indicator).
5. Configure the MV64361 PCI host bridges' address-decode windows,
   interrupt routing, and parity/error handling. See `02-memory-
   controller.md` and `03-pci.md`.
6. Configure the VT8231 legacy devices (interrupt router, IDE
   reset release, SuperIO, PS/2 keyboard controller, serial
   consoles).
7. Decompress the Forth/OpenFirmware image from flash into RAM and
   jump to its entry point.

The rewrite is free to choose any self-consistent decompression
format for the packaged Forth core. The original firmware uses a
custom gzip-variant (`DONA` magic TOC + flash-sector-segmented
DEFLATE) that has not been fully reverse-engineered; the new BIOS
does NOT need to preserve compatibility with that format unless a
repack-into-original-flasher path is explicitly desired.

Exit state:
- DRAM initialised and tested.
- MV64361 basic register config complete; PCI windows decoded.
- VT8231 legacy IO initialised.
- Forth image resident in RAM at an agreed load address (stock
  firmware uses `0x01000000`; the rewrite may choose any 4 KiB-
  aligned address above the top of the exception vectors and below
  the system heap).
- Control transferred to the Forth entry with r3 = pointer to a
  machdep handoff struct (defined below).

### Phase 2 — Forth / OpenFirmware initialisation

Runs the Forth engine, which performs the standard IEEE-1275
initialisation.

Responsibilities:

1. Set up Forth heap and stacks.
2. Build the device tree: create nodes for `/`, `/cpus/cpu@0`,
   `/memory`, `/chosen`, `/aliases`, `/openprom`, `/options`,
   `/packages`, `/pci@<host-bridge-0>`, `/pci@<host-bridge-1>`.
3. Register all built-in Forth words (core dictionary, IEEE-1275
   device-tree words, FCode interpreter).
4. Read NVRAM: parse the M48T59 contents, decode environment
   variables, populate `/options` properties. See `08-nvram.md`.
5. If `use-nvramrc?` is true, evaluate `nvramrc`; otherwise run the
   default startup sequence: `probe-all install-console banner`.
6. Decide auto-boot: if `auto-boot?` is true AND no key was held
   during `auto-boot-timeout` (default 500 ms), evaluate
   `boot-command`; otherwise present the `ok` prompt.

Exit state:
- Either the Forth interpreter's `ok` prompt on the active console,
  or a boot loader running from Phase 3.

### Phase 3 — Boot loader

Runs when `boot` is executed. Loads an OS boot image and transfers
control per IEEE-1275 and (for AmigaOS 4) the amigaboot.of loader
protocol.

Responsibilities:

1. Parse the boot argument(s): `boot [device] [filename] [args...]`.
   - Device may be a pathname, an alias, or empty (use `boot-device`
     env var).
   - Filename may be omitted (use `boot-file` or implicit
     `amigaboot.of` on AOS-style paths).
2. Read the boot file from the device. Recognise ELF, a.out, and
   CHRP-script forms.
3. Set up the client interface handler and publish its entry
   pointer to r5 of the loaded image.
4. Transfer control to the image's entry point with the IEEE-1275
   client interface register convention (r3 = 0, r4 = 0, r5 = CI
   handler).

Exit state:
- OS running. Firmware remains reachable via the client interface
  until the OS calls `quiesce` or `exit`.

### Phase 4 — Client interface service

Persists for the lifetime of the OS until `quiesce`. Each CI call
enters Forth, dispatches to the appropriate service word, and
returns.

Responsibilities:
- All of the IEEE-1275 client interface services — see
  `06-client-interface.md`.

## Handoff struct (Phase 1 → Phase 2)

The bootstrap passes a small struct to the Forth entry point. Not
all fields need to be populated in every build, but the schema must
be stable.

```c
struct pegasos2_handoff {
    u32 magic;              /* 'P','G','2','H' = 0x50473248 */
    u32 version;            /* struct revision, starts at 1   */
    u32 cpu_pvr;            /* from SPR 287 at cold-boot time */
    u32 cpu_clock_hz;       /* effective CPU clock after W83194 */
    u32 bus_clock_hz;       /* effective FSB in Hz             */
    u64 dram_size;          /* bytes                            */
    u32 flags;              /* bit 0: cold-boot; bit 1: memtest_ok */
    u32 serial_console;     /* VT8231 UART I/O base — typically 0x3F8 */
    u32 nvram_base;         /* ISA I/O base for M48T59 access       */
    u32 reserved[8];
};
```

Rationale: this struct saves the Forth core from re-deriving values
the bootstrap already knows (CPU PVR, DRAM size, NVRAM base). It
MUST NOT be grown in a backward-incompatible way; increment
`version` when adding fields.

## Supported operating systems (contract)

| OS               | Entry expected | Path |
|------------------|-----------------|------|
| AmigaOS 4.x      | `boot cd amigaboot.of` (or Amedia's two-stage `boot cd bootloader_prepare; boot cd amigaboot.of`), `boot hd:0 amigaboot.of bootdevice=<name>` | Must implement the AOS amigaboot.of ABI; see `07-boot-loader.md`. |
| MorphOS          | `boot cd` with the MorphOS installer ISO; uses a CHRP boot script. | SmartFirmware-compatible CHRP script support required. |
| Linux (Debian/Fedora PPC) | `boot cd yaboot` (or `zImage` direct); `boot hd:0 yaboot` | Yaboot or kernel-direct ELF loading required. |

Test criteria: see `10-test-plan.md`. The QEMU test matrix for each
OS must reach at least its installer/boot-menu stage from a clean
`firmware-raw.bin` build.

## Strategic goals for the rewrite

1. **Correctness** — the two headline bugs of the stock firmware
   must NOT be reproduced (see `09-known-bugs.md`). The x86
   emulator must be complete enough for modern VGA BIOSes; PCI
   enumeration must recurse through bridges.
2. **Clean-room integrity** — the rewrite is coded from these specs
   only. No verbatim text, no decompiler output, no symbol names
   copied from the disassembly. Upstream open-source
   (`openbios/smartfirmware`) is an allowed starting point; any
   Pegasos2 machdep code is original.
3. **Serial-console friendliness** — default env vars for fresh
   NVRAM must produce a usable serial console even when no display
   is present. Specifically, `install-console` must probe for a
   real framebuffer and fall back to serial if absent. See
   `05-of-runtime.md` § console routing.
4. **QEMU-first development** — every subsystem must have a QEMU
   repro in `10-test-plan.md` that a CI pipeline can run without
   hardware.
5. **Diagnostic output that doesn't spam** — log lines must
   correspond to real errors, not to "emulator quirk detected"
   warnings.
6. **Fit within 512 KiB flash** — the original does; the rewrite
   should too, even if the packing method differs.

## Out of scope for this rewrite

- Running the original bPlan flasher utility (we replace the flash
  image directly; users flash via JTAG / direct programmer / an OS-
  side flashing tool).
- Running on the original Pegasos I (not supported by AOS 4.x
  either).
- A full Forth cross-compiler / development environment. Only the
  runtime and interpreter are required.
