# 01 — CPU init: MPC7447/7447A bring-up

Phase 1 starts with the CPU in its reset state. This chapter
specifies the minimum CPU configuration the firmware must
establish before jumping into the Forth runtime.

## PVR detection

Read SPR 287 (PVR, Processor Version Register). The expected
values on Pegasos II:

| Upper 16 bits | Core                   | Notes |
|----------------|------------------------|-------|
| 0x8002         | MPC7447 / 7447A        | G4e core; the shipped CPU on most Pegasos II boards. |
| 0x8003         | MPC7455 / 7457         | G4 with on-chip L3 controller; some later Pegasos II builds. |
| 0x8000         | MPC7400 / 7410         | First-gen G4; unsupported but detectable. |

The firmware MUST support 0x8002 and 0x8003 families and SHOULD
at least detect 0x8000 and refuse to boot with a clear message.

The low 16 bits distinguish sub-revisions (e.g. 0x0102 = 7447A
rev 1.2 as seen in stock boot trace). Log the exact value in the
banner for diagnostic purposes.

## Reset vector

The MPC7447 hard-reset vector is `0xFFFxxxxx` depending on the
HRESET_HIGH state and the HID0[NHR] setting. On Pegasos II the
reset vector lives in flash, which is mapped to the top 512 KiB of
the 32-bit address space. The bootstrap MUST:

1. Be position-independent for the first few instructions. A
   `b .` loop at the reset vector while setting up MSR is
   acceptable; a long jump to an absolute address inside flash
   works once MSR[IP] is cleared (exception vectors at 0x00000xxx
   vs 0xFFF00xxx).
2. Immediately disable interrupts (MSR[EE] = 0).
3. Switch exception vectors to low memory (MSR[IP] = 0) after
   exception handlers are installed.

## Cache configuration

### L1 caches

On reset both L1 caches are disabled and invalid. The firmware:

1. Invalidates the I-cache (`HID0 |= ICFI`; wait; `HID0 &= ~ICFI`).
2. Enables the I-cache (`HID0 |= ICE`).
3. Invalidates the D-cache via `dcbi` on each line, or uses
   `HID0[DCFI]` where supported.
4. Enables the D-cache (`HID0 |= DCE`) only after memory is set up.

Set `HID0[BHT]` (branch-history table enable) for performance.
Set `HID0[BTCD]` (branch-target cache disable) = 0 to enable BTC.

### L2 cache

The 7447 has an integrated 512 KiB L2 cache. Enabling it is done
via L2CR (SPR 1017). The firmware should:

1. Write L2CR with bits: L2E=0 (disabled), L2PE=1 (parity enable
   if supported), L2I=1 (invalidate); wait for L2I to clear.
2. Once memory is initialised and the decompressor has run, re-
   write L2CR with L2E=1.
3. Clear L2I and L2HWF before handing off to the OS.

### L3 cache (7457 only)

Handled via L3CR (SPR 1018). Similar init sequence. The stock
Pegasos II firmware supports this; the rewrite should match.

## BATs (Block Address Translation)

During Phase 1 before DRAM is programmed, only the flash and the
MV64361 register bank are guaranteed to be accessible. Set up
BATs that map at least:

