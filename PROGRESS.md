# Implementation progress

This file is the handoff document for the next impl-agent session.
Read it after `CLAUDE.md` and `docs/START-HERE.md`.

## Licence-hygiene audit (2026-04-26)

Tree-wide audit of every vendored upstream file's licence
header surfaced four files that LICENSES/SciTech-x86emu.txt
mis-described as SciTech permissive but were actually GPLv2-only
or had no licence header at all:

  upstream/x86emu/besys.c    -- GPLv2-only (Freescale wrapper)
  upstream/x86emu/atibios.c  -- GPLv2-only (Freescale wrapper)
  upstream/x86emu/vesa.c     -- no header (inherits U-Boot GPLv2-or-later)
  upstream/x86emu/vesa.h     -- no header (inherits U-Boot GPLv2-or-later)

None of those was ever compiled into the firmware (the Makefile
only walks `upstream/x86emu/x86emu/`, which is the SciTech-
licensed realmode-emulator core). To keep the source bundle
honest about licensing and to prevent any future build rule from
silently inheriting GPL onto the firmware, all four were deleted
along with three SciTech-licensed but unused wrapper files
(bios.c, biosemu.c, biosemui.h) that came from the same U-Boot
import. Bit-identical firmware before/after (md5 unchanged).
LICENSES/SciTech-x86emu.txt + LICENSE updated to reflect the
trimmed tree. Q10 filed against `docs/START-HERE.md` +
`CLEAN-ROOM-BOUNDARY.md` for separately mis-describing
`openbios/smartfirmware` as GPLv2 (it's actually CodeGen
permissive).

## Hardware-audit refactor (2026-04-26)

Schematic-vs-code audit (against `references/Pegasos_2b5.pdf`)
landed two corrections:

- **VT8231 internal RTC replaces the never-existent M48T59
  driver.** The Pegasos II beta-5 schematic shows no separate
  NVRAM chip; wall-clock and 114 bytes of battery-backed CMOS
  live inside the VT8231 southbridge (CR2032-fed). The old
  `machdep/pegasos2/m48t59.{c,h}` was deleted and replaced by
  `vt8231_rtc.{c,h}`, exposing the same byte-level + RTC API to
  SF's `machine_nvram_*` and to `get-time-of-day` /
  `set-time-of-day`. NVRAM size dropped 1 KiB → 114 bytes; SF
  truncates oversized writes automatically. Q7 is resolved.

- **W83194 SMBus FSB probe removed; renamed to neutral
  `pegasos2_clockgen_fsb_hz`.** The actual chip is an
  ICS9248-151 with an incompatible register layout; the prior
  W83194 decoder would have corrupted FSB readings on real
  hardware. The function now returns 0 unconditionally so
  `timer_calibrate` falls back to `PEGASOS2_FSB_HZ_DEFAULT`
  (133 MHz, the board strap). The SMBus host-side primitives
  stay in place ready for a future ICS9248-151 decoder. Q8 is
  resolved.

Verified: three-test regression matrix is `0 0 1` as expected,
AOS4 hd1.raw end-to-end smoke boots through ExecSG and into the
kernel.

## One-line status (2026-04-25)

OF Forth runtime bring-up is in progress as a multi-commit
series. Commits 1..8 of N are done (OF series exit criteria all
met -- clean banner + interactive REPL via `42 .` echoes `42 ok`
on mon:stdio; file-backed default boot is 2,208 bytes with no
trace noise).

Post-OF milestones landed this session (plan: NVRAM → CI Tier-A →
ExtInt → CI Tier-B → SMBus):

1. **NVRAM M48T59** (`22b5f68`). machdep/pegasos2/m48t59.{c,h} +
   SF `machine_nvram_*` wiring for the spec 08 system partition
   (M48T59 offsets 0x0200..0x05FF). Correct per spec for real
   hardware; no-op on QEMU because pegasos2 does not instantiate
   an isa-m48t59 (SPEC-QUESTIONS.md Q4).

2. **CI Tier-A** (`d74338b`, `707c4e4`, `420e3d0`). Added
   upstream client.c to OF_SUBSET; wrote ci_handler() wrapper as
   the spec 06 entry point; registered a `test-ci` Forth word
   that builds a finddevice call-struct and invokes the
   dispatcher. Observed on mon:stdio: `test-ci` prints
   `ret=0 phandle=0x5134A8`.

3. **ExtInt 0x500 dispatcher** (`7314443`, `0e32580`, `4f4b1a4`).
   QEMU source inspection nailed the MV64361 IC cascade (main-
   IC regs at +0x004..0x024; VT8231 PIC → GPP31 → main cause 59).
   E0 landed register constants + preflight probe. E1 replaced
   the panic stub with a real dispatcher + ei_install API.
   **E2 wired UART1 RX as the first consumer** -- requires
   level-mode GPP via CUNIT_ARBITER bit 10 PLUS an INTA-cycle
   read of PCI1_INTA_VIRT at handler entry to advance the
   i8259 state machine; without both, the cascade storms at
   ~300 kHz (see SPEC-QUESTIONS Q5). Keystroke-driven REPL now
   works with a single interrupt per character.

4. **CI Tier-A extension** (`018c3f8`). Extended the synthetic
   test-ci Forth word to chain finddevice + getprop, exercising
   both a 1-arg/1-ret and a 4-arg/1-ret dispatch shape. Returns
   phandle of /chosen and the decoded stdout ihandle.

4b. **CI `boot` service + OS-context callback** (`ba17b84`,
    `35d0833`). Closed the spec-06 loop on QEMU:
    - install_client_services added to install_list so SF
      registers /openprom/client-services and client_interface()
      dispatches via execute_static_method_name instead of the
      linear fallback scan (CI/5).
    - test_kernel/kernel.S extended with a "milliseconds" CI
      callback via the r5 pointer after KERNEL OK. Proves the
      firmware's CI handler is reachable from OS context (not
      just firmware Forth) -- observed ` CI.ms=XXXXXXXX` with a
      plausible uptime value before the twi halt.
    - `test-ci-boot ( addr len -- )` Forth word invokes
      ci_handler with {"boot", bootspec} and reports the return.
      Failure path (`" nosuchdev" test-ci-boot`) returns -1 and
      cleanly continues at ok. Success path (`" hd:0 /test.elf"
      test-ci-boot` with RDB image) transfers control through
      f_client_boot -> boot_load -> open-dev -> FFS2 -> ELF
      loader -> machine_jump_os -> test kernel -> r5 callback.
      Full spec-06 + spec-07 round-trip in one command.

5. **Spec-07 boot loader** (`b25795c`..`b2699a6`). Multi-commit
   slice of the spec-07 boot path that proves the full register
   handoff end-to-end on QEMU and gets the firmware spec-compliant
   against docs/07-boot-loader.md.
   Boot 1/N (`b25795c`): `boot-kernel` Forth word + ELF32-PPC-BE
   header validator + machine_jump_os asm trampoline (spec-07
   register convention: r3=0, r4=0, r5=&ci_handler, r6=0, r7=entry;
   MSR[EE] cleared).
   Boot 2/N (`cb71f77`): PT_LOAD walker with memcpy/zero-fill + a
   minimal in-tree test kernel (`test_kernel/kernel.S`) that prints
   "KERNEL OK r5=XXXXXXXX" via UART1 MMIO and traps. The r5 print
   is our evidence: it shows the firmware's ci_handler address
   reaching the OS.
   Boot 3/N (`0bf3942`): hardening pass -- restructured validator
   into two passes (validate-all, then load-all), plus 8 new bounds
   checks (phentsz, phnum, phoff overflow, per-PT_LOAD
   p_offset/p_vaddr overflow, filesz<=memsz, no flash overlap,
   e_entry coverage). New `test-boot-bad` Forth word mutates 6
   scratch ELF headers; `6/6 PASS` observed on mon:stdio.
   Boot 4/N (`d9000dd`): spec-07 audit follow-up -- accept ET_DYN
   (spec §ELF), set r1 to a valid stack above image top per §register
   state, new `set-bootargs` Forth word + test-ci read-back to
   populate /chosen/bootargs per §AOS4.
   Boot 4/N+1 (`26fe157`): `heap-info` Forth word surfaces spec-07
   §Load-address compliance at runtime; flagged the pre-existing
   4 MiB pool (0x300000..0x6FFFFF) as OUT-OF-SPEC since it
   overlapped the 0x400000 default kernel load area.
   Boot 4/N+2 (`b2699a6`): relocated x86emu to 0x01000000 (was
   0x00200000) to free that window for a 2 MiB SF pool at
   0x00200000..0x003FFFFF -- now spec-07 compliant; heap-info
   prints "OK".
   Boot 5/N (`4cb021f`): spec-07 §register-state translation-on
   compliance -- machine_jump_os now programs IBAT0/DBAT0 for DRAM
   (0x00000000..0x0FFFFFFF, cacheable R/W) and IBAT1/DBAT1 for
   MV64361+PCI-IO+flash (0xF0000000..0xFFFFFFFF, I+G R/W), then
   sets MSR[IR|DR] alongside clearing MSR[EE]. Test kernel extended
   to read + print MSR and DBAT0U/DBAT1U at entry. Observed on
   test-boot: `MSR=00000032 DBAT0U=00001FFF DBAT1U=F0001FFF` --
   IR=1, DR=1, RI=1, EE=0, exactly the spec §register-state
   contract.
   Real block-device + FS reading still deferred (G6 non-ELF
   formats, G7 device-path resolution).

