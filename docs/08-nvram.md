# 08 — NVRAM layout and environment variables

The Pegasos II NVRAM is an ST M48T59 TimeKeeper: 8 KiB battery-
backed SRAM plus an integrated real-time clock. On-board it is
wired into the VT8231's ISA address space. The firmware uses it to
persist IEEE-1275 environment variables, `nvramrc`, and device
aliases.

## M48T59 access

ISA I/O access through two 16-bit address-index / 8-bit data
registers mapped into the VT8231 SuperIO region:

| I/O port | Name            | Access |
|----------|-----------------|--------|
| 0x74     | NVRAM index low  | write-only |
| 0x75     | NVRAM index high | write-only |
| 0x77     | NVRAM data       | read/write |

To read byte at NVRAM offset `N` (0..8191):

```
out 0x74, N & 0xFF
out 0x75, (N >> 8) & 0xFF
in  al, 0x77
```

The RTC fields (seconds/minutes/hours/day/date/month/year/century)
live in the top 8 bytes at NVRAM offsets 0x1FF8..0x1FFF. They are
BCD-encoded.

## NVRAM partitioning

IEEE-1275 §A.5.2 defines a "NVRAM partition" format using a 16-
byte header per partition. Each partition has a type byte, a
checksum byte, a two-byte length (in 16-byte blocks), and a 12-
character name.

The M48T59's 8 KiB is divided as:

| Offset range     | Size | Partition type | Name            | Purpose |
|-------------------|------|----------------|------------------|---------|
| 0x0000..0x01FF   | 512 B | — (raw)       | — | OF private — scratch, last-selftest, boot-counter |
| 0x0200..0x05FF   | 1 KiB | 0x70 system   | `system`        | Environment variables |
| 0x0600..0x17FF   | 4.5 KiB | 0x71 freepart | `free`          | Unused, reservable by OS |
| 0x1800..0x1FF7   | 2 KiB | 0x72 OS-specific | `system`     | OS-selected (Linux uses this for its own settings) |
| 0x1FF8..0x1FFF   | 8 B  | — (raw)       | — | RTC timekeeping registers |

Partitions before 0x1FF8 can be resized by the firmware at
build time; the RTC bytes are fixed by the M48T59 hardware.

The system partition at 0x0200 stores OF env vars as ASCII
`name=value` lines, each line terminated by `\n`. The partition
length in its 16-byte header specifies the total bytes allocated
(including the header).

## NVRAM write semantics

Writes to the M48T59 are non-volatile immediately; there is no
flush concept. The firmware must:

1. Compute the new partition contents in RAM.
2. Verify the new length fits the partition's allocation.
3. Write the new bytes to NVRAM (byte-by-byte; no block write is
   supported by the chip).
4. Recompute the partition's checksum and write the header.

Concurrent writes are not possible because the firmware is single-
threaded; OSes accessing NVRAM via the client interface must not
call `setenv` concurrently with each other. Document this in the
client-interface spec.

## Environment variables

The following env vars are required; defaults listed are what a
fresh build's NVRAM should contain on first boot.

| Name                | Type     | Default        | Purpose |
|---------------------|----------|----------------|---------|
| `auto-boot?`        | bool     | `false`        | If true, evaluate `boot-command` after Phase 2. Default false so users can interact without race. |
| `auto-boot-timeout` | int (ms) | `500`          | Pause window for a keypress to abort auto-boot. |
| `boot-command`      | string   | `boot`         | Command to run when auto-booting. |
| `boot-device`       | string   | `cd hd`        | Space-separated list of devices/aliases to try. |
| `boot-file`         | string   | (empty)        | Specific file to load from boot-device. |
| `diag-switch?`      | bool     | `false`        | Enable extended diagnostics. |
| `diag-device`       | string   | `net`          | Device for diag mode. |
| `diag-file`         | string   | `diag`         | File for diag mode. |
| `input-device`      | string   | `serial`       | **Default `serial`, not `keyboard`**, so systems without a PS/2 or USB keyboard still show a prompt. This is the fix for bug #3d. |
| `output-device`     | string   | `serial`       | Same reasoning. |
| `screen-#rows`      | int      | `0`            | 0 = auto (largest allowable). |
| `screen-#columns`   | int      | `0`            | 0 = auto. |
| `inverse-video`     | bool     | `false`        | — |
| `oem-banner`        | string   | (empty)        | — |
| `oem-banner?`       | bool     | `false`        | — |
| `oem-logo`          | bytes    | (empty)        | — |
| `oem-logo?`         | bool     | `false`        | — |
| `use-nvramrc?`      | bool     | `false`        | — |
| `nvramrc`           | string   | (empty)        | Forth script evaluated at boot if `use-nvramrc?` is true. |
| `fcode-debug?`      | bool     | `false`        | — |
| `security-mode`     | string   | `none`         | — |
| `security-password` | string   | (empty)        | — |
| `last-selftest`     | string   | (empty)        | Firmware-managed; human-readable last-selftest result. |
| `enable-parallel?`  | bool     | `false`        | **New in our rewrite.** Gate the VT8231 parallel-port remap to avoid QEMU emulation bug 10090. |
| `emulator-verbose?` | bool     | `false`        | **New.** Global gate for the x86-emulator diagnostic log. Off by default so normal boots are quiet. |

