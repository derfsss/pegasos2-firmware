/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Pegasos2 SmartFirmware platform glue: the registries that tie
 *  the per-port machdep + Forth + filesystem code together. This
 *  file is intentionally small -- the heavy lifting lives in
 *  topic-specific neighbours:
 *
 *    machine_hooks.c   machine_init_load / machine_init_program /
 *                      machine_go (SF machdep contract)
 *    forth_diag.c      diagnostic Forth words (test-ci, set-bootargs,
 *                      heap-info, test-ci-boot)
 *    rtc_word.c        get-time-of-day / set-time-of-day +
 *                      install_pegasos2_ci_services
 *    smart_boot.c      smart-boot OS-priority dispatcher
 *    partition_pkg.c   RDB partition packages + install hook
 *    pci_tree.c, ide_driver.c, boot_kernel.c, ci_entry.c, ...
 *
 *  pegasos2_words.h forward-declares the Forth-word handlers
 *  collected from those neighbours so init_pegasos2[] below can
 *  reference them.
 */

#include "defs.h"
#include "fs.h"
#include "exe.h"
#include "pegasos2_words.h"

/* --------------------------------------------------------------- *
 *  g_filesys[] -- filesystem driver registry consumed by fs/fs.c's *
 *  file_system() dispatcher. Iterated by file_system on each of    *
 *  disklbl.c's load / list-files paths until one entry accepts     *
 *  (returns NO_ERROR or R_END) or all return E_NO_FILESYS.         *
 *                                                                  *
 *  Ordering rules:                                                 *
 *   - Partition parsers (g_dos_partition, g_amiga_rdb) go FIRST.   *
 *     They read the disk's MBR / RDB and, if one is found,         *
 *     recursively invoke file_system on each partition's slice.    *
 *     If no signature, they return E_NO_FILESYS and iteration      *
 *     continues with whole-disk FS readers.                        *
 *   - iso9660 next because CDs never have an MBR/RDB wrapper.      *
 *   - FAT + ext2 cover whole-disk (unpartitioned) images too;      *
 *     order between them doesn't matter (each magic-checks).       *
 *  NULL terminator required.                                       *
 * --------------------------------------------------------------- */
extern Filesys g_dos_partition;    /* upstream/fs/dospart.c */
extern Filesys g_amiga_rdb;        /* machdep/pegasos2/of/amiga_rdb.c */
extern Filesys g_amiga_ffs;        /* machdep/pegasos2/of/amiga_ffs.c */
extern Filesys g_amiga_sfs;        /* machdep/pegasos2/of/amiga_sfs.c */
extern Filesys g_amiga_pfs3;       /* machdep/pegasos2/of/amiga_pfs3.c */
extern Filesys g_fs_exfat;         /* machdep/pegasos2/of/fs_exfat.c */
extern Filesys g_iso9660_compat;   /* machdep/.../iso9660_compat.c
                                    * (path-normaliser around the
                                    * upstream g_iso9660_fs reader) */
extern Filesys g_dos_fat;          /* upstream/fs/dosfat.c */
extern Filesys g_linux_ext2fs;     /* upstream/fs/ext2fs.c */

Filesys *g_filesys[] = {
	/*
	 * ISO9660 first. The Debian PPC and MorphOS install discs
	 * both ship a hybrid layout: an Apple Partition Map (or a
	 * DOS MBR with valid 0x55 0xAA boot signature) at sector 0
	 * for Mac/PC dual-boot, AND a Joliet/Rock Ridge ISO9660
	 * volume at the standard sector 16 offset. With dos_partition
	 * walking first, it consumes the path, recurses into "partition
	 * 0" and the ISO9660 reader never gets a turn -- net result
	 * "filesystem not supported" on every CHRP-bootable Linux/
	 * MorphOS install disc that isn't a pure ISO9660 like AOS4's.
	 *
	 * Putting iso9660_compat first is safe because the upstream
	 * reader scans sectors 16..31 for the ISO9660 ID string and
	 * returns E_NO_FILESYS quickly on disks that lack it; HD's
	 * with RDB / MBR fall through to the partition parsers below
	 * without any data corruption.
	 */
	&g_iso9660_compat,               /* ISO9660 (CDs, hybrid CDs) */
	/* Partition parsers next so they can carve a HD before
	 * whole-disk FS readers probe the surface. */
	&g_dos_partition,                /* MBR / DOS partition table */
	&g_amiga_rdb,                    /* Amiga Rigid Disk Block */
	/* Whole-disk / post-partition FS readers. */
	&g_amiga_ffs,                    /* OFS/FFS/Intl/LNFS (DOS\0..\3,\6,\7) */
	&g_amiga_sfs,                    /* SmartFileSystem (SFS\0, SFS\2) */
	&g_amiga_pfs3,                   /* PFS3 (PFS\1, PFS\2, AFS\1) */
	&g_fs_exfat,                     /* exFAT */
	&g_dos_fat,
	&g_linux_ext2fs,
	NULL
};

