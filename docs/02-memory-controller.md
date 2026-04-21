# 02 — Memory controller: Marvell MV64361 (Discovery II)

The MV64361 is a monolithic ASIC that provides CPU interface,
DDR SDRAM controller, two PCI 2.3 host bridges, two Gigabit MAC
PHYs, interrupt controller, and various ancillary units.

This chapter covers Phase 1's SDRAM bring-up and the MV64361
register initialisation. PCI parts are in `03-pci.md`.

## Register bank

The MV64361's internal registers are one contiguous 64 KiB bank.
On Pegasos II, this bank is mapped at CPU-physical
`0xF1000000..0xF100FFFF`. The bootstrap programmes the CPU-side
mapping via BATs before touching the registers (see `01-cpu-
init.md`).

All MV64361 registers are 32-bit, accessed big-endian by default.
The chip supports little-endian mode via a chip-select control
field; the firmware MUST leave it in big-endian mode.

Register addressing convention in this document:
- `MV_<NAME>` denotes a symbolic register name from the Marvell
  Discovery II System Controller Programmer's Reference (the
  firmware's per-board register table may use different symbols;
  that's fine).
- `@+0xXXX` is the register's offset within the 64 KiB bank. Full
  CPU address = `0xF100_0000 + offset`.

## Major register groups

| Offset    | Size    | Group |
|-----------|---------|-------|
| 0x000..0x0FF | 256 B | CPU interface control |
| 0x100..0x1FF | 256 B | Address decode windows (CPU → PCI0) |
| 0x200..0x2FF | 256 B | Address decode windows (CPU → PCI1) |
| 0x300..0x3FF | 256 B | Address decode windows (PCI0 → RAM) |
| 0x400..0x4FF | 256 B | Address decode windows (PCI1 → RAM) |
| 0x500..0x5FF | 256 B | SDRAM controller (timing, banks, refresh) |
| 0x600..0x6FF | 256 B | SDRAM address decode (which CS covers which address range) |
| 0x700..0x7FF | 256 B | SDRAM error and diagnostic |
| 0x800..0x8FF | 256 B | Device (DDC) bus — SPD access |
| 0x900..0x9FF | 256 B | Main interrupt controller |
| 0xA00..0xAFF | 256 B | CPU interrupt mask |
| 0xB00..0xBFF | 256 B | Device PCI error + coherency |
| 0xC00..0xCFF | 256 B | PCI0 internal registers (including cfg-addr/data at +0xCF8/+0xCFC) |
| 0xD00..0xDFF | 256 B | PCI0 BARs and snoop config |
| 0xE00..0xEFF | 256 B | Timer/counter |
| 0xF00..0xFFF | 256 B | GPP (general purpose pins) + SDMA |
| 0x1000..0x1FFF | 4 KiB | Gigabit Ethernet 0 |
| 0x2000..0x2FFF | 4 KiB | Gigabit Ethernet 1 |
| 0x3000..0x3FFF | 4 KiB | Gigabit Ethernet 2 (unused on Pegasos) |
| 0x4000..0x4FFF | 4 KiB | DMA engine (IDMA) |
| 0x8000..0x8FFF | 4 KiB | PCI1 mirror of 0xC00..0xCFF (cfg-addr at +0x8CF8, etc.) |
| 0x9000..0x9FFF | 4 KiB | PCI1 mirror of 0xD00..0xDFF |

The full register list is in the Marvell Discovery II
Programmer's Reference Manual. The firmware's per-board table
should pick the subset it actually touches.

## SDRAM initialisation

DDR SDRAM must be initialised after CPU PLL lock but before any
DRAM access. The sequence the firmware MUST perform:

### 1. Read SPD EEPROMs

Each installed DIMM has an SPD (Serial Presence Detect) EEPROM at
I2C slave address 0x50 + bank number (so DIMM0 = 0x50, DIMM1 =
0x51, up to 0x53 for 4 DIMM slots — Pegasos II has 2 slots).

SPD access on Pegasos II goes through the MV64361's I2C
controller at `@+0x8000..+0x801F` (the "Device (DDC)" group).
Alternative: the VT8231 SMBus can reach the same I2C bus — the
stock firmware uses the VT8231 path during late init but the
MV64361 path during early init when VT8231 isn't configured yet.

