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

## Q5. MV64361 ExtInt cascade produces storm on UART RX under QEMU (RESOLVED, 4f4b1a4)

**Resolution:** Pegasos2's cascade requires a two-step configuration
that was not obvious from spec 02 / 04 alone:

1. CUnit arbiter control bit 10 must be set to place the GPP
   interrupt plane in level-triggered mode. Edge mode is QEMU's
   reset default but leaves main cause bit 59 latched after the
   source deasserts.
2. The handler must read `PCI_1_INTERRUPT_ACKNOWLEDGE_VIRTUAL_REG`
   (offset 0xCB4) as its first action. In level mode with GPP31
   asserted, this read invokes `pic_read_irq`/`pic_intack` on the
   master i8259, advancing ISR/IRR/int_out. Without this read
   the i8259 IRR bit stays set, "intr" stays high, GPP31 stays
   high, main cause 59 stays asserted, and the 0x500 vector
   re-fires at ~300 kHz.

Commit 4f4b1a4 implements both. Interactive REPL keystrokes now
route through a single interrupt each (confirmed via `-d int`:
2 EXTERNAL per 2 chars typed).

Kept as an open question for spec clarity: neither spec 02 nor
04 names the `PCI_1_INTA_VIRT` register or describes the INTA-
cycle equivalent; this was derived from QEMU source + Marvell
Discovery II register map. Spec update suggested below.

**Original (now-resolved) finding below for history.**

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

---

## Suggested replacement text for docs/02-memory-controller.md §"Interrupt controller"

Consolidates the Q5 + Q6 findings into a drop-in rewrite of the
current three-paragraph §"Interrupt controller" subsection of
`docs/02-memory-controller.md`. Ready for the spec author to
paste verbatim.