/* --------------------------------------------------------------- *
 *  g_exec_list[] -- binary-image loaders for SF's f_load/f_go      *
 *  path. exec_is_exec() iterates this list until one entry's       *
 *  is_exec returns TRUE; exec_load() then invokes that entry's     *
 *  load callback. pegasos2_ppc_elf_exec is defined in              *
 *  boot_kernel.c and wraps our spec-07 validator + PT_LOAD walker. *
 *  NULL-terminated.                                                *
 * --------------------------------------------------------------- */
extern Exec_entry pegasos2_ppc_elf_exec;

Exec_entry *g_exec_list[] = {
	&pegasos2_ppc_elf_exec,
	NULL
};

/* --------------------------------------------------------------- *
 *  Default NVRAM contents (g_nvram[])                              *
 * --------------------------------------------------------------- */

/*
 * SmartFirmware's nvram.c declares `extern struct nvram_data g_nvram[]`
 * and iterates until it sees a NULL .name terminator. Defaults branch
 * on CONFIG_TARGET (set in the Makefile to qemu or hw, materialised
 * here as PEGASOS_TARGET_QEMU=1 or PEGASOS_TARGET_HW=1). All other
 * code paths are identical -- runtime probing handles every
 * hardware-present / hardware-absent split.
 */
struct nvram_data {
	char *name;
	char *val;
};

/*
 * Real Pegasos II keeps OF env vars in the VT8231 RTC's 114-byte
 * battery-backed CMOS area (CR2032-fed; SPEC-QUESTIONS.md Q7).
 * User `setenv` changes flow through SF's save_config ->
 * set_nvram -> machine_nvram_write (machdep.c) -> CMOS bytes, and
 * persist across reboots. So the HW defaults below are
 * pessimistic: auto-boot? = false so the user reaches the ok
 * prompt and can edit boot-command before turning auto-boot on.
 *
 * QEMU pegasos2 models the same VT8231 RTC, so machine_nvram_read
 * works during a single qemu run, but qemu does not by default
 * persist the CMOS bytes across qemu invocations. Each fresh
 * `qemu-system-ppc -M pegasos2 ...` therefore presents an empty
 * CMOS bank, machine_nvram_read returns "corrupt", and load_nvram
 * falls back to these compile-time defaults. So the QEMU defaults
 * are optimistic: auto-boot? = true with a 3-second countdown,
 * since that's the most useful turnkey behaviour for testing.
 */
