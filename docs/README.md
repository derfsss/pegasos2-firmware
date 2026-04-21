# Pegasos2 BIOS rewrite — specification

These documents describe the **target behaviour** of the new Pegasos2
BIOS. They are produced by a "spec author" agent from reverse-
engineering the original (copyrighted) bPlan/CodeGen SmartFirmware
image. The **implementation author** is a separate agent that must
never see the original binary, its disassembly, or any decompiled
source. Only the paraphrased behavioural descriptions, data-structure
layouts, register maps, and public references in this `docs/` tree
are allowed as input to the implementation agent.

Clean-room separation rules are documented in the project memory
under `feedback_cleanroom.md`. Specifically for this tree:

- **Allowed:** behaviour descriptions, algorithmic pseudocode
  written from scratch, IEEE-1275 / CHRP conformance requirements,
  hardware register maps (from public datasheets), device-tree node
  shapes, test plans, failure modes the new implementation must
  avoid.
- **Forbidden:** verbatim PPC instruction sequences, decompiled C
  output, symbol names copied from the original binary's debug
  information, copied control flow annotated with original VMAs.

## Organisation

| File | Scope |
|------|-------|
| `00-overview.md`          | Hardware baseline, boot flow, rewrite goals |
| `01-cpu-init.md`          | MPC7447 setup, MMU, caches, exceptions |
| `02-memory-controller.md` | Marvell MV64361 (Discovery II) initialisation |
| `03-pci.md`               | PCI host bridge, configuration cycles, enumeration |
| `04-southbridge.md`       | VIA VT8231 (ISA, IDE, USB, AC97, SuperIO) |
| `05-of-runtime.md`        | Forth / OpenFirmware core, device tree, FCode |
| `06-client-interface.md`  | IEEE-1275 client services exposed to the booted OS |
| `07-boot-loader.md`       | ELF / a.out / CHRP-script loading, `load`/`go` words |
| `08-nvram.md`             | NVRAM layout, environment variables, boot-command |
| **`09-known-bugs.md`**    | Issues in the original firmware that the new BIOS must NOT reproduce |
| `10-test-plan.md`         | QEMU-based automated tests + real-HW manual checklist |

All chapters are drafted. **Implementation agents should read
`START-HERE.md` first** — it gives the suggested implementation
order and explicit non-goals.

The specs are paraphrased from reverse-engineering the original
stock firmware plus public hardware datasheets. No verbatim PPC
instructions, symbol tables, or decompiler output appear anywhere
in this tree. Sources and public datasheet references are cited
in each chapter's bibliography-style pointers; the copyrighted
firmware itself is not cited.

Clean-room separation rules are in `../CLEAN-ROOM-BOUNDARY.md` at
the project root. Implementation agents must not read anything
outside this `docs/` tree (plus the two root-level policy docs
`README.md` and `CLEAN-ROOM-BOUNDARY.md`).
