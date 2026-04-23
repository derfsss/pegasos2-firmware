# Questions for spec-author maintainer

Per `CLEAN-ROOM-BOUNDARY.md` rule 3, when implementation reveals
spec ambiguity or conflict with a validated reference (here: QEMU's
pegasos2 emulation), the impl agent files a question and does NOT
edit `docs/` directly. This file collects those questions.

Each entry should record: the spec location, what was found, what
the implementation did as a workaround, and what resolution is
suggested.

---

## Q1. VT8231 sits on PCI1, not PCI0 (spec 04 line 6)

**Spec claim:** `docs/04-southbridge.md` line 6:

> It sits on PCI0 (community convention: slot 0xC = device 12) as a
> multi-function device with seven functions.

**Observed:** On QEMU `-M pegasos2` (v10.2.2), VT8231 is attached to
MV64361's PCI1 host bridge at bus 0, dev 0x0C, fn 0. Config cycles
through PCI0's config register pair (0xCF8/0xCFC) return "empty";
config cycles via PCI1's config register pair (0xC78/0xC7C) reach
`vt8231-isa 00:0c.0` and succeed.

**Impl workaround:** `reset.S` uses PCI1 config (at MV64361 offset
0xC78/0xC7C) and UART1 physical address `0xFE0003F8` (PCI1 I/O base
+ ISA 0x3F8), not `0xF80003F8`.

**Suggested resolution:** Either (a) update spec 04 to say PCI1, or
(b) confirm the placement is correct on real hardware and update
the spec to describe both emulator and real-HW placements.

---

## Q2. PCI1 config register offsets (spec 03 lines 32-33)

**Spec claim:** `docs/03-pci.md` table at lines 32-33:

| `0xF1008CF8` | PCI1 Configuration Address register |
| `0xF1008CFC` | PCI1 Configuration Data register    |

**Observed:** QEMU's `hw/pci-host/mv64361.c` model treats offset
0x8CF8 as unimplemented (falls through to the `default:` arm of its
write dispatch; `-d unimp` logs it silently). The offsets that
QEMU's model actually dispatches as PCI1 CA/CD are:

| MV64361 offset | Register |
|----------------|----------|
| 0xC78          | PCI1 Configuration Address |
| 0xC7C          | PCI1 Configuration Data    |

Spec 03 lists 0xC78/0xC7C at lines 34-35 as "PCI0 internal self-
config register" — a distinct feature. QEMU does not appear to
implement that internal-self-config path at 0xC78; its `mv64361.c`
uses 0xC78 for PCI1 regular config instead.

**Impl workaround:** `reset.S` writes PCI1 CA/CD at 0xC78/0xC7C.

**Suggested resolution:** Spec author should re-check the Marvell
MV64361 Programmer's Reference. Possible outcomes:

1. The spec is right for real HW and QEMU's model is wrong / uses
   a compatibility-alias for a different chip variant.
2. The spec incorrectly documents the internal self-config offsets.
   Real HW uses 0xC78/0xC7C as PCI1 regular config (matching QEMU),
   and the "mirror at +0x8000" convention is folklore.
3. Both offsets work on real HW (one is an alias); only QEMU
   restricts to 0xC78.

The impl code should eventually be made to work on real hardware
too; knowing which offset is authoritative matters for that.

---

## Q3. VT8231 UART1 enable chain (spec 04 lines 47-63)

**Spec claim:** `docs/04-southbridge.md` lines 47-63 describe the
VT8231 SuperIO "enter-extended-function-mode, index, data, exit"
access sequence for programming UART1 enable/disable and I/O base,
citing the VT8231 datasheet §3 without reproducing specifics.

**Observed:** On QEMU VT8231 emulation, UART1 does not respond at
ISA port 0x3F8 at reset even after PCI_COMMAND I/O decode is set.
Two additional pokes are required:

