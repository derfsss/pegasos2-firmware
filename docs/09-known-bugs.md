# 09 — Known bugs in the original firmware

These are the defects in the bPlan/CodeGen SmartFirmware 1.2
(2004-10-08) build that motivated the rewrite. The new implementation
**must not reproduce any of these**. Each section gives the
user-visible symptom, the underlying misbehaviour, the behaviour the
new BIOS is required to provide, and a concrete test the
implementation must pass.

---

## Bug 1 — x86 emulator rejects modern PCI GPU Option ROMs

### Symptom (on real hardware)

When a modern PCI or PCI-via-riser graphics card is installed on a
Pegasos II running the stock 1.2 firmware, the card is not
initialised. The serial console shows a burst of diagnostic messages
early in boot, centred on:

```
MISC: UNHANDLED 32 BIT DATA PREFIX
AT CS:IP = <seg>:<off> <byte>
INTERNAL ERROR: 0000000A=UNHANDLED 32BIT PREFIX
```

Other diagnostic flavours that often follow, depending on the exact
card and Option ROM, include:

```
INTERNAL ERROR: 0000000E=UNIMPLEMENTED EXTENDED OPCODE
INTERNAL ERROR: 0000000F=SIB DECODING NOT IMPLEMENTED
UNHANDLED INT 10 FUNCTION <nnnn> WITHIN EMULATION
Failed to emulate CS:IP [<seg>:<off>]=<6 bytes>
VM using <N> x86 cycles for GFX init
```

After the burst, boot continues to the OpenFirmware prompt, but the
device has not been programmed. No framebuffer is mapped, no mode
list is installed, and the subsequent `install-console` step finds no
viable display — either hangs redirecting output to a dead VGA or
falls back to serial depending on the card.

### Underlying cause

The firmware contains a single x86 real-mode emulator used to run
PCI Option ROMs during `probe-all`. That emulator targets roughly the
subset of x86 in use by VGA BIOSes produced in the late 1990s and has
three structural limitations:

1. **Prefix handling is incomplete.** The operand-size prefix `0x66`
   is decoded into a pre-handler that sets a "32-bit mode" flag in
   the VM state, dispatches the next opcode's handler, and then
   checks whether that downstream handler cleared the flag. No
   downstream handler does this, so every occurrence of an
   `0x66`-prefixed instruction raises the spurious error shown in
   the symptom. The companion `0x67` address-size prefix is
   correctly handled (it unconditionally clears its flag before
   returning), proving that the pattern is known — the `0x66`
   variant is a point defect rather than a design gap.
2. **Many common 32-bit instruction forms are not emulated.** The
   F7-group (MUL/DIV/IMUL/IDIV/TEST/NEG/NOT) has only 16-bit
   implementations; the two-byte-escape `0F xx` table has entries
   for only a handful of late-90s opcodes; the SIB addressing mode
   is explicitly unimplemented.
3. **The INT 10h dispatcher is missing common services.**
   AH=`0x01` (set cursor shape), AH=`0x03` (get cursor position),
   AH=`0x07` (scroll down), and AH=`0x13` (write string) are not
   stubbed. When a VGA BIOS calls one of these and the firmware
   returns through an unhandled path, the Option ROM typically
   interprets the resulting garbage as an error and bails.

### Required behaviour (new BIOS)

The x86 real-mode emulator must be capable of executing commodity
VGA BIOSes produced through at least 2020. Specifically:

- **Prefix handling.** All six segment override prefixes (`0x26`,
  `0x2E`, `0x36`, `0x3E`, `0x64`, `0x65`), both size prefixes
  (`0x66`, `0x67`), and `LOCK` / `REP` / `REPNE` (`0xF0` / `0xF2` /
  `0xF3`) must set their respective state flags, dispatch the
  following instruction, and clear the flag afterwards regardless
  of whether the downstream handler consumed it. No per-instruction
  handler should be expected to participate in prefix bookkeeping.
