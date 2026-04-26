# Contributing to Pegasos II Firmware

Thanks for your interest! The firmware is a small clean-room
research project, so contribution overhead is intentionally low,
but a few things matter.

## What kinds of contributions are welcome

- **Bug reports** — especially for QEMU configurations we
  haven't exercised (other QEMU versions, Linux/Mac hosts, other
  AOS4 install variants).
- **Hardware-validation reports** — if you're brave enough to
  flash this onto a real Pegasos II board (please have a flash
  programmer + ROM backup handy first), tell us what worked and
  what didn't.
- **OS-loader additions** — Linux yaboot-style, MorphOS, AOS3,
  etc. The framework's already in place; see
  [HACKING.md § Adding an OS loader](HACKING.md#adding-an-os-loader).
- **Boot-media additions** — CD-ROM, USB, network, floppy.
- **Filesystem-reader additions or fixes** — see
  [HACKING.md § Adding a filesystem reader](HACKING.md#adding-a-filesystem-reader).
- **Documentation improvements** — typo fixes, clarifications,
  better examples.

## Before you start work

For anything bigger than a typo fix, **open an issue first** with
what you're planning. The maintainer wants to:

- Confirm the change fits the project's scope.
- Check there's nothing already in flight that would conflict.
- Discuss design before code if the change is structural.

For typos / wording / small bug fixes, just open a PR.

## Clean-room boundary

This project was written without reference to the original
Pegasos II ROM bytes. **Don't** include in any contribution:

- Decompiled / disassembled output from the stock SmartFirmware
  ROM.
- Code derived from binary analysis of any commercial AOS / MorphOS
  / firmware blob.
- Fragments copied from any GPL'd codebase (we ship under a
  BSD-3-style license; GPL inclusion would be a license conflict).

The `CLEAN-ROOM-BOUNDARY.md` file at the repo root has the formal
policy. If your contribution draws on a public spec or
permissively-licensed source tree, cite it in the commit message
so the audit trail stays clean.

## License obligations for contributors

By submitting a pull request you agree:

- Your contribution is released under the same CodeGen
  SmartFirmware-style BSD-3-clause license as the rest of the
  clean-room rewrite (full text in `LICENSES/CodeGen-smartfirmware.txt`).
- You hold the right to license it that way (i.e. you wrote it,
  or it's already under a compatible license + you're carrying
  the upstream attribution).
- You're OK with being credited via the git commit metadata.

We **cannot** accept contributions that include or rely on
proprietary third-party software (Hyperion AmigaOS components,
MorphOS bootloaders, etc.). The firmware's interaction with
those is at runtime, by reading user-supplied files off disk;
it never bundles them.

## Code style

- **C99 / GNU11**, freestanding (`-ffreestanding -fno-builtin`).
  Don't add anything that needs a hosted libc.
- **Big-endian, 32-bit PowerPC.** No size-of-`long` games, no
  byte-order assumptions. Use the `be16`/`be32`/`be64`/`le16`/
  `le32`/`le64` helpers from `byteswap.h` for all on-disk and
  on-wire data.
- **Tabs for indent, 8-column tab stop.** Match the surrounding
  files; `git diff --check` should be quiet.
- **Names:** types `PascalCase`, functions and variables
  `snake_case`, globals prefixed `g_`, Forth-word handlers
  prefixed `f_`, install hooks prefixed `install_`.
- **Comments explain WHY, not WHAT.** A comment that just
  paraphrases the next line is noise. Good comments document
  hidden constraints, surprising behaviour, references to specs,
  and gotchas.
- **No defect-hunt narrative in comments.** Don't write
  "previously this was wrong because..." — describe the
  finished state. Commit messages carry the change history.
- **Static visibility** for anything that doesn't need to be
  global. Long-lived globals get a comment explaining why
  they're shared.

## Commit shape

- Subject line under ~70 characters, present tense.
  `smart-boot: add MorphOS loader` ✅
  `Added a MorphOS loader to smart_boot.c` ❌
- Body explains the WHY of the change and the testing done.
- Reference docs/ chapters or external specs in the body when
  the change is spec-driven.
- One logical change per commit. Refactoring belongs in its own
  commit, not bundled with a feature.
- All contributions get a `Co-Authored-By:` trailer for any
  human or AI assistant who materially helped.

## Testing

Before opening a PR, run the three-test regression matrix and
the AOS4-on-hd1 smoke check:

```bash
# 1. Default boot, no disk attached - expect 0 error matches
rm -f build/serial.txt
timeout 10 qemu-system-ppc -M pegasos2 -m 512 \
    -bios build/firmware-raw.bin \
    -serial "file:build/serial.txt" -display none

# 2. PCI bridge regression - expect 0 error matches
rm -f build/serial-bridge.txt
timeout 10 qemu-system-ppc -M pegasos2 -m 512 \
    -bios build/firmware-raw.bin \
    -serial "file:build/serial-bridge.txt" -display none \
    -device pci-bridge,id=pbr1,bus=pci.1,chassis_nr=1,addr=0x5 \
    -device e1000,bus=pbr1,addr=0x1

# 3. EXCEPTION_TEST build - expect EXACTLY ONE error match
make clean
make CFLAGS='-m32 -mbig-endian -mcpu=7450 -msoft-float -mno-altivec \
            -ffreestanding -fno-builtin -fno-pic -fno-stack-protector \
            -fno-asynchronous-unwind-tables -O2 -g -std=gnu11 \
            -Wall -Wextra -Werror -DPEGASOS_TARGET_QEMU=1 \
            -DEXCEPTION_TEST=1 -Imachdep/pegasos2/x86compat \
            -Imachdep/pegasos2 -Iupstream/x86emu/include'
rm -f build/serial-exc.txt
timeout 10 qemu-system-ppc -M pegasos2 -m 512 \
    -bios build/firmware-raw.bin \
    -serial "file:build/serial-exc.txt" -display none

# Verdict
grep -Ec "INTERNAL ERROR|UNHANDLED|Failed to emulate|STUCK CS:IP|!! PANIC" \
    build/serial.txt build/serial-bridge.txt build/serial-exc.txt
# Expected: 0 0 1
```

Plus, for any change that touches the boot or partition path:

```bash
# AOS4 end-to-end on hd1.raw (peg2-upd3 install).
make clean && make
qemu-system-ppc -M pegasos2 -m 1024 \
    -bios build/firmware-raw.bin \
    -drive file=hd1.raw,format=raw,if=none,id=disk \
    -device ide-hd,drive=disk,bus=ide.0 \
    -serial mon:stdio
# Expect: smart-boot picks DH0 -> amigaboot menu ->
#         "Booting configuration AmigaOS 4.1 Final" ->
#         60+ Loading lines.
```

Mention test results in the PR description.

## Building

```bash
sudo apt install gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu

make                       # CONFIG_TARGET=qemu (default)
make CONFIG_TARGET=hw      # real-hardware-tuned
make clean
```

The output is `build/firmware-raw.bin` exactly 524 288 bytes
ready to flash or hand to QEMU's `-bios` flag.

## Where to go next

- **[README.md](README.md)** — project overview + quick start.
- **[FEATURES.md](FEATURES.md)** — every feature reference.
- **[BOOT.md](BOOT.md)** — internal boot path documentation.
- **[HACKING.md](HACKING.md)** — how to add Forth words, FS
  readers, OS loaders.
- **[PROGRESS.md](PROGRESS.md)** — implementation history /
  open questions.
- **[CODE-REVIEW.md](CODE-REVIEW.md)** — recent code-review
  findings.
- **[CLEAN-ROOM-BOUNDARY.md](CLEAN-ROOM-BOUNDARY.md)** — formal
  clean-room policy.