struct nvram_data g_nvram[] = {
	{ "real-mode?",           "false"     },
	{ "security-mode",        "none"      },
	/*
	 * Point at /failsafe (created by install_failsafe with
	 * device_type="serial") rather than a /serial path we'd
	 * otherwise have to define ourselves. install-console runs
	 * `output-device output input-device input` which opens the
	 * path as a device-tree node; /failsafe's open/close/read/
	 * write methods route through our failsafe_write (uart_putc)
	 * and failsafe_read (polled uart_getc).
	 */
	{ "input-device",         "/failsafe" },
	{ "output-device",        "/failsafe" },
	/*
	 * Boot defaults. A bare `boot` at the ok prompt (no args)
	 * triggers SF's do_load (admin.c) which falls back to
	 * boot-device + boot-file when the user didn't supply them.
	 *     `cd` expands via /aliases/cd to
	 *       /pci@80000000/ide@C,1/cd@1,0 (install_aliases)
	 *     `/test.elf;1` is what genisoimage's ISO9660 Level-1 PVD
	 *       uses for the Makefile test-iso target.
	 *
	 * SF main.c reads auto-boot? at startup; if true it prints
	 * "Auto-boot in N seconds - press ESC to abort, ENTER to
	 * boot:" and waits auto-boot-timeout milliseconds. Any key
	 * during the countdown aborts to the ok prompt; ENTER skips
	 * the wait and runs boot-command immediately; timeout expiry
	 * runs boot-command. boot-command is interpreted as Forth,
	 * so it can be any sequence of words.
	 */
	{ "boot-device",          "cd"             },
	{ "boot-file",            "/test.elf;1"    },
	/*
	 * boot-command runs as Forth at auto-boot expiry (or via
	 * ENTER during the countdown). `smart-boot` is our priority-
	 * aware dispatcher in smart_boot.c: it walks every disk's
	 * RDB partition packages, classifies each by DosType
	 * (AmigaOS family / Linux / MorphOS), and dispatches to the
	 * appropriate per-OS loader following the
	 * `boot-os-priority` NVRAM var. For AmigaOS-family
	 * partitions the loader is amigaboot.of, which has its own
	 * boot-priority sort + selection menu, so we hand off
	 * without specifying which DH#: -- amigaboot picks.
	 */
	{ "boot-command",         "smart-boot"     },
	/*
	 * boot-os-priority: comma-separated list of OS families to
	 * try in order. First family with a candidate partition
	 * (DosType match + BootPri >= 0) wins. Names: amigaos,
	 * linux, morphos.
	 */
	{ "boot-os-priority",     "amigaos,morphos,linux" },
#if defined(PEGASOS_TARGET_HW)
	{ "auto-boot?",           "false"          },
	{ "auto-boot-timeout",    "5000"           },
#elif defined(PEGASOS_TARGET_QEMU)
	{ "auto-boot?",           "true"           },
	{ "auto-boot-timeout",    "3000"           },
#else
#error "PEGASOS_TARGET_QEMU or PEGASOS_TARGET_HW must be defined (CONFIG_TARGET=qemu|hw)"
#endif
	{ "use-nvramrc?",         "false"          },
	{ NULL, NULL }                              /* terminator */
};

/* --------------------------------------------------------------- *
 *  Default font for machine_font                                    *
 * --------------------------------------------------------------- */

static Byte g_default_font[] = {
#include FONT_FILE
};

/*
 * Forth word: machine-font (-- addr width height advance min-char
 * #glyphs ). Pushes the compiled-in default font descriptor on the
 * data stack. Needed by fb.c / display.c; the IFCKSP macro checks
 * stack headroom before the 6 pushes.
 */
CC(machine_font)
{
	IFCKSP(e, 0, 6);
	PUSH(e, g_default_font);
	PUSH(e, FONT_WIDTH);
	PUSH(e, FONT_HEIGHT);
	PUSH(e, FONT_ADVANCE);
	PUSH(e, FONT_FIRST);
	PUSH(e, FONT_COUNT);
	return NO_ERROR;
}

/* --------------------------------------------------------------- *
 *  logo_pixmap placeholder                                          *
 * --------------------------------------------------------------- */

/*
 * admin.c's banner path references logo_pixmap[] as a default OEM
 * logo. The block is gated by `if (logo)` derived from NVRAM
 * config; with our defaults logo stays 0 and the pixmap is never
 * dereferenced. Supply a 1-byte placeholder so the link resolves.
 */
Byte logo_pixmap[1] = { 0 };

/* --------------------------------------------------------------- *
 *  ppc_get_version                                                  *
 * --------------------------------------------------------------- */

/*
 * Called by cpu-ppc.c to fill in the /cpu "cpu-version" property.
 * SPR 287 == PVR; the upper 16 bits identify the core family, the
 * lower 16 bits the sub-revision. On the MPC7447A QEMU pegasos2
 * models the value is 0x80020102.
 */
uInt
ppc_get_version(void)
{
	uInt pvr;
	__asm__ volatile ("mfspr %0, 287" : "=r"(pvr));
	return pvr;
}

/* --------------------------------------------------------------- *
 *  Forth-word registry (init_list) + package install registry      *
 *  (install_list).                                                  *
 * --------------------------------------------------------------- */

