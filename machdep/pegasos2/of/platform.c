/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Platform glue for the SmartFirmware OF runtime. This file provides
 *  the per-port tables (init_list, install_list), Forth-word stubs
 *  (machine_font, machine_probe_all, ...), CPU-version reader, and the
 *  initial NVRAM template -- things bebox/amd64/i386 ports put in
 *  their machdep.c but that we kept separate from our machine_*
 *  interface definitions to limit file size.
 */

#include "defs.h"
#include "fs.h"
#include "exe.h"

/* --------------------------------------------------------------- *
 *  g_filesys[] -- filesystem driver registry consumed by fs/fs.c's *
 *  file_system() dispatcher. Iterated by file_system on each of    *
 *  disklbl.c's load / list-files paths until one entry accepts     *
 *  (returns NO_ERROR or R_END) or all return E_NO_FILESYS.         *
 *                                                                  *
 *  Ordering rules:                                                 *
 *   - Partition parsers (g_dos_partition) go FIRST. They read the  *
 *     disk's MBR / disklabel and, if one is found, recursively     *
 *     invoke file_system on each partition's slice. If no MBR      *
 *     signature, they return E_NO_FILESYS and the iteration        *
 *     continues with whole-disk FS readers.                        *
 *   - iso9660 next because CDs never have an MBR wrapper.          *
 *   - FAT + ext2 cover whole-disk (unpartitioned) images too;      *
 *     order between them doesn't matter (each magic-checks).       *
 *                                                                  *
 *  Arc FS-B (Amiga) will add &g_amiga_rdb (partition) and the      *
 *  Amiga filesystem readers (FFS2 first) to this list. NULL        *
 *  terminator required.                                            *
 * --------------------------------------------------------------- */
extern Filesys g_dos_partition;    /* upstream/fs/dospart.c */
extern Filesys g_amiga_rdb;        /* machdep/pegasos2/of/amiga_rdb.c */
extern Filesys g_amiga_ffs;        /* machdep/pegasos2/of/amiga_ffs.c */
extern Filesys g_amiga_sfs;        /* machdep/pegasos2/of/amiga_sfs.c */
extern Filesys g_amiga_pfs3;       /* machdep/pegasos2/of/amiga_pfs3.c */
extern Filesys g_fs_exfat;         /* machdep/pegasos2/of/fs_exfat.c */
extern Filesys g_iso9660_fs;       /* upstream/fs/iso9660.c */
extern Filesys g_dos_fat;          /* upstream/fs/dosfat.c */
extern Filesys g_linux_ext2fs;     /* upstream/fs/ext2fs.c */

Filesys *g_filesys[] = {
	/* Partition parsers first so they can carve the disk before
	 * whole-disk FS readers probe the surface. */
	&g_dos_partition,                /* MBR / DOS partition table */
	&g_amiga_rdb,                    /* Amiga Rigid Disk Block */
	/* Whole-disk / post-partition FS readers. */
	&g_amiga_ffs,                    /* OFS/FFS/Intl/LNFS (DOS\0..\3,\6,\7) */
	&g_amiga_sfs,                    /* SmartFileSystem (SFS\0, SFS\2) */
	&g_amiga_pfs3,                   /* PFS3 (PFS\1, PFS\2, AFS\1) */
	&g_fs_exfat,                     /* exFAT */
	&g_iso9660_fs,
	&g_dos_fat,
	&g_linux_ext2fs,
	NULL
};

/* --------------------------------------------------------------- *
 *  g_exec_list[] -- binary image loaders for SF's f_load/f_go     *
 *  path. exec_is_exec() iterates this list until one entry's      *
 *  is_exec returns TRUE; exec_load() then invokes that entry's    *
 *  load callback. pegasos2_ppc_elf_exec is defined in             *
 *  boot_kernel.c and wraps our spec-07 validator + PT_LOAD walker *
 *  so the M1..M5 hardening coverage applies to real boot too.     *
 *  NULL-terminated. M8 may add &pegasos2_aout_exec etc. here.     *
 * --------------------------------------------------------------- */
extern Exec_entry pegasos2_ppc_elf_exec;

