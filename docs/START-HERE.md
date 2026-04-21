# Implementation agent — start here

You are the clean-room implementation agent. You are writing a new
BIOS for the Pegasos II PowerPC motherboard, working solely from
the specifications in this `docs/` tree plus publicly available
hardware documentation. You must not read any other directory in
the surrounding repository; see `../CLEAN-ROOM-BOUNDARY.md`.

## What exists in this tree

| File | Purpose |
|------|---------|
| `README.md` | Tree overview and clean-room rules |
| `START-HERE.md` (this file) | Your entry point + suggested order |
| `00-overview.md` | Hardware targets, memory map, boot phases, handoff struct, strategic goals. **Read first.** |
| `01-cpu-init.md` | MPC7447 PVR, caches, BATs, exceptions, time base |
| `02-memory-controller.md` | Marvell MV64361 registers, SDRAM init, address decode |
| `03-pci.md` | PCI host bridges, Type-0/Type-1 cycles, full recursive enumeration (this is the **Bug 2 fix**, one of the two headline deliverables) |
| `04-southbridge.md` | VIA VT8231 sub-functions (ISA, IDE, USB, AC'97, SuperIO, PM, SMBus) |
| `05-of-runtime.md` | Forth engine, device tree, default startup script, **console routing including the Bug 3d fix** (health-checked `install-console`, serial-first defaults) |
| `06-client-interface.md` | IEEE-1275 client-interface handler, service list, AOS amigaboot.of contract |
| `07-boot-loader.md` | Image formats, `boot` syntax, register state at OS transfer. Also contains the full `10-test-plan` section. |
| `08-nvram.md` | M48T59 layout, environment variables, our new defaults (`input-device=/serial`, `enable-parallel?=false`, `emulator-verbose?=false`) |
| `09-known-bugs.md` | The two headline bugs (x86 emulator and PCI walker) plus follow-ons. Describes what the OLD firmware did; each section's "Required behaviour" is your spec for the NEW firmware. |

## Suggested implementation order

This follows the boot-order of the phases you will implement.
Each step has testable criteria in its chapter's "Tests" section;
use those to mark a step done.

1. **Stand up a build system** targeting PowerPC 32-bit big-endian.
   Choose either:
   - Fork `github.com/openbios/smartfirmware` (GPLv2) and add a
     Pegasos2 machdep layer. This is the lowest-effort start.
     Comply with the GPL's terms if you go this route.
   - Write from scratch in C + minimal PPC assembly.
   Either way, produce a `firmware-raw.bin` file of at most 512 KiB.

2. **Implement CPU init** per `01-cpu-init.md`. Reach a banner on
   UART1. (Do not yet depend on DRAM.)

3. **Implement DRAM init** per `02-memory-controller.md`. Reach a
   point where you can run code from DRAM. Validate on QEMU
   `pegasos2` with `-m 512`.

4. **Implement PCI enumeration** per `03-pci.md`. This is
   non-negotiable: the algorithm must recurse into PCI-to-PCI
   bridges and issue Type-1 cycles for non-primary buses. Validate
   with the nested-bridge test in `03-pci.md` § Tests #1 and #2.

5. **Implement VT8231 init** per `04-southbridge.md`. Pay attention
   to the real-time IDE timeouts — this is a specific regression
   requirement from the original.

6. **Implement the OF runtime** per `05-of-runtime.md`. Focus on
   reaching the `ok` prompt on UART1 with no display. The
   `install-console` health check is the key correctness point
   here; the default env vars (`input-device=serial`,
   `output-device=serial`) make this work out-of-box.

7. **Implement NVRAM** per `08-nvram.md`. `setenv` / `printenv`
   work; values persist across reboot.

8. **Implement the boot loader and client interface** per
   `07-boot-loader.md` and `06-client-interface.md`. Pass
   `boot cd amigaboot.of` with an AmigaOS 4.1 install CD.

9. **Implement the x86 emulator** for PCI Option ROMs. This is the
   Bug 1 fix; see `05-of-runtime.md` § "x86 emulation" and
   `09-known-bugs.md` § "Bug 1 — Required behaviour" for the
   opcode / INT 10h coverage requirements.

10. **Run the full test plan** in `07-boot-loader.md` § test plan.
    Tier 1 (unit tests) and Tier 2 (automated QEMU boots) must all
    be green before you hand the build off for real-hardware
    testing (Tier 3).

## Explicit non-goals

- Do not try to be bug-for-bug compatible with the original. You
  don't have access to the original's behaviour; only the
  required-behaviour specs. If something in your implementation
  differs from the original in a way users notice, check whether
  the spec covers it; if not, file a question via the maintainer
  (see `../CLEAN-ROOM-BOUNDARY.md`).

- Do not try to re-implement the original's custom flash-layout
  compression (the `DONA`-prefixed gzip-variant with flash-sector
  markers). Produce a `firmware-raw.bin` that can be flashed
  directly or packed in whatever layout your build chooses.

- Do not add features beyond what the spec describes. Additional
  scope lengthens the clean-room audit trail.

## When your implementation is done

A successful rewrite:

1. Boots to `ok` on serial with no display and no NVRAM on QEMU
   `pegasos2`.
2. `show-devs /pci@*` lists every PCI device reachable, including
   those behind bridges.
3. `boot cd amigaboot.of` with the AmigaOS 4.1 install ISO
   attached via `-cdrom` reaches the AmigaOS installer.
4. All test rows T2.1..T2.10 in `07-boot-loader.md` § test plan
   pass on a single clean build.
5. No `INTERNAL ERROR`, `UNHANDLED`, `Failed to emulate`, or
   `STUCK CS:IP` log lines appear in any of those tests' serial
   output.

Document your build and its test results. Hand off to the project
maintainer for clean-room audit and real-hardware acceptance.
