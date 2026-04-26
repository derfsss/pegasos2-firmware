# Hardware references

Public reference material used to inform the clean-room rewrite.
Anything that lives here is third-party documentation kept verbatim;
do not edit the contents.

## Pegasos_2b5.pdf

The Pegasos II beta-5 hardware schematic, published by Genesi /
bPlan as a public asset on `powerdeveloper.org`:

- Source: <https://www.powerdeveloper.org/assets/files/pegasos/Pegasos_2b5.pdf>
- 32-sheet schematic covering CPU, memory, MV64361 ("Spider"),
  AGP / PCI fan-out, clock generator, mechanical, VT8231 sub-blocks
  (POWER, PAR, FDD, IRDA, NET, BAT, IDE, SER, KBMS, GAME, USB),
  PCI slots, VT6306 IEEE-1394, and the front panel.
- Beta-5 revision; production boards may differ in detail. Use
  alongside the chip datasheets (MPC7447A, MV64361, VT8231, etc.)
  rather than as a sole authority.

This file is included so that future audits of the clean-room
codebase against the actual board hardware do not depend on the
upstream URL remaining live.

The schematic is a public hardware-design document; redistribution
in unmodified form for the purpose of supporting open firmware /
operating-system development on the Pegasos II is consistent with
the way Genesi originally published it. If a copyright holder
contacts us with a different position, we will replace the PDF
with an external citation.

## Findings recorded against this schematic

- `SPEC-QUESTIONS.md` Q7 [RESOLVED] — schematic showed no M48T59
  NVRAM chip; only the VT8231 internal RTC + CR2032 backup is
  present. The M48T59 driver was deleted and replaced by
  `machdep/pegasos2/vt8231_rtc.{c,h}` (114 bytes of CMOS RAM +
  MC146818-compatible time-of-day).
- `SPEC-QUESTIONS.md` Q8 [RESOLVED] — clock generator on the
  schematic is `ICS9248-151`, not the Winbond W83194 the
  earlier code targeted. The W83194 decoder was removed; a
  proper ICS9248-151 decoder is left as a TODO and the firmware
  falls back to the board-default 133 MHz FSB.
- `SPEC-QUESTIONS.md` Q9 — schematic enumerates several blocks
  the docs do not (VT6306 FireWire, 88E1111 GbE PHY, RTL8201 FE
  PHY, VT8231 FDD/PAR/IRDA/GAME sub-blocks). Out of scope for
  v0.5 firmware; documentation-completeness only.
