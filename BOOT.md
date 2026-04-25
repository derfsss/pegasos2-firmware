# Boot architecture

Internal documentation of the boot path from reset to OS entry.
Companion to `docs/05-of-runtime.md` (the spec author's high-level
description) and `docs/07-boot-loader.md` (the spec for OS handoff).

## Reset → Phase 1 (firmware init)

`reset.S` brings the CPU up: clears MSR[IP], programs the four DRAM
BATs (4 × 256 MiB = 1 GiB cacheable identity-mapped from address 0),
copies the exception vectors into low memory, and hands off to
`phase1_c_main()` (`machdep/pegasos2/phase1.c`).

Phase 1 is a deterministic hardware bring-up:

1. MV64361 PCI0 IO window enable.
2. VT8231 SuperIO and UART1 init (console online).
3. PCI tree enumeration (`pci_walker.c`) prints all visible devices.
4. x86emu self-test (boot ROM execution prep).
5. Bochs-VGA option ROM POST through x86emu.
6. Time-base calibration (W83194 SMBus probe with board-default
   fallback when the chip is unreachable).
7. Decrementer self-test, syscall self-test.
8. Hand off to SmartFirmware's `main()`.

## Phase 2 (SmartFirmware OF runtime)

SF's `main()` runs the install list (`install_list[]` in
`machdep/pegasos2/of/platform.c`):

```
install_root            -- /
install_chosen          -- /chosen + /memory pointer
install_memory          -- /memory with reg + available
init_options_from_nvram -- /options from compile-time defaults +
                           live M48T59 NVRAM overlay (HW only;
                           QEMU's pegasos2 has no M48T59 model)
install_powerpc_cpu     -- /cpus/PowerPC,7447A@0
install_display         -- framebuffer (when SM501 present)
install_failsafe        -- always-on UART1 console package
install_pci_tree        -- /pci@80000000 + /pci@c0000000
install_deblocker       -- /packages/deblocker
install_disklabel       -- /packages/disk-label
install_ide_driver      -- VT8231 IDE controller + child disk@N,N
install_aliases         -- /aliases/hd, cd, disk, cdrom
install_partition_packages
                        -- read RDB on each HD; create child
                           partition packages with name=pb_DriveName,
                           device_type=block, dostype, boot-priority
install_client_services -- /openprom/client-services dispatch table
install_pegasos2_ci_services
                        -- pegasos2 extensions to client-services
                           (get/set-time-of-day, etc.)
```

The DRAM size reported in `/memory` is **probed at runtime** (in
`machine_initialize`) by walking down from the BAT-covered top in
16 MiB steps with unique write-readback patterns and a sentinel
anchor at 0x00200000 to detect aliasing. Without this probe we
would publish more memory than physically exists when QEMU is
launched with `-m N` smaller than the BAT extent, and any
allocator reading `/memory/available` would happily hand out
addresses past EOM.

## Auto-boot

`main.c:262-316` of upstream SF reads three NVRAM variables at
startup:

| variable | role |
|----------|------|
| `auto-boot?` | `true` enables auto-boot |
| `auto-boot-timeout` | wait in ms before running boot-command |
| `boot-command` | Forth interpreted at expiry / on ENTER |

During the countdown, any keypress aborts; ENTER skips the wait
and runs `boot-command` immediately; ESC (or any non-ENTER key)
aborts to the `ok` prompt.

Compile-time defaults live in `g_nvram[]` in
`machdep/pegasos2/of/platform.c` and branch on `CONFIG_TARGET`:

| variable | qemu | hw |
|----------|------|-----|
| `auto-boot?` | `true` | `false` |
| `auto-boot-timeout` | `3000` | `5000` |
| `boot-command` | `smart-boot` | `smart-boot` |
| `boot-os-priority` | `amigaos,morphos,linux` | (same) |

On real hardware the M48T59 NVRAM persists user `setenv` changes
across reboots; on QEMU the chip is not modelled so each boot
reverts to the compile-time defaults.

## smart-boot (priority dispatcher)

`smart-boot` is a Forth word (in
`machdep/pegasos2/of/partition_pkg.c`) that:

1. Reads `boot-os-priority` (comma-separated OS-family names).
2. Walks every RDB partition exposed by
   `install_partition_packages`.
3. Classifies each by DosType into one of:
   - `amigaos`: DOS\\0..\\7, SFS\\0..\\2, PFS\\1..\\3, AFS\\1
   - `linux`: LNX\\\*, EXT2/EXT3/EXT4
   - `morphos`: MOR\\\*
4. For each priority entry in order, picks the highest-`BootPri`
   partition matching that family (`BootPri < 0` opts a partition
   out, per RDB convention).
5. Dispatches to the per-family loader. Today only the AmigaOS
   loader is wired (it hands off to `amigaboot.of` which has its
   own RDB BootPri sort plus a selection menu); Linux and MorphOS
   loaders print a "not implemented yet" message and let
   smart-boot continue to the next family.
6. Falls back to plain `boot` if the entire priority list is
   exhausted with no successful loader.

## ELF handoff

When a loader (e.g. `amigaboot.of`) is loaded into memory, the
ELF is validated by `boot_kernel.c` and entered with the
register state required by the OF client-interface contract
(spec 07 §register-state):

```
r1  = stack pointer in claim'd memory
r3  = 0xCAFE0000 (entry argc/argv on classic CHRP; not used by
                  amigaboot, but follows the convention)
r5  = client-interface entry point (our ci_handler)
MSR = MSR_IR | MSR_DR | MSR_RI (translation on, machine-check
                                 enabled, EE clear -- amigaboot
                                 enables it itself)
BAT0 = identity-mapped DRAM 0x00000000..0x0FFFFFFF (cacheable RW)
BAT1 = identity-mapped MV64361 + flash 0xF0000000..0xFFFFFFFF
       (I-O guarded, cache-inhibited)
```

