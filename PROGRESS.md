# Implementation progress

This file is the handoff document for the next impl-agent session.
Read it after `CLAUDE.md` and `docs/START-HERE.md`.

## One-line status (2026-04-22)

Phase 1 is substantively complete on QEMU. Both headline bugs
(spec 09 Bug 1 and Bug 2) are implemented and pass their spec-
defined tests. Exception vectors + panic handler now installed
at 0x00000100..0x00001300 with MSR[IP]=0, so any Phase 2+ fault
prints a register dump on UART1 instead of vanishing into
unmapped flash. Phases 2–4 (Forth runtime, NVRAM, boot loader,
client interface) are NOT started.

## What the firmware currently does

A successful boot of `build/firmware-raw.bin` on
`qemu-system-ppc -M pegasos2 -m 512 -bios ... -serial ... -display none`
produces **1,832 bytes** of serial output containing, in order:

1. Banner with PVR (0x80020102 = MPC7447A) and DRAM round-trip OK.
2. Console address and stack pointer.
3. Exception-vector install confirmation (`MSR[IP]=0`).
4. Full PCI enumeration across both MV64361 host bridges,
   including recursion through PCI-to-PCI bridges (`-device
   pci-bridge,...` topologies render the correct tree). Each
   device prints its BAR sizes AND assigned addresses (e.g.
   `BAR0: mem32 pref size=0x01000000 -> 0x80000000`) plus the
   command-register bits it enables (`cmd: MEM IO MASTER`).
5. Synthetic x86-emulator self-test (MOV AX / 0F FE PADDB / MOV
   AX / HLT) passes.
6. bochs-VGA Option ROM POSTed to completion -- its ROM BAR is
   now assigned by the walker (typically 0x81010000 on QEMU) and
   phase1 reads that address back from config space rather than
   hard-coding one. Returns to our HLT trampoline at
   CS:IP=0x0050:0001.