Exec_entry *g_exec_list[] = {
	&pegasos2_ppc_elf_exec,
	NULL
};

/* --------------------------------------------------------------- *
 *  Default NVRAM contents (g_nvram)                                 *
 * --------------------------------------------------------------- */

/*
 * SmartFirmware's nvram.c declares `extern struct nvram_data g_nvram[]`
 * and iterates until it sees a NULL .name terminator. We ship an empty
 * table for now (just the sentinel); all OF env vars fall back to
 * compile-time defaults in admin.c / packages.c / etc. When the M48T59
 * driver lands, machine_nvram_read fills in overrides.
 */
struct nvram_data {
	char *name;
	char *val;
};

struct nvram_data g_nvram[] = {
	{ "real-mode?",           "false"     },
	{ "security-mode",        "none"      },
	/*
	 * Point at /failsafe (created by install_failsafe with
	 * device_type="serial") rather than a /serial path we would
	 * otherwise have to define ourselves.  install-console runs
	 * `output-device output input-device input` which opens the
	 * path as a device-tree node; /failsafe's open/close/read/
	 * write methods route through our failsafe_write (uart_putc)
	 * and failsafe_read (polled uart_getc).
	 */
	{ "input-device",         "/failsafe" },
	{ "output-device",        "/failsafe" },
	/*
	 * Boot defaults (Block 7/N). A bare `boot` at the ok prompt
	 * (no args) triggers SF's do_load (admin.c:1435) which falls
	 * back to these when the user didn't supply a device or file.
	 *     `cd` expands via /aliases/cd to
	 *       /pci@80000000/ide@C,1/cd@1,0 (Block 5/N install_aliases)
	 *     `/test.elf;1` is what genisoimage's ISO9660 Level-1 PVD
	 *       uses for our Makefile test-iso target (Block 5/N). For
	 *       real AOS4 boot the user would `setenv boot-file
	 *       /amigaboot.of;1` at the ok prompt.
	 *
	 * auto-boot? stays false so the default three-test regression
	 * matrix output (no CD, unchanged 2208 bytes) is preserved.
	 * Users enable auto-boot interactively:
	 *     ok setenv auto-boot? true
	 *     ok reset
	 * SF's main.c:262-316 then runs `boot-command` verbatim
	 * (default "boot") after a 1-second countdown.
	 */
	{ "boot-device",          "cd"             },
	{ "boot-file",            "/test.elf;1"    },
	{ "boot-command",         "boot"           },
	{ "auto-boot?",           "false"          },
	{ "auto-boot-timeout",    "1000"           },
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
 * data stack. Needed by fb.c / display.c; the `IFCKSP` macro checks
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
 *  Forth-word stubs expected by the machdep interface               *
 * --------------------------------------------------------------- */

/*
 * The EC() macros in defs.h declare these as extern Retcode fns. A
 * working port provides real ones; we stub them for now so the link
 * resolves. Commit 6+ replaces these with real behaviour.
 *
 *   machine_probe_all      -- "probe-all" device-tree walker hook
 *   machine_secondary_diag -- extended diagnostics
 *   machine_init_program   -- fixup-on-load for an executable image
 *   machine_go             -- jump-to-loaded-binary helper
 *   machine_init_load      -- prep-the-loader, for "load-base" etc.
 */
CC(machine_probe_all)       { (void)e; return NO_ERROR; }
CC(machine_secondary_diag)  { (void)e; return NO_ERROR; }

/*
 * machine_init_load (--): called by SF's admin.c:try_load before
 * invoking the disk's `load` method. Must set e->load to a
 * DRAM address where the file contents will be buffered. Per
 * spec 07 §Load-address, the canonical default is 0x00400000
 * (4 MiB mark) -- situated above SF's malloc pool
 * (0x00200000..0x003FFFFF, Boot 4/N+2 §Load-address) and below
 * x86emu's 1 MiB buffer (0x01000000..0x010FFFFF, also Boot
 * 4/N+2). Uses g_machine_memory+g_machine_memory_size exactly
 * like bebox/i386 machdeps; that arithmetic on our setup yields
 * 0x00400000 because the SF pool ends there.
 */
CC(machine_init_load)
{
	/*
	 * Spec 07 §Load-address: default kernel load at 0x00400000
	 * (4 MiB mark). SF pool ends at 0x003FFFFF (Boot 4/N+2); the
	 * default load buffer starts immediately above. x86emu's 1
	 * MiB buffer is at 0x01000000 (Boot 4/N+2), so we have 12 MiB
	 * of clear buffer between 0x00400000 and 0x00FFFFFF for
	 * typical kernel images (amigaboot.of is ~36 KiB, vmlinux
	 * ~1-4 MiB).
	 *
	 * NOT `g_machine_memory + g_machine_memory_size` (the
	 * bebox/i386 pattern) because our g_machine_memory_size
	 * reports the whole DRAM available to the OS (510 MiB on
	 * QEMU -m 512), not just the malloc pool. That sum lands at
	 * DRAM top (0x20000000) which is past the last valid byte.
	 */
	e->load = (Byte *)0x00400000u;
	return NO_ERROR;
}

/*
 * machine_init_program (--): called by f_init_program from
 * admin.c:1223 after the image is in e->load / e->loadlen.
 * Probes g_exec_list[] to pick a handler and stashes it in
 * e->loadentry; returns NO_ERROR on match or E_BAD_IMAGE if
 * nothing recognises the magic. The i386 machdep does the
 * same one-liner (i386/machdep.c:621-626). try_load calls
 * execute_word("init-program") at admin.c:1428 after the disk's
 * load method fills e->load, so by the time we see e->load is
 * fully populated.
 */
CC(machine_init_program)
{
	return exec_is_exec(e) ? NO_ERROR : E_BAD_IMAGE;
}

/*
 * machine_go (--): the final transfer. Called from admin.c:1263
 * when the user types `go` (or when `boot` expands to `load` +
 * `go`). At entry e->load/e->loadlen contain the raw file bytes;
 * exec_load() hands them to our handler's load callback which
 * walks PT_LOADs and sets e->entrypoint + g_boot_image_high_end.
 * Then we compute r1 and call machine_jump_os to perform the
 * spec-07 handoff (BATs + MSR[IR|DR] + r3..r7 per
 * boot_kernel.S). Does not return.
 *
 * If exec_load returns an error (malformed image) we propagate
 * the error back to f_go, which reports it and returns the user
 * to the ok prompt -- same behaviour as test-boot-bad validating
 * the Forth-level boot-kernel word.
 */
extern uInt g_boot_image_high_end;
extern void machine_jump_os(uInt entry, uInt ci_handler_addr,
			    uInt stack_top);
extern int  ci_handler(void *args);

CC(machine_go)
{
	if (!exec_is_exec(e))
		return E_BAD_IMAGE;

	Retcode ret = exec_load(e);
	if (ret != NO_ERROR)
		return ret;

	/*
	 * Stack pointer handed to the OS. Matches the f_boot_kernel
	 * Forth-path convention: 4 KiB of headroom above the highest
	 * PT_LOAD end, aligned down to 16 (PPC SysV ABI). If the +4KiB
	 * would overflow the address space, pin to high_end (the OS
	 * relocates r1 immediately after entry).
	 */
	uInt stack_top = g_boot_image_high_end + 0x1000u;
	if (stack_top < g_boot_image_high_end)
		stack_top = g_boot_image_high_end;
	stack_top &= ~0xFu;

	cprintf(e, "machine_go: e_entry=0x%X r1=0x%X; transferring...\n",
		(unsigned)e->entrypoint, (unsigned)stack_top);

	/* Echo /chosen/bootpath + /chosen/bootargs so we can verify
	 * what the loaded image will see. The OS reads these via
	 * `getprop /chosen bootargs ...` shortly after entry and uses
	 * them to choose an OS root device, kernel commandline, etc.
	 * (Spec 07 §AOS4 lists `bootdevice=` as the canonical AOS4
	 * argument that selects which RDB partition to boot from.) */
	{
		Byte *prop_val = NULL;
		Int   prop_len = 0;
		if (prop_get_str(e->chosen->props, "bootpath", CSTR,
				 &prop_val, &prop_len) == NO_ERROR && prop_val)
			cprintf(e, "  /chosen/bootpath = \"%S\"\n",
				prop_val, prop_len);
		prop_val = NULL; prop_len = 0;
		if (prop_get_str(e->chosen->props, "bootargs", CSTR,
				 &prop_val, &prop_len) == NO_ERROR && prop_val)
			cprintf(e, "  /chosen/bootargs = \"%S\"\n",
				prop_val, prop_len);
	}

	machine_jump_os((uInt)e->entrypoint,
			(uInt)(uPtr)&ci_handler, stack_top);

	/* Unreachable; machine_jump_os does not return. */
	return NO_ERROR;
}


/* --------------------------------------------------------------- *
 *  test-ci -- synthetic IEEE-1275 client-interface smoke test      *
 * --------------------------------------------------------------- */

/*
 * `test-ci ( -- )` exercises the IEEE-1275 client-interface
 * dispatcher across two services with different arg/return
 * signatures and prints each round-trip result:
 *
 *   finddevice "/chosen"   -- nargs=1 nrets=1 (path -> phandle)
 *   getprop <ph> "stdout"  -- nargs=4 nrets=1 (phandle, name,
 *                             buf, buflen -> actual length)
 *
 * A healthy dispatcher prints:
 *
 *     test-ci: finddevice /chosen ret=0 phandle=0xXXXXXX
 *     test-ci: getprop stdout     ret=0 len=4 ihandle=0xXXXXXX
 *
 * The stdout property holds an encoded ihandle (big-endian 32-bit
 * pointer, IEEE-1275 §3.3.3.1.1 "encoded-int") pointing at the
 * install-console-chosen output device. 4-byte length is expected
 * on a 32-bit build. The synthetic test is our own firmware
 * calling ci_handler directly: no OS, no r5-handoff -- that path
 * lands with the spec-07 boot loader.
 */
extern int ci_handler(void *args);

static uInt
be32_load(const uChar *p)
{
	return ((uInt)p[0] << 24) | ((uInt)p[1] << 16) |
	       ((uInt)p[2] << 8)  | (uInt)p[3];
}

CC(f_test_ci)
{
	Cell fd_args[5];
	Cell gp_args[8];
	static uChar buf[64];
	int ret;
	Cell phandle;
	Cell actual;

	/* 1) finddevice "/chosen" -- 1 arg, 1 return */
	fd_args[0] = (Cell)(uPtr)"finddevice";
	fd_args[1] = 1;
	fd_args[2] = 1;
	fd_args[3] = (Cell)(uPtr)"/chosen";
	fd_args[4] = 0;

	ret = ci_handler(fd_args);
	phandle = fd_args[4];

	cprintf(e, "test-ci: finddevice /chosen ret=%d phandle=0x%X\n",
	        ret, (unsigned)phandle);

	if (ret != 0 || phandle == 0)
		return NO_ERROR;

	/* 2) getprop <phandle> "stdout" buf 64 -- 4 args, 1 return */
	for (int i = 0; i < (int)sizeof buf; i++)
		buf[i] = 0;

	gp_args[0] = (Cell)(uPtr)"getprop";
	gp_args[1] = 4;
	gp_args[2] = 1;
	gp_args[3] = phandle;
	gp_args[4] = (Cell)(uPtr)"stdout";
	gp_args[5] = (Cell)(uPtr)buf;
	gp_args[6] = (Cell)sizeof buf;
	gp_args[7] = 0;

	ret = ci_handler(gp_args);
	actual = gp_args[7];

	cprintf(e, "test-ci: getprop stdout     ret=%d len=%d ihandle=0x%X\n",
	        ret, (int)actual,
	        (actual >= 4) ? (unsigned)be32_load(buf) : 0u);

	/* 3) getprop <phandle> "bootargs" buf 64 -- exercises the
	 * bootargs property the OS will query per spec 07 §AOS4. Starts
	 * as "" from chosen.c's install_chosen; set-bootargs updates it. */
	for (int i = 0; i < (int)sizeof buf; i++)
		buf[i] = 0;

	gp_args[0] = (Cell)(uPtr)"getprop";
	gp_args[1] = 4;
	gp_args[2] = 1;
	gp_args[3] = phandle;
	gp_args[4] = (Cell)(uPtr)"bootargs";
	gp_args[5] = (Cell)(uPtr)buf;
	gp_args[6] = (Cell)sizeof buf;
	gp_args[7] = 0;

	ret = ci_handler(gp_args);
	actual = gp_args[7];

	cprintf(e, "test-ci: getprop bootargs   ret=%d len=%d str=\"%s\"\n",
	        ret, (int)actual, (char *)buf);

	return NO_ERROR;
}

/*
 * `set-bootargs ( addr len -- )` — copy a Forth counted-string into
 * a firmware-owned buffer and publish it on /chosen/bootargs via
 * prop_set_str. Spec 07 §AOS4: "Everything after amigaboot.of on the
 * boot command line MUST be passed via /chosen/bootargs." Today we
 * have no boot-command parser so this is a manual setter; when the
 * spec-07 `boot` CLI lands it will call this internally.
 *
 * Truncates silently at BOOTARGS_MAX so a wild stack can't smear our
 * .bss. Empty string is allowed (clears bootargs).
 */
#define BOOTARGS_MAX 255
static char machdep_bootargs[BOOTARGS_MAX + 1];

CC(f_set_bootargs)
{
	Int  len;
	Cell addr;

	IFCKSP(e, 2, 0);
	POP(e, len);
	POP(e, addr);

	if (len < 0) len = 0;
	if (len > BOOTARGS_MAX) len = BOOTARGS_MAX;

	const char *src = (const char *)(uPtr)addr;
	for (Int i = 0; i < len; i++)
		machdep_bootargs[i] = src[i];
	machdep_bootargs[len] = '\0';

	if (e->chosen == NULL || e->chosen->props == NULL) {
		cprintf(e, "set-bootargs: /chosen not installed yet\n");
		return NO_ERROR;
	}

	Retcode rc = prop_set_str(e->chosen->props,
	                          (Byte *)"bootargs", CSTR,
	                          (Byte *)machdep_bootargs, CSTR);
	if (rc != NO_ERROR)
		cprintf(e, "set-bootargs: prop_set_str failed rc=%d\n", (int)rc);
	else
		cprintf(e, "set-bootargs: /chosen/bootargs = \"%s\" (%d bytes)\n",
		        machdep_bootargs, (int)len);

	return NO_ERROR;
}

/*
 * `heap-info ( -- )` prints the SF malloc-pool bounds so a human
 * can verify the pool is outside every region a boot-kernel or
 * AOS bootstrap might try to load into. docs/07 §Load-address
 * originally said "heap between 0x200000 and 0x400000", but real-
 * world amigaboot.of is linked at 0x200000 and its PT_LOAD stomped
 * the pool there. The pool moved to 0x01100000 (just past the
 * x86emu buffer at 0x01000000) so the classic AOS load area
 * 0x00200000..0x00FFFFFF is fully free for bootstraps + kernel
 * modules.
 */
extern Byte *g_machine_memory;
extern uInt  g_machine_memory_used;

CC(f_heap_info)
{
	uInt base = (uInt)(uPtr)g_machine_memory;
	uInt size = g_machine_memory_used;
	uInt end  = base + size;

	cprintf(e, "heap-info: pool 0x%X..0x%X (%d KiB)\n",
	        (unsigned)base, (unsigned)(end - 1), (int)(size / 1024));

	/* Accept pool anywhere that clears the AOS bootstrap + kernel
	 * load area (0x200000..0x00FFFFFF) AND the x86emu buffer
	 * (0x01000000..0x010FFFFF). The new-firmware layout puts the
	 * pool at 0x01100000..0x012FFFFF. */
	const char *status =
	    (base >= 0x01100000u && end <= 0x02000000u)
	      ? "OK (pool clear of AOS bootstrap + x86emu regions)"
	      : "OUT-OF-SPEC (pool overlaps AOS bootstrap or x86emu)";
	cprintf(e, "heap-info: %s\n", status);
	return NO_ERROR;
}

extern Retcode f_boot_kernel(Environ *e);
extern Retcode f_test_boot(Environ *e);
extern Retcode f_test_boot_bad(Environ *e);
extern Retcode f_ls_pci(Environ *e);
extern Retcode f_test_ide_probe(Environ *e);
extern Retcode f_test_read_block(Environ *e);
extern Retcode f_test_iso_ls(Environ *e);
extern Retcode f_test_aliases(Environ *e);

/*
 * get-time-of-day ( -- second minute hour day month year )
 *
 * Spec 06 §"Time of day" CI service. Reads the M48T59 RTC and
 * pushes six integers: second/minute/hour/day/month/year (year
 * is the full 4-digit value, e.g. 2026). If the chip is absent
 * (QEMU pegasos2 -- no M48T59 model) we push an epoch-like
 * fallback (1970-01-01 00:00:00) and print a one-line notice.
 * An OS that expects this CI service will still receive well-
 * formed values; it can detect the fallback by recognising the
 * epoch. A later real-hardware session will validate the real
 * read path.
 */
extern int m48t59_read_rtc(int *year, int *month, int *day,
                           int *hour, int *minute, int *second);
extern int m48t59_write_rtc(int year, int month, int day,
                            int hour, int minute, int second);

CC(f_get_time_of_day)
{
	int yr, mo, da, hr, mi, se;
	if (m48t59_read_rtc(&yr, &mo, &da, &hr, &mi, &se) != 0) {
		/* M48T59 not present -- fall back to a fixed epoch so
		 * the CI contract stays predictable on QEMU. */
		yr = 1970; mo = 1; da = 1;
		hr = 0; mi = 0; se = 0;
	}
	IFCKSP(e, 0, 6);
	PUSH(e, (Cell)se);
	PUSH(e, (Cell)mi);
	PUSH(e, (Cell)hr);
	PUSH(e, (Cell)da);
	PUSH(e, (Cell)mo);
	PUSH(e, (Cell)yr);
	return NO_ERROR;
}

/*
 * set-time-of-day ( second minute hour day month year -- )
 *
 * Writes a new wall clock into the M48T59 (if present). Silently
 * does nothing when the chip is absent; we don't want a test
 * harness on QEMU to appear to fail when it's really just a
 * platform limitation.
 */
CC(f_set_time_of_day)
{
	Cell yr, mo, da, hr, mi, se;
	IFCKSP(e, 6, 0);
	POP(e, yr);
	POP(e, mo);
	POP(e, da);
	POP(e, hr);
	POP(e, mi);
	POP(e, se);
	(void)m48t59_write_rtc((int)yr, (int)mo, (int)da,
	                       (int)hr, (int)mi, (int)se);
	return NO_ERROR;
}

/*
 * `test-ci-boot ( addr len -- )` -- invoke ci_handler with the
 * "boot" service and the given Forth counted-string as bootspec.
 * Mirrors what an OS-resident boot loader would do via its saved
 * r5 handler pointer: spec 06 §"Required services" maps "boot"
 * onto SF's f_client_boot, which splits the bootspec at the first
 * whitespace into device + filename/args and hands off to
 * boot_load (the same entry point interactive `boot` uses).
 *
 * On success control transfers to the loaded kernel and we never
 * return. On failure ci_handler returns nonzero and we print the
 * code so the user can diagnose.
 *
 * Typical smoke uses:
 *     " nosuchdev" test-ci-boot    ->  CI=-1 (service returned error)
 *     " cd /test.elf" test-ci-boot -> boots embedded ISO if -cdrom set
 *     " hd:0 /test.elf" test-ci-boot -> boots from Amiga HD partition
 *
 * The string must be NUL-terminated before invocation because SF's
 * f_client_boot treats the bootspec as a C string. We copy into a
 * static buffer and append the terminator.
 */
#define CI_BOOTSPEC_MAX 255
static char ci_bootspec_buf[CI_BOOTSPEC_MAX + 1];

CC(f_test_ci_boot)
{
	Int  len;
	Cell addr;
	Cell args[4];

	IFCKSP(e, 2, 0);
	POP(e, len);
	POP(e, addr);

	if (len < 0) len = 0;
	if (len > CI_BOOTSPEC_MAX) len = CI_BOOTSPEC_MAX;
	const char *src = (const char *)(uPtr)addr;
	for (Int i = 0; i < len; i++)
		ci_bootspec_buf[i] = src[i];
	ci_bootspec_buf[len] = '\0';

	cprintf(e, "test-ci-boot: CI boot \"%s\"\n", ci_bootspec_buf);

	/* Build the spec-06 CI call struct: service="boot", nargs=1,
	 * nreturns=0, args[0] = bootspec pointer. f_client_boot signals
	 * errors via the dispatch Retcode (which client_interface maps
	 * to a non-zero ci_handler return). */
	args[0] = (Cell)(uPtr)"boot";
	args[1] = 1;
	args[2] = 0;
	args[3] = (Cell)(uPtr)ci_bootspec_buf;

	int ret = ci_handler(args);

	/* If CI boot succeeded we don't reach here -- control is now
	 * in the loaded image. So any return here is by definition
	 * the error path. */
	cprintf(e, "test-ci-boot: ci_handler returned %d "
	           "(0=ok/image-jumped, nonzero=service error)\n",
	        ret);
	return NO_ERROR;
}

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
			"( -- sec min hr day mo yr)  read M48T59 RTC (fallback 1970-01-01)") },
	{ (Byte *)"set-time-of-day", f_set_time_of_day, INVALID_FCODE, F_NONE, T_FUNC HELP(
			"(sec min hr day mo yr --)  write M48T59 RTC (noop if absent)") },
	{ NULL, NULL, INVALID_FCODE, F_NONE, T_FUNC HELP("") }
};