amigaboot.of then:

- Walks the device tree via the CI (`peer`/`child`/`getprop`)
  to find every `device_type=block` node.
- Calls `block-size` and `#blocks` on each via `call-method`.
- Creates a 104-byte per-disk record and links it into a list at
  amigaboot-internal address `0x20F2BC`.
- Iterates that list, reading block 0 of each disk via the OF
  `seek` + `read` methods, looking for the RDB `RDSK` magic.
- For each RDB-formatted disk, parses partitions and tries to
  mount the FS (amigaboot ships its own FFS / SFS / NGFS readers).
- Looks for `/Kicklayout` at the root of each mountable partition.
- Builds a menu of bootable AOS installs ordered by `de_BootPri`,
  auto-selects the highest-priority entry after a 3-second
  countdown (or whatever the user picks).
- Loads the listed Kickstart modules + jumps to the AOS exec.

## Client interface (CI)

Our `ci_handler` (in `machdep/pegasos2/of/ci_entry.c`) takes the
IEEE-1275 packed argument array, dispatches by service name, and
returns through the same array.

Three workarounds for amigaboot.of's specific code shape are wired
in here:

- **ihandle fixup**: amigaboot's "scan block devices" pass calls
  `block-size`/`#blocks` with a valid ihandle, then immediately
  calls `read`/`seek`/`call-method` with `ihandle = NULL` because
  the ihandle is in a register the call-method return clobbers.
  We track the most-recently-used valid ihandle and substitute it
  for any incoming NULL on the IO services.
- **MSR[EE] enable**: amigaboot's installer leaves EE clear, which
  hangs any RX-driven path during a CI call. The dispatcher
  wrapper saves MSR, sets EE, runs the call body, restores MSR.
- **nextprop interception**: SF's `f_client_nextprop` has an
  unguarded NULL deref when the previous property name is the
  last on a node. We intercept the service and walk `pkg->props`
  directly, returning `-1` (per spec) when the chain is exhausted.

## Filesystem readers

Used by the boot path's `boot hd:N file` syntax and by the
exfat/iso9660 utilities exposed via Forth.

| reader | DosType | bytes/block | source |
|--------|---------|-------------|--------|
| amiga_rdb | `RDSK` (partition table only) | 512 | `machdep/pegasos2/of/amiga_rdb.c` |
| amiga_ffs | `DOS\\0..\\7` (OFS/FFS/Intl/LNFS/FFS2) | 512 or 1024 | `machdep/pegasos2/of/amiga_ffs.c` |
| amiga_sfs | `SFS\\0`, `SFS\\2` | 512 | `machdep/pegasos2/of/amiga_sfs.c` |
| amiga_pfs3 | `PFS\\1..\\3`, `AFS\\1` | 512 | `machdep/pegasos2/of/amiga_pfs3.c` |
| iso9660 | CD media | 2048 | upstream/iso9660.c |
| dosfat / exfat | CD/HD MBR + FAT | 512 | upstream/dosfat.c, machdep/pegasos2/of/fs_exfat.c |
| ext2fs | Linux | 1024–4096 | upstream/ext2fs.c |

`amiga_rdb` recurses into `file_system()` with the partition's
byte range; the contained reader matches by DosType and walks its
own on-disk structures. Cross-reader geometry hints
(`g_amiga_part_byte_size`, `g_amiga_part_block_size`) are set by
amiga_rdb just before recursing, so amiga_ffs can compute the FFS
root block exactly via the canonical `(lowKey + highKey) / 2`
formula instead of probing.

## Build flag matrix

| flag | values | effect |
|------|--------|--------|
| `CONFIG_TARGET` | `qemu` (default) / `hw` | NVRAM defaults: auto-boot policy |
| `EXTRA_CFLAGS=-DCI_TRACE_LIMITED=1` | -- | Compact CI-call trace (first occurrence per service + all calls until call 5000 + non-zero rc); for diagnosing OF compatibility |
| `EXTRA_CFLAGS=-DCI_TRACE=1` | -- | Full CI trace, every call. Very chatty |
| `CFLAGS+=-DEXCEPTION_TEST=1` | -- | Deliberate Program-exception path for the regression matrix's third test |

## Test matrix

Three smoke tests live in the project root:

```bash
QEMU=/e/Emulators/QEMU/QEMU_Install/qemu-system-ppc.exe

# 1. Default boot (no disk, no network) — expect 0 errors
timeout 10 "$QEMU" -M pegasos2 -m 512 \
    -bios "$(pwd -W)/build/firmware-raw.bin" \
    -serial "file:$(pwd -W)/build/serial.txt" -display none

# 2. PCI bridge regression (spec 03 Bug 2) — expect 0 errors
timeout 10 "$QEMU" -M pegasos2 -m 512 \
    -bios "$(pwd -W)/build/firmware-raw.bin" \
    -serial "file:$(pwd -W)/build/serial-bridge.txt" -display none \
    -device pci-bridge,id=pbr1,bus=pci.1,chassis_nr=1,addr=0x5 \
    -device e1000,bus=pbr1,addr=0x1

# 3. EXCEPTION_TEST build — expect 1 error (the deliberate panic)
make clean && make CFLAGS='... -DEXCEPTION_TEST=1' && timeout 10 ...

grep -Ec "INTERNAL ERROR|UNHANDLED|Failed to emulate|STUCK CS:IP|!! PANIC" \
    build/serial.txt build/serial-bridge.txt build/serial-exc.txt
# Expected: 0 0 1
```