1. VT8231 fn 0 PCI cfg register `0x50` bit 2 = 1 ("SuperIO config
   I/O ports enable"). Until this is set, writes to ISA 0x3F0/0x3F1
   do not reach the SuperIO config window.
2. SuperIO index 0xF2 ("Function select") bit 2 = 1. Reset default
   is 0x03 (bits 0,1 set, UART1 bit 2 clear). Writing 0x07 enables
   UART1.
3. SuperIO index 0xF4 at reset = 0xFE, which the QEMU model
   interprets as `(0xFE & 0xFE) << 2 = 0x3F8`. No change needed.

**Impl workaround:** `reset.S` performs all three. The "enter/exit
extended-function-mode" magic writes the spec alludes to are not
required on QEMU — writes to 0x3F0/0x3F1 are directly routed once
PCI cfg 0x50 bit 2 is set.

**Suggested resolution:** Spec 04 should explicitly name:
- The PCI cfg 0x50 bit 2 SuperIO-unlock (gate for the config
  window on QEMU; likely present on real HW too).
- The "Function select" register at SuperIO index 0xF2 and the
  UART1 enable bit.
- Whether the classic VIA "write 0x87 twice to unlock" magic is
  needed on real HW or a QEMU-emulation shortcut only.

---

## Q4. M48T59 NVRAM is not emulated on QEMU pegasos2 (spec 08)

**Spec claim:** `docs/08-nvram.md` §"Tests" item 7:

> Under QEMU, using `-drive if=mtd,file=nvram.bin,format=raw` (or
> the machine's equivalent), the NVRAM state must persist across
> emulator restarts.

And `docs/08-nvram.md` §"M48T59 access" documents ISA I/O at
ports 0x74 / 0x75 / 0x77 as the access path, implying the chip is
reachable through the VT8231 ISA bus.

**Observed:** QEMU 10.2's pegasos2 machine does NOT instantiate an
isa-m48t59 device. `hw/ppc/pegasos.c` creates the MV64361 and
VT8231 south bridge but leaves NVRAM to the RTAS hypercalls
(`RTAS_NVRAM_FETCH` / `_STORE`) that VOF-using guests call. The
only persistent state is the VT8231's built-in mc146818-compatible
RTC (accessed by the QEMU property alias `rtc-time`).

`qemu-system-ppc -M pegasos2 -drive if=mtd,file=... ,format=raw`
errors out with "machine type does not support if=mtd,bus=0,unit=0".

**Impl workaround:** Ship a correct-per-spec M48T59 driver
(machdep/pegasos2/m48t59.c) and wire it into the SF
`machine_nvram_*` hooks. Reads of the VT8231 ISA window at
0x74/0x77 on QEMU land on unmapped I/O space and return 0, so the
SF magic check (0xBE 0xEF) fails on every boot and SF falls back
to compile-time defaults — same observable behaviour as the
pre-driver stubs. The code path is only executable end-to-end on
real Pegasos II hardware. In-session `setenv` + `printenv`
round-trip works because SF caches NVRAM in `g_nvram_image` after
the first load_nvram() call; cross-boot persistence requires real
HW.

**Suggested resolution:** Either:

1. Update spec 08 §"Tests" item 7 to acknowledge that pegasos2 in
   upstream QEMU has no M48T59 backing, and real-HW-only testing
   is the current baseline for NVRAM.
2. Sponsor a QEMU patch adding `isa-m48t59` to the pegasos2
   machine at ISA base 0x74 (the device already exists as
   `hw/rtc/m48t59-isa.c`; the wiring into `hw/ppc/pegasos.c`
   would be ~5 lines). This would make cross-boot NVRAM
   persistence testable without real hardware.

Related downstream: the documented "`-drive if=mtd,...`" syntax
would need a corresponding `drive_add` or `isa_register_ioport`
wiring in the pegasos2 machine realize function.

---

## Q5. MV64361 ExtInt cascade produces storm on UART RX under QEMU

**Spec claim:** `docs/02-memory-controller.md` §"Interrupt controller"
and `docs/04-southbridge.md` §"Legacy devices" together imply a
standard cascade: VT8231 i8259 → MV64361 main-IC cause bit →
CPU external-interrupt line, ack'd via specific-EOI to i8259 and
RBR read on the 16550 UART.

**Observed (attempted in-session work, not committed):** A
straightforward UART-RX-driven interrupt path on QEMU pegasos2
produces a ~300 kHz external-interrupt storm (~1.4M vectors in
4 seconds), SRR0 locked to the caller's `uart_poll_rx` at
`mmio_read8(LSR)`. Attempted configurations:

1. Edge-triggered GPP (MV64361 default): handler drains UART,
   EOIs master i8259 (0x20 -> 0x20), then writes
   `GPP_INT_CAUSE &= ~(1<<31)` to clear the latched main-IC
   cause bit 59. QEMU storms immediately post-rfi.
2. Level-triggered GPP via `CUNIT_ARBITER_CONTROL_REG` bit 10.
   Same storm.
3. Omitting the firmware i8259 init (ICW1..ICW4) and relying on
   QEMU's default PIC state: input is delivered via the polling
   fallback (no storm), but no interrupt-driven path.

Examining `hw/pci-host/mv64361.c mv64361_gpp_irq`, the level-mode
auto-clear path at line 857 reads
`!(val & 0xff << b)` which, because `<<` binds tighter than `&`,
computes `val & (0xff << b)`. For GPP31 this becomes
`val & (0xff << 3) = val & 0x7F8`. That masks bits 3..10 of val,
NOT bits 24..31 as byte-selection would suggest. The clear only
triggers when bits 3..10 happen to be zero.

**QEMU is not the variable we get to change.** The original bPlan
Pegasos2 BIOS boots on this exact QEMU model, so whatever QEMU
does IS the target behaviour -- our replacement firmware must
accommodate it, same as we must accommodate real hardware. The
question is what register dance the original BIOS performs that
avoids the storm. Clean-room rules (no binary inspection) mean we
infer it from docs, QEMU source, and experimentation.

**Impl workaround:** None committed. The ExtInt dispatcher
infrastructure (commit `0e32580`, ExtInt E1) remains active but
with no registered handler -- no interrupts route to CPU,
polling in `failsafe_read` remains the sole input path and
works. The commit sequence at `9129467` is the last clean state.

**Paths forward (not yet explored):**

1. **Level-mode via CUNIT_ARBITER_CONTROL_REG + careful timing.**
   My attempt wrote CUNIT bit 10 then immediately unmasked GPP
   and ei_install'd. Maybe the CUNIT write needs a barrier or
   an explicit MV64361 read-back before it takes effect. Or the
   order of (CUNIT write, GPP_INT_CAUSE clear, GPP_INT_MASK0 set,
   ei_install) matters.
2. **Explicit MAIN_INT_MASK gating.** Spec 02 line 197-200 names
   both `MAIN_INT_MASK_LOW/HIGH` (which internal units can
   interrupt) and `CPU_INT_MASK_LOW/HIGH` (which propagate to
   CPU). QEMU exposes CPU_INT_MASK at 0x014/0x01C; MAIN_INT_MASK
   isn't in mv643xx.h near those offsets. If there's a separate
   main-mask layer, our config may leak interrupts that real HW
   would gate.
3. **Don't use UART RX as the first consumer.** A Forth REPL is
   happy with polled input. The natural first consumer is either
   a timer source (MV64361 timer 0 at cause bit 8) or an OS-side
   event delivered once the spec-07 boot loader exists.
4. **Polling remains fine.** SF's Annex A "serial" contract is
   explicitly polled; interactive REPL round-trips work without
   any ExtInt activation. The E1 dispatcher can stay dormant
   until a consumer genuinely needs it.

Related: any future first-consumer commit should plan for
verifiable single-interrupt round-trip evidence before adding a
ring-buffer consumer path -- e.g. a phase1 self-test that arms
the cascade, injects one UART byte, verifies handler increments
a counter exactly once.

---

## Q6. MV64361 IC register offsets: spec 02 vs QEMU

**Spec claim:** `docs/02-memory-controller.md` line 189:

> The MV64361's main interrupt controller lives at `@+0x0C68..0x0C74`.

**Observed:** QEMU 10.2 emulates the main-IC at the offsets in
`hw/pci-host/mv643xx.h`:

- `MV64340_MAIN_INTERRUPT_CAUSE_LOW  = 0x004`
- `MV64340_MAIN_INTERRUPT_CAUSE_HIGH = 0x00C`
- `MV64340_CPU_INTERRUPT0_MASK_LOW   = 0x014`
- `MV64340_CPU_INTERRUPT0_MASK_HIGH  = 0x01C`
- `MV64340_CPU_INTERRUPT0_SELECT_CAUSE = 0x024`

The spec's `@+0x0C68..0x0C74` range in QEMU's layout is
unassigned; 0xC78/0xC7C are the PCI1 config-register pair. Writes
to 0xC68..0xC74 would hit nothing on QEMU.

**Impl workaround:** We use the QEMU offsets (0x004..0x024 via
`MV_IC_MAIN_CAUSE_LOW`/etc. in `mv64361.h`). This matches the
Marvell Discovery II Programmer's Reference, so real HW should
agree with QEMU. The spec is likely either a typo or a stale
reference to an earlier Discovery-series chip.

**Suggested resolution:** Update `docs/02-memory-controller.md`
§"Interrupt controller" to name the actual Discovery II register
offsets (0x004..0x024 + the 0xF100..0xF11x GPP plane) or cite the
Marvell Programmer's Reference by table number so future
impl agents don't spend time searching for registers at 0xC68.