7. Decrementer self-test: MSR[EE] enabled briefly, busy-spin,
   MSR[EE] disabled, accumulated tick count reported (non-zero
   delta proves 0x900 handler runs and rfi's back). MSR[ME] and
   MSR[RI] are also set at reset time.
8. Clean halt.

No `INTERNAL ERROR`, `UNHANDLED`, `Failed to emulate`, `STUCK
CS:IP`, or `!! PANIC` strings appear anywhere in the default
build's output.

Building with `-DEXCEPTION_TEST=1` added to CFLAGS triggers a
deliberate `twi 31, r0, 0` at the end of phase1, which exercises
the panic path end-to-end and produces a full register dump
(vector 0x700 Program, SRR0 = address of `twi`, SRR1 = 0x00020000
= trap bit, all 32 GPRs, LR/CTR/XER/CR/MSR/DAR/DSISR). Not
enabled in the default build.

## Commit history (as of this writing)

```
(new)    PCI walker: prefetchable-memory routing + bridge pref window
23a5632  Decrementer handler + MSR[ME]/[RI]; ms-tick API
cda478f  PCI walker: bridge MEM/IO window programming
e59c2c7  PCI walker: BAR address assignment + cmd-register enable
48ce9a7  PCI walker: BAR sizing (non-destructive probe)
cea94f8  Exception vectors + panic handler (spec 01 §Exception vectors)
d2f58fc  x86emu: 0F FE (PADDB) handler (spec 09 bullet)
c5b0807  Option-ROM execution: bochs-VGA VBIOS runs clean (Bug 1 fix)
d7ecb1   x86emu self-test (6b+6c): sys glue, DRAM-backed data/bss
c2b83c4  Integrate x86emu core into the build (compile + link only)
bb9f7a6  PCI enumeration: recursive walker with Type-1 cycles (Bug 2 fix)
0549055  Phase 1 C transition: stack in DRAM, MMIO drivers split out
24dfeba  Phase 1 DRAM stuck-bit test
cced4b5  Phase 1 banner on UART1
2ad2eec  Build skeleton: WSL toolchain, linker script, reset-vector stub
5a0fc2c  Baseline: vendor smartfirmware + x86emu, establish licensing
```

## Status against `docs/START-HERE.md`

| Step | Topic | Status |
|---|---|---|
| 1 | Build system | Done. Makefile + linker script + reset trampoline. |
| 2 | CPU init + banner on UART1 | Done on QEMU. Exception vectors installed at 0x100..0x1300 with MSR[IP]=0, MSR[ME]=1, MSR[RI]=1. Decrementer (0x900) has a real handler + millisecond tick counter; ExtInt (0x500) and Syscall (0xC00) still panic-stubbed. Real-HW init (cache invalidate, BAT setup, clock-gen probe, TB calibration) deferred. |
| 3 | DRAM init | Done on QEMU (QEMU pre-wires DRAM). Real-HW DDR init sequence (docs/02 §"DDR init sequence" steps 1–12, SPD probe, mode-register programming) is **not implemented**. |
| 4 | PCI enumeration (Bug 2 fix) | Done. Spec 03 Tests #1 + #2 pass. Full PCI resource pipeline: sizing, BAR assignment with split non-prefetch/prefetch/IO allocators, cmd-register enable, and bridge MEM/PREFETCH/IO-window programming (BASE/LIMIT at 0x20/0x22, 0x24/0x26, 0x1C/0x1D; 1 MiB / 4 KiB granularity; disabled-LIMIT<BASE encoding when a window is unused). 64-bit-above-4 GiB BAR placement not supported (the CPU can't address that region; the firmware writes high-dword=0 for 64-bit BARs). |
| 5 | VT8231 full init | Partial -- UART1 chain only. IDE, USB, AC'97, PM, SMBus, PIC are not initialised. |
| 6 | OF Forth runtime | Not started. |
| 7 | NVRAM (M48T59) | Not started. |
| 8 | Boot loader + client interface | Not started. |
| 9 | x86 emulator (Bug 1 fix) | Core delivered. bochs-VGA POSTs cleanly. 0F FE patched. Other spec-09 opcodes (0F 01 / 20 / 22) + INT 10h fallback stubs pending -- no ROM we run currently trips them. |
| 10 | Test plan | No automated harness. Manual `qemu-system-ppc ... -serial file:...` + `grep -E ...` is the current loop. |

## Architecture

### Source layout

```
machdep/pegasos2/
├── reset.S              reset-vector trampoline (data/bss/vectors copy, MSR[IP]=0, stack init, call C)
├── firmware.ld          linker script (flash @0xFFF00000, dram @0x00100000, vectors @0x00000000)
├── phase1.c             Phase-1 C entry: bring up hardware, run tests
├── exceptions.S         vector stubs at 0x100..0x1300 + common_trap + real 0x900 decrementer handler + _ms_tick_count
├── panic.c              panic_dump(): UART1 register dump for unrecoverable exceptions
├── timer.c/h            get_msecs(), timer_arm(), enable_ei()/disable_ei() MSR[EE] toggles
├── pegasos2.h           memory-map constants (flash, MV64361, PCI windows, UART)
├── io.h                 inline-asm MMIO accessors (BE + LE variants, byte)
├── uart16550.c/h        polled 16550 driver
├── mv64361.c/h          MV64361 register I/O + PCI config (both hosts)
├── vt8231.c/h           VT8231 bring-up (PCI cmd + SuperIO unlock + UART1 enable)
├── pci.h                standard PCI 2.3 constants
├── pci_walker.c/h       recursive PCI enumerator (Bug 2 fix)
├── x86_glue.c/h         x86emu sys glue (1 MiB buffer @0x00200000, I/O routing, BDA, IVT)
├── x86emu_stubs.c       libc stubs for vendored emulator (memset/cpy, printf no-op, etc.)
└── x86compat/           U-Boot compat shims (common.h, asm/types.h, asm/io.h, pci.h)
```

### Memory layout (runtime, on QEMU)

```
0x00000000..0x00001FFF   exception vectors (8 KiB, copied from flash LMA)
0x00002000..0x000FFFFF   scratch + stack (stack grows down from 0x100000)
0x00100000..0x001FFFFF   .data + .bss + panic_frame + panic_stack
0x00200000..0x002FFFFF   x86emu 1 MiB buffer (X86EMU_MEM_PADDR)
0x0000_xxxx              DRAM, 512 MiB total on -m 512
0x80000000..0xBFFFFFFF   PCI1 mem0 window (direct-mapped)
0xF1000000..0xF100FFFF   MV64361 register bank (little-endian from CPU)
0xF8000000..0xF8FFFFFF   PCI0 I/O window (after our enable)
0xFE000000..0xFEFFFFFF   PCI1 I/O window (default-enabled by QEMU)
0xFFF00000..0xFFF7FFFF   flash (512 KiB) -- our firmware here; QEMU mapping
```

### Linker layout

`.reset` at flash offset 0x100 (reset vector 0xFFF00100). `.text`
+ `.rodata` follow in flash. `.data` has LMA in flash, VMA in
DRAM; the reset trampoline copies it to DRAM before calling C.
`.bss` is NOLOAD in DRAM and zeroed by the trampoline. PPC small-
data (`.sdata*`, `.sbss*`) covered too.

## Build + test workflow

Primary toolchain: WSL Ubuntu 24.04 with `gcc-powerpc-linux-gnu`
(13.3.0) and `binutils-powerpc-linux-gnu` (2.42). Docker image
`walkero/amigagccondocker:os4-gcc11` is the fallback.

```bash
# From Windows / MSYS2 bash:
wsl.exe -- bash -c \
  "cd /mnt/c/msys64/home/rich_/Projects/Pegasos2-bios-impl && make"

# Or directly inside WSL:
cd /mnt/c/msys64/home/rich_/Projects/Pegasos2-bios-impl
make            # produce build/firmware-raw.bin (exactly 524288 bytes)
make clean
make info       # print toolchain versions
```

QEMU lives at `E:\Emulators\QEMU\QEMU_Install\qemu-system-ppc.exe`
(10.2.2). Standard test invocation (from MSYS2 bash):

```bash
timeout 6 /e/Emulators/QEMU/QEMU_Install/qemu-system-ppc.exe \
  -M pegasos2 -m 512 \
  -bios "$(pwd -W)/build/firmware-raw.bin" \
  -serial "file:$(pwd -W)/build/serial.txt" \
  -display none
```

To exercise Bug 2 (recursive walker), add:

```bash
-device pci-bridge,id=pbr1,bus=pci.1,chassis_nr=1,addr=0x5 \
-device e1000,bus=pbr1,addr=0x1
```

## Gotchas found during implementation

These are real footguns the next agent will hit if they don't
know. Most are documented inline in `SPEC-QUESTIONS.md` too.

### 1. MV64361 MMIO is little-endian from the CPU's view (on QEMU)

Writing `0x000FBDFF` via PPC-native `stw` arrives at the model as
`0xFFBD0F00`. Confirmed via `-trace mv64361_reg_write`. All 32-bit
register accesses must go through `stwbrx` / `lwbrx`. The
`mmio_{read,write}32_le` helpers in `io.h` are the right layer.
Byte accesses (`stb`/`lbz`) are endian-invariant.

### 2. Spec 04 says VT8231 on PCI0; QEMU puts it on PCI1

Consequence: UART1 is at CPU physical `0xFE0003F8`, not
`0xF80003F8`. See `SPEC-QUESTIONS.md` Q1.

### 3. Spec 03 says PCI1 config at 0x8CF8/0x8CFC; QEMU uses 0xC78/0xC7C

Consequence: use the `PCI_HOST_0/PCI_HOST_1` enum + the helper in
`mv64361.c`; don't hand-code offsets. `SPEC-QUESTIONS.md` Q2.

### 4. VT8231 UART1 is gated behind two enable flips

- PCI fn 0 cfg register 0x50 bit 2 = unlock SuperIO config ports.
- SuperIO index 0xF2 bit 2 = UART1 function-enable.
- Default 0xF4 = 0xFE which the emulator decodes as I/O base 0x3F8,
  so no change needed there.

`SPEC-QUESTIONS.md` Q3.

### 5. Default `.data` / `.bss` placement ruins global writes

The old linker script put `.data` and `.bss` inside flash. Writes
to `sys_rdb` (set by `X86EMU_setupMemFuncs`) silently dropped on
QEMU's flash emulation. Fixed in commit cd7ecb1 -- `.data` has LMA
in flash and VMA in DRAM; the reset trampoline copies it.

If a future change re-introduces this pattern (e.g. a third memory
region without matching trampoline init), global-variable writes
will silently fail again -- and this is a very hard bug to notice
because no diagnostic fires.

### 6. MV64361 `BASE_ADDR_ENABLE` bit semantics are inverted

A **1** bit means **disabled**. Reset default is `0x000FBFFF` (bit
14 clear = PCI1 I/O enabled by default). Our `mv64361_enable_pci0_io_window`
writes `0x000F3DFF` (clears bits 9 and 15 = PCI0 I/O + PCI1 mem0
enabled). See `mv64361.c` for the full mask.

If you need another window (PCI0 mem0/mem1/mem2, PCI1 mem1/mem2),
clear the corresponding bit. Region numbers:
`-trace mv64361_region_map` prints `Mapping pciN-XXX-win ...` for
each enabled window along with its number.

### 7. PCI config data register is PCI-byte-order (LE)

`pci_cfg_write32` uses `stwbrx` on the way to the data register so
the value lands on the PCI bus in the expected byte order. Already
handled by the `mv64361.c` helpers; don't hand-roll PCI config
cycles outside those helpers.

### 8. x86emu compiled with `-D__KERNEL__ -U_FORTIFY_SOURCE`

These flags apply only to `upstream/x86emu/x86emu/*.c`. They skip
the U-Boot include chain's pull of glibc `<stdio.h>/<stdlib.h>/<string.h>`
which would otherwise expand `printf` into `__printf_chk` and
expect a libc that doesn't exist here. If a new file from the
vendored tree gets added to the build, add it to the
`$(BUILD)/x86emu_%.o:` rule (Makefile line ~90).

### 9. `-Os` triggers gcc's out-of-line register-save helpers

Specifically `_restgpr_28_x` / `_restgpr_30_x` etc., which live
in `libgcc.a` that we're not linking. Keep `CFLAGS` at `-O2` or
higher. Dropping to `-Os` will surface link errors.

### 10. Reset-vector addresses: QEMU vs real hardware

Spec 00 says real-HW flash is at `0xFFF80000..0xFFFFFFFF`; QEMU
maps it at `0xFFF00000..0xFFF7FFFF`. We build for QEMU's layout.
A real-HW-ready build would need to either relocate to 0xFFF80000
or ship two copies for aliasing -- this is a known open item.

### 11. `DEC_TICKS_PER_MS` is duplicated in asm and C

The decrementer re-arm value (25000) is hardcoded both in
`exceptions.S` (the 0x900 handler's `li r4, 25000`) and in
`timer.h` (the `DEC_TICKS_PER_MS` macro). Nothing enforces
these stay in sync; if one is changed without the other, the
tick rate and the macro disagree, which only matters when the
macro is used for a duration calculation. The asm side can't
easily include timer.h (it pulls in C declarations GAS
doesn't understand), so either leave the duplication with a
cross-reference comment (current choice) or promote the
constant to a `.set` directive that both sides share via a
preprocessor pass. If the value is ever recalibrated on real
HW per spec 01 §"Clock detection", update both sites.

### 12. Exception-vector install timing

`reset.S` copies the `.vectors` section from its flash LMA to
VMA 0x00000000, then clears MSR[IP] *before* calling
`phase1_c_main`. This means exceptions fire into our handlers
for the entirety of Phase 1 C. The vectors themselves live at
0x0100..0x1300; the common save-all path is at 0x1400;
`_panic_frame` (168 bytes) and `_panic_stack` (4 KiB) are in
`.bss`. Any exception calls `panic_dump()` which prints a full
register dump on UART1 with a distinctive `!! PANIC:` prefix
(so existing test-grep patterns like `grep -E "PANIC|UNHANDLED"`
catch it), then spins.

If future code needs to take recoverable exceptions (decrementer
tick, external interrupt, syscall for the client interface),
the respective vector stub must be replaced with a real handler
that returns via `rfi` instead of falling through to
`common_trap`. The stubs' `VECTOR_STUB` macro in `exceptions.S`
is a natural split point -- replace one stub at a time without
disturbing the rest.

## Spec questions pending maintainer response

`SPEC-QUESTIONS.md` has three active items, all of which this
impl agent worked around by matching QEMU rather than spec:

- Q1: VT8231 host-bridge assignment (spec says PCI0, QEMU says PCI1)
- Q2: PCI1 config register offsets (spec says 0x8CF8/0x8CFC, QEMU says 0xC78/0xC7C)
- Q3: VT8231 SuperIO unlock sequence (spec describes generally; QEMU needs specific bits)

When the maintainer responds, reconcile `machdep/pegasos2/mv64361.h`
offset constants and `machdep/pegasos2/vt8231.c` bit choices.

## Suggested next milestones

Pick based on appetite. Each is roughly a single focused commit.

### Near-term, unblocks later work

**External-interrupt (0x500) dispatch + Syscall (0xC00)
trampoline.** MSR[ME]/[RI] are set, Decrementer (0x900) runs a
real handler that feeds `get_msecs()`, but 0x500 and 0xC00 still
panic. External-interrupt needs MV64361 main/GPP IC init -> VT8231
PIC init -> registered-handler table + dispatch logic; tested by
arming one device's IRQ and observing the handler fire. Syscall
needs the IEEE-1275 client-interface entry point (spec 06) to
dispatch to -- it's roughly 20 lines of asm but the body is the
whole client interface, so land both together.

**OF Forth runtime bring-up.** The biggest remaining piece.
`upstream/smartfirmware/bin/of/` ships the Forth interpreter and
OF device-tree framework under CodeGen's source license (same
license as our rewrite). A thin Pegasos2 machdep layer slots in;
phase1 hands off to it instead of halting. Spec 05 §"install-
console" (serial-first defaults, health-checked attach) is the
headline feature. Multi-commit: machdep shim, OF build into the
image, phase1 handoff, initial `ok` prompt.


### Mid-term, enables real work

**Forth / OpenFirmware runtime bring-up.** Biggest remaining
piece. `upstream/smartfirmware/bin/of/` has the Forth interpreter
and OF device-tree framework as starting material (CodeGen
source license, see `LICENSES/`). A thin Pegasos2 machdep layer
slots it in; phase1 then hands off to it instead of halting.

**NVRAM (M48T59).** Spec 08. ~300 lines of code, self-contained.
Enables `setenv / printenv` for future Forth configuration.

### Longer-term, stretch

**Real-hardware CPU init.** Cache invalidate/enable, BAT setup
per spec 01, MSR bits, clock-gen (Winbond W83194) probing over
SMBus, exception handler install. Most of this is pure asm or
tight C and can be tested only on hardware.

**SeaBIOS-derived VGA ROM test** (spec 09 Bug 1 Test #2 part 2).
Our current test uses Bochs VBIOS via QEMU's `-vga std`. Running
a SeaBIOS-derived ROM would exercise a different code path.

**Radeon R200 VBIOS test** (spec 09 Bug 1 Test #3). Needs a real
ROM dump from a Radeon 9200/9250; once available, feed it
through the same loader path as bochs-VGA.

## References inside this tree (in addition to `docs/`)

- `CLAUDE.md` — role briefing, clean-room rules.
- `CLEAN-ROOM-BOUNDARY.md` — the formal policy.
- `README.md` — project summary.
- `SPEC-QUESTIONS.md` — maintainer-routed spec-clarity issues.
- `VERSIONS.md` — upstream pins + local modifications.
- `LICENSES/` — CodeGen and SciTech license texts.
- `upstream/smartfirmware/` — CodeGen's SmartFirmware source
  (public), pinned at commit 06ef397.
- `upstream/x86emu/` — U-Boot 2015.d `drivers/bios_emulator/`,
  minus the atibios/vesa/biosemu/bios/besys wrappers. One local
  modification (0F FE handler) documented in `VERSIONS.md`.
