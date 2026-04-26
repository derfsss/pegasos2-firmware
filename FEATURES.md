# Firmware features reference

Detailed reference for everything the firmware exposes to the
user — Forth words, NVRAM variables, build flags, debugging
hooks, filesystem and OS support.

For an overview of what the firmware *is*, see
[README.md](README.md). For internal documentation of the boot
path itself, see [BOOT.md](BOOT.md).

---

## Table of contents

- [The boot model](#the-boot-model)
  - [Auto-boot](#auto-boot)
  - [smart-boot dispatcher](#smart-boot-dispatcher)
  - [Manual `boot` syntax](#manual-boot-syntax)
- [NVRAM variables](#nvram-variables)
- [Custom Forth words](#custom-forth-words)
- [Build matrix](#build-matrix)
- [Filesystem support](#filesystem-support)
- [OS loader support](#os-loader-support)
- [Debugging tools](#debugging-tools)
- [Regression test matrix](#regression-test-matrix)
- [Known limitations](#known-limitations)

---

## The boot model

Three independent mechanisms cooperate to get from power-on to
OS entry: **auto-boot** decides *whether* and *when* to boot,
**smart-boot** decides *what* to boot, and the **manual `boot`
command** lets you do anything by hand from the `ok` prompt.

### Auto-boot

Provided by upstream SmartFirmware (`upstream/smartfirmware/bin/of/main.c`).
At startup the firmware reads three NVRAM variables:

| variable | purpose |
|----------|---------|
| `auto-boot?` | `true` enables auto-boot; anything else disables it |
| `auto-boot-timeout` | wait, in milliseconds, before running boot-command |
| `boot-command` | Forth interpreted at expiry / on ENTER |

If `auto-boot?` is `true`:

```
Auto-boot in 3 seconds - press ESC to abort, ENTER to boot:
```

is printed, the firmware ticks down once per second, and at
expiry runs `boot-command` as Forth.

| keypress during countdown | effect |
|---------------------------|--------|
| `ENTER` (or `RETURN`) | skip the wait, run `boot-command` immediately |
| `ESC` (or anything else) | abort to the `ok` prompt |
| (no keypress) | wait the full timeout, then run `boot-command` |

Defaults differ per build target — see
[Build matrix](#build-matrix) below.

### smart-boot dispatcher

`smart-boot` is a Forth word added by this firmware that
inspects the disks at runtime and picks a boot target by OS
family. It's the default value of `boot-command` on both
target builds.

It walks every RDB partition exposed by the firmware's
`install_partition_packages` step, classifies each by its
`DosType` field, and dispatches per the
**`boot-os-priority`** NVRAM variable.

#### OS-family classification

| DosType bytes (top 3) | family |
|------------------------|--------|
| `DOS\\0` … `DOS\\7` | `amigaos` (OFS, FFS, Intl, DC, LNFS / FFS2) |
| `SFS\\0` … `SFS\\2` | `amigaos` (SmartFileSystem) |
| `PFS\\1` … `PFS\\3` | `amigaos` (Professional FileSystem 3) |
| `AFS\\1`            | `amigaos` (alias for PFS) |
| `MOR\\?`            | `morphos` (MorphOS-format partition) |
| `LNX\\?` / `EXT2/3/4` | `linux` (Linux-on-Amiga partition) |
| anything else       | (unknown — not a smart-boot candidate) |

#### Priority list syntax

`boot-os-priority` is a comma-separated list of family names.
Tokens are case-sensitive; whitespace around tokens is ignored.

```
amigaos,linux,morphos      # try AmigaOS first, then Linux, then MorphOS
linux                       # only consider Linux partitions
amigaos4,linux              # 'amigaos4' is an alias for 'amigaos'
```

Recognised tokens: `amigaos`, `linux`, `morphos`. Aliases:
`amigaos4` / `amigaos3` / `amiga` → `amigaos`.

#### Per-partition opt-out

Partitions with their RDB `BootPri` field set to a negative
value (–128 by AmigaOS convention, but any value < 0 is
treated as opt-out) are skipped by smart-boot regardless of
their family. This lets you tag e.g. a Linux data partition
that shouldn't ever auto-boot.

#### Tie-breaking within a family

When a family has more than one candidate partition,
smart-boot picks the one with the highest `BootPri`. The
per-OS loader (e.g. `amigaboot.of`) then does its own
finer-grained selection if needed.

#### Per-family loaders

| family | loader | status |
|--------|--------|--------|
| `amigaos` | `boot hd:0 amigaboot.of` (amigaboot's own boot menu picks the actual partition) | working in QEMU; real-hardware validation pending |
| `linux`   | not yet implemented; smart-boot prints a notice and falls through to the next family | placeholder |
| `morphos` | not yet implemented; smart-boot prints a notice and falls through | placeholder |

If the entire priority list is exhausted with no successful
loader, smart-boot falls back to plain `boot` (which uses
`boot-device` + `boot-file` like classic Open Firmware).

### Manual `boot` syntax

```
boot [device-spec] [file-path] [args...]
```

Examples:

```
ok boot                                     \ use boot-device / boot-file
ok boot hd:0                                \ try boot blocks on hd, partition 0
ok boot hd:0 amigaboot.of                   \ load /amigaboot.of from hd:0
ok boot hd:0 amigaboot.of bootdevice=DH1    \ ditto, with extra args passed to amigaboot
ok boot cd /test.elf                        \ load /test.elf from cd-rom
```

`device-spec` accepts:

- `hd` (alias for the first hard disk)
- `cd` (alias for the first CD/DVD drive)
- `hd:N` / `cd:N` (partition index N — 0 = first RDB partition)
- A full IEEE-1275 path like `/pci@80000000/ide@C,1/disk@0,0`

`file-path` is the absolute path within the partition's
filesystem. Anything after `file-path` is concatenated and
passed as `bootargs` to the loaded image (visible to the OS
via `/chosen/bootargs`).

---

## NVRAM variables

Edit any of these from the `ok` prompt with `setenv NAME VALUE`
and (on real hardware) `reset-all` to apply. On QEMU's
`pegasos2` machine the M48T59 NVRAM is not modeled, so changes
do not survive reset; defaults reload every boot.

### Boot policy

| variable | default (QEMU) | default (HW) | meaning |
|----------|----------------|--------------|---------|
| `auto-boot?` | `true` | `false` | enable auto-boot countdown |
| `auto-boot-timeout` | `3000` | `5000` | countdown length in ms |
| `boot-command` | `smart-boot` | `smart-boot` | Forth interpreted at expiry |
| `boot-os-priority` | `amigaos,morphos,linux` | (same) | smart-boot OS-family order |
| `boot-device` | `cd` | `cd` | fallback device for plain `boot` |
| `boot-file` | `/test.elf;1` | `/test.elf;1` | fallback path for plain `boot` |

### Console + general

| variable | default | meaning |
|----------|---------|---------|
| `input-device` | `/failsafe` | path of the input device package |
| `output-device` | `/failsafe` | path of the output device package |
| `real-mode?` | `false` | run with translation off (debugging only) |
| `security-mode` | `none` | `none` / `command` / `full` (per IEEE-1275) |
| `use-nvramrc?` | `false` | run NVRAM-resident Forth at boot |

### Custom (this firmware's additions)

| variable | meaning |
|----------|---------|
| `os4_commandline` | passed to AmigaOS 4 kernel (defaults to `serial debuglevel=1`) |
| `boot-os-priority` | smart-boot family-order list (see above) |

### Listing all variables

```
ok printenv                  \ list every defined option with its current value
ok printenv auto-boot?       \ show one variable
```

---

## Custom Forth words

Words this firmware adds beyond the standard SmartFirmware set.
All are usable from the `ok` prompt; many are diagnostic /
hardware-validation helpers that the maintainer added during
bring-up.

### Boot path

| word | stack effect | description |
|------|-------------|-------------|
| `smart-boot` | `(--)` | walk RDB partitions, pick by `boot-os-priority`, dispatch to per-OS loader |
| `boot-kernel` | `(load-addr --)` | validate ELF at load-addr, transfer per spec 07 (used internally by `boot`) |
| `set-bootargs` | `(addr len --)` | publish a string on `/chosen/bootargs` (spec 07 §AOS4) |

### Hardware probes

| word | stack effect | description |
|------|-------------|-------------|
| `ls-pci` | `(--)` | walk `/pci@80000000` and `/pci@c0000000`; print every child device |
| `test-ide-probe` | `(--)` | print each IDE disk/cd discovered by `install_ide_driver` |
| `test-read-block` | `(--)` | open `cd@0,0`, read LBA 16, verify ISO9660 `CD001` signature |
| `test-iso-ls` | `(--)` | list the root directory of the first ATAPI ISO9660 volume |
| `test-aliases` | `(--)` | print every entry under `/aliases` (cd, cdrom, hd, disk) |

### Time / RTC (real hardware only — QEMU has no M48T59 model)

| word | stack effect | description |
|------|-------------|-------------|
| `get-time-of-day` | `( -- sec min hr day mo yr)` | read M48T59 RTC; falls back to 1970-01-01 if no chip |
| `set-time-of-day` | `(sec min hr day mo yr --)` | write M48T59 RTC; no-op if no chip |

(Both are also exposed as `/openprom/client-services` methods so
the OS can read/set the wall clock through the CI.)

### Memory + diagnostics

| word | stack effect | description |
|------|-------------|-------------|
| `heap-info` | `(--)` | print SF malloc-pool bounds + spec-07 load-address compliance verdict |
| `test-ci` | `(--)` | invoke `ci_handler` with `finddevice "/"` and print result (proves CI dispatch) |
| `test-ci-boot` | `(addr len --)` | invoke `ci_handler("boot", <bootspec>)` from Forth |
| `test-boot` | `(--)` | copy a built-in test kernel to `0x800000` and transfer |
| `test-boot-bad` | `(--)` | exercise `boot-kernel` hardening with 6 malformed ELF headers |

### Stock SmartFirmware words worth knowing

| word | stack effect | description |
|------|-------------|-------------|
| `setenv NAME VALUE` | parsed from input | set NVRAM variable, persisted on real HW |
| `printenv [NAME]` | parsed | list one or all NVRAM variables |
| `set-defaults` | `(--)` | reset NVRAM to compile-time defaults |
| `dev <path>` | parsed | set the current package to `<path>` |
| `ls [path]` | parsed | list children of `<path>` (or current package) |
| `pwd` | `(--)` | print current package path |
| `.properties` | `(--)` | dump current package's properties |
| `peer / child / parent` | `(phandle -- phandle)` | device-tree navigation primitives |
| `reset-all` | `(--)` | reset the whole CPU (re-runs reset.S) |
| `boot ...` | parsed | load + transfer to a kernel ELF (see [Manual boot syntax](#manual-boot-syntax)) |

---

## Build matrix

```bash
make                                 # CONFIG_TARGET=qemu (default)
make CONFIG_TARGET=hw                # real Pegasos II
make CONFIG_TARGET=foo               # error: must be qemu or hw

make clean                           # remove build/
make info                            # toolchain versions
make EXTRA_CFLAGS='-DCI_TRACE=1'     # full CI call trace baked in
```

### CONFIG_TARGET

Selects compile-time defaults for NVRAM. All other code paths
are identical — runtime probing handles every hardware-present /
hardware-absent split (M48T59, W83194 SMBus, SM501 framebuffer
all degrade gracefully).

| | `qemu` | `hw` |
|--|--------|------|
| `auto-boot?` default | `true` | `false` |
| `auto-boot-timeout` default | 3000 ms | 5000 ms |
| `boot-command` default | `smart-boot` | `smart-boot` |
| Macro defined | `PEGASOS_TARGET_QEMU=1` | `PEGASOS_TARGET_HW=1` |

### Optional debugging flags

| flag | size impact | effect |
|------|-------------|--------|
| `-DCI_TRACE_LIMITED=1` | ~3 KiB | log first occurrence per CI service + every call up to call 5000 + every non-zero rc; useful for OS-compat work |
| `-DCI_TRACE=1` | ~5 KiB | log every CI call verbosely; very chatty but complete |
| `-DEXCEPTION_TEST=1` | negligible | install a deliberate Program-exception path (used by the third regression test) |

Pass via `EXTRA_CFLAGS`:

```bash
make clean
make EXTRA_CFLAGS='-DCI_TRACE_LIMITED=1'
```

---

## Filesystem support

| reader | DosType | block size | source | use |
|--------|---------|-----------|--------|-----|
| `amiga-rdb` | `RDSK` (partition table only) | 512 | `machdep/pegasos2/of/amiga_rdb.c` | parse partition list, recurse into per-partition reader |
| `amiga-ffs` | `DOS\\0..\\7` | 512 or 1024 | `machdep/pegasos2/of/amiga_ffs.c` | OFS, FFS, Intl, DirCache, LNFS, FFS2 |
| `amiga-sfs` | `SFS\\0`, `SFS\\2` | 512 | `machdep/pegasos2/of/amiga_sfs.c` | SmartFileSystem (read-only) |
| `amiga-pfs3` | `PFS\\1..\\3`, `AFS\\1` | 512 | `machdep/pegasos2/of/amiga_pfs3.c` | PFS3 (read-only) |
| `iso9660` | CD media | 2048 | `upstream/smartfirmware/bin/of/iso9660.c` | CD/DVD volumes |
| `dosfat` | MBR + FAT | 512 | `upstream/smartfirmware/bin/of/dospart.c` + `dosfat.c` | DOS/Windows + Linux EFI partitions |
| `exfat` | MBR + exFAT | 512 | `machdep/pegasos2/of/fs_exfat.c` | Modern Windows / portable storage |
| `ext2fs` | Linux | 1024–4096 | `upstream/smartfirmware/bin/of/ext2fs.c` | Linux ext2/3/4 |

### Reader scope

All readers are **read-only**. The firmware doesn't write to user
data partitions under any circumstance. (NVRAM is the one
mutable storage area, and that's its own M48T59 chip, not a
filesystem.)

### Long-filename support

`amiga-ffs` supports the AOS4 LNFS extension (DOS\6/\7 with
filenames up to 107 characters). Stored at the
Name-and-Comment area starting at byte `BSIZE-184` of each file
header.

### Cross-FS partition geometry hint

When `amiga-rdb` recurses into a per-partition reader, it
publishes the partition's byte size + FS-block size via two
extern globals (`g_amiga_part_byte_size`,
`g_amiga_part_block_size`). The FS reader uses these to compute
on-disk structure positions exactly instead of probing — relevant
mainly to FFS2 on large partitions, where the FS-block size is
1024 not 512 and root-block lookups differ accordingly.

---

## OS loader support

### AmigaOS 4 (working in QEMU; real-hardware validation pending)

Boots via `amigaboot.of` placed at the partition root.
`amigaboot.of` is property of Hyperion Entertainment and is not
distributed with this firmware; users provide it themselves on
their boot disk (it ships on every AmigaOS 4 install medium).

The firmware:

1. Loads `/amigaboot.of` (an ELF) into memory at 0x200000.
2. Sets the register state per the spec-07 contract: BATs,
   MSR, r5 = CI handler entry.
3. Jumps to amigaboot.of's entry point.

amigaboot then walks the device tree, parses the RDB itself,
mounts each partition's filesystem, looks for `/Kicklayout`,
parses it, loads the listed Kickstart modules, and jumps into
exec.kernel.

The firmware's `partition_pkg.c` exposes per-partition packages
with all the methods amigaboot needs (`block-size`, `#blocks`,
`max-transfer`, `read-blocks`, `seek`, `read`, plus the
`dostype` and `boot-priority` properties).

Three workarounds are wired into `ci_entry.c` for amigaboot
compatibility:

- **Null-ihandle fixup**: amigaboot's "scan block devices" pass
  calls `read`/`seek`/`call-method` with `ihandle = NULL`. We
  substitute the most-recently-used valid ihandle.
- **MSR[EE] enable during CI calls**: amigaboot's installer
  leaves EE clear; the CI dispatcher wrapper saves MSR, enables
  EE for the call, then restores MSR.
- **Spec-compliant `nextprop`**: SF's stock `f_client_nextprop`
  has an unguarded NULL deref when the previous property is the
  last on a node. We intercept and return `-1` per IEEE-1275.

### MorphOS (placeholder)

The smart-boot framework recognises `MOR\?` DosType partitions
and routes them to a `loader_morphos()` stub. The stub prints
a "not implemented" notice and lets smart-boot try the next
family. Adding the real loader is a future task — likely a
single `interp_text("boot hd:N morphos.of")` once we have a
MorphOS-formatted disk to validate against.

### Linux (placeholder)

The smart-boot framework recognises `LNX\?` and `EXT2`/`EXT3`/
`EXT4` DosType partitions and routes them to a
`loader_linux()` stub. Like MorphOS, this currently prints
"not implemented" and falls through. A real loader needs:

- Reading the kernel ELF (or Linux's wrapped image format) from
  the matched partition (`/boot/vmlinux` typically).
- Optionally locating an initrd.
- Building a Linux-style `cmdline=` boot argument.
- Setting up the Linux register-state contract.

Open Firmware on PowerPC traditionally uses **yaboot** for
Linux; reimplementing yaboot's logic is the cleanest approach.

### Adding a new loader

Three changes:

1. In `partition_pkg.c`, extend `classify_dostype()` with a new
   tag-prefix → family-name mapping if your OS uses a new
   DosType.
2. Add the family name + any aliases to `os_family_match()`.
3. Implement `loader_<family>(Environ *e, Package *part)` —
   it should `interp_text` whatever Forth boot sequence is
   right (or load+jump directly via `boot_kernel.c` for raw
   ELF), then return `NO_ERROR` on success or an error code on
   failure (smart-boot will continue to the next family).

---

## Debugging tools

### CI tracing

Two levels of granularity:

- **`-DCI_TRACE_LIMITED=1`**: prints first occurrence per
  service name + every call until the 5000th + every non-zero
  rc. Compact enough to follow OS-loader bring-up; ~50–200 KB
  of trace per boot.
- **`-DCI_TRACE=1`**: prints every CI call. Used for finding
  obscure compatibility issues; can produce 50+ MB per boot.

Both write to UART1 (the firmware's standard console).

### GDB on QEMU

```bash
$QEMU -M pegasos2 -m 1024 \
    -bios build/firmware-raw.bin \
    -serial mon:stdio \
    -gdb tcp::1234 -S         # S = freeze CPU at reset
```

In another terminal:

```bash
gdb-multiarch build/firmware.elf
(gdb) target remote localhost:1234
(gdb) break ci_handler
(gdb) c
```

Caveat: QEMU's `pegasos2` gdbstub reports `powerpc:common64` while
gdb-multiarch defaults to `powerpc:common`. Register views may
show `pc=0 msr=0` for a running CPU — those are not real. Trust
`-d int -D trace.log` for exception events and serial output for
actual CPU progress.

### QEMU `-trace`

Useful when diagnosing IDE / SCSI:

```bash
$QEMU ... -trace ide_bus_exec_cmd -trace ide_sector_read \
        --trace file=ide.log
```

This captures every ATA command and every sector read at the
hypervisor level — extremely helpful when diagnosing why a
disk isn't being read correctly.

### QEMU monitor

When running with `-monitor stdio`, you can inspect physical
memory live:

```
(qemu) xp /16wx 0x00200000     # examine 16 words of physical RAM
(qemu) info registers          # CPU register dump
(qemu) info pci                # PCI device tree
```

### Forth interactively

```
ok dev /pci@80000000          \ navigate to a node
ok ls                          \ list children
ok .properties                 \ dump properties of current node
ok 1234 .                      \ check the Forth REPL is alive (prints 4D2 in default radix 16)
ok decimal 1234 .              \ same in decimal (prints 1234)
```

### Three-test regression matrix

A quick "did I break anything" sanity check used during
development:

```bash
QEMU=/path/to/qemu-system-ppc

# 1. Default boot, no disk attached — expect zero error matches
rm -f build/serial.txt
timeout 10 "$QEMU" -M pegasos2 -m 512 \
    -bios build/firmware-raw.bin \
    -serial "file:build/serial.txt" -display none

# 2. PCI bridge regression test — expect zero error matches
rm -f build/serial-bridge.txt
timeout 10 "$QEMU" -M pegasos2 -m 512 \
    -bios build/firmware-raw.bin \
    -serial "file:build/serial-bridge.txt" -display none \
    -device pci-bridge,id=pbr1,bus=pci.1,chassis_nr=1,addr=0x5 \
    -device e1000,bus=pbr1,addr=0x1

# 3. EXCEPTION_TEST build — expect EXACTLY ONE error match
#    (the deliberate panic the test is proving works)
make clean
make CFLAGS='... -DEXCEPTION_TEST=1 ...'
rm -f build/serial-exc.txt
timeout 10 "$QEMU" -M pegasos2 -m 512 \
    -bios build/firmware-raw.bin \
    -serial "file:build/serial-exc.txt" -display none

# Verdict
grep -Ec "INTERNAL ERROR|UNHANDLED|Failed to emulate|STUCK CS:IP|!! PANIC" \
    build/serial.txt build/serial-bridge.txt build/serial-exc.txt
# Expected: 0 0 1
```

---

## Regression test matrix

Beyond the three-test smoke check above, the practical
end-to-end test is hd1.raw boot:

```bash
$QEMU -M pegasos2 -m 512 \
    -bios build/firmware-raw.bin \
    -drive file=hd1.raw,format=raw,if=none,id=disk \
    -device ide-hd,drive=disk,bus=ide.0 \
    -serial mon:stdio
```

A successful run prints:

1. The firmware's PCI enumeration + self-tests.
2. `RDB partitions on HD: partition 0: DH0 ...`
3. The SmartFirmware banner.
4. The auto-boot countdown.
5. `smart-boot: picking amigaos partition DH0`.
6. amigaboot's banner: `AmigaOS 4.x OpenFirmware Bootloader V53.21`.
7. Its boot menu listing the AmigaOS 4.1 Final install.
8. `Booting configuration AmigaOS 4.1 Final`.
9. `Loading loader / kernel / FastFileSystem / SmartFilesystem ...`
   for 60+ Kickstart modules.
10. `[_impl_InitCode] Initializing pri 79 module SmartFilesystem ...`
    and similar from the AOS exec runtime — actual kernel running.

---

## Known limitations

- **No SDL/framebuffer console**: the firmware uses UART1 as its
  primary console. SM501 framebuffer init (`install_display`)
  exists but isn't wired into the SF console package; you need
  serial.
- **Linux + MorphOS loaders are placeholders**: smart-boot
  recognises and routes to them, but the loader bodies are
  not yet implemented. The AmigaOS 4 path (hand-off to
  amigaboot.of) is the only loader exercised end-to-end, and
  only in QEMU.
- **Real-hardware validation pending**: the firmware compiles
  for HW (`make CONFIG_TARGET=hw`) and the runtime probes are
  in place, but no real Pegasos II board has flashed it yet.
- **Read-only filesystem support**: no FS reader writes user data.
  The OS handles its own writes once it's running; the firmware
  is just a loader.
- **NVRAM persistence is HW-only**: QEMU's `pegasos2` machine
  doesn't model the M48T59 chip, so `setenv` changes don't
  survive `reset-all` on QEMU. They do on real hardware (the
  M48T59 has battery backup).
- **No USB boot**: the IDE controller is the only boot path.
  USB enumeration is left to the OS.
- **One IDE bus, master only**: the firmware only probes
  ide.0 unit 0. (Master/slave on a single bus + secondary IDE
  bus are easy extensions, but not yet wired.)
- **`auto-boot-timeout` granularity is 1000ms** in the
  user-facing countdown, but the underlying poll is 10ms.
- **`amigaboot.of` is not, and cannot be, shipped with the
  firmware** — it is property of Hyperion Entertainment.
  Users provide it themselves: every AmigaOS 4 install medium
  contains a copy at its root, which gets installed onto the
  hard drive's boot partition during a normal AOS4 install.
