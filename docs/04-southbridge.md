# 04 — VIA VT8231 southbridge integration

The VT8231 is a multi-function PCI device that provides every
legacy I/O subsystem on the Pegasos II: ISA, IDE, USB, AC'97 audio
and modem, SuperIO (serial/parallel/PS/2), power management, and
SMBus. It sits on PCI0 (community convention: slot 0xC = device 12)
as a multi-function device with seven functions.

## PCI function map

| fn | Class  | Purpose                          |
|----|--------|----------------------------------|
| 0  | 0x0601 | ISA bridge (PCI-to-ISA)          |
| 1  | 0x0101 | IDE controller (primary + secondary channels) |
| 2  | 0x0C03 | USB UHCI controller 0            |
| 3  | 0x0C03 | USB UHCI controller 1            |
| 4  | 0x0680 | Bridge (power management / ACPI) |
| 5  | 0x0401 | AC'97 audio                      |
| 6  | 0x0703 | MC'97 modem                      |

All seven functions must appear in the device tree as children of
the ISA bridge node (or as siblings — both conventions are used on
Pegasos2; the firmware MUST be consistent per the published
device-tree binding).

## Function 0 — ISA bridge and SuperIO

### Config-space init

The firmware must programme the VT8231 configuration registers
(PCI config offsets 0x40..0xFF) to establish:

