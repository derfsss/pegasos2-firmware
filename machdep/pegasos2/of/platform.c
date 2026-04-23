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
	{ "auto-boot?",           "false"     },  /* no boot path yet */
	{ "use-nvramrc?",         "false"     },
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
CC(machine_init_program)    { (void)e; return NO_ERROR; }
CC(machine_go)              { (void)e; return NO_ERROR; }
CC(machine_init_load)       { (void)e; return NO_ERROR; }

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

extern Retcode f_boot_kernel(Environ *e);
extern Retcode f_test_boot(Environ *e);
extern Retcode f_test_boot_bad(Environ *e);

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
	{ NULL, NULL, INVALID_FCODE, F_NONE, T_FUNC HELP("") }
};

/* --------------------------------------------------------------- *
 *  Stubs for exe/ symbol-lookup helpers                             *
 * --------------------------------------------------------------- */

/*
 * debug.c and exec.c call exec_addr2sym / exec_sym2addr / exec_length
 * for developer-level symbol lookup on loaded images. Their real
 * implementations live in upstream/smartfirmware/bin/of/exe/exe.c
 * which we defer (its transitive closure pulls in ELF/COFF/a.out
 * loaders we don't need for the ok prompt).
 *
 * Returning NULL / 0 keeps the callers on their "not found" path,
 * which is the correct behaviour when no image has been loaded yet.
 * The real file can replace these stubs at a later commit.
 */
struct Sym_ent;  /* opaque -- defined in exe/exe.h */

struct Sym_ent *
exec_addr2sym(Environ *e, void *list, Cell addr)
{
	(void)e; (void)list; (void)addr;
	return NULL;
}

struct Sym_ent *
exec_sym2addr(Environ *e, void *list, Byte *name, Int len)
{
	(void)e; (void)list; (void)name; (void)len;
	return NULL;
}

Retcode
exec_length(Environ *e, Cell *out)
{
	(void)e;
	if (out) *out = 0;
	return NO_ERROR;
}

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

const Command install_list[] = {
	install_root,
	install_chosen,
	install_memory,
	init_options_from_nvram,
	install_powerpc_cpu,
	install_display,
	install_failsafe,
	NULL
};