static const Initentry init_pegasos2[] = {
	{ (Byte *)"test-ci", f_test_ci, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  invoke ci_handler with `finddevice \"/\"` and print result") },
	{ (Byte *)"boot-kernel", f_boot_kernel, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(load-addr --)  validate ELF at load-addr, transfer per spec 07") },
	{ (Byte *)"test-boot", f_test_boot, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  copy built-in test kernel to 0x800000 and transfer") },
	{ (Byte *)"test-boot-bad", f_test_boot_bad, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  exercise boot-kernel hardening with 6 malformed ELF headers") },
	{ (Byte *)"set-bootargs", f_set_bootargs, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(addr len --)  publish string on /chosen/bootargs (spec 07 §AOS4)") },
	{ (Byte *)"heap-info", f_heap_info, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  print SF malloc-pool bounds + spec 07 load-address compliance") },
	{ (Byte *)"ls-pci", f_ls_pci, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  walk /pci@80000000 and /pci@c0000000 and print each child device") },
	{ (Byte *)"test-ide-probe", f_test_ide_probe, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  print each IDE disk/cd discovered by install_ide_driver") },
	{ (Byte *)"test-read-block", f_test_read_block, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  open cd@0,0, read LBA 16, verify ISO9660 CD001 signature") },
	{ (Byte *)"test-iso-ls", f_test_iso_ls, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  list the root directory of the first ATAPI ISO9660 volume") },
	{ (Byte *)"test-aliases", f_test_aliases, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  print every entry under /aliases (cd, cdrom, hd, disk)") },
	{ (Byte *)"test-ci-boot", f_test_ci_boot, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(addr len --)  invoke ci_handler(\"boot\", <bootspec>) from Forth") },
	{ (Byte *)"get-time-of-day", f_get_time_of_day, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"( -- sec min hr day mo yr)  read VT8231 RTC (fallback 1970-01-01)") },
	{ (Byte *)"set-time-of-day", f_set_time_of_day, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(sec min hr day mo yr --)  write VT8231 RTC (noop if absent)") },
	{ (Byte *)"smart-boot", f_smart_boot, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(--)  walk RDB partitions; pick by `boot-os-priority` (amigaos,linux,morphos); dispatch to per-OS loader") },
	{ NULL, NULL, INVALID_FCODE, F_NONE, T_FUNC HELP("") }
};

extern const Initentry init_funcs[];
extern const Initentry init_funcs64[];
extern const Initentry init_control[];
extern const Initentry init_packages[];
extern const Initentry init_forth[];
extern const Initentry init_admin[];
extern const Initentry init_nvedit[];
extern const Initentry init_other[];
extern const Initentry init_device[];
extern const Initentry init_debug[];
extern const Initentry init_filesystem[];  /* fs/fs.c: $list-files, list-files */

const Initentry *init_list[] = {
	init_funcs,
	init_funcs64,
	init_control,
	init_packages,
	init_forth,
	init_admin,
	init_nvedit,
	init_other,
	init_device,
	init_debug,
	init_filesystem,
	init_pegasos2,
	NULL
};

/*
 * install_list[] order (run during SF's install_packages()):
 *
 *  - install_pci_tree must run before install_ide_driver, which
 *    walks the PCI tree to find the VT8231 IDE controller.
 *  - install_deblocker and install_disklabel must run before any
 *    attempt to open() a disk child (atadisk's f_ata_disk_open
 *    calls $open-package on both /packages/deblocker and
 *    /packages/disk-label). install_ide_driver itself only calls
 *    probe_ata_disks (IDENTIFY + READ_CAPACITY) which doesn't
 *    open the disks, so the order between ide_driver and
 *    deblocker/disklabel isn't strictly constrained, but
 *    deblocker/disklabel earlier reads more naturally.
 *  - install_aliases must run AFTER install_ide_driver because it
 *    derives /aliases/cd and /aliases/hd from the IDE children's
 *    paths.
 *  - install_partition_packages must run AFTER install_aliases
 *    (and after the disk children exist) because it opens the
 *    disk via open-dev and walks its RDB.
 *  - install_client_services creates /openprom/client-services
 *    and registers the IEEE-1275 CI method table under it so
 *    client_interface() dispatches via execute_static_method_name
 *    instead of the linear fallback scan.
 *  - install_pegasos2_ci_services adds pegasos2-specific CI
 *    services (get-time-of-day etc.) to /openprom/client-services.
 *    Must run after install_client_services.
 */
EC(install_root);
EC(install_root_cells);
EC(install_chosen);
EC(install_memory);
EC(init_options_from_nvram);
EC(install_powerpc_cpu);
EC(install_display);
EC(install_failsafe);
EC(install_pci_tree);
EC(install_deblocker);
EC(install_disklabel);
EC(install_ide_driver);
EC(install_aliases);
EC(install_partition_packages);
EC(install_client_services);
EC(install_pegasos2_ci_services);

const Command install_list[] = {
	install_root,
	install_root_cells,
	install_chosen,
	install_memory,
	init_options_from_nvram,
	install_powerpc_cpu,
	install_display,
	install_failsafe,
	install_pci_tree,
	install_deblocker,
	install_disklabel,
	install_ide_driver,
	install_aliases,
	install_partition_packages,
	install_client_services,
	install_pegasos2_ci_services,
	NULL
};