> ## Interrupt controller
>
> The MV64361's main interrupt controller lives in the low
> register bank at `@+0x004..0x024`. It exposes a 64-bit cause
> register as two 32-bit halves plus per-CPU mask and
> select-cause registers:
>
> | Offset | Name                         | Access | Notes |
> |--------|------------------------------|--------|-------|
> | 0x004  | MAIN_INTERRUPT_CAUSE_LOW     | RO     | Cause bits 0..31  |
> | 0x00C  | MAIN_INTERRUPT_CAUSE_HIGH    | RO     | Cause bits 32..63 |
> | 0x014  | CPU_INTERRUPT0_MASK_LOW      | RW     | 1 = route to CPU0 |
> | 0x01C  | CPU_INTERRUPT0_MASK_HIGH     | RW     |                   |
> | 0x024  | CPU_INTERRUPT0_SELECT_CAUSE  | RO     | Highest-priority pending |
>
> Each cause bit corresponds to an internal unit; the sources
> most relevant to Pegasos2 firmware are:
>
> | Bit    | Source                                         |
> |--------|------------------------------------------------|
> | 8..11  | MV64361 timers 0..3                            |
> | 12     | PCI0 INT aggregated (also via GPP 12..15)      |
> | 16     | PCI1 INT aggregated (also via GPP 12..15)      |
> | 56..59 | GPP plane, 8 pins per bit (P0_GPP0_7..P0_GPP24_31) |
>
> ### GPP (general-purpose pins) interrupt plane
>
> GPP 0..31 are cascade inputs that collapse into main-cause
> bits 56..59 in blocks of eight pins each. On Pegasos2 these
> are wired as:
>
> - GPP 12..15: PCI INTA..D, ORed across both host bridges, so
>   any PCI interrupt rings this band (cause bit 57 =
>   P0_GPP8_15).
> - GPP 31: aggregated "intr" output from the VT8231
>   southbridge's i8259 master PIC. Every ISA/legacy
>   interrupt (UART1/2 on IRQ 4/3, IDE on 14/15, USB, AC'97,
>   RTC, ...) arrives here (cause bit 59 = P0_GPP24_31).
>
> | Offset | Name                   | Access | Notes |
> |--------|------------------------|--------|-------|
> | 0xF108 | GPP_INTERRUPT_CAUSE    | RW     | Semantics depend on mode (below) |
> | 0xF10C | GPP_INTERRUPT_MASK0    | RW     | Per-pin enable for CPU0 route    |
> | 0xF110 | GPP_LEVEL_CONTROL      | RW     | Per-pin polarity inversion       |
> | 0xF114 | GPP_INTERRUPT_MASK1    | RW     | Per-pin enable for CPU1 route    |
> | 0xF300 | CUNIT_ARBITER_CONTROL  | RW     | Bit 10 = GPP plane level mode    |
>
> Bit 10 of the CUnit arbiter control register selects the GPP
> plane's trigger mode:
>
> - 0 (reset default): edge-triggered. Cause bits latch on the
>   rising edge of a pin and are cleared only by explicit writes
>   to `GPP_INTERRUPT_CAUSE`.
> - 1: level-triggered. Cause bits track the pin state and
>   auto-clear when the source deasserts.
>
> **Firmware must select level mode (bit 10 = 1) before arming
> any cascade input on GPP 31.** Under edge mode, an RX-ready
> pulse from the UART latches cause 59 until the firmware
> writes `GPP_INTERRUPT_CAUSE`; the clear window races the next
> edge and the handler re-fires before the write lands,
> producing an interrupt storm. Under level mode the cause
> tracks the i8259 "intr" line and deasserts naturally once the
> handler has drained the pending IRQ and EOI'd.
>
> ### PCI interrupt acknowledge (INTA-cycle equivalent)
>
> Unlike x86, PowerPC does not issue an INTA bus cycle on
> exception entry. The MV64361 provides software-visible
> INTA-virtual registers that stand in for that cycle:
>
> | Offset | Name                                      |
> |--------|-------------------------------------------|
> | 0xC34  | PCI_0_INTERRUPT_ACKNOWLEDGE_VIRTUAL       |
> | 0xCB4  | PCI_1_INTERRUPT_ACKNOWLEDGE_VIRTUAL       |
>
> On Pegasos2 the VT8231 southbridge sits on PCI1, so the
> relevant register is `PCI_1_INTERRUPT_ACKNOWLEDGE_VIRTUAL` at
> `0xCB4`. Reading it, with the GPP plane in level mode AND the
> cascade pin (GPP 31) asserted, invokes an INTA cycle on the
> master i8259:
>
> - Sets ISR for the highest-priority pending IRQ.
> - Clears the IRR bit for that IRQ (for edge-sensitive inputs).
> - Re-evaluates the PIC's "intr" output.
>
> The returned value is `IRQ_BASE + irq_number` (default PC
> convention, 0x20 + n). Firmware typically discards the return
> value; the side effects are the point.
>
> The read is a no-op unless both gating conditions hold (level
> mode on, pin asserted). In edge mode, or with the pin low, it
> returns 0 without touching PIC state.
>
> ### Handler discipline for a cascaded IRQ
>
> A handler registered on main-cause bit 59 (VT8231 PIC
> cascade) must perform three steps in order:
>
> 1. Read `PCI_1_INTERRUPT_ACKNOWLEDGE_VIRTUAL` (0xCB4). The
>    returned value is usually discarded; the side effect
>    (master i8259 ISR set, IRR cleared, "intr" re-evaluated)
>    is what matters. Without this read the i8259 keeps "intr"
>    asserted, GPP 31 stays high, main cause 59 remains set,
>    and the CPU re-takes 0x500 indefinitely.
> 2. Perform the device-specific acknowledge. For UART1, drain
>    the RBR until `LSR[DR] = 0`. Each RBR read clears the
>    UART's own interrupt output.
> 3. Issue a non-specific EOI to the master i8259 by writing
>    `0x20` to I/O port `0x20`. This clears the ISR bit set in
>    step 1.
>
> Only with all three steps does the cascade deassert cleanly.
>
> ### Programming model
>
> For Pegasos2 the CPU is the master interrupt target. Phase 1
> firmware programmes:
>
> - `CPU_INTERRUPT0_MASK_LOW` / `_HIGH`: which main-cause bits
>   reach the CPU's external-interrupt line.
> - `GPP_INTERRUPT_MASK0`: which GPP pins propagate up into the
>   main-cause high word.
> - `CUNIT_ARBITER_CONTROL` bit 10: set to 1 before arming any
>   cascade input (mandatory for GPP 31).
>
> During Phase 1, mask all causes (CPU_INT_MASK = 0,
> GPP_INT_MASK0 = 0). Phase 2 selectively unmasks as drivers
> register handlers via the ExtInt dispatcher.