6. **Block-device + FS infrastructure** (M1 of the 7-milestone
   plan landed; M2-M7 pending). First step toward making
   `boot cd amigaboot.of` work end-to-end.
   Block 1/N (`338af27`): pegasos2 `install_pci_tree()` creates
   /pci@80000000 (VT8231 primary bus) and /pci@c0000000 (secondary
   expansion bus) top-level device-tree nodes matching QEMU's
   reverse-engineered pegasos2.dts shape. Each PCI device found
   by our existing pci_walker.c becomes a child package with the
   standard IEEE-1275 PCI-binding properties: vendor-id, device-id,
   revision-id, class-code, subsystem-id (swapped with
   subsystem-vendor-id per Pegasos2 convention), interrupts, reg
   (config-space + per-BAR), assigned-addresses (from live BAR
   reads). Node naming follows QEMU's convention (class-derived
   genus: ide/isa/usb/display/sound/other + slot[,func] suffix).
   Root's #address-cells is set to 1 directly on e->root->props
   to match CHRP pegasos2 convention without pulling in upstream
   root.c's MMU-hook prerequisites via ROOT_ADDRESS_CELLS.
   New `ls-pci` Forth word walks both /pci@X packages and prints
   child nodes; observed on QEMU, all VT8231 sub-functions (fn0-
   fn6) plus bochs-VGA present with correct classifications.
   Block 2/N (`a79e0f7`): VT8231 PCI IDE driver attaches and
   IDENTIFYs the AOS4 install CD. Pulled upstream isa/atadisk.c
   unchanged (1236 LOC, already uses get_msecs timeouts so no
   port needed). Replaced isa/ata.c (ISA-bus glue, hard-coded
   legacy ports) with pegasos2-specific ide_driver.c that
   translates PCI I/O port N -> CPU MMIO 0xFE000000+N via
   mmio_read8/mmio_read16_le (new 16-bit LE helpers in io.h).
   install_ide_driver scans /pci@80000000 children for class-code
   0x0101XX, reads BAR0-3 (legacy-compat falls back to
   0x1F0/0x3F6/0x170/0x376), calls probe_ata_disks with the
   pegasos2 read/write callbacks. Controller methods (open/close/
   decode-unit/encode-unit) installed on the IDE package so
   M3/M4 can navigate /pci@.../ide@c,1/cd@0 paths. test-ide-probe
   Forth word verifies: with Pegasos2InstallCD-53.54.iso attached,
   reports `cd@0,0 ATAPI` (IDENTIFY_PACKET + READ_CAPACITY both
   succeeded).
   Gotcha worth recording: prop_get_str() truncates binary
   properties at the first NUL byte, which silently fails for
   PCI binding properties (reg/assigned-addresses/ranges) whose
   physhi often has a leading 0x00. Always use find_table +
   ent->v.array + ent->len for binary props.
   Block 3/N (`287c96a`): block reads via the deblocker work end-
   to-end, verified by reading LBA 16 of the AOS4 install CD and
   confirming the `\x01CD001\x01` ISO9660 primary volume descriptor
   signature. Pulled 3 upstream files (~1031 LOC, all CodeGen-BSD):
   `deblock.c` (/packages/deblocker, variable-size cached I/O),
   `disklbl.c` (/packages/disk-label, required by atadisk's open),
   `fs/fs.c` (file_system dispatcher; `g_filesys[]` stays empty
   for M3, M4 will fill with iso9660). Added to pci_tree.c: PCI
   bus decode-unit/encode-unit (config-space form only) and
   open/close -- necessary so `open-dev` can walk
   /pci@80000000/ide@C,1/cd@1,0. Fixed M1 bug: child package names
   now store the genus only ("ide") per IEEE-1275 §3.5 -- M1
   baked "@<unit>" into the name property which caused
   package_name to double the suffix ("ide@C,1@C,1") once
   encode-unit started working, AND broke path matching since
   name_match compared "ide" to "ide@C,1". Replaced M2's
   `f_ide_dma_alloc` E_ABORT stub with real malloc/free (Pegasos2
   has no IOMMU; any malloc'd buffer is DMA-capable). Linked
   `-lgcc` for `__udivdi3`/`__umoddi3` (deblocker's byte-offset
   seek uses 64-bit division).
   Block 4/N (`73ca06c`): ISO9660 filesystem driver registered.
   Pulled upstream `fs/iso9660.c` (555 LOC, CodeGen-BSD), added
   &g_iso9660_fs to the machdep's g_filesys[] so disklbl's
   file_system() dispatch fires the iso9660 action for FS_PROBE /
   FS_LIST / FS_LOAD. Added init_filesystem to init_list[] (exposes
   $list-files + list-files Forth words interactively). New
   test-iso-ls Forth word opens the first ATAPI child and invokes
   list-files on "/", which walks the iso9660 root and prints
   volume ID + one line per entry. Verified on the AOS4 Install CD
   (`Pegasos2InstallCD-53.54.iso`): volume ID "AmigaOS 4.1 Final
   Edition", 10+ entries including `amigaboot.of` at 36644 bytes
   (the M6 target) and `bootloader_prepare`.
   Block 5/N (`817aade`): device aliases + test-ISO target. Adds
   install_aliases to install_list that walks the IDE children and
   writes /aliases/cd + /aliases/cdrom (first ATAPI) and
   /aliases/hd + /aliases/disk (first ATA) as string properties.
   /aliases node is pre-created by SF's install_packages before
   install_list runs, so no package installer needed. Verified
   alias expansion via SF's resolve_path: `ls cd` and
   `list-files cd /` both resolve to /pci@80000000/ide@C,1/cd@1,0
   and exercise the full open-dev + iso9660 chain. New test-iso
   Makefile target: stages test_kernel.elf into build/iso_stage/
   and produces build/test.iso (~440 KB ISO9660+Joliet+Rock Ridge)
   via genisoimage; invoked on demand (`make test-iso`) for M6's
   end-to-end boot test. Gotcha recorded: install-time diagnostic
   uart_puts calls are ~2 ms/char on QEMU, ballooning probe
   runtime from 5s to >25s; looked like a hang until stripped.
   Block 6/N (`c6a1fc5`): full spec-07 boot path works end-to-end.
   `boot cd /test.elf;1` at the ok prompt runs open-dev through
   the PCI/IDE/deblocker/disklabel/iso9660 chain, loads the file
   to 0x00400000, fires init-program + go, which hits our new
   exec_is_exec / exec_load plumbing and hands off via the Boot
   5/N register-state trampoline. Observed output:
   `machine_go: e_entry=0x800000 r1=0x8012B0; transferring...
   KERNEL OK r5=FFF42E04 r1=008012B0 MSR=00000032
   DBAT0U=00001FFF DBAT1U=F0001FFF`. Refactored boot_kernel.c
   to expose elf32_ppc_be_is_exec + elf32_ppc_be_load matching
   SF's Exec_entry signature; registered as &pegasos2_ppc_elf_exec
   in platform.c's g_exec_list[]. Implemented real
   machine_init_load (e->load = 0x00400000 per spec 07),
   machine_init_program (exec_is_exec dispatch), and machine_go
   (exec_load + machine_jump_os). Pulled exe/exe.c into OF_SUBSET
   for the dispatcher. All 8 Boot 3/N hardening checks now apply
   to `boot cd` path too.
   Block 7/N (`9fa3456`): NVRAM boot defaults. g_nvram[] extended
   with boot-device=cd, boot-file=/test.elf;1, boot-command=boot,
   auto-boot?=false, auto-boot-timeout=1000. Bare `boot` at the
   ok prompt now reaches KERNEL OK via the defaults. Auto-boot
   path verified by temporarily flipping the default to true:
   shows countdown, runs boot-command on expiry, boots cleanly.
   Graceful failure when CD absent (no filesystem recognized -> ok).
   Three-test matrix unchanged (2208/2694/2731).

The 7-milestone block+FS arc (M1..M7) is now complete on QEMU
for ISO9660 media. An AOS4 install CD can replace the synthetic
test.iso and the full open-dev / load / go chain fires
identically; amigaboot.of wouldn't run to completion because
it expects M48T59 RTC + framebuffer services we've deferred, but
the firmware side of the spec-07 boot handoff is proven.

7. **Post-M7 filesystem expansion roadmap** (planning). User
   requested FFS2 (DOS\7 LongName FFS) as the priority Amiga
   filesystem for AOS4 boot; SFS-00 + PFS3 + FAT/exFAT as
   nice-to-haves. Investigation of C:\msys64\home\rich_\sdk
   produced:
     - hardblocks.h: complete RDB structure spec (RigidDiskBlock,
       PartitionBlock, FileSysHeaderBlock, LoadSegBlock, etc.)
     - dos.h: DOS\0..\7 ID codes documented
     - Examples/FastFileSystem: plugin API only, not reader
   Public references identified: Ralph Babel's Amiga Guru Book
   (ch. 15 FFS format), Commodore AmigaDOS Technical Reference
   Manual, John Hendrikx's SFS spec PDF, Microsoft FAT32/exFAT
   specs. Source-only references (GPL/LGPL -- read to understand,
   don't paste): tonioni/pfs3aio (PFS3), aminet SFS sources,
   AROS afs.handler, adflib. BSD source trees checked: no help
   (Berkeley FFS != Amiga FFS despite the name; BSDs have no
   Amiga FS support).
   Planned arcs:
     Arc FS-A PC filesystems (DONE, `57f0039`): pulled SF's
       dosfat.c + dospart.c + ext2fs.c; added to g_filesys[]
       with dospart first (MBR recursion), iso9660 + dosfat +
       ext2fs after. Tiny compat header for a dead extern in
       dosfat.c (`struct device devices[]`). Verified boot hd
       /test.elf works from FAT16 (+13.5 KiB firmware growth).
       ext2 caveat: SF's reader handles rev0 and feature-free
       rev1 only; modern mkfs.ext2 defaults not recognised --
       format with `-r 0 -O none` for boot-readable ext2.
     Arc FS-B Amiga filesystems (FFS2-first per user):
       B1 RDB partition parser (DONE, `9d324f7`) -- pegasos2
          amiga_rdb.c walks RigidDiskBlock + PartitionBlock
          chain, computes partition byte offsets from DosEnvec
          geometry, recurses into file_system per partition.
          Handles DOS\0..\7 DosType identification + SFS + PFS
          + CDFS in dostype_label(). Gotcha: SF's cprintf can't
          safely pass uLong via %lu -- split 64-bit values into
          explicit uInt halves. Test image generator in
          test_kernel/mkrdb.py. Regression timeout bumped from
          10s to 25s (install-packages now iterates 6 Filesys
          probes and takes longer to reach ok).
       B2 OFS/FFS/Intl/LongName reader (DONE) -- pegasos2
          amiga_ffs.c readonly reader covering DOS\0..\3 (OFS +
          FFS + Intl variants) and DOS\6/\7 (LongName incl.
          FFS2). Clean-room from Clevy adflib FAQ + Barthel
          DCFS/LNFS reference; no GPL/LGPL code read. open_volume
          verifies T_HEADER+ST_ROOT+checksum at block 880 (floppy
          default) with fallback probe list; walk_path does case-
          folded hash lookup (hash = len; h = h*13 + fold(c) &
          0x7FF; h % 72); read_file_contents follows data_blocks[]
          downwards from BSIZE-208 and chases T_LIST extension
          chain via OFF_EXTENSION. LNFS NaC-aware extract_name.
          mkrdb.py extended with an FFS2 skeleton (boot block,
          root at 880, file header + extension block for > DB_MAX
          data blocks, raw FFS data blocks). Verified end-to-end:
          `list-files hd:0,/` prints `Amiga FFS volume "TEST":
          67788 test.elf`; `boot hd:0 /test.elf` yields `KERNEL
          OK` + expected PANIC at 0x700 (test_kernel twi halt).
          Gotcha: `load hd:0,/test.elf` splices boot-file onto
          s->args and breaks name hashing -- use `load hd:0
          /test.elf` (space) or `load hd:0` with boot-file set.
       B3 DirCache DOS\4/\5 (DONE) -- amiga_ffs.c's hash-table
          walk already ignores DC blocks; the only missing piece
          was Intl folding on DOS\4 (bit 2 set, bit 1 clear).
          Fixed to set is_intl whenever DOSTYPE_DC_BIT is set.
          mkrdb.py generalised with --dostype N (N=0..7).
       B4 SFS (DONE) -- amiga_sfs.c covers SFS\0 (classic) and
          SFS\2 (AOS4). Clean-room from AROS LGPL blockstructure.h
          / objects.h / btreenodes.h / bitmap.h used as on-disk
          format spec only (no code copy). Linear ObjectContainer
          walk for dir lookup, extent B+-tree traversal for file
          data. Softlinks recognised but not followed. Also
          hardened amiga_ffs.c dispatch: early reject based on
          'DOS\' boot-block magic + short-circuit probe loop on
          first read error, so SFS/PFS partitions don't produce
          ATA-timeout spam when amiga_ffs probes past the end.
          mkrdb.py --sfs N (N=0 or 2) generator.
       B5 PFS3 (DONE) -- amiga_pfs3.c covers PFS\1, PFS\2, AFS\1.
          Clean-room from tonioni/pfs3aio's blocks.h + struct.h
          (BSD 4-clause, used as on-disk format spec only, no
          code copy -- advertising clause therefore does not
          propagate). 3-level anode lookup (rootblock indexblocks
          -> IB anodeblocks -> AB anodes). Anode-chain directory
          walk + file read. Small-disk variant only. mkrdb.py
          --pfs N (N=1 or 2) generator.
     Arc FS-C exFAT (DONE, `0b390c7`) -- fs_exfat.c readonly
     reader for Microsoft exFAT. Clean-room from the 2019
     public spec at learn.microsoft.com/windows/win32/fileio/
     exfat-specification. Boot-sector parse, FAT chain walk,
     32-byte directory entry sets (0x85 File + 0xC0 Stream Ext
     + 0xC1 File Name). 512-byte sectors, first FAT only,
     root directory only, no sub-dir traversal. test_kernel/
     mkexfat.py builds a whole-disk image from pure Python
     (no sudo, no mkfs.exfat dependency); writes skip the
     Allocation Bitmap and Up-case Table entries that a
     stricter reader would require.

   Real-HW deferred follow-ups completed at the code level
   (validation awaits a real-HW session):
     - M48T59 CI get-time-of-day / set-time-of-day (`6fc21ac`):
       RTC register read + BCD decoding; QEMU fallback to a
       1970-01-01 epoch. Registered in /openprom/client-services
       via install_pegasos2_ci_services.
     - W83194 SMBus FSB probe (`1227018`): VT8231 fn 4 SMBus
       host controller + W83194 register 0x03 decode;
       timer_calibrate() now probes and falls back to board
       defaults. QEMU pegasos2 has no fn 4 SMBus model so
       fallback always triggers.
     - VT8231 completeness audit (`4a18d89`): PIRQ-A..D router
       configured per docs/04 board convention; SuperIO
       config-window re-locked after UART1 enable. Documents
       the VT8231 sub-devices we intentionally skip.

   Real-world AOS4 disk smoke test (`3e52c1d`): booted from a
   live AmigaOS 4.1 Update 3 install disk (qcow2, SFS\0 on RDB,
   LowCyl=32). Surfaced two amiga_sfs.c bugs that the synthetic
   mkrdb.py test didn't: (a) real SFS formatters put the root
   block at partition block 0 not 1; (b) checksums are emitted
   with sum == 0xFFFFFFFF rather than sum == 0 despite the
   AROS header comment suggesting the latter. Also normalised
   sfs_walk_path() so subdirectory listing works. After the
   fixes:
     ok list-files hd:0 /           -> [AmigaOS]
     ok list-files hd:0 /AmigaOS    -> full 4.1 install listing
     ok boot hd:0 /AmigaOS/amigaboot.of
         elf32:   PT_LOAD #0 -> 0x200000 (61536/61536)
         machine_go: e_entry=0x200000 r1=0x210060; transferring...
         (amigaboot.of runs then faults inside its first CI
          call with a service-name pointer outside DRAM --
          separate issue; the firmware's FS + ELF loader +
          spec-07 handoff path is proven on a real image.)

   amigaboot.of CI bring-up series (`97d102d`..`d59215f`):
     CI panic / pool overlap (`97d102d`): amigaboot.of's PT_LOAD
       region overlaps the firmware malloc pool at 0x00200000
       (kernel + AOS4 boot images both link there). Relocated
       SF malloc pool to 0x01100000. Decoupled
       /memory/available reporting from pool placement so claim()
       still hands out 0x200000..0x010FFFFF.
     CI/8 (`a39db61`): two amigaboot.of crash classes fixed --
       (a) FP-Unavailable interrupt at 0x800 because amigaboot
       executes lfd/fmadd/frsp early and we did not set MSR[FP];
       boot_kernel.S now ORs FP|IR|DR (0x2030). (b) CI dispatch
       was overflowing C stack down past amigaboot's .bss into
       its saved-CI-handler slot at 0x20F290; ci_entry.c ci_handler
       grows a private 64 KiB stack via an asm shim that swaps r1
       on entry/exit.
     CI/9 + BAT/2 (`4719b60`): claim() returning addresses above
       256 MiB DSI'd because the BAT trampoline only covered
       BAT0 (0..256 MiB DRAM) + BAT1 (MV64361/PCI-IO/flash).
       boot_kernel.S now programs four DRAM BATs covering
       0..768 MiB so any -m 512+ claim() is translated.
     `boot hd:0 amigaboot.of bootdevice=NAME` syntax (`6b5dcd5`):
       SF's f_disklbl_load concatenates the loadargs ("amigaboot.of
       bootdevice=DH0") into the FS reader's path argument, so
       Amiga FS readers saw "/amigaboot.of bootdevice=DH0" and
       reported "file not found". amiga_rdb.c now strips at the
       first whitespace before recursing on the selected
       partition. /chosen/bootargs still gets the full string
       via SF's separate stash, so the OS sees `bootdevice=DH0`
       downstream.
     machine_go bootargs echo + CI_TRACE_LIMITED scaffold (`d59215f`):
       machine_go now prints /chosen/bootpath + /chosen/bootargs
       at handoff. CI_TRACE_LIMITED compile-time switch (off by
       default) prints first 30 CI calls + first occurrence of
       each new service + any non-zero rc -- 100x less log spam
       than full CI_TRACE.

   Current state (2026-04-25, paused for next agent):
     ok boot hd:0 amigaboot.of bootdevice=DH0
     elf32:   PT_LOAD #0 -> 0x200000 (61536/61536)
     machine_go: e_entry=0x200000 r1=0x210060; transferring...
       /chosen/bootpath = "/pci@80000000/ide@C,1/disk@0,0"
       /chosen/bootargs = "amigaboot.of bootdevice=DH0"
     [AmigaOS 4.x OpenFirmware Bootloader V53.21]
     No bootable devices found. Press any key to return to firmware.

   amigaboot.of now runs cleanly to its banner, parses /chosen/
   bootargs, but cannot find a bootable device. Confirmed:
     - File-load works (SFS\0 on RDB, 61536-byte ELF, ET_DYN).
     - /chosen/bootargs reaches amigaboot.of verbatim (echo proves it).
     - bootdevice=DH0 matches the actual RDB pb_DriveName on the
       test disk (NOT the FS volume label "AmigaOS" -- formatter
       sets that separately).
   Open question for the next agent: amigaboot.of's RDB-partition
   discovery via IEEE-1275 calls. Path forward:
     1. Build with -DCI_TRACE_LIMITED=1, capture the boot log,
        identify which CI services amigaboot.of uses to enumerate
        partitions (likely open-dev with a partition path,
        finddevice, getprop on /chosen, or call-method
        get-partition-info).
     2. If amigaboot.of expects /aliases/dh0 (or similar named
        alias), expose RDB partitions through device-aliases so
        bootdevice=DH0 resolves.
     3. If amigaboot.of expects a children-of-disk device-tree
        node per partition, add partition packages under
        /pci@.../ide@.../disk@N pull from the RDB at install time.
   The amigaboot.of binary is /amigaboot.of on hd0.qcow2 (62100
   bytes per the previous session's analysis); disassembly of
   the device-discovery functions (around 0x205F20 and 0x208438
   per the prior session notes) is the next concrete step.

11. **FFS2 reader: 1024-byte FS-blocks + correct LNFS NaC offset**
    (this session, uncommitted). Smoke-tested against
    `peg2-upd3 hd1.raw` (AOS4.1FE Update 3 install, FFS2 DOS\7
    DH0). Three bugs found and fixed:
    - amiga_ffs.c probed root-block at hardcoded list of likely
      offsets (880, 1760, ..., 1048576, 2097152). On a 1 GiB
      partition with root at 1047552, the next-tried offset
      2097152 lands past end-of-disk; QEMU's IDE returns ABRT
      and SF's atadisk surfaces this as "ATA device not present
      or not responding" -- a misleading message that disguised
      a probe-table walking off the disk. Fix: amiga_rdb now
      publishes partition byte size + FS-block size via
      `g_amiga_part_byte_size` + `g_amiga_part_block_size`
      (extern globals, set per-partition before recursing into
      file_system); amiga_ffs uses those to compute root via
      the canonical `(lowKey + highKey) / 2` formula.
    - amiga_ffs assumed 512-byte FS blocks. AOS4 FFS2 on >=1 GiB
      partitions uses `de_SectorPerBlock=2` for 1024-byte blocks;
      `de_SizeBlock * 4 * de_SectorPerBlock` is the canonical
      formula. amiga_ffs.c parameterised: `g_ffs.bsize` runtime,
      `MAX_BSIZE=4096` for static buffers, all `OFF_*` macros
      now expressions in `g_ffs.bsize`. Heap overflow guard:
      we override SF's caller-supplied 512-byte buf with our
      own MAX_BSIZE buffer, since `disklbl.c` passes
      `s->blocksize=512`.
    - LNFS (DOS\6/\7) NaC offset was wrong: had `BSIZE-92`,
      should be `BSIZE-184`. Per Olaf Barthel's LNFS writeup,
      the Name-and-Comment area starts at the original COMMENT
      offset (BSIZE-184) and extends forward to the original
      NAME offset (BSIZE-80). With BSIZE=1024, the file's
      filename BSTR sits at byte 0x348; with BSIZE-92=932 we
      read from pure-zero filler and every name lookup
      returned 0.

    Verified end-to-end on hd1.raw: `boot hd:0 Kickstart/
    Kicklayout` walks Kickstart dir hash table -> finds
    Kicklayout (block 520333) -> reads file contents (sectors
    1185516..1185521) -> SF rejects with "bootimage format is
    of an unknown format" (correct -- Kicklayout is a config
    file, not an ELF). 18 IDE reads, no errors.

    hd0.qcow2 (SFS) regression-tested: still loads amigaboot.of
    cleanly and reaches "AmigaOS 4.x OpenFirmware Bootloader
    V53.21 / No bootable devices found" -- the previously-
    paused state.

    Note: hd1.raw does NOT contain `/amigaboot.of`. peg2-upd3
    boots via `bboot -kernel` + `kickstart.zip -initrd`, not
    the OF/amigaboot.of path. To boot AOS4 from hd1 with our
    firmware, either copy amigaboot.of onto hd1's DH0:, or
    teach the firmware to parse Kicklayout itself (the bboot
    approach). The FS reader is now ready for either path.

12. **CONFIG_TARGET=qemu|hw build flag** (this session). Adds a
    Makefile knob that branches the compile-time NVRAM defaults
    in init_options_from_nvram. All other code paths are
    identical and use runtime probing (M48T59 magic + checksum,
    W83194 SMBus ACK, SM501 PCI scan) to adapt to the hardware
    actually present. The macros land as -DPEGASOS_TARGET_QEMU=1
    or -DPEGASOS_TARGET_HW=1 in CFLAGS + SF_CFLAGS.

    QEMU defaults (auto-boot? = true, auto-boot-timeout = 3000):
      - 3-second countdown on every fresh boot, since QEMU has
        no battery-backed M48T59 to persist user setenv changes
        across reset; defaults are loaded fresh every time.
      - boot-command = `boot hd:0 amigaboot.of bootdevice=DH0`,
        the documented AOS4 bring-up command. Falls back to ok
        prompt cleanly when no disk is attached (default + bridge
        regression tests still pass).
      - ESC during countdown aborts to ok; ENTER skips the wait.

    HW defaults (auto-boot? = false, auto-boot-timeout = 5000):
      - drops straight to ok prompt for safety; user enables
        auto-boot interactively.
      - `setenv auto-boot? true` + `setenv boot-command "<cmd>"`
        flow through SF's save_config -> set_nvram ->
        machine_nvram_write (machdep.c) -> M48T59 system
        partition (offsets 0x0200..0x05FF). Battery-backed, so
        changes persist across reboots and power cycles.

    Build:
      make                    # CONFIG_TARGET=qemu (default)
      make CONFIG_TARGET=hw   # firmware tuned for real Pegasos II
      make CONFIG_TARGET=foo  # error: must be qemu or hw

    Three-test regression matrix passes on both targets
    (default 0, bridge 0, EXCEPTION_TEST 1).
   cache, M4 ISO9660 FS + fs/fs dispatcher, M5 /aliases + test-
   media generation, M6 machine_go → machine_jump_os integration
   + `boot cd /test.elf` end-to-end, M7 NVRAM defaults +
   auto-boot. Pre-resolution of Q5 (SF exe-handler integration)
   already decided: register elf32_ppc_be_is_exec/load from our
   existing boot_kernel.c as SF's ELF handler via g_exec_list[],
   preserves Boot 3/N hardening + Boot 4/N ET_DYN acceptance.

Default file-backed boot is 2,208 bytes with 0 forbidden strings
across default + bridge + EXCEPTION_TEST. Maintainer-accepted
deferrals: RTC-via-M48T59 (tier-B get-time-of-day) and the
W83194 SMBus FSB probe are real-HW-only and held for a later
session per the "build for QEMU use first, real machine later"
directive. Spec-07 block-device + FS readers (for `boot` over
real disks) are a future milestone; see ub2lb / Sam460 U-Boot
for reference templates.

Phase 1 is substantively complete on QEMU. Both headline bugs
(spec 09 Bug 1 and Bug 2) are implemented and pass their spec-
defined tests. Exception vectors + panic handler now installed
at 0x00000100..0x00001300 with MSR[IP]=0, so any Phase 2+ fault
prints a register dump on UART1 instead of vanishing into
unmapped flash. Phases 2–4 (Forth runtime, NVRAM, boot loader,
client interface) are NOT started.

## What the firmware currently does

A successful boot of `build/firmware-raw.bin` on
`qemu-system-ppc -M pegasos2 -m 512 -bios ... -serial ... -display none`
produces **2,208 bytes** of serial output containing, in order:

1. Banner with PVR (0x80020102 = MPC7447A) and DRAM round-trip OK.
2. Console address and stack pointer.
3. Exception-vector install confirmation (`MSR[IP]=0`).
4. Full PCI enumeration across both MV64361 host bridges,
   including recursion through PCI-to-PCI bridges (`-device
   pci-bridge,...` topologies render the correct tree). Each
   device prints its BAR sizes AND assigned addresses (e.g.
   `BAR0: mem32 pref size=0x01000000 -> 0x80000000`) plus the
   command-register bits it enables (`cmd: MEM IO MASTER`).
5. Synthetic x86-emulator self-test (MOV AX / 0F FE PADDB / MOV
   AX / HLT) passes.
6. bochs-VGA Option ROM POSTed to completion -- its ROM BAR is
   now assigned by the walker (typically 0x81010000 on QEMU) and
   phase1 reads that address back from config space rather than
   hard-coding one. Returns to our HLT trampoline at
   CS:IP=0x0050:0001.
7. Clock calibration + decrementer self-test. `timer_calibrate()`
   seeds `_dec_reload` from Pegasos II board defaults
   (FSB=133 MHz → TB=33.25 MHz → 33250 ticks/ms) and the 0x900
   handler loads its reload value from that word. Then MSR[EE]
   enabled briefly, busy-spin, MSR[EE] disabled, tick count
   reported (non-zero delta proves the handler runs and rfi's
   back). MSR[ME] and MSR[RI] are set at reset time.
8. Syscall round-trip: `li r3, 0x1337; sc; mr X, r3` + print.
   0xC00 trampoline saves all 32 GPRs + SPRs, the C stub
   syscall_dispatch() overwrites frame.gpr[3]=0xBABE, trampoline
   restores and rfi's; the r3 read after sc shows 0xBABE.
9. SmartFirmware banner: "Welcome...", "SmartFirmware(tm)
   Copyright 1996-2001 by CodeGen, Inc.", "All Rights Reserved."
10. `ok` prompt. Default boot idles here waiting for input;
    `-serial mon:stdio` with `42 .` piped in echoes `42 ok`.

No `INTERNAL ERROR`, `UNHANDLED`, `Failed to emulate`, `STUCK
CS:IP`, or `!! PANIC` strings appear anywhere in the default
build's output.

Building with `-DEXCEPTION_TEST=1` added to CFLAGS triggers a
deliberate `twi 31, r0, 0` at the end of phase1, which exercises
the panic path end-to-end and produces a full register dump
(vector 0x700 Program, SRR0 = address of `twi`, SRR1 = 0x00020000
= trap bit, all 32 GPRs, LR/CTR/XER/CR/MSR/DAR/DSISR). Not
enabled in the default build.

## Commit history (as of this writing)

```
9d324f7  Arc FS-B Block 1: Amiga Rigid Disk Block partition parser
57f0039  Arc FS-A: FAT12/16/32 + ext2 + DOS MBR partitions
9fa3456  Block 7/N: NVRAM boot defaults -- bare `boot` + auto-boot work
c6a1fc5  Block 6/N: boot cd /test.elf end-to-end -- full spec-07 flow works
817aade  Block 5/N: /aliases/cd + /aliases/hd + test-iso Makefile target
73ca06c  Block 4/N: ISO9660 filesystem + test-iso-ls lists AOS4 CD root
287c96a  Block 3/N: block reads via deblocker -- CD001 at LBA 16 verified
a79e0f7  Block 2/N: VT8231 PCI IDE driver attaches + IDENTIFY works
3e52c1d  amiga_sfs: two bugs found by real-world AOS4 SFS\0 disk
4a18d89  VT8231/audit: PIRQ router + SuperIO re-lock (spec 04 completeness)
1227018  W83194/1: VT8231 SMBus host + W83194 FSB probe wired into timer_calibrate
6fc21ac  M48T59/2 + CI/7: get-time-of-day / set-time-of-day via M48T59 RTC
0b390c7  Arc FS-C: exFAT readonly filesystem reader
90d39d2  PROGRESS.md: record CI/5 + CI/6 (client-services install + test-ci-boot)
35d0833  CI/6: test-ci-boot Forth word exercises spec-06 boot service
ba17b84  CI/5: install_client_services + test-kernel CI callback
b93c907  PROGRESS.md: record Arc FS-B B3/B4/B5 (DirCache + SFS + PFS3) completion
6a35c51  Arc FS-B Block 5: PFS3 readonly reader (PFS\1 / PFS\2 / AFS\1)
f83b8a3  Arc FS-B Block 4: SmartFileSystem readonly reader (SFS\0, SFS\2)
83323de  Arc FS-B Block 3: DirCache DOS\4/\5 + generalize mkrdb.py
fba8df7  PROGRESS.md: record Arc FS-B B2 (Amiga FFS2 reader) completion
dc4a0ba  Arc FS-B Block 2: Amiga OFS/FFS/LNFS readonly reader (FFS2)
ef6542e  PROGRESS.md: record Arc FS-B B1 (RDB parser) completion
9d324f7  Arc FS-B Block 1: Amiga Rigid Disk Block partition parser
7966cac  PROGRESS.md: record Arc FS-A (FAT + ext2) completion
57f0039  Arc FS-A: FAT12/16/32 + ext2 + DOS MBR partitions
91eb4ff  PROGRESS.md: record Block 7/N + post-M7 FS expansion roadmap
9fa3456  Block 7/N: NVRAM boot defaults -- bare `boot` + auto-boot work
c6a1fc5  Block 6/N: boot cd /test.elf end-to-end -- full spec-07 flow works
817aade  Block 5/N: /aliases/cd + /aliases/hd + test-iso Makefile target
73ca06c  Block 4/N: ISO9660 filesystem + test-iso-ls lists AOS4 CD root
287c96a  Block 3/N: block reads via deblocker -- CD001 at LBA 16 verified
a79e0f7  Block 2/N: VT8231 PCI IDE driver attaches + IDENTIFY works
338af27  Block 1/N: PCI device-tree installer + ls-pci smoke test
4cb021f  Boot 5/N: BATs + MSR[IR|DR] at OS handoff (spec 07 translation-on)
b2699a6  Boot 4/N+2: relocate x86emu to 0x01000000; spec-07 heap compliance
26fe157  Boot 4/N+1: heap-info Forth word + flag spec-07 heap-placement gap
d9000dd  Boot 4/N: spec-07 register-state compliance (ET_DYN + r1 + bootargs)
978648a  PROGRESS.md: record Boot 3/N (boot-kernel hardening)
0bf3942  Boot 3/N: boot-kernel hardening + test-boot-bad smoke (spec 07)
ef6fb28  PROGRESS.md: record Boot 1/N + Boot 2/N (spec 07 ELF loader)
cb71f77  Boot 2/N: PT_LOAD walker + test-boot smoke test (end-to-end spec 07)
b25795c  Boot 1/N: boot-kernel Forth word -- ELF32 PPC BE header parse + spec 07 handoff
48ec7d7  SPEC-QUESTIONS: drop-in replacement text for docs/02 §Interrupt controller
7ba2ba2  PROGRESS.md + SPEC-QUESTIONS: record ExtInt E2 landing; resolve Q5
4f4b1a4  ExtInt E2: UART1 RX consumer via level-mode GPP + INTA-cycle handler
018c3f8  CI/4: test-ci exercises finddevice + getprop for varied arg dispatch
0e32580  ExtInt E1: 0x500 dispatcher + handler-registration infrastructure
7314443  ExtInt E0: MV64361 IC register map + preflight probe
420e3d0  CI/3: synthetic client-interface smoke test via Forth word test-ci
707c4e4  CI/2: ci_handler entry wrapper for IEEE-1275 client interface
d74338b  CI/1: add client.c to OF_SUBSET for IEEE-1275 client interface
d2af7ed  PROGRESS.md + SPEC-QUESTIONS.md: record M48T59 landing + QEMU gap
22b5f68  M48T59 NVRAM driver + SF machine_nvram_* wiring (spec 08)
52e1379  OF bring-up 8/N: retire phase1 hand-off scaffolding; trim syscall print
8d018f5  OF bring-up 7/N: disable -DDEBUG; clean banner + ok prompt
136b190  OF bring-up 6/N: failsafe_read is strictly non-blocking
c646154  OF bring-up 5/N: interactive Forth REPL on serial via /failsafe
f23d7ec  OF bring-up 4/N: OF runs from firmware.bin; reaches `ok` via failsafe
1d9c910  OF bring-up 3/N: full SF subset + platform glue; of-test closes
e208c6c  OF bring-up 2/N: machdep.c stubs + of-test partial-link target
a0f0c6f  OF bring-up 1/N: SF machdep.h scaffold + 3-file subset compiles
19bd9a7  Decrementer reload calibrated; _dec_reload runtime-configurable
ffaaf7c  Syscall (0xC00) trampoline + stub dispatcher
80fd426  PCI walker: prefetchable-memory routing + bridge pref window
23a5632  Decrementer handler + MSR[ME]/[RI]; ms-tick API
cda478f  PCI walker: bridge MEM/IO window programming
e59c2c7  PCI walker: BAR address assignment + cmd-register enable
48ce9a7  PCI walker: BAR sizing (non-destructive probe)
cea94f8  Exception vectors + panic handler (spec 01 §Exception vectors)
d2f58fc  x86emu: 0F FE (PADDB) handler (spec 09 bullet)
c5b0807  Option-ROM execution: bochs-VGA VBIOS runs clean (Bug 1 fix)
d7ecb1   x86emu self-test (6b+6c): sys glue, DRAM-backed data/bss
c2b83c4  Integrate x86emu core into the build (compile + link only)
bb9f7a6  PCI enumeration: recursive walker with Type-1 cycles (Bug 2 fix)
0549055  Phase 1 C transition: stack in DRAM, MMIO drivers split out
24dfeba  Phase 1 DRAM stuck-bit test
cced4b5  Phase 1 banner on UART1
2ad2eec  Build skeleton: WSL toolchain, linker script, reset-vector stub
5a0fc2c  Baseline: vendor smartfirmware + x86emu, establish licensing
```

## Status against `docs/START-HERE.md`

| Step | Topic | Status |
|---|---|---|
| 1 | Build system | Done. Makefile + linker script + reset trampoline. |
| 2 | CPU init + banner on UART1 | Done on QEMU. Exception vectors installed at 0x100..0x1300 with MSR[IP]=0, MSR[ME]=1, MSR[RI]=1. Decrementer (0x900) has a real handler + millisecond tick counter. Syscall (0xC00) has a trampoline + C-stub dispatcher (full save-all / dispatch / restore-all / rfi pipeline; `sc` round-trip test passes). ExtInt (0x500) still panic-stubbed -- deferred to its first consumer. Real-HW init (cache invalidate, BAT setup, clock-gen probe, TB calibration) deferred. |
| 3 | DRAM init | Done on QEMU (QEMU pre-wires DRAM). Real-HW DDR init sequence (docs/02 §"DDR init sequence" steps 1–12, SPD probe, mode-register programming) is **not implemented**. |
| 4 | PCI enumeration (Bug 2 fix) | Done. Spec 03 Tests #1 + #2 pass. Full PCI resource pipeline: sizing, BAR assignment with split non-prefetch/prefetch/IO allocators, cmd-register enable, and bridge MEM/PREFETCH/IO-window programming (BASE/LIMIT at 0x20/0x22, 0x24/0x26, 0x1C/0x1D; 1 MiB / 4 KiB granularity; disabled-LIMIT<BASE encoding when a window is unused). 64-bit-above-4 GiB BAR placement not supported (the CPU can't address that region; the firmware writes high-dword=0 for 64-bit BARs). |
| 5 | VT8231 full init | Partial -- UART1 chain only. IDE, USB, AC'97, PM, SMBus, PIC are not initialised. |
| 6 | OF Forth runtime | Not started. |
| 7 | NVRAM (M48T59) | Not started. |
| 8 | Boot loader + client interface | Not started. |
| 9 | x86 emulator (Bug 1 fix) | Core delivered. bochs-VGA POSTs cleanly. 0F FE patched. Other spec-09 opcodes (0F 01 / 20 / 22) + INT 10h fallback stubs pending -- no ROM we run currently trips them. |
| 10 | Test plan | No automated harness. Manual `qemu-system-ppc ... -serial file:...` + `grep -E ...` is the current loop. |

## Architecture

### Source layout

```
machdep/pegasos2/
├── reset.S              reset-vector trampoline (data/bss/vectors copy, MSR[IP]=0, stack init, call C)
├── firmware.ld          linker script (flash @0xFFF00000, dram @0x00100000, vectors @0x00000000)
├── phase1.c             Phase-1 C entry: bring up hardware, run tests
├── exceptions.S         vector stubs at 0x100..0x1300 + common_trap + real 0x900 decrementer + 0xC00->syscall_trampoline + _ms_tick_count + _syscall_frame
├── panic.c              panic_dump(): UART1 register dump for unrecoverable exceptions
├── syscall.c            syscall_dispatch(): 0xC00 C-side stub (prints r3/srr0, returns 0xBABE in r3)
├── timer.c/h            get_msecs(), timer_arm(), enable_ei()/disable_ei() MSR[EE] toggles
├── of/                  SmartFirmware OF machdep (commits 1-3)
│   ├── machdep.h        types, constants, banner strings
│   ├── machdep.c        machine_* / failsafe_* / dprintf / u_sleep stubs
│   └── platform.c       init_list / install_list / g_nvram / machine_font / exe-stubs / ppc_get_version
├── pegasos2.h           memory-map constants (flash, MV64361, PCI windows, UART)
├── io.h                 inline-asm MMIO accessors (BE + LE variants, byte)
├── uart16550.c/h        polled 16550 driver
├── mv64361.c/h          MV64361 register I/O + PCI config (both hosts)
├── vt8231.c/h           VT8231 bring-up (PCI cmd + SuperIO unlock + UART1 enable)
├── pci.h                standard PCI 2.3 constants
├── pci_walker.c/h       recursive PCI enumerator (Bug 2 fix)
├── x86_glue.c/h         x86emu sys glue (1 MiB buffer @0x00200000, I/O routing, BDA, IVT)
├── x86emu_stubs.c       libc stubs for vendored emulator (memset/cpy, printf no-op, etc.)
└── x86compat/           U-Boot compat shims (common.h, asm/types.h, asm/io.h, pci.h)
```

### Memory layout (runtime, on QEMU)

```
0x00000000..0x00001FFF   exception vectors (8 KiB, copied from flash LMA)
0x00002000..0x000FFFFF   scratch + stack (stack grows down from 0x100000)
0x00100000..0x001FFFFF   .data + .bss + panic_frame + panic_stack
0x00200000..0x002FFFFF   x86emu 1 MiB buffer (X86EMU_MEM_PADDR)
0x0000_xxxx              DRAM, 512 MiB total on -m 512
0x80000000..0xBFFFFFFF   PCI1 mem0 window (direct-mapped)
0xF1000000..0xF100FFFF   MV64361 register bank (little-endian from CPU)
0xF8000000..0xF8FFFFFF   PCI0 I/O window (after our enable)
0xFE000000..0xFEFFFFFF   PCI1 I/O window (default-enabled by QEMU)
0xFFF00000..0xFFF7FFFF   flash (512 KiB) -- our firmware here; QEMU mapping
```

### Linker layout

`.reset` at flash offset 0x100 (reset vector 0xFFF00100). `.text`
+ `.rodata` follow in flash. `.data` has LMA in flash, VMA in
DRAM; the reset trampoline copies it to DRAM before calling C.
`.bss` is NOLOAD in DRAM and zeroed by the trampoline. PPC small-
data (`.sdata*`, `.sbss*`) covered too.

## Build + test workflow

Primary toolchain: WSL Ubuntu 24.04 with `gcc-powerpc-linux-gnu`
(13.3.0) and `binutils-powerpc-linux-gnu` (2.42). Docker image
`walkero/amigagccondocker:os4-gcc11` is the fallback.

```bash
# From Windows / MSYS2 bash:
wsl.exe -- bash -c \
  "cd /mnt/c/msys64/home/rich_/Projects/Pegasos2-bios-impl && make"

# Or directly inside WSL:
cd /mnt/c/msys64/home/rich_/Projects/Pegasos2-bios-impl
make            # produce build/firmware-raw.bin (exactly 524288 bytes)
make clean
make info       # print toolchain versions
```

QEMU lives at `E:\Emulators\QEMU\QEMU_Install\qemu-system-ppc.exe`
(10.2.2). Standard test invocation (from MSYS2 bash):

```bash
timeout 6 /e/Emulators/QEMU/QEMU_Install/qemu-system-ppc.exe \
  -M pegasos2 -m 512 \
  -bios "$(pwd -W)/build/firmware-raw.bin" \
  -serial "file:$(pwd -W)/build/serial.txt" \
  -display none
```

To exercise Bug 2 (recursive walker), add:

```bash
-device pci-bridge,id=pbr1,bus=pci.1,chassis_nr=1,addr=0x5 \
-device e1000,bus=pbr1,addr=0x1
```

## Gotchas found during implementation

These are real footguns the next agent will hit if they don't
know. Most are documented inline in `SPEC-QUESTIONS.md` too.

### 1. MV64361 MMIO is little-endian from the CPU's view (on QEMU)

Writing `0x000FBDFF` via PPC-native `stw` arrives at the model as
`0xFFBD0F00`. Confirmed via `-trace mv64361_reg_write`. All 32-bit
register accesses must go through `stwbrx` / `lwbrx`. The
`mmio_{read,write}32_le` helpers in `io.h` are the right layer.
Byte accesses (`stb`/`lbz`) are endian-invariant.

### 2. Spec 04 says VT8231 on PCI0; QEMU puts it on PCI1

Consequence: UART1 is at CPU physical `0xFE0003F8`, not
`0xF80003F8`. See `SPEC-QUESTIONS.md` Q1.

### 3. Spec 03 says PCI1 config at 0x8CF8/0x8CFC; QEMU uses 0xC78/0xC7C

Consequence: use the `PCI_HOST_0/PCI_HOST_1` enum + the helper in
`mv64361.c`; don't hand-code offsets. `SPEC-QUESTIONS.md` Q2.

### 4. VT8231 UART1 is gated behind two enable flips

- PCI fn 0 cfg register 0x50 bit 2 = unlock SuperIO config ports.
- SuperIO index 0xF2 bit 2 = UART1 function-enable.
- Default 0xF4 = 0xFE which the emulator decodes as I/O base 0x3F8,
  so no change needed there.

`SPEC-QUESTIONS.md` Q3.

### 5. Default `.data` / `.bss` placement ruins global writes

The old linker script put `.data` and `.bss` inside flash. Writes
to `sys_rdb` (set by `X86EMU_setupMemFuncs`) silently dropped on
QEMU's flash emulation. Fixed in commit cd7ecb1 -- `.data` has LMA
in flash and VMA in DRAM; the reset trampoline copies it.

If a future change re-introduces this pattern (e.g. a third memory
region without matching trampoline init), global-variable writes
will silently fail again -- and this is a very hard bug to notice
because no diagnostic fires.

### 6. MV64361 `BASE_ADDR_ENABLE` bit semantics are inverted

A **1** bit means **disabled**. Reset default is `0x000FBFFF` (bit
14 clear = PCI1 I/O enabled by default). Our `mv64361_enable_pci0_io_window`
writes `0x000F3DFF` (clears bits 9 and 15 = PCI0 I/O + PCI1 mem0
enabled). See `mv64361.c` for the full mask.

If you need another window (PCI0 mem0/mem1/mem2, PCI1 mem1/mem2),
clear the corresponding bit. Region numbers:
`-trace mv64361_region_map` prints `Mapping pciN-XXX-win ...` for
each enabled window along with its number.

### 7. PCI config data register is PCI-byte-order (LE)

`pci_cfg_write32` uses `stwbrx` on the way to the data register so
the value lands on the PCI bus in the expected byte order. Already
handled by the `mv64361.c` helpers; don't hand-roll PCI config
cycles outside those helpers.

### 8. x86emu compiled with `-D__KERNEL__ -U_FORTIFY_SOURCE`

These flags apply only to `upstream/x86emu/x86emu/*.c`. They skip
the U-Boot include chain's pull of glibc `<stdio.h>/<stdlib.h>/<string.h>`
which would otherwise expand `printf` into `__printf_chk` and
expect a libc that doesn't exist here. If a new file from the
vendored tree gets added to the build, add it to the
`$(BUILD)/x86emu_%.o:` rule (Makefile line ~90).

### 9. `-Os` triggers gcc's out-of-line register-save helpers

Specifically `_restgpr_28_x` / `_restgpr_30_x` etc., which live
in `libgcc.a` that we're not linking. Keep `CFLAGS` at `-O2` or
higher. Dropping to `-Os` will surface link errors.

### 10. Reset-vector addresses: QEMU vs real hardware

Spec 00 says real-HW flash is at `0xFFF80000..0xFFFFFFFF`; QEMU
maps it at `0xFFF00000..0xFFF7FFFF`. We build for QEMU's layout.
A real-HW-ready build would need to either relocate to 0xFFF80000
or ship two copies for aliasing -- this is a known open item.

### 13. SmartFirmware `vbprintf` returns 0; callers must walk buf

`upstream/smartfirmware/bin/of/stdlib.c:vbprintf` finishes with
`return strlen(buf)` -- but `buf` has been advanced through the
formatting loop so it points at the NUL terminator, and strlen
returns 0. SF's own callers (cprintf, bprintf, etc.) ignore the
return value and scan buf for the real length. Our machdep
`dprintf` initially trusted the return, so no DPRINTF output
appeared even though the buffer was correctly written.

Fix in `machdep/pegasos2/of/machdep.c:dprintf`: NUL-seed the
buffer before the call, walk it after.

### 12. `get_msecs` signature collides between timer.h and defs.h

Resolved in Commit 4: our phase-1 counter renamed to
`pegasos2_get_msecs_ticks()`; SF's `get_msecs(Environ*)` (in
other.c) is the sole `get_msecs` symbol.

### 11. `_dec_reload` must be non-zero before MSR[EE] is set

The 0x900 handler reads its reload value from `_dec_reload`
(a `.bss` word). `.bss` is zero-initialised, so if MSR[EE] is
enabled before `timer_calibrate()` has written a non-zero
value, the handler writes 0 to SPR 22 and the decrementer
fires again immediately, looping forever (no forward progress
on the interrupted code). `phase1_c_main()` orders
`timer_calibrate()` before `enable_ei()`; any future code that
enables interrupts must preserve that order. Previously this
gotcha was "value duplicated between asm and C" -- the runtime
word eliminates the duplication but introduces this
initialisation-order requirement instead.

### 12. Exception-vector install timing

`reset.S` copies the `.vectors` section from its flash LMA to
VMA 0x00000000, then clears MSR[IP] *before* calling
`phase1_c_main`. This means exceptions fire into our handlers
for the entirety of Phase 1 C. The vectors themselves live at
0x0100..0x1300; the common save-all path is at 0x1400;
`_panic_frame` (168 bytes) and `_panic_stack` (4 KiB) are in
`.bss`. Any exception calls `panic_dump()` which prints a full
register dump on UART1 with a distinctive `!! PANIC:` prefix
(so existing test-grep patterns like `grep -E "PANIC|UNHANDLED"`
catch it), then spins.

If future code needs to take recoverable exceptions (decrementer
tick, external interrupt, syscall for the client interface),
the respective vector stub must be replaced with a real handler
that returns via `rfi` instead of falling through to
`common_trap`. The stubs' `VECTOR_STUB` macro in `exceptions.S`
is a natural split point -- replace one stub at a time without
disturbing the rest.

## Spec questions pending maintainer response

`SPEC-QUESTIONS.md` has three active items, all of which this
impl agent worked around by matching QEMU rather than spec:

- Q1: VT8231 host-bridge assignment (spec says PCI0, QEMU says PCI1)
- Q2: PCI1 config register offsets (spec says 0x8CF8/0x8CFC, QEMU says 0xC78/0xC7C)
- Q3: VT8231 SuperIO unlock sequence (spec describes generally; QEMU needs specific bits)

When the maintainer responds, reconcile `machdep/pegasos2/mv64361.h`
offset constants and `machdep/pegasos2/vt8231.c` bit choices.

## OF bring-up sequence (in progress, multi-commit)

Maintainer-approved plan to get SmartFirmware's OF runtime
reaching an `ok` prompt on UART1.  Each commit below is
individually buildable and keeps the three-test matrix
(default / bridge / EXCEPTION_TEST) green for phase1.  "Done"
per-commit is narrower than "useful" -- don't expect a boot
with OF behaviour until Commit 6+.

| # | Title | Exit criterion |
|---|-------|----------------|
| 1 | Scaffold: machdep.h + 3-file SF subset compiles | `make of-sf-subset` builds build/of_{errs,stdlib,alloc}.o; `make` produces identical firmware.bin |
| 2 | Malloc pick + machdep stubs for the ~22 machine_* / failsafe_* / isa_* / do_* functions | `make of-test` partial-links SF subset + machdep.o; remaining undefineds are just our existing uart/_ms_tick_count (satisfied at commit 6 when we link into firmware.bin) |
| 3 | Grow the SF subset (forth/funcs/exec/table/admin/control/cmdio/display/device/chosen/memory/root/cpu-ppc/packages/debug/nvedit/nvram + platform glue for init_list / install_list / g_nvram / machine_font / ppc_get_version) | `of-test` links with zero SF-side undefineds (only _ms_tick_count + uart_* remain, resolved at firmware link) |
| 4 | Call into OF main() from phase1_c_main() | ✅ DONE -- default boot emits full SF banner + `ok` prompt via failsafe output; install-console errors due to missing /serial node; interpret() reads 0 bytes from failsafe_read and idles |
| 5 | failsafe_read polled UART RX + NVRAM input/output-device=/failsafe | ✅ DONE -- install-console opens /failsafe (device_type=serial, SF-provided); `42 .` piped via `-serial mon:stdio` echoes `42 ok` after clearing the pager prompt with `f` |
| 6 | failsafe_read strictly non-blocking per SF "serial" contract | ✅ DONE -- the Commit-5 "block on first byte" shape deadlocked key_down() which polls read on every paginated line. File-backed boot now reaches `ok` without a keystroke; the previous need to press `f` turned out to be compensating for this bug, not the banner pager |
| 7 | Disable -DDEBUG, shed ~70 KiB of DPRINTF trace output | ✅ DONE -- default boot: 2,350 bytes verbatim banner + `ok` (was 72 KiB); mon:stdio `42 .` echoes `42 ok` without trace interleave |
| 8 | Retire phase1 hand-off scaffolding; trim the inner `[syscall ...]` diagnostic | ✅ DONE -- 2,350 → 2,208 bytes; phase1 tests flow directly into SF's banner with no transitional decoration |
| Final | Enter the interpret() read-eval loop | ✅ DONE at Commit 5 -- `ok` prompt appears on UART1; simple Forth like `42 .` echoes |

Decisions taken during the planning pass:
- OF machdep lives at `machdep/pegasos2/of/`, not inside the
  vendored `upstream/` tree.  Keeps the clean-room audit trail
  honest: upstream is the verbatim import; our Pegasos2
  machdep is our own work.
- Expand the QEMU flash temporarily if Commit 3 busts 512 KiB.
  Compression (spec 00's DONA scheme or SF's zrun) is a
  separate follow-up, not a blocker.
- Keep the existing exception vectors (panic / decrementer /
  syscall stub).  The pegasos2 machdep's machine_initialize()
  does **not** `memcpy` SF's handlers over them (unlike bebox).
  machine_probe_read/write instead drive our panic path via a
  "probe active" flag that the DSI handler checks before
  dumping, advancing SRR0 and rfi'ing on match.
- `DPRINTF` (via `#define DEBUG` in SF_CFLAGS) was kept on from
  Commit 1 through 6; Commit 7 turns it off now that `ok` is
  reliable.  Re-enable only to diagnose a specific OF regression.

## Other near-term milestones (orthogonal to OF bring-up)

Each is roughly a single focused commit and can run in parallel
with the OF work above if useful.

### Near-term, unblocks later work

**W83194 SMBus probe to replace the assumed 133 MHz FSB.**
Today `timer_calibrate()` hardcodes the Pegasos II board
default (FSB=133 MHz). Real HW may be wired for 166 MHz on
later boards. Spec 01 §"Clock detection" gates the probe on a
functional SMBus driver (VT8231 fn 4, address 0x69). Once that
lands, `timer_calibrate()` calls the probe first and falls back
to the default on failure. Does not affect QEMU (the emulated
TB is independent of our probe result there).

**External-interrupt (0x500) dispatch.** MSR[ME]/[RI] are set,
Decrementer (0x900) runs a real handler feeding `get_msecs()`,
Syscall (0xC00) has a working trampoline + stub dispatcher.
ExtInt still panics. Lands with its first consumer -- the
plausible one is OF UART RX for interactive Forth, which would
want handler registration anyway. MV64361 main/GPP IC register
survey + VT8231 PIC init + registered-handler table + dispatch
logic; test by arming the consumer's IRQ and observing the
handler fire. Spec 02 §"Interrupt controller" / spec 04
§"VT8231 PIC" have the register maps.

**IEEE-1275 client-interface body (spec 06).** The 0xC00
trampoline is done. syscall_dispatch() currently returns a
sentinel 0xBABE. Expanding it to decode the caller's
call-structure pointer in r3 and route to OF-exposed services
(getprop / setprop / finddevice / call-method / open / close /
boot / exit / interpret, ...) is substantial -- several hundred
LOC of service plumbing that only makes sense once the OF
device tree exists. Lands with the OF runtime.

**OF Forth runtime bring-up.** The biggest remaining piece.
`upstream/smartfirmware/bin/of/` ships the Forth interpreter and
OF device-tree framework under CodeGen's source license (same
license as our rewrite). A thin Pegasos2 machdep layer slots in;
phase1 hands off to it instead of halting. Spec 05 §"install-
console" (serial-first defaults, health-checked attach) is the
headline feature. Multi-commit: machdep shim, OF build into the
image, phase1 handoff, initial `ok` prompt.


### Mid-term, enables real work

**Forth / OpenFirmware runtime bring-up.** Biggest remaining
piece. `upstream/smartfirmware/bin/of/` has the Forth interpreter
and OF device-tree framework as starting material (CodeGen
source license, see `LICENSES/`). A thin Pegasos2 machdep layer
slots it in; phase1 then hands off to it instead of halting.

**NVRAM (M48T59).** Spec 08. ~300 lines of code, self-contained.
Enables `setenv / printenv` for future Forth configuration.

### Longer-term, stretch

**Real-hardware CPU init.** Cache invalidate/enable, BAT setup
per spec 01, MSR bits, clock-gen (Winbond W83194) probing over
SMBus, exception handler install. Most of this is pure asm or
tight C and can be tested only on hardware.

**SeaBIOS-derived VGA ROM test** (spec 09 Bug 1 Test #2 part 2).
Our current test uses Bochs VBIOS via QEMU's `-vga std`. Running
a SeaBIOS-derived ROM would exercise a different code path.

**Radeon R200 VBIOS test** (spec 09 Bug 1 Test #3). Needs a real
ROM dump from a Radeon 9200/9250; once available, feed it
through the same loader path as bochs-VGA.

## References inside this tree (in addition to `docs/`)

- `CLAUDE.md` — role briefing, clean-room rules.
- `CLEAN-ROOM-BOUNDARY.md` — the formal policy.
- `README.md` — project summary.
- `SPEC-QUESTIONS.md` — maintainer-routed spec-clarity issues.
- `VERSIONS.md` — upstream pins + local modifications.
- `LICENSES/` — CodeGen and SciTech license texts.
- `upstream/smartfirmware/` — CodeGen's SmartFirmware source
  (public), pinned at commit 06ef397.
- `upstream/x86emu/` — U-Boot 2015.d `drivers/bios_emulator/`,
  minus the atibios/vesa/biosemu/bios/besys wrappers. One local
  modification (0F FE handler) documented in `VERSIONS.md`.
