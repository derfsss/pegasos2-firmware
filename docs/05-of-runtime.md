# 05 — OpenFirmware runtime: Forth engine, device tree, startup

This chapter specifies the Forth-based OpenFirmware runtime that
lives in Phase 2 of the boot flow. It is the OS-visible half of the
firmware and must conform to IEEE-1275-1994 plus errata (§A "Core
Requirements" and §H.1..H.4 "Bindings").

## Forth engine

The runtime must provide a complete ANS-Forth-subset interpreter
with IEEE-1275 extensions. The core model:

- 32-bit cell size. Double-cell words (`D+`, `D-`, `*/`) available.
- Separate data stack and return stack. Stack depth: no
  architectural limit; practical runtime reserves at least 8 KiB
  per stack.
- Dictionary lives in the firmware heap. NVRAM-backed `nvramrc` is
  evaluated as if typed at the prompt.
- Exception model: `throw` and `catch` for error propagation per
  ANS-Forth §9.
- Byte-addressable memory model; word access is naturally aligned
  with BE ordering.

### Required core words

The following ANS-Forth and IEEE-1275 words must be implemented.
This list is not exhaustive — consult IEEE-1275 §7 for the full
set. Implementations may be in Forth itself or in PPC assembly for
performance, as long as the observable behaviour matches.

**Stack manipulation:** `DUP DROP SWAP OVER ROT ?DUP PICK ROLL >R R>
R@ 2DUP 2DROP 2SWAP 2OVER DEPTH`.

**Arithmetic:** `+ - * / MOD /MOD */ */MOD ABS NEGATE MIN MAX
LSHIFT RSHIFT AND OR XOR NOT INVERT 1+ 1- 2* 2/`. Also the
unsigned-and-double forms `U< U> UM* UM/MOD`.

**Comparison & flow:** `= <> < > <= >= 0= 0< 0> IF ELSE THEN CASE
OF ENDOF ENDCASE BEGIN UNTIL WHILE REPEAT AGAIN DO LOOP +LOOP
LEAVE UNLOOP I J EXIT`.

**Memory:** `@ ! C@ C! W@ W! L@ L! +! 2@ 2! FILL MOVE COMPARE
COUNT`.

**I/O:** `EMIT TYPE CR SPACE KEY KEY? ACCEPT EXPECT`.

**Dictionary:** `CREATE DOES> VARIABLE CONSTANT : ; WORDS FIND
EXECUTE IMMEDIATE ['] [COMPILE] LITERAL POSTPONE`.

**IEEE-1275 extensions:** `FINDDEVICE DEV CD LS PWD DEVALIAS
SHOW-DEVS .PROPERTIES PROPERTIES OPEN CLOSE $CALL-METHOD
PACKAGE NEW-DEVICE FINISH-DEVICE END-PACKAGE GET-PROP ENCODE-INT
DECODE-INT ENCODE-STRING DECODE-STRING ENCODE+ ENCODE-PHYS
DECODE-PHYS SEL-DEV`.

**Timing / control:** `MS GET-MSECS MSEC@ RESET-ALL`.

### The `ok` prompt

After startup (Phase 2), if auto-boot is not selected, the
interpreter presents the user with the string `ok ` (three chars
including trailing space) and waits for input. A newline after
each successful command re-emits `ok `; an error pushes an error
message and re-emits.

The prompt is written via the active output device. See § console
routing below for the rules that device must satisfy for the `ok`
prompt to be visible.

## Device tree

The device tree is an in-memory forest rooted at `/` and populated
at startup. Nodes have a name, a parent, optional siblings, and a
property list. Properties are (name, value) pairs where the value
is an opaque byte string; standardised properties are decoded per
IEEE-1275.

### Required root children

| Node path   | Type | Purpose |
|-------------|------|---------|
| `/cpus/cpu@0` | cpu | PVR, clock-frequency, `reg` (cpu number) |
| `/memory@0`   | memory | `reg` list of (base, size) RAM banks |
| `/chosen`     | chosen | `bootargs`, `stdin`, `stdout`, `bootpath`, `mmu` |
| `/aliases`    | aliases | Device path aliases including `cd`, `hd`, `net`, `serial`, `keyboard` |
| `/options`    | options | NVRAM-backed env vars exposed as properties |
| `/openprom`   | openprom | `model` = `"SmartFirmware"` or `"Pegasos2OFW <version>"`, `version` |
| `/packages`   | packages | Supported package interfaces (disk-label, deblocker, etc.) |
| `/pci@<n0>`   | pci | See `03-pci.md`. One per MV64361 host bridge. |
| `/pci@<n1>`   | pci | Second host bridge. |

### Device aliases

Per IEEE-1275 §3.1 the `/aliases` node maps short names to full
paths. The following aliases are mandatory for OS compatibility:

| Alias     | Target (example) | Purpose |
|-----------|-------------------|---------|
| `serial`  | `/pci@...vt8231/serial@2F8` | UART used as console |
| `ttya`    | same as `serial` | POSIX-style compatibility |
| `keyboard`| `/pci@...vt8231/super-io/keyboard@60` | PS/2 keyboard if present |
| `screen`  | `/pci@.../display@X` | Default video; absent if no display probed |
| `cd`      | `/pci@...vt8231/ide@1/cd@0` | CD-ROM master on secondary IDE (AOS/AOS4 convention) |
| `hd`      | `/pci@...vt8231/ide@0/disk@0` | HD master on primary IDE |
| `net`     | `/pci@.../ethernet@X` | First Ethernet NIC |

The AmigaOS install PDF's `boot cd amigaboot.of` and `boot hd:0
amigaboot.of` commands rely on the `cd` and `hd` aliases being
defined exactly as above.

## Default startup script

Per IEEE-1275 and the SmartFirmware manual, if `use-nvramrc?` is
false or `nvramrc` is empty or mis-parses, the firmware runs the
default sequence:

```
probe-all install-console banner
```

### `probe-all` behaviour

`probe-all` walks all buses using each host bridge's
implementation-defined walker. For Pegasos2 this is the PCI walker
of `03-pci.md`, applied to both host bridges in turn.

For each discovered device, `probe-all` must:

1. Create a device-tree node with its PCI properties populated
   (vendor-id, device-id, class-code, assigned-addresses, etc.).
2. If the device is a PCI-to-PCI bridge, recurse into its
   secondary bus.
3. If the device exposes an FCode ROM (PCI Expansion ROM Header +
   FCode image type), decompress / interpret it and install the
   FCode-defined methods on the device's node.
4. If the device has a class-code indicating a legacy VGA device
   and the PCI Expansion ROM is an x86 BIOS image, invoke the x86
   emulator to initialise it. See § x86 emulation and bug #1.

`probe-all` must NOT commit partial state on failure — if an
Option ROM execution fails, the device-tree node must still exist
with its static properties populated; only the FCode-installed
methods (if any) are missing.

### `install-console` behaviour (and the fix for bug #3d)

`install-console` selects the console input and output devices. The
stock firmware picks any display-class device unconditionally, even
if that device was never initialised — this is bug #3d.

The new BIOS must implement a **health check** before committing:

```
word install-console ( -- )
    /* Try to find a viable display. */
    display := find-in-device-tree(class=display)
    if display != nil:
        open display
        if display exposes a working framebuffer word set
           (e.g. `draw-rectangle`, `write`, `fill-rectangle`) and
           returns success from a bounded self-test:
            output-device := display
            input-device  := find-in-device-tree(class=keyboard) or serial
            return
        close display

    /* Fall back to serial. */
    output-device := resolve-alias("serial")
    input-device  := resolve-alias("serial")
```

The health-check must be conservative: if the display hasn't been
initialised (e.g. its Option ROM failed under bug #1), the self-
test MUST fail, so serial is used. The health-check MUST time out
if the display hangs, so that a broken display cannot brick the
prompt.

After the console is installed, properties `/chosen/stdin` and
`/chosen/stdout` must be populated with the full paths to the
selected devices.

### `banner` behaviour

`banner` displays the firmware version, build date, and basic
system info (CPU model/clock, DRAM size, copyright). Goes to the
output device selected by `install-console`.

The banner is cosmetic but the OS may parse some variants of it
(e.g. for identifying the firmware); keep the first-line format
stable across versions. Suggested format:

```
Pegasos2 OpenFirmware <version> (<date>)
MPC7447A at <clock> MHz, <dram> MiB DDR
Copyright (c) <year> <authors>. Licensed under <license>.
```

## x86 emulation and bug #1

`probe-all` invokes the x86 emulator whenever it finds a VGA class
device (class 0x03xx) that has a PCI Expansion ROM with image type
0 (x86 BIOS). The emulator runs the Option ROM's entry point with
the BDA correctly initialised and returns when the Option ROM
executes `retf` to the caller.

The emulator is a SEPARATE IEEE-1275 package and not part of the
Forth core; but because bug #1 was catastrophic, the minimum
behavioural requirements are specified here as well:

1. All 8086 instructions must be correctly emulated.
2. All 80186/286 real-mode extensions (push/pop/pusha/popa, imul
   imm, bound, enter/leave, shift-imm) must be emulated.
3. All 386 real-mode extensions reachable via the `0x66` / `0x67`
   prefixes, the `0x0F` two-byte escape, and SIB-addressing must
   be emulated. This includes MOVZX, MOVSX, conditional moves on
   486+, and at minimum the common 0x0F xx opcodes used by VGA
   BIOSes (0x01, 0x20, 0x22, 0x80..0x8F, 0xB6, 0xB7, 0xBE, 0xBF,
   0xFE). See `09-known-bugs.md` § Bug 1 for the full required set.
4. All INT 10h AH values 0x00..0x1F must be stubbed, at minimum
   returning non-error status and for AH=0x03 returning BDA-backed
   cursor position.
5. Error reporting must go through a SINGLE diagnostic channel that
   can be globally disabled by an env var (e.g. `emulator-verbose?
   false`). Unhandled-opcode errors must include the opcode bytes
   and CS:IP of the failure for diagnostics but MUST NOT spam
   hundreds of lines for a single Option ROM run.

## Environment variables

See `08-nvram.md` for the full list, storage format, and defaults.
The Forth interpreter exposes the env vars as properties on
`/options` and provides `setenv`, `printenv`, `set-default`,
`set-defaults`, `nvedit`, `nvstore`, `nvquit`, `nvrun`, `nvalias`,
`nvunalias` per the SmartFirmware manual.

## Forth tokens and FCode

The firmware must implement an FCode interpreter capable of
executing IEEE-1275-encoded byte tokens from PCI Expansion ROMs
(image type 1) and from files loaded by `load`.

Required FCode tokens: at minimum the "core" and "required" sets
in IEEE-1275 §H.2 (about 200 tokens). FCode-encoded PCI option
cards for PPC (rare but present in some Sun and Apple hardware) may
be installed on Pegasos2 slots and must work.

The FCode interpreter must handle:
- Token 0x00..0xFF (single-byte tokens).
- Extended tokens 0x10 XX (two-byte).
- Literal tokens (b(lit), b(")) and literal strings.
- Control-flow tokens (b?branch, bbranch, bloop, b(leave),
  b(endcase)).

## Time services

The firmware must expose:

- `get-msecs` — returns a 32-bit millisecond counter that
  monotonically increases from boot. Resolution must be at least
  10 ms, ideally the MPC7447 time-base register precision.
- `ms` — the word `( n -- )` that delays for `n` milliseconds.
- The time-of-day clock from the M48T59 RTC accessible as a
  package under `/pci@.../rtc`. See `08-nvram.md`.

## Memory management

The Forth heap lives in DRAM, allocated at startup by the
bootstrap. Size: at least 4 MiB; the stock firmware uses around
2 MiB but we expect extensions to push this larger.

`claim` and `release` words manage physical memory ranges for the
client OS, per IEEE-1275. The firmware must maintain a free-list
of physical memory and honour `claim (align size virt -- base)`.

The new BIOS MUST avoid the stock firmware's "malloc pool conflict
at 0x400000" issue (noted in the amigans forum thread 10090) by
placing the Forth heap above 0x00200000 and below the start of any
address an OS loader might claim as its load address (AmigaOS 4's
Kickstart typically loads around 0x100000; place our heap above
that).

## Console routing summary

| State | Input source | Output sink |
|-------|--------------|--------------|
| Phase 1 (bootstrap)      | — | VT8231 UART1 (`0x3F8`) |
| Phase 2 pre-console-install | VT8231 UART1 | VT8231 UART1 |
| Phase 2 post-`install-console`, display healthy | keyboard@60 (PS/2) | display@X (framebuffer) |
| Phase 2 post-`install-console`, display missing/failed | VT8231 UART1 | VT8231 UART1 |
| `setenv input-device /serial` (explicit) | UART1 regardless | (unchanged) |
| `setenv output-device /serial` (explicit) | (unchanged) | UART1 regardless |

The critical guarantee: **even with no display hardware present, the
prompt MUST be usable on serial.** This is what the stock firmware
fails at, and it's the single most visible difference between our
rewrite and the original.

## Tests

1. With stock NVRAM defaults AND no display device, the firmware
   must reach an `ok` prompt on UART1 and accept commands.
2. `show-devs` at the prompt must enumerate every device present on
   all PCI buses (primary + bridges).
3. `printenv auto-boot?` must print the current value; `setenv
   auto-boot? false` must change it; on reboot, the change must
   persist (NVRAM write verified).
4. `nvedit`, `nvstore`, `nvrun` must allow interactively defining
   an `nvramrc` script and running it.
5. A synthetic FCode test ROM loaded via `load /pci/scsi/disk:fcode`
   must execute its installation code and leave device-tree
   properties as the FCode defines.