---

Applying this replacement also obsoletes the entries below from
the current text:

- The "`MAIN_INT_MASK_LOW` / `MAIN_INT_MASK_HIGH`" bullet in
  the programming model -- QEMU (and the Marvell reference) do
  not expose a separate main-mask layer; CPU_INT_MASK is the
  only gate between cause bits and the CPU's interrupt line.
- The "`@+0x0C68..0x0C74`" register range in the leading
  paragraph -- actual offsets are `@+0x004..0x024` as tabled.

---

## Q7. The Pegasos II hardware schematic shows no M48T59 chip (spec 08) [RESOLVED 2026-04-26]

**Resolution:** The M48T59 driver was removed and replaced by a
VT8231-internal-RTC driver (`machdep/pegasos2/vt8231_rtc.{c,h}`)
that exposes the southbridge's MC146818-compatible RTC + 114
bytes of battery-backed CMOS RAM at ISA I/O 0x70/0x71. SF's
`machine_nvram_*` hooks now use the 114-byte CMOS window; SF
auto-truncates writes that would exceed it. Wall-clock services
(`get-time-of-day` / `set-time-of-day`) point at the same chip.
Spec 08 still describes an M48T59 layout that does not exist on
the board; the spec author is asked to rewrite it around the
VT8231 RTC, but no further code changes are required.



**Spec claim:** `docs/08-nvram.md` (whole chapter) and
`docs/00-overview.md` line 18 identify the Pegasos II NVRAM as

> ST M48T59 TimeKeeper (8 KiB SRAM + RTC). Battery-backed. Holds OF
> environment variables, device aliases, and `nvramrc`. Accessed
> via ISA I/O.

with ports `0x74` (addr lo) / `0x75` (addr hi) / `0x77` (data) on
the VT8231 ISA bus, and a four-partition layout filling the chip's
8 KiB.

