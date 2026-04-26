# Boot architecture

Internal documentation of the boot path from reset to OS entry.
Companion to `docs/05-of-runtime.md` (the spec author's high-level
description) and `docs/07-boot-loader.md` (the spec for OS handoff).

## End-to-end flow

```
              ┌──────────────────────────────────────┐
              │  Power-on / reset                    │
              └────────────────┬─────────────────────┘
                               │
              ┌────────────────▼─────────────────────┐
              │  reset.S                             │
              │   - clear MSR[IP]                    │
              │   - program 4 DRAM BATs (0..1 GiB)   │
              │   - copy vectors to 0x100..0x1300    │
              │   - jump phase1_c_main               │
              └────────────────┬─────────────────────┘
                               │
              ┌────────────────▼─────────────────────┐
              │  Phase 1 (machdep/pegasos2/phase1.c) │
              │   - MV64361 PCI0 IO window enable    │
              │   - VT8231 SuperIO + UART1 init      │
              │   - PCI tree enum (BARs assigned)    │
              │   - x86emu self-test                 │
              │   - bochs-VGA option-ROM POST        │
              │   - W83194 FSB / time-base calibrate │
              │   - decrementer + syscall self-test  │
              └────────────────┬─────────────────────┘
                               │
              ┌────────────────▼─────────────────────┐
              │  Phase 2 (SmartFirmware main())      │
              │   - run install_list[] hooks         │
              │     -> /, /chosen, /memory, /options │
              │     -> /cpus, /pci@*, /aliases       │
              │     -> /pci@.../ide@C,1/disk@0,0     │
              │     -> ...partition packages         │
              │   - init_pegasos2[] Forth words      │
              │   - read NVRAM (M48T59 / defaults)   │
              │   - print SmartFirmware banner       │
              └────────────────┬─────────────────────┘
                               │
              ┌────────────────▼─────────────────────┐
              │  auto-boot? = true  ─────────────────┐
              │  no key during countdown             │
              └────────────────┬─────────────────────┘
                               │
              ┌────────────────▼─────────────────────┐
              │  smart-boot (smart_boot.c)           │
              │   - read boot-os-priority NVRAM      │
              │   - walk RDB partitions              │
              │   - classify each by DosType:        │
              │       amigaos / linux / morphos      │
              │   - dispatch to first matching loader│
              └────────────────┬─────────────────────┘
                               │
                  ┌────────────┴────────────┐
                  │                         │
       ┌──────────▼─────────┐    ┌──────────▼──────────┐
       │ amigaos:           │    │ linux / morphos:    │
       │  boot hd:N         │    │   not yet wired     │
       │       amigaboot.of │    │   smart-boot falls  │
       │  via SF's f_boot,  │    │   through to next   │
       │  exec_load,        │    │   priority entry    │
       │  machine_jump_os   │    │                     │
       └──────────┬─────────┘    └─────────────────────┘
                  │
       ┌──────────▼─────────────────────────────────────┐
       │  machine_jump_os (boot_kernel.S)               │
       │   - program user BATs (DRAM + MV64361/flash)   │
       │   - set MSR[IR|DR|RI], clear MSR[EE]           │
       │   - r1 = stack, r3 = 0xCAFE0000, r5 = ci_handler│
       │   - bctr to e->entrypoint                      │
       └──────────┬─────────────────────────────────────┘
                  │
       ┌──────────▼─────────────────────────────────────┐
       │  amigaboot.of (Hyperion property; not in repo) │
       │   - device-tree walk via CI                    │
       │   - parse RDB on each block device             │
       │   - mount partition's FS                       │
       │   - read /Kicklayout                           │
       │   - load Kickstart modules                     │
       │   - jump exec.kernel                           │
       └────────────────────────────────────────────────┘
```

## Memory map at OS handoff (QEMU `-m 1024`)

