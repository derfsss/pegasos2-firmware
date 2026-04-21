# Pegasos2 BIOS — clean-room rewrite project

A clean-room reimplementation of the bPlan/CodeGen SmartFirmware 1.2
(2004-10-08) for the Pegasos II PowerPC motherboard. The goal is a
drop-in firmware that fixes two long-standing defects of the stock
build while remaining compatible with AmigaOS 4, MorphOS, and Linux.

## Project status (2026-04-21)

**Phase 0 complete, tree tidied for implementation handoff.** All
reverse-engineering artefacts have been consolidated into a single
password-protected archive (`spec-author-archive.zip`) and removed
from the working tree. What remains is the clean-room
specification and its policy wrapper.

## Tree contents

```
/
├── README.md                   (this file — safe for impl agent)
├── CLEAN-ROOM-BOUNDARY.md      (formal clean-room policy — safe for impl agent)
├── docs/                       (THE DELIVERABLE — safe for impl agent)
│   ├── README.md
│   ├── START-HERE.md           (impl agent's entry point)
│   ├── 00-overview.md .. 09-known-bugs.md
├── spec-author-archive.zip     (241 MB, password-protected; DO NOT EXTRACT)
```

The archive contains all spec-author-side material:
reverse-engineering notes, Ghidra/objdump output, RE tooling
scripts, captured serial logs, diagnostic runtime patches, and the
extracted original firmware binary. **The impl agent must not
extract or reconstruct this archive.** The password is held by
the project maintainer and is not to be provided to
implementation agents.

## Two-sentence project summary

The stock Pegasos II BIOS (1) fails on modern PCI Option ROMs
because its x86 emulator doesn't handle the `0x66` operand-size
prefix or several `0x0F`-escaped opcodes, and (2) makes devices
behind PCI-to-PCI bridges invisible because its enumerator doesn't
issue Type-1 configuration cycles. A new firmware built to the
specifications in `docs/` fixes both, boots cleanly to a serial
`ok` prompt out of the box, and preserves AmigaOS 4's
`amigaboot.of` contract unchanged.

## Where to start

- **Implementation agent:** read
  [`docs/README.md`](docs/README.md), then
  [`docs/START-HERE.md`](docs/START-HERE.md) for the suggested
  build order and success criteria.
- **Anyone auditing the clean-room process:** read
  [`CLEAN-ROOM-BOUNDARY.md`](CLEAN-ROOM-BOUNDARY.md).
- **Someone browsing:** read this file, then
  [`docs/09-known-bugs.md`](docs/09-known-bugs.md) for the
  motivating bugs and the required behaviour of the new BIOS.

## Licensing (suggested for the rewrite)

The stock firmware is a copyrighted work of CodeGen / bPlan. This
project does not redistribute it; extracted copies exist only
inside the password-protected archive.

The specification in `docs/` is original writing by the
spec-author agent, paraphrased from reverse-engineering the stock
firmware and consulting public hardware datasheets. It is
intended to be licensed compatibly with the eventual rewrite —
suggested **CC-BY-4.0 or CC0** for the spec, to avoid constraining
the implementation's licence choice.

The rewrite itself (implementation phase, not in this tree) is
expected to be **GPLv2** to inherit from `openbios/smartfirmware`
if used as a starting point, or **BSD-2-Clause** if written from
scratch.

## Contact / attribution

Reverse-engineering and specification authored using Claude Code
(agentic coding sessions 1–10, 2026-04-20 to 2026-04-21). All
analysis used publicly available datasheets (Marvell MV64360/61
Programmer's Reference, VIA VT8231 datasheet, Motorola MPC7447A
user manual, IEEE-1275-1994 + errata, PCI 2.3 specification) plus
community documentation. A full bibliography is inside the
archive (not reproduced here to keep this file minimal and
impl-agent-safe).