- **Operand-size expansion.** Every instruction with a 32-bit
  variant reachable via `0x66` must implement that variant. The
  high-frequency set that VGA BIOSes exercise includes
  MUL/DIV/IMUL/IDIV (`F6`/`F7` groups), the register-direct MOV
  variants (`88`..`8B`, `89 /r`), shift/rotate (`D0`..`D3`, `C0`,
  `C1`), compare (`38`..`3D`), arithmetic (`00`..`05`, `28`..`2D`,
  `80`..`83`), and loop counter updates (`E0`..`E2`, `E8`, `E9`,
  `EB`). The new emulator should either build a complete 32-bit
  table or parameterise a shared handler on the operand-size flag.
- **Address-size expansion.** When `0x67` sets the 32-bit address
  flag, address computation must use full 32-bit register contents
  with the SIB byte decoded. Segment bases must be honoured.
- **Two-byte-escape opcodes.** The `0F xx` table must at minimum
  include `0F 01` (LGDT/LIDT/LMSW), `0F 20`/`0F 22` (MOV from/to
  CR0), `0F 80..8F` (near-jcc), `0F B6`/`0F B7` (MOVZX), `0F BE`/
  `0F BF` (MOVSX), `0F FE` (MMX-style PADDB — VGA BIOSes have been
  observed using this for byte-parallel arithmetic on palette
  data).
- **INT 10h services.** Every AH value between 0x00 and 0x1F
  inclusive must return a non-error status to the caller. Services
  that would write to VGA memory may be no-ops for the purposes of
  Option ROM execution as long as they update the BIOS Data Area
  cursor position and return the correct registers.
- **BIOS Data Area initialisation.** Bytes 0x400..0x4FF of linear
  memory (the classic 256-byte BDA) must be zeroed on VM startup.
  Specifically, BDA+0x485 (character-cell height) is read by common
  Option ROMs before any call sets it; returning 0 or 16 is fine,
  returning uninitialised garbage is not.
- **IVT population.** Interrupt vectors 0x00..0x1F must point at
  stub handlers that `IRET` immediately, rather than at
  uninitialised memory. Vectors 0x10..0x1F should instead point at
  the firmware's own INT handler dispatcher.

### Test plan (implementation must pass)

1. A synthetic test ROM that executes one instruction from each of
   the required prefix/opcode/INT combinations must run to the final
   `HLT` without producing any error-log line on serial.
2. A SeaBIOS-derived VGA ROM, and a Bochs VGABIOS-derived ROM, must
   both execute to the `INT 10h AX=0003 (set mode 3)` call and
   return without serial errors.
3. A Radeon R200-class VBIOS (Radeon 9200 / 9250) must execute its
   initialisation to completion, at which point `dev /pci@…/display`
   must expose `assigned-addresses` and `ranges` properties
   consistent with the card's BAR layout.
4. A Radeon R500-or-later VBIOS attached via PCI-to-PCIe riser must
   execute at least to the point of posting its EDID read.

### Phase-0 interim patches (for diagnostic use against the original firmware)

A 19-instruction runtime patch set silences all of Bug 1's headline
error classes plus follow-on errors 3a (UNIMPLEMENTED EXTENDED
OPCODE), 3b (SIB), and auxiliary error-code 1 (EA CALCULATION). It
also installs a real stub for INT 10h AH=03. Details in
`patches/vga-combined-phase0.md`. Validation via
`scripts/validate_vga.py` with tiers `BUG1`, `V1`, `V3`, and `V2V3`.

**This is a silence-and-stub patch set, not a correct
implementation.** After applying it the Option ROM still halts at
the same x86 cycle count (~985) because the underlying missing
functionality (32-bit arithmetic, `0F FE`, proper INT 10 returns)
has not been added — only the error outputs have been suppressed.
The clean-room implementation must do the work properly per the
"Required behaviour" section above.

---