- Interrupt router for PIRQ A..D. Each PIRQ is routed to one of
  the ISA IRQs 3,4,5,7,9,10,11,12,14,15. The mapping is
  board-specific (defined in the Pegasos II interrupt-routing
  table; the firmware's per-board constants table supplies it).
- ISA aperture size (typically fixed at 64 KiB I/O below
  PCI0_IO_WINDOW_BASE).
- Enable decode windows for SuperIO addresses.
- Enable bus mastering (config offset 0x04 bit 2).
- Set the ROM shadow / decode region for the boot flash access
  through ISA. On Pegasos2 the flash is also accessible as an
  ISA-memory-mapped region via the VT8231; the firmware's flash
  programming code uses this path.

### SuperIO (embedded in function 0)

The VT8231 SuperIO provides:

| Device | Default IO | IRQ |
|--------|-----------|-----|
| UART 1 (COM1) | 0x3F8 | 4 |
| UART 2 (COM2) | 0x2F8 | 3 |
| Parallel port | 0x378 | 7 |
| PS/2 keyboard | 0x60/0x64 | 1 |
| PS/2 mouse    | 0x60/0x64 | 12 |
| Floppy (not routed on Pegasos2) | — | — |

The firmware MUST programme each SuperIO logical device's
enable/disable and I/O base via the VT8231 SuperIO config
registers. The register layout and access sequence (enter-
extended-function-mode, index, data, exit) are documented in the
VT8231 datasheet §3.

#### Known issue: parallel-port remap conflict (QEMU-only)

Per forum thread 10090, QEMU's `vt82c686.c` emulation of the
VT8231 mishandles the parallel-port remap at SuperIO config offset
0xF6 — it moves the parallel port's I/O base to a location that
overlaps with VGA MMIO, causing VGA Option ROM execution to fail.
This is a QEMU bug, not a firmware bug, but the firmware can
avoid triggering it by leaving the parallel port enable bit clear
unless the OS explicitly re-enables it via the client interface.

The new BIOS's default NVRAM should NOT enable the parallel port
at boot. Provide an env var `enable-parallel?` (default false) so
users can opt in. This sidesteps the QEMU emulation bug entirely
without requiring a QEMU patch.

## Function 1 — IDE controller

Native-mode (non-legacy) PCI IDE. Two channels, each with
master/slave.

### Configuration

| Config offset | Purpose |
|---------------|---------|
| 0x09          | Programming interface: 0x8F for native dual (read-only, set by the chip based on subsystem config) |
| 0x10..0x1F    | BARs: command block, control block, DMA engine for each channel |
| 0x40          | IDE timing register (primary and secondary) |
| 0x41          | UDMA timing |
| 0x42..0x45    | Miscellaneous |

The firmware programmes the BARs (typically at I/O ports 0x1F0,
0x3F6 for primary and 0x170, 0x376 for secondary — legacy
positions, even in native mode for compatibility).

### Drive detection

For each channel and drive (master/slave), the firmware issues the
ATA IDENTIFY DEVICE (ECh) or ATAPI IDENTIFY PACKET DEVICE (A1h)
command and parses the returned 512-byte data block to:

1. Confirm the drive is present and responsive.
2. Determine the drive type (ATA fixed disk vs ATAPI CD-ROM / DVD).
3. Extract the model string, serial number, capacity, and
   capabilities.
4. Choose the highest UDMA mode supported by both the drive and
   the VT8231 channel timing.

Device-tree nodes:

- ATA fixed disk: `/pci@.../ide@<ch>/disk@<n>` with properties
  `name="disk"`, `device_type="block"`, `reg`, `ata-id-page`.
- ATAPI: `/pci@.../ide@<ch>/cd@<n>` with `device_type="block"`
  (so generic block-device clients can use it) and an additional
  `atapi` flag property.

### Known issue: QEMU IDE detection hang

With `-drive if=none,id=cd,file=...iso,format=raw -device
ide-cd,drive=cd,bus=ide.1`, the stock firmware's IDE detection
loops for an extended period (observed: ~110 seconds of busy-wait
at the second channel's IDENTIFY-DEVICE poll). The root cause
appears to be the firmware's timeout loop counting CPU cycles, not
milliseconds, and the emulated response being faster than the
firmware expects for "drive absent".

The new BIOS should use **real-time-based** timeouts (via
`get-msecs`, not cycle counting), with a total IDE-probe budget
of no more than 5 seconds per channel. Per-command timeouts should
be 2 seconds for IDENTIFY, 10 seconds for READ/WRITE.

## Function 2, 3 — USB UHCI controllers

Two independent UHCI controllers. Each has a 32-byte I/O port BAR
(BAR4) and issues interrupts on PIRQ D by default.

The firmware MUST enumerate USB devices at startup only to the
extent required for:

- USB keyboard / USB HID mouse (so `input-device` can be a USB
  keyboard when no PS/2 is present).
- USB mass storage (so `boot usb` can load from a USB stick —
  optional for phase 0 of the rewrite but a strong nice-to-have).

Detailed USB enumeration (hubs, etc.) can be deferred to the OS;
the firmware needs enough to mount a boot device.

Device-tree nodes:

- `/pci@.../usb@<n>` for each controller (name `usb`,
  `device_type="usb"`).
- Children per enumerated device: `/pci@.../usb@<n>/kbd@<port>`,
  `/pci@.../usb@<n>/mass-storage@<port>`, etc.

## Function 4 — Power management / ACPI bridge

Registers a PCI function of class 0x068000 (Other Bridge). Exposes
ACPI power-management registers via an I/O BAR. On real hardware
this is used by the OS to initiate soft-off (S5). The firmware
needs to:

1. Expose the node in the device tree as `/pci@.../pm@<addr>`.
2. On the OS's request (via client interface `reset-all`),
   issue a full system reset by writing to the PM reset register.
3. The firmware itself does not need to use power management
   during boot.

## Function 5 — AC'97 audio

Class 0x0401. BAR0 and BAR1 are I/O ports for the AC'97 codec
registers and the bus-master DMA channels.

The firmware MUST NOT initialise the codec (that's OS-side
responsibility). It MUST expose the device in the device tree with
`name="sound"` and `device_type="sound"`, `compatible="via,vt8231-
ac97"`.

## Function 6 — MC'97 modem

Class 0x0703. Rarely used on real Pegasos2 systems. The firmware
must create a device-tree node but otherwise leave the function
alone.

## SMBus (embedded in function 4 or 5 depending on VT8231 rev)

The VT8231 SMBus host controller is used by the bootstrap to:

1. Read SPD EEPROMs on the DDR SDRAM DIMMs (slave addresses
   0x50..0x53) during Phase 1 memory init.
2. Programme the Winbond W83194 clock generator (slave address
   0x69) to set FSB.

The SMBus host controller's registers are at I/O port base
`PM_BASE + 0x90..0x9F` (PM_BASE is set by config register 0x90 of
the pmu function). Read and write sequences follow the SMBus 2.0
protocol.

The firmware MUST expose the SMBus host in the device tree as
`/pci@.../pm/smbus@90` so the OS can use it for monitoring / etc.

## Interrupt router

The VT8231 has an internal PIRQ→ISA-IRQ router controlled by
config offsets 0x54..0x57. On Pegasos2 the PIRQ routing is:

| PIRQ | ISA IRQ assignment (typical) |
|------|------------------------------|
| A    | IRQ 11 |
| B    | IRQ 10 |
| C    | IRQ 9  |
| D    | IRQ 5  |

These values are community convention and should be confirmed
against the board-specific per-board constants table the firmware
carries.

The PIC (i8259-compatible pair inside VT8231 function 0) is the
child of the VT8231 ISA bridge in the device tree. The firmware
must install an `interrupt-map` on the ISA bridge node mapping
ISA IRQ numbers to MV64361 IC inputs.

## Legacy devices (inside the ISA bridge)

Exposed at fixed I/O addresses per standard PC layout:

| Device | I/O | Purpose |
|--------|-----|---------|
| i8259 PIC master | 0x20/0x21 | Primary PIC |
| i8259 PIC slave  | 0xA0/0xA1 | Cascaded PIC |
| i8254 PIT        | 0x40..0x43 | Legacy 1.19 MHz timer |
| i8237 DMAC       | 0x00..0x0F + 0xC0..0xDF | Legacy ISA DMA |
| i8042 keyboard controller | 0x60/0x64 | PS/2 KBD + mouse |
| M48T59 NVRAM + RTC | 0x70..0x77 + ISA mem window | See `08-nvram.md` |
| SuperIO config entry | 0x2E/0x2F | Config index/data |

The firmware MUST initialise the i8259 pair (mask all IRQs during
boot, set edge/level triggers per PCI spec: PIRQs are level) and
the i8254 (channel 0 for timer-tick, channel 2 for PC speaker if
enabled).

Device-tree node for the ISA bridge:

```
/pci@.../isa@C  (unit address = 0xC = VT8231 device number)
  name = "isa"
  device_type = "isa"
  compatible = "via,vt8231-isa", "isa"
  #address-cells = 2
  #size-cells = 1
  ranges = (maps ISA I/O to PCI I/O window)
  interrupt-parent = <phandle-of-vt8231-pic>
  interrupt-controller
  #interrupt-cells = 2
```

## Required FCode methods on the ISA bridge

Clients that want to access ISA I/O via the firmware go through
the ISA bridge node and use these methods:

- `encode-unit` / `decode-unit` — parse/format ISA unit addresses
  like `i@378` (I/O port 0x378).
- `open` / `close` — instance the bridge for access.
- `read-b` / `read-w` / `read-l` / `write-b` / `write-w` /
  `write-l` on children — generate the appropriate ISA I/O cycles.

## Tests

1. VT8231 device-tree nodes must appear at standard paths with
   standard properties.
2. A `show-devs /pci@*/isa@*` must list all seven functions plus
   the PS/2 keyboard controller, PIC, PIT, and NVRAM children.
3. `boot cd` with a virtual ATAPI CD-ROM must complete IDE probe
   in under 5 seconds and find the disc.
4. `setenv enable-parallel? true` followed by reboot must NOT
   cause VGA initialisation to fail under QEMU with a video card
   ROM loaded. (This is a regression test for the
   QEMU-vt82c686.c bug.)
5. Writing to the M48T59 NVRAM via the `setenv` path must result
   in the new value being present after a power cycle (or QEMU
   restart with the NVRAM backing file preserved).
6. The PS/2 keyboard controller, when given a character via QEMU's
   emulated keyboard, must deliver a scan code that the firmware's
   `keyboard` package can translate, and the Forth prompt must
   accept the character.