**Observed:** The publicly available Pegasos II hardware schematic
[`Pegasos_2b5.pdf`](https://www.powerdeveloper.org/assets/files/pegasos/Pegasos_2b5.pdf),
sheet 23 of 32 ("VT8231 Batterie"), shows no M48T59 anywhere in
the design. The only battery-backed component is the **VT8231
southbridge's own internal RTC**:

  - VT8231 pins `RTCX1` (E3) / `RTCX2` (F5) connect to a 32.768 KHz
    crystal (X2) with 10 pF / 22 pF loading caps (C65, C67).
  - VT8231 pin `VBAT` (E1) is fed by a CR2032 lithium coin cell
    (B1) through a BAT54C Schottky diode (D1) and a 330 R resistor
    (R47), with C66 100 nF decoupling.

That matches QEMU's `pc-bios/dtb/pegasos2.dts`, which declares an
`rtc@i70` node `compatible = "ds1385-rtc"` at I/O port `0x70-0x71`
(the standard PC/AT RTC address pair, which is what the VT8231
internal RTC presents to ISA software). It also matches the
Linux CHRP NVRAM driver, which talks to NVRAM via RTAS hypercalls
rather than direct chip I/O. Q4's "QEMU has no M48T59 model" is
explained by this finding -- there's nothing for QEMU to model.

**Impl observation:** Our `machdep/pegasos2/m48t59.c` driver
targets a chip that the schematic suggests does not exist. On
QEMU the absence is benign (reads return 0xFF, magic-check fails,
SF falls back to compile-time defaults; we already documented
this in Q4). On real hardware the driver would presumably also
fail silently, with the same fallback behaviour, but we'd lose
cross-boot OF env-var persistence for users who flash the firmware
and expect it to remember `setenv` changes.

**Suggested resolution:** Re-investigate the Pegasos II NVRAM
hardware. Three likely outcomes:

1. **The board really has only the VT8231 internal RTC** (128 bytes
   of CMOS RAM behind the standard PC RTC interface at 0x70-0x71).
   In that case `docs/08-nvram.md` needs a complete rewrite around
   that chip, and SF's NVRAM partitioning needs to fit in 128 bytes
   (or move OF env vars into a reserved region of the boot flash
   itself).

2. **A different board revision has the M48T59.** Pegasos_2b5 is
   beta-5; production boards may have added one. If so, the
   schematic for the production revision would be the citation we
   need, and the docs are correct for those boards but not for the
   beta.

3. **OF env vars on the original firmware live in flash, not in
   battery-backed RAM.** The original CodeGen SmartFirmware may
   reserve a sector of the AMD Am29F040B boot flash for env-var
   persistence; the M48T59 reference in our docs would then be
   either a red herring or a different storage tier we haven't
   identified.

Resolution requires the maintainer (the spec author) to confirm
which case applies. Until then `m48t59.c` stays in tree but is
effectively a no-op on every target.

Related: this finding is logged in BOOT.md / FEATURES.md as
"NVRAM persistence is HW-only and not yet validated on metal";
the schematic finding tightens that to "and the documented
mechanism may not match the actual hardware."

---

## Q8. Clock generator is ICS9248-151, not Winbond W83194 (spec 00) [RESOLVED 2026-04-26]

**Resolution:** The W83194-specific decoder
(`vt8231_w83194_fsb_hz`) was removed; the public-facing function
is now `pegasos2_clockgen_fsb_hz` and returns 0 unconditionally,
so `timer_calibrate` falls through to `PEGASOS2_FSB_HZ_DEFAULT`
(133 MHz, the board-strap default Genesi documents). The
`vt8231_smbus_probe` / `vt8231_smbus_read_byte` helpers stay in
place; a future commit can wire an ICS9248-151 register decoder
(per the IDT/ICS public datasheet) onto them once a real board
is available to verify against. Spec 00 / spec 05 should
identify the chip as ICS9248-151; this is left as a docs-side
clarification.



**Spec claim:** `docs/00-overview.md` line 23 lists the board's
clock generator as

> Winbond W83194 (programmable, SMBus-controlled).

and `docs/05-clock-and-timer.md` describes a register layout
matching Winbond's W83194-family datasheet (FSB select in the low
three bits of register 0x03 at SMBus slave address 0x69).

**Observed:** The Pegasos II beta-5 schematic
(`references/Pegasos_2b5.pdf`, sheet "Clock Generator") identifies
the board's clock chip as

> **ICS9248-151**

(an Integrated Circuit Systems / IDT part, not a Winbond part).
ICS9248 family register layout differs from W83194: ICS9248
typically uses a multi-byte block-read protocol with the FSB
select bits at different offsets, and slave addresses are factory-
strapped per variant rather than fixed at 0x69.

**Impl observation:** Our `machdep/pegasos2/vt8231.c` exports
`vt8231_w83194_fsb_hz()`, which probes SMBus slave 0x69 and reads
register 0x03 against a Winbond table:

```c
#define W83194_SMBUS_ADDR   0x69u
#define W83194_REG_FSB_CFG  0x03u
static const unsigned fsb_table[8] = {
    66666667u, 75000000u, 83000000u, 100000000u,
   120000000u, 133000000u, 150000000u, 166666667u
};
```

On QEMU the SMBus controller has no slave devices attached, so
`vt8231_smbus_probe` returns no-ack and we fall through to
`PEGASOS2_FSB_HZ_DEFAULT` (133 MHz). The visible behaviour on
QEMU is therefore identical to "no probe" -- the Winbond/ICS9248
mismatch is invisible there.

On real hardware the probe would talk to whatever device answers
slave-address 0x69 (which may be the ICS9248-151, or may be no
device, depending on the part's strap configuration). If it does
ack, the byte read from "register 0x03" would be interpreted
against the wrong table and could yield a wildly incorrect FSB
guess, which would in turn corrupt every downstream timer
calibration (decrementer, busclk, MV64361 SDRAM refresh, VT8231
PIT) that consumes the result. The current code path is
load-bearing on real hardware in a way it isn't on QEMU.

**Suggested resolution:** Either

1. Rename `vt8231_w83194_fsb_hz` -> `vt8231_clockgen_fsb_hz`,
   replace the Winbond register layout with the ICS9248-151 byte
   protocol per its public datasheet, and update spec 00 / spec
   05 to identify the chip correctly; or

2. If it turns out a later board revision swapped the chip,
   document both cases and probe at runtime by reading the SMBus
   manufacturer ID before deciding which decoder table to use.

Until this is resolved the safe path is to treat the SMBus probe
as advisory only and keep `PEGASOS2_FSB_HZ_DEFAULT` as the
authoritative source -- which is already what every QEMU run
does, but the comments in `vt8231.c` should reflect that the
real-HW probe is unverified rather than implying it is correct.

---

## Q9. Hardware blocks present on the board but absent from docs

**Spec claim:** `docs/00-overview.md` enumerates the chip
inventory used by the firmware. The list covers MPC7447A,
MV64361, VT8231, AMD Am29F040B flash, and the (now-disputed)
M48T59 / W83194.

**Observed:** The Pegasos II beta-5 schematic
(`references/Pegasos_2b5.pdf`) shows several additional on-board
devices that the spec does not enumerate:

  - **VT6306** -- VIA IEEE-1394a (FireWire) OHCI controller, on a
    dedicated PCI lane. Has its own sheet in the schematic.
  - **Marvell 88E1111** -- Gigabit Ethernet PHY wired to MV64361's
    primary GbE MAC.
  - **Realtek RTL8201** -- 10/100 Ethernet PHY wired to MV64361's
    secondary MAC. (Pegasos II thus has two Ethernet ports.)
  - **VT8231 sub-blocks** for floppy disk (FDD), parallel port
    (PAR), IrDA (IRDA), PS/2 keyboard/mouse (KBMS), serial (SER),
    game port (GAME), USB, and onboard power management (POWER).
    Some of these are referenced by spec 04 only obliquely.

**Impl observation:** None of these is on our roadmap for v0.5 --
the firmware does not enumerate FireWire, does not configure
either Ethernet PHY, and does not expose floppy / parallel / IrDA
/ game-port to the OS. They appear in the schematic as items the
target OS (AOS4 / MorphOS / Linux) is expected to drive itself
once it has been handed control. On QEMU only a subset of these
is modelled, so even an attempt to bring them up would be hard
to validate.

**Suggested resolution:** Spec 00's chip inventory could mention
these blocks for completeness even if they remain explicitly
out-of-scope for the firmware. The `roadmap` sections in
`README.md` and `FEATURES.md` already list "network boot",
"floppy boot", and "AC'97 audio init" as future items; an entry
for "FireWire bring-up (VT6306)" would be consistent. No code
change is implied by this question -- it is purely a
documentation-completeness note tied to the schematic now stored
in `references/`.