## Bug 2 — PCI walker does not descend past PCI-to-PCI bridges

### Symptom

A modern graphics card or other PCI-Express device attached to the
Pegasos II via a PCI-to-PCIe riser is invisible to the firmware. In
the device tree, `show-devs` lists only onboard devices — no bridge
node appears and no device beneath it appears. On a card that also
needs Option ROM execution (see Bug 1), this compounds: the Option
ROM would never run even if Bug 1 were fixed, because the device's
configuration space was never enumerated in the first place.

### Underlying cause

The `probe-all` implementation does not recursively enumerate the
secondary buses that PCI-to-PCI bridges expose. Specifically, the
firmware:

- Discovers devices directly attached to each of the two host
  bridges on the MV64361.
- For each device found, populates a device-tree node with its
  configuration-space properties (`vendor-id`, `device-id`,
  `class-code`, `assigned-addresses`, etc.).
- **Does not** inspect the header type (configuration space
  register at offset 0x0E) or the class code (offset 0x0A..0x0B)
  to detect whether a device is a PCI-to-PCI bridge
  (class = 0x0604, header type = 0x01).
- Therefore never programs the bridge's primary/secondary/
  subordinate bus-number registers (offsets 0x18..0x1A) and
  never issues Type 1 configuration cycles to probe devices on
  the bridge's downstream bus.

Evidence: the string `pci-bridge-number` is present in the ROM's
`.rodata` with read-side xrefs (consumer code that walks ancestor
nodes to find the nearest bridge), but no code path ever
**writes** a `pci-bridge-number` property onto a newly created
node. No bridge node is ever created in the first place.

### Required behaviour (new BIOS)

`probe-all` must perform full recursive enumeration of all PCI
buses reachable from each host bridge:

1. For each primary bus (host-bridge secondary), iterate
   device/function triples `(dev, fn)` for `dev = 0..31`, `fn =
   0..7` (stop at `fn=0` if the device is single-function, i.e.
   header-type bit 7 clear at config offset 0x0E).
2. For each discovered device, read class-code (offset 0x0A..0x0B)
   and header-type (offset 0x0E).
3. If the device is a PCI-to-PCI bridge (class 0x0604,
   header-type 0x01):
   a. Assign the next available bus number as the bridge's
      secondary bus. Program the bridge's BUS_PRIMARY (0x18),
      BUS_SECONDARY (0x19), and BUS_SUBORDINATE (0x1A) registers.
   b. Temporarily set SUBORDINATE to 0xFF during enumeration so
      that Type 1 cycles to the new bus are accepted.
   c. Recurse into the secondary bus.
   d. After recursion completes, update SUBORDINATE to the highest
      bus number actually assigned in the subtree.
4. Create an OF device-tree node for each discovered device,
   populating at minimum: `vendor-id`, `device-id`, `class-code`,
   `revision-id`, `header-type`, `reg` (bus/dev/fn), `ranges`
   (for bridges only), `assigned-addresses`, `bus-range` (bridges
   only), `pci-bridge-number` (bridges only), and `interrupts`.
5. For each bridge, also program its memory and I/O window
   base/limit registers (offsets 0x1C..0x23 for I/O and
   0x20..0x23 for memory base/limit) such that downstream BAR
   allocations fall inside the window.

### Hardware-specific requirement (MV64361)

The Marvell MV64361 requires a register-pair write before each
Type 1 configuration transaction. Specifically:

- Config Address register (CA): offset +0xCF8 from the host
  bridge's register bank. Format per PCI spec: bit 31 enable,
  bits 23..16 bus, 15..11 dev, 10..8 fn, 7..2 reg, 1..0 byte-
  enable. For Type 1 (non-zero bus), set bit 0.
- Config Data register (CD): offset +0xCFC. Access width matches
  the transaction width.