### Changes from the stock firmware's defaults

The original firmware had `input-device=keyboard` and
`output-device=screen` as defaults. Our new BIOS uses `serial` for
both, so a headless system comes up to a usable prompt on UART1
out of the box.

Users who DO have a display can re-point with
`setenv input-device keyboard` followed by `setenv output-device
screen`.

## Device aliases

The `/aliases` node is populated at Phase 2 from the
`devalias`es stored in NVRAM (if `use-nvramrc?` includes a
`devalias` command) plus these built-in defaults:

| Alias   | Default target |
|---------|----------------|
| `cd`    | `/pci@<pci0>/isa/ide@1/cd@0` |
| `hd`    | `/pci@<pci0>/isa/ide@0/disk@0` |
| `serial`| `/pci@<pci0>/isa/serial@3F8` (COM1) |
| `ttya`  | alias of `serial` |
| `ttyb`  | `/pci@<pci0>/isa/serial@2F8` (COM2) |
| `keyboard` | `/pci@<pci0>/isa/keyboard@60` |
| `screen`| (unresolved until `install-console` probes a display) |
| `net`   | `/pci@<pci1>/ethernet@B` (SysKonnect on-board — real HW; QEMU uses whatever is wired via `-device`) |

## Firmware's own NVRAM state

In addition to the standard env vars, the firmware keeps these
internal state fields in the raw partition at offsets 0..0x1FF:

| Offset | Size | Name            | Purpose |
|--------|------|------------------|---------|
| 0x00   | 4 B  | magic            | `"PGFW"` (0x50474657) |
| 0x04   | 4 B  | cold-boot count  | Incremented each cold boot; used for wear-levelling of partition writes. |
| 0x08   | 4 B  | last-boot-millis | Timestamp of last successful boot. |
| 0x0C   | 4 B  | flags            | Bit 0: last boot reached `ok`. Bit 1: last boot ran `boot-command`. |
| 0x10   | 16 B | reserved         | — |
| 0x20   | 32 B | last-selftest    | Null-terminated string of the last self-test result. |
| 0x40   | 0xC0 | (unused)         | — |
| 0x100  | 0xFF | (reserved)       | For future use. |

## NVRAM editor (`nvedit` and friends)

Per the SmartFirmware manual, the editor provides an in-memory
buffer initialised from `nvramrc` and an emacs-subset editor:
`^B`/`^F` (char), `^P`/`^N` (line), `^A`/`^E` (bol/eol), `^D`
(delete), `^K` (kill line), `^C` (exit without save). The words
`nvstore` / `nvquit` commit or discard the buffer; `nvrun`
evaluates it as Forth code without committing.

## Tests

1. Fresh build boots to `ok` prompt on UART1 with no display. This
   verifies the `input-device=serial / output-device=serial`
   default.
2. `setenv auto-boot? true`; reboot; the firmware auto-boots after
   `auto-boot-timeout` ms or on any keypress at the prompt.
3. `setenv diag-switch? true`; reboot; extended diagnostics run.
4. `nvedit` + `nvstore` correctly persist an `nvramrc` script; on
   reboot the script evaluates.
5. Writing a partition larger than its allocation must return an
   error without corrupting the NVRAM.
6. Cold-boot count increments monotonically; OS access to
   `/nvram/cold-boot-count` via client interface reflects the
   current value.
7. Under QEMU, using `-drive if=mtd,file=nvram.bin,format=raw`
   (or the machine's equivalent), the NVRAM state must persist
   across emulator restarts.