/*
 * install_pegasos2_ci_services -- run after install_client_services
 * to extend the /openprom/client-services dictionary with the
 * pegasos2-specific time-of-day services spec 06 §"Time of day"
 * defines. Upstream SF's client_services_methods[] has
 * `milliseconds` but not get-time-of-day/set-time-of-day, so an OS
 * that asks for them via the CI name dispatcher would otherwise
 * get "service unknown". init_entries appends to an existing dict.
 */
static const Initentry init_pegasos2_ci[] = {
	{ (Byte *)"get-time-of-day", f_get_time_of_day,
	  INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"set-time-of-day", f_set_time_of_day,
	  INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ NULL, NULL, INVALID_FCODE, F_NONE, T_FUNC HELP("") }
};

CC(install_pegasos2_ci_services)
{
	if (e->client == NULL || e->client->dict == NULL)
		return NO_ERROR;    /* install_client_services didn't run;
		                     * drop silently -- next call into the
		                     * CI name dispatcher will hit the
		                     * fallback linear scan */

	/*
	 * Pre-populate /options with the AOS4 kernel command line so
	 * the kernel logs to serial. amigaboot.of reads this via
	 * getprop "/options/os4_commandline" and forwards it to the
	 * kernel boot args. "serial munge debuglevel=7" tells the
	 * AmigaOS 4.1 kernel to mirror its kprintf output to UART1
	 * with verbose level 7 -- otherwise the kernel runs silently
	 * because it normally renders to a framebuffer that we don't
	 * yet expose. Setting it as a /options property (rather than
	 * via setenv from the OF prompt) avoids the SF "no such
	 * configuration variable" gate that rejects unknown names.
	 */
	if (e->options != NULL && e->options->props != NULL) {
		/* "serial debuglevel=1" matches the working AmigaQemuTests
		 * append config; the AOS4 kernel parses this from
		 * /options/os4_commandline and routes kprintf to UART1. */
		(void)prop_set_str(e->options->props,
		             (Byte *)"os4_commandline", CSTR,
		             (Byte *)"serial debuglevel=1", CSTR);
	}

	return init_entries(e, e->client->dict, init_pegasos2_ci);
}

