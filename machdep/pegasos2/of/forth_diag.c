/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Diagnostic Forth words. None of these are required for normal
 *  boot; they're hand-driven smoke checks for the IEEE-1275 client
 *  interface (test-ci, test-ci-boot), the spec-07 bootargs path
 *  (set-bootargs), and the SF malloc-pool layout (heap-info).
 *
 *  Registered in init_pegasos2[] in platform.c.
 */

#include "defs.h"
#include "byteswap.h"

extern int ci_handler(void *args);

/*
 * `test-ci ( -- )` exercises the IEEE-1275 client-interface
 * dispatcher across two services with different arg/return
 * signatures and prints each round-trip result:
 *
 *   finddevice "/chosen"   -- nargs=1 nrets=1 (path -> phandle)
 *   getprop <ph> "stdout"  -- nargs=4 nrets=1 (phandle, name,
 *                             buf, buflen -> actual length)
 *   getprop <ph> "bootargs"-- ditto, exercises the bootargs
 *                             property the OS will query per
 *                             spec 07 §AOS4
 *
 * A healthy dispatcher prints:
 *
 *     test-ci: finddevice /chosen ret=0 phandle=0xXXXXXX
 *     test-ci: getprop stdout     ret=0 len=4 ihandle=0xXXXXXX
 *     test-ci: getprop bootargs   ret=0 len=N str="..."
 *
 * The stdout property holds an encoded ihandle (big-endian 32-bit
 * pointer per IEEE-1275 §3.3.3.1.1 "encoded-int") pointing at the
 * install-console-chosen output device. 4-byte length is expected
 * on a 32-bit build.
 */
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
	        (actual >= 4) ? (unsigned)be32(buf) : 0u);

	/* 3) getprop <phandle> "bootargs" buf 64 -- exercises the
	 * bootargs property the OS will query per spec 07 §AOS4. Starts
	 * empty per chosen.c install_chosen; set-bootargs updates it. */
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
 * `set-bootargs ( addr len -- )` -- copy a Forth counted-string into
 * a firmware-owned buffer and publish it on /chosen/bootargs via
 * prop_set_str. Spec 07 §AOS4: "Everything after the boot file on
 * the boot command line MUST be passed via /chosen/bootargs."
 *
 * Truncates silently at BOOTARGS_MAX so a wild stack can't smear
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
		cprintf(e, "set-bootargs: prop_set_str failed rc=%d\n",
		        (int)rc);
	else
		cprintf(e, "set-bootargs: /chosen/bootargs = \"%s\""
		        " (%d bytes)\n",
		        machdep_bootargs, (int)len);

	return NO_ERROR;
}

/*
 * `heap-info ( -- )` prints the SF malloc-pool bounds so a human
 * can verify the pool is outside every region a boot-kernel or
 * AOS bootstrap might try to load into.
 *
 * The pool was originally placed at 0x00200000 (per docs/07
 * §Load-address) but real-world amigaboot.of is also linked at
 * 0x00200000 and its PT_LOAD stomped the pool there. The pool was
 * relocated to 0x01100000 (just past the x86emu buffer at
 * 0x01000000) so the classic AOS load area
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

	/* Acceptable range: pool above the AOS bootstrap + kernel load
	 * area (0x00200000..0x00FFFFFF) AND above the x86emu buffer
	 * (0x01000000..0x010FFFFF). */
	const char *status =
	    (base >= 0x01100000u && end <= 0x02000000u)
	      ? "OK (pool clear of AOS bootstrap + x86emu regions)"
	      : "OUT-OF-SPEC (pool overlaps AOS bootstrap or x86emu)";
	cprintf(e, "heap-info: %s\n", status);
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
 *     " nosuchdev"      test-ci-boot   -> CI=-1 (service error)
 *     " cd /test.elf"   test-ci-boot   -> boots embedded ISO
 *     " hd:0 /test.elf" test-ci-boot   -> boots from Amiga HD
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