Read the first 128 bytes of each SPD. Parse per JEDEC SPD spec:

- Byte 0: SPD length (default 128).
- Byte 2: Memory type (0x07 = DDR, 0x08 = DDR2; Pegasos2 targets
  DDR = 0x07).
- Bytes 3,4: Row and column address bits.
- Byte 5: Number of banks.
- Byte 9: CAS latency supported.
- Byte 10: tRP / tRCD / tRAS timings (coded).
- Bytes 31-62: Refresh rate, burst lengths, module size.

From these values the firmware derives the SDRAM controller
programming.

### 2. Programme SDRAM controller registers

| Register               | Offset | Contents |
|------------------------|--------|----------|
| `MV_SDRAM_CONFIG`      | +0x448 | General config: interleaving, burst, refresh |
| `MV_SDRAM_TIMING_HIGH` | +0x4C0 | tCDLR, tDEC, tMRD, tWR, tRP, tRCD (upper) |
| `MV_SDRAM_TIMING_LOW`  | +0x4C4 | tRAS, tRTP, tWTR, tRRD, refresh-rate |
| `MV_SDRAM_MODE`        | +0x4C8 | CAS latency, burst length, DLL enable |
| `MV_SDRAM_DATA_SIZE_0` | +0x504 | Size of bank 0 (CS0) |
| `MV_SDRAM_DATA_SIZE_1` | +0x50C | Size of bank 1 (CS1) |
| `MV_SDRAM_OP_MODE`     | +0x418 | Operating mode: nop / pre-all / refresh / mode-reg-set / normal |
| `MV_SDRAM_CS0_LOW`     | +0x008 | CPU→SDRAM CS0 address low |
| `MV_SDRAM_CS0_HIGH`    | +0x010 | CPU→SDRAM CS0 address high (= low + size) |
| (similar for CS1..3)   |        | |

### 3. DDR init sequence

Per Discovery II datasheet §7.3:

```
1. Apply power. Wait 200 us for DRAM power-up.
2. Issue 200 NOPs (via `MV_SDRAM_OP_MODE = 0`; wait for idle).
3. Issue PRECHARGE-ALL (`MV_SDRAM_OP_MODE = 1`; wait).
4. Issue MODE-REGISTER-SET (`MV_SDRAM_OP_MODE = 3`; wait).
   Programmes DRAM-side CL / burst / etc.
5. Issue EXT-MODE-REGISTER-SET (`MV_SDRAM_OP_MODE = 4`; wait).
   Enables DLL and on-die termination.
6. Issue PRECHARGE-ALL again.
7. Issue 2 REFRESH commands (`MV_SDRAM_OP_MODE = 2`; wait).
8. Issue MRS again with DLL-reset bit cleared.
9. Wait 200 tCK.
10. Issue PRECHARGE-ALL.
11. Issue 8 REFRESH commands.
12. Switch to normal mode (`MV_SDRAM_OP_MODE = 7`).
```

After step 12, DRAM is ready for normal access.

### 4. Validate

Simple stuck-bit test:

- Write 0xAAAAAAAA to the first 4 KiB of each bank.
- Read back. Compare.
- Write 0x55555555. Read back.

The stock firmware additionally runs a multi-pass random/linear
fill test. The rewrite should implement it behind
`diag-switch?=true`; without that flag, the quick stuck-bit test
is sufficient to catch gross failures.

## Address decode windows

The MV64361 routes CPU accesses and DMA accesses through decode
windows. Each window has a base-address register and a size
register; a CPU or DMA transaction falling inside a window is
routed to the window's target unit.

### CPU → PCI0 windows

Default Pegasos2 mapping (firmware must programme these):

| CPU range                 | PCI0 target          | Window reg offset |
|---------------------------|----------------------|--------------------|
| `0xC0000000..0xDFFFFFFF`  | PCI0 memory 0        | +0x0190 (mask), +0x0194 (base) |
| `0xF8000000..0xF8FFFFFF`  | PCI0 I/O             | +0x0198 (mask), +0x019C (base) |
| `0xF9000000..0xF9FFFFFF`  | PCI0 memory 1 (alias)| +0x01A0 (mask), +0x01A4 (base) |