/* --------------------------------------------------------------- *
 *  exec_* helpers -- now supplied by upstream/smartfirmware/bin/of/ *
 *  exe/exe.c, pulled into OF_SUBSET in Block 6/N alongside the      *
 *  registration of pegasos2_ppc_elf_exec in g_exec_list[] above.    *
 * --------------------------------------------------------------- */

/* --------------------------------------------------------------- *
 *  logo_pixmap placeholder                                          *
 * --------------------------------------------------------------- */

/*
 * admin.c's banner path references logo_pixmap[] as a default OEM
 * logo. The block is gated by `if (logo)` which is derived from
 * NVRAM config -- with our empty g_nvram, logo stays 0 and the
 * pixmap is never dereferenced. Supply a 1-byte placeholder so the
 * link resolves.
 */
Byte logo_pixmap[1] = { 0 };

/* --------------------------------------------------------------- *
 *  ppc_get_version                                                  *
 * --------------------------------------------------------------- */

/*
 * Called by cpu-ppc.c to fill in the /cpu "cpu-version" property.
 * SPR 287 == PVR; the upper 16 bits identify the core family, the
 * lower 16 bits the sub-revision. On our MPC7447A-flavoured QEMU
 * the value is 0x80020102.
 */
uInt
ppc_get_version(void)
{
	uInt pvr;
	__asm__ volatile ("mfspr %0, 287" : "=r"(pvr));
	return pvr;
}