The `config-l@ / config-l! / config-w@ / config-w! / config-b@ /
config-b!` Forth primitives in the machdep layer must consult the
bus number of the device being accessed and form the correct CA
value (Type 0 when bus number equals the host bridge's primary
bus; Type 1 otherwise).

### Test plan

1. Under QEMU: with `-device pci-bridge,id=pbr1,bus=pci.1,
   chassis_nr=1,addr=0x5 -device e1000,bus=pbr1,addr=0x1`, the
   OF device tree must contain a node whose name begins with
   `pci-bridge` at `@5` under the host, and an `ethernet` child
   under that bridge at `@1,0`. The `bus-range` property of the
   bridge must be `{1 1}` (secondary=subordinate=1 since there
   are no sub-bridges in this test).
2. The same test with a device behind TWO nested bridges
   (`-device pci-bridge,id=pbr2,bus=pbr1,chassis_nr=2`) must
   produce a correctly nested tree and a top-level bridge
   `bus-range` of `{1 2}`.
3. On real hardware with a PCI-to-PCIe riser and a Radeon RX-series
   card, `show-devs /pci` must list the bridge and the display
   device, and `dev /pci/display; .properties` must produce a
   complete property list including `assigned-addresses`.

### Phase-0 interim patch

None. Unlike Bug 1, the minimal surface-silencing patch that would
not change observable behaviour is empty — the only way to make
bridge-behind devices visible is to actually implement the
recursion and config-cycle plumbing. This is part of the clean-room
rewrite proper.

---

## Follow-on issues (not phase-0 blockers)

The phase-0 patch for Bug 1 exposes three additional emulator gaps
that were previously masked by the early `0x66` failure. These are
**not regressions** caused by the patch; they are pre-existing
defects in the same emulator. Documenting them here so the
implementation does not reproduce them.

### 3a — `0F FE` unimplemented

Observed on at least one Option ROM attempting MMX-style byte-
parallel arithmetic on palette data. Raised as emulator error
code 0x0E (`UNIMPLEMENTED EXTENDED OPCODE`). Required in new BIOS:
see Bug 1's "Two-byte-escape opcodes" bullet.

### 3b — SIB decoding unimplemented

Raised as error code 0x0F. Required in new BIOS: full SIB
decoding as part of 32-bit address computation under the `0x67`
prefix.

### 3c — Effective-address overflow warnings

With the phase-0 Bug 1 patch, the `0x66` prefix is effectively
ignored, so 32-bit arithmetic instructions run in 16-bit mode. This
produces correct results only when the affected registers fit in 16
bits. When they don't, the EA falls outside the current 64 KiB
segment and the emulator emits a warning. In the clean-room
implementation this cannot occur because 32-bit variants are
properly implemented.

### 3d — `install-console` redirects I/O to VGA even when no VGA is present

The default `install-console` Forth word unconditionally switches OF
I/O to the first display-class device, whether or not that device
has actually been initialised by Option ROM execution. When the
Option ROM failed (common due to Bug 1), writes go to an
uninitialised device and input can never be received. Required in
new BIOS: `install-console` must confirm the display it selects has
a functional framebuffer and keyboard route; otherwise it must fall
back to serial.

---

## Bug summary table

| ID | Subsystem      | Severity | Phase-0 patch | Clean-room fix required |
|----|-----------------|----------|---------------|--------------------------|
| 1  | x86 emulator    | Blocks modern GPU init | Partial, validated | Yes (full 32-bit impl) |
| 2  | PCI enumeration | Hides entire bridged devices | None feasible | Yes (recursion + Type 1) |
| 3a | x86 emulator    | Inherited from #1 | Inherited | Covered by #1 full fix |
| 3b | x86 emulator    | Inherited from #1 | Inherited | Covered by #1 full fix |
| 3c | x86 emulator    | Inherited from #1 | Inherited | Covered by #1 full fix |
| 3d | OF console init | Blocks interactive recovery | Needs NVRAM or `-nographic` workaround | Yes (console health-check) |