```
  Address       Region                              Owner / lifetime
  ───────────   ─────────────────────────────────   ────────────────
  0x00000000    Exception vectors (256 bytes/each)  firmware (permanent)
       ...      0x100=DSI 0x500=ExtInt 0x700=Prog
                0x800=FP-Unavail 0x900=Decr ...
                0x1300=last vector
  0x00001400    free DRAM
       ...
  0x00100000    firmware C stack (grows down to     firmware
                 ~0x000FFFC0; 64 KiB headroom)
  0x00200000    OS-loadable region  ┐               OS / boot-loader
       ...                          │ machine_init_load
  0x003FFFFF                        │ default e->load
                                    ┘
  0x00400000    boot-loader buffer (default for     OS / boot-loader
                 SF's `load`/`go`; amigaboot.of
                 PT_LOAD lands here)
       ...
  0x01000000    x86emu real-mode memory buffer      firmware
       ...      (option-ROM execution sandbox)
  0x010FFFFF
  0x01100000    SF malloc pool                      firmware
       ...      (2 MiB; package nodes,
  0x012FFFFF    allocated properties, etc.)
  0x01300000    free DRAM                           OS / boot-loader
       ...
  0x2CFE0000    amigaboot.of's claimed working      amigaboot
       ...      memory (32 MiB; per-disk records,
  0x2EFDFFFF    file-load buffers, FS state)
  0x2EFE0000    free DRAM
       ...
  0x2F000000    amigaboot.of's claim for kernel     amigaboot
       ...      modules (1 MiB; bootimage etc.)
  0x2F0FFFFF
  0x2F100000    free DRAM
       ...      (DRAM extends to wherever the DRAM
                 probe in machine_initialize found
                 the actual end -- 0x40000000 on
                 -m 1024, 0x20000000 on -m 512)
  ───────────
  0xF0000000    MV64361 register window             memory-mapped IO
       ...
  0xFE000000    VT8231 PCI I/O window
       ...      (0xFE0003F8 = UART1 etc.)
  0xFFF00000    Flash ROM image (firmware-raw.bin)  read-only
       ...
  0xFFFFFFFF    reset vector at 0xFFFFFFFC
```

## Device-tree shape post-install_list

```
  /                                           (root, "bPlan-CodeGen,Pegasos2")
  ├── chosen                                  /chosen/bootpath, /chosen/bootargs
  ├── memory                                  reg = <available DRAM extents>
  ├── options                                 NVRAM env vars (auto-boot?, ...)
  ├── packages
  │   ├── deblocker                           SF block-cache layer
  │   ├── disk-label                          partition + load orchestrator
  │   └── client-services                     CI dispatch table
  │       └── (get-time-of-day, ...)          pegasos2 extensions
  ├── aliases                                 hd, cd, disk, cdrom
  ├── cpus
  │   └── PowerPC,7447A@0                     PVR, clock, cache info
  ├── failsafe                                always-on UART1 console
  ├── display                                 SM501 framebuffer (when present)
  ├── pci@80000000                            VT8231 primary bus
  │   ├── host                                MV64361 host bridge
  │   ├── display                             bochs-VGA (or sm501)
  │   ├── isa                                 VT8231 fn0
  │   ├── ide@C,1                             VT8231 fn1 (IDE)
  │   │   ├── disk@0,0                        primary master HD
  │   │   │   └── DH0 (or other partition)    install_partition_packages
  │   │   │       (dostype=DOS\7, boot-priority=0,
  │   │   │        block-size=512, #blocks=...)
  │   │   └── cd@1,0                          secondary master CD-ROM
  │   ├── usb@C,2 / usb@C,3                   VT8231 fn2/fn3 (UHCI)
  │   ├── other@C,4                           VT8231 fn4 (RTC area)
  │   ├── sound@C,5                           VT8231 fn5 (AC'97)
  │   └── ...                                 VT8231 fn6 (SMBus)
  └── pci@c0000000                            secondary expansion bus
      └── ...                                 anything plugged in
```

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