/* --------------------------------------------------------------- *
 *  init_list -- per-file Forth-word init tables                     *
 * --------------------------------------------------------------- */

/*
 * Each init_<name> symbol below is defined in the bin/of/<name>.c
 * file's init_<name>_table[] via the INIT_TABLE() macro. table.c
 * reads this array to register all the Forth words the runtime
 * knows about.
 *
 * We only list the tables whose .c files are in our OF_SUBSET --
 * trimming fb / tokenizer etc. until those files come in.
 */
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

/* --------------------------------------------------------------- *
 *  install_list -- package/method installers run at init time      *
 * --------------------------------------------------------------- */

/*
 * Each entry is a Forth word that table.c's init_forth_words()
 * executes once, in order, to bring up the associated package.
 * We include the "must-have for ok prompt" minimum:
 *   install_root         creates /            (must be first)
 *   install_chosen       creates /chosen      (must be second --
 *                        install_memory references e->chosen->props
 *                        to publish its instance, and the amd64 /
 *                        coldfire / edb7312 ports all document
 *                        "install_chosen should be second")
 *   install_memory       creates /memory
 *   init_options_from_nvram   pulls NVRAM env vars into /options
 *   install_powerpc_cpu  creates /cpu
 *   install_display      creates /display (stubbed without fb.c)
 *   install_failsafe     creates the failsafe console package
 *
 * Deferred until their owning .c files land:
 *   install_stdio, install_pci, install_obptftp,
 *   install_deblocker, install_disklabel
 */