| BAT | Virtual base | Physical base | Size | Permissions | Purpose |
|-----|---------------|----------------|------|--------------|---------|
| IBAT0 | 0xFFF00000  | 0xFFF00000     | 1 MiB | RX, I=1, G=1 | Flash (execute bootstrap) |
| DBAT0 | 0xFFF00000  | 0xFFF00000     | 1 MiB | RW, I=1, G=1 | Flash (to read rodata during decompress) |
| DBAT1 | 0xF1000000  | 0xF1000000     | 1 MiB | RW, I=1, G=1 | MV64361 registers |
| DBAT2 | 0x00000000  | 0x00000000     | 256 MiB | RW, I=0 | DRAM (after it's up; enable only after SDRAM init) |
| IBAT1 | 0x00000000  | 0x00000000     | 256 MiB | RX, I=0 | DRAM execution (for the Forth runtime) |

The size-256-MiB BATs assume the firmware targets a 256 MiB
minimum DRAM. On larger systems, use 512 MiB BATs (requires MMU
TLB entries in addition, as BATs cap at 256 MiB per entry on the
7447).

After Phase 1, the Forth runtime may replace these BATs with a
more elaborate mapping including a larger DRAM cover and unmapped
regions for guest OS use.

## MSR setup

Initial MSR during Phase 1:

| Bit | Symbol | Value | Meaning |
|-----|--------|-------|---------|
| 13  | PR     | 0     | Supervisor |
| 14  | FP     | 0     | FPU off (until caches are up) |
| 15  | ME     | 1     | Machine-check enable |
| 16  | FE0    | 0     | — |
| 17  | SE     | 0     | — |
| 18  | BE     | 0     | — |
| 19  | FE1    | 0     | — |
| 21  | IP     | 0     | Exception vectors at 0x000xxxxx (after install) |
| 25  | IR     | 1     | Instruction translation on |
| 26  | DR     | 1     | Data translation on |
| 27  | RI     | 1     | Recoverable interrupt |
| 15  | EE     | 0     | Interrupts disabled |

After Phase 2 hand-off, the Forth runtime re-enables MSR[EE] once
it's safe (stacks and exception handlers fully ready).

## Exception vectors

Install vectors at 0x00000100..0x00000F00 as soon as DRAM is up.
The firmware must provide handlers for at least:

| Vector | Purpose |
|--------|---------|
| 0x00100 | System reset |
| 0x00200 | Machine check |
| 0x00300 | DSI (data storage interrupt) |
| 0x00400 | ISI (instruction storage interrupt) |
| 0x00500 | External interrupt |
| 0x00600 | Alignment |
| 0x00700 | Program (illegal instruction, FPU trap, trap) |
| 0x00800 | FPU unavailable |
| 0x00900 | Decrementer |
| 0x00C00 | System call |
| 0x00D00 | Trace |
| 0x01300 | Instruction address breakpoint |

Handler behaviour:
- Fatal exceptions (MC, DSI, ISI, Alignment, Program) log the
  register state and drop to a "firmware panic" prompt on UART1,
  NOT the Forth prompt (which may not be usable if the fault is
  severe).
- External interrupt: dispatch to the MV64361 IC, which in turn
  dispatches to the VT8231 PIC, which in turn dispatches to the
  registered device-driver handler. See `04-southbridge.md`.
- Decrementer: update the millisecond counter; re-arm for the
  next tick.
- System call: IEEE-1275 client-interface entry. See
  `06-client-interface.md`.

## Clock detection

Stock firmware reads from the Winbond W83194 to configure FSB.
Failure is tolerated (the boot-strap log shows "Reading W83194:
FAILED. Setting Front Side Bus to 133MHz... FAILED." and
proceeds).

The new BIOS should:

1. Attempt W83194 programming via VT8231 SMBus (address 0x69).
2. If it succeeds, set the target FSB (typical: 133 MHz for
   MPC7447, 166 MHz for some 7447A boards).
3. If it fails, use a default FSB derived from the reset-strap
   pins.
4. Compute the effective CPU clock as FSB × CPU_MULTIPLIER (read
   from HID1[PLL_CFG] per the MPC7447 user manual).
5. Record the result in the handoff struct at field
   `cpu_clock_hz` and `bus_clock_hz`.

## Time base

The PPC time base (SPR 268:269) is used for fine-grained timing.
The firmware MUST:

1. Write 0 to TBU and TBL at cold boot.
2. Expose `get-msecs` by reading the TB and dividing by
   (bus-clock / 1000000 / 4) — the time base increments at
   bus-clock/4.
3. Reset the decrementer (SPR 22) to produce a 1 ms tick for the
   external-interrupt handler.

## FPU

Enable the FPU (`MSR[FP] = 1`) before Phase 2 so Forth can use
floating point. Initialise FPSCR to a sane state (all exceptions
masked; IEEE-754 default rounding).

AltiVec is optional; if enabled (`MSR[VEC] = 1`), the firmware
MUST save/restore VRs on exception entry. The stock firmware does
not use AltiVec; the rewrite may keep it disabled unless needed.

## Handoff to Phase 2

After CPU init is complete and DRAM is up (see
`02-memory-controller.md`), the bootstrap decompresses the Forth
core to its load address and jumps to its entry point. At the
jump:

- MSR as above.
- CPU and FSB clocks recorded in handoff struct.
- L1 I-cache enabled; L1 D-cache enabled (after DRAM is stable);
  L2 invalidation done, L2 enable pending.
- r3 = pointer to `struct pegasos2_handoff` (see `00-overview.md`).
- r4..r12 = undefined.

## Tests

1. PVR read returns 0x8002xxxx or 0x8003xxxx on real hardware and
   QEMU.
2. Machine-check exception with a deliberate bad load to the ROM
   high region triggers the panic handler and prints register
   state on UART1.
3. `get-msecs` returns monotonically increasing values at
   approximately 1 kHz.
4. On a deliberate illegal-instruction trap, the Program
   exception handler catches it and prints diagnostic info
   without locking up the system.
5. Enabling/disabling L1 D-cache via a firmware word does not
   corrupt in-flight DRAM transactions.