### CPU → PCI1 windows

| CPU range                 | PCI1 target | Window reg offset |
|---------------------------|-------------|--------------------|
| `0x80000000..0xBFFFFFFF`  | PCI1 memory 0 | +0x0210 (mask), +0x0214 (base) |
| `0xFE000000..0xFEFFFFFF`  | PCI1 I/O   | +0x0218 (mask), +0x021C (base) |
| `0xFD000000..0xFDFFFFFF`  | PCI1 memory 1 | +0x0220 (mask), +0x0224 (base) |
| `0xFF800000..0xFFFFFFFF`  | PCI1 memory 3 (QEMU-only alias) | +0x0230 (mask), +0x0234 (base) |

### PCI → DRAM windows

Each host bridge has up to 4 inbound windows. For a DMA-capable
PCI device to reach system memory, the firmware must set up at
least one inbound window that covers the DRAM range.

Minimum setup: one window per bridge that maps PCI addresses
`0x00000000..size-1` to CPU/DRAM addresses `0x00000000..size-1`
(direct mapping).

| Register               | Offset | Contents |
|------------------------|--------|----------|
| `PCI0_BAR0_REMAP`      | +0xC48 | PCI address = CPU address, 0 |
| `PCI0_BAR0_SIZE`       | +0xC8C | Size mask (e.g. 0x0FFFFFFF for 256 MiB) |
| `PCI0_MEM_INTERNAL_0`  | +0xC08 | BAR0 function config (enable, snoop, etc.) |

PCI1 mirrors at +0x8C48 etc.

## Interrupt controller

The MV64361's main interrupt controller lives at `@+0x0C68..0x0C74`.
It has 32 main cause / 32 main mask bits plus 32 high-cause / 32
high-mask bits. Each bit corresponds to an internal unit (PCI0,
PCI1, GbE0, GbE1, I2C, IDMA, timer, GPP groups).

For Pegasos2, the CPU is the master interrupt target. The
firmware programmes:

- `MAIN_INT_MASK_LOW` / `MAIN_INT_MASK_HIGH`: which units can
  interrupt.
- `CPU_INT_MASK_LOW`  / `CPU_INT_MASK_HIGH`: which main bits
  propagate to the CPU's external interrupt line.

During Phase 1, mask all interrupts. Phase 2 selectively unmasks
as drivers are initialised.

## Timer / counter

Four free-running timers at `@+0x0850..0x085F`. The firmware uses
timer 0 as the millisecond-tick source for `get-msecs`:

- Load timer 0 with `bus-clock / 4000` (one tick per ms).
- Enable the timer (set enable bit in `TIMER_CTRL`).
- Unmask the timer interrupt; install handler that increments a
  software millisecond counter.

The MPC7447's Decrementer SPR is ALSO a valid tick source and
simpler (no MV64361 plumbing); the firmware SHOULD prefer the
Decrementer for simplicity. The MV64361 timer is useful only if
the OS wants fine-grained timers for other purposes.

## Tests

1. On a real Pegasos2 with 2× 512 MiB DDR DIMMs, boot correctly
   with 1 GiB detected (`/memory/reg` = `0x00000000 0x40000000`).
2. On QEMU with `-m 512`, boot with 512 MiB detected.
3. With one DIMM populated, boot correctly with the installed
   size. With zero DIMMs, boot fails cleanly with a diagnostic
   on UART1.
4. PCI0 memory window decode: a read from
   `0xC0001000` (PCI0 mem + 0x1000) must issue a PCI memory read
   transaction on bus 0.
5. PCI1 I/O window decode: a read from `0xFE000000` must issue a
   PCI I/O read on bus 0 of PCI1.
6. Inbound DMA test: a PCI master device writing to its own BAR-
   mapped DMA area can reach DRAM at offset 0.
7. Interrupt routing test: triggering a GbE0 RX interrupt
   delivers an External Interrupt to the CPU and the handler
   finds the correct cause bit set.