EC(install_root);
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

/*
 * Install order notes:
 *  - install_pci_tree must run before install_ide_driver (which
 *    walks the PCI tree to find the VT8231 IDE controller).
 *  - install_deblocker and install_disklabel must run before any
 *    attempt to open() a disk child (atadisk's f_ata_disk_open
 *    calls $open-package on both /packages/deblocker and
 *    /packages/disk-label). install_ide_driver ITSELF only calls
 *    probe_ata_disks, which does IDENTIFY+READ_CAPACITY and
 *    creates disk-package nodes but does not open them, so order
 *    between ide_driver and deblocker/disklabel is not strictly
 *    constrained here. Placing deblocker/disklabel before
 *    ide_driver for clarity.
 *  - install_aliases must run AFTER install_ide_driver because
 *    it derives /aliases/cd and /aliases/hd by walking the IDE
 *    children's paths (find_first_cd / find_first_hd).
 *  - install_client_services creates /openprom/client-services and
 *    registers the IEEE-1275 CI method table under it so
 *    client_interface() dispatches via execute_static_method_name
 *    instead of the linear fallback scan. Spec 06 §"Required
 *    services" lists the services; all live in upstream client.c.
 *    Order-independent relative to the device-tree installers,
 *    but we run it last so any future CI service that consults
 *    the populated device tree can do so.
 */
const Command install_list[] = {
	install_root,
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
	/* install_partition_packages, */  /* TEMP: isolate nextprop crash */
	install_client_services,
	install_pegasos2_ci_services,
	NULL
};
