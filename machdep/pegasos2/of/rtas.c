/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  CHRP Run-Time Abstraction Services (/rtas) stub.
 *
 *  CHRP-spec OSes -- MorphOS Quark, the kernel inside Linux PPC's
 *  CHRP boot wrapper, and AIX/PowerLinux variants of NetBSD and
 *  FreeBSD -- expect a /rtas node in the device tree as a side
 *  channel to firmware-mediated services that don't fit the
 *  IEEE-1275 client-interface model: power-off, reboot, NVRAM
 *  fetch/store, time-of-day, PCI config-space access from a
 *  privileged context, etc.
 *
 *  An OS uses /rtas like this:
 *    1. finddevice("/rtas")            -> phandle (or -1 if absent)
 *    2. getprop("rtas-size")            -> code-size in bytes
 *    3. claim() that many bytes        -> phys-addr `dst`
 *    4. open-package /rtas + execute its `instantiate-rtas` method
 *       with `dst` on the stack         -> entry-point `entry`
 *       (the firmware copies the RTAS body to `dst`)
 *    5. Fill in args struct at some phys-addr `args`:
 *         args[0] = token (e.g. RTAS_GET_TIME_OF_DAY = 3)
 *         args[1] = nargs
 *         args[2] = nrets
 *         args[3..] = method args
 *    6. Call `entry(args)` with r3=args  -> r3=success/error,
 *       results written into args[3+nargs..]
 *
 *  Without /rtas, MorphOS Quark calls finddevice("/rtas"), gets -1,
 *  and silently quiesces with rc=-1. We saw that with the 3.19
 *  install CD via CI tracing -- the kernel never reaches its own
 *  console init.
 *
 *  This file installs the bare minimum to satisfy the lookup +
 *  use the hypercall path that QEMU's pegasos2 model already
 *  implements. On real Pegasos II hardware the original CodeGen
 *  SmartFirmware ROM's /rtas stub talks to dedicated south-bridge
 *  registers; we cannot test that path until we have a real board,
 *  so the hypercall path here is QEMU-only. A `#ifdef
 *  PEGASOS_TARGET_QEMU` guard could shrink the stub on HW builds,
 *  but the stub is 20 bytes -- not worth the conditional.
 *
 *  Hypercall protocol (per QEMU hw/ppc/pegasos.c):
 *    r3 = KVMPPC_H_RTAS = 0xF000  (KVMPPC_HCALL_BASE + 0x0)
 *    r4 = args_real (physical address of the args struct)
 *    sc 1                          -- LEV=1 system-call instruction;
 *                                     PowerPCVirtualHypervisor's
 *                                     hypercall hook intercepts.
 *    on return: r3 = H_SUCCESS (0) or H_PARAMETER (-4) etc.
 *
 *  The OS-callable entry point we hand back from instantiate-rtas
 *  is therefore a 5-instruction trampoline:
 *
 *    mr   r4, r3              -- save args pointer for hypercall
 *    lis  r3, 0               -- r3 = 0x00000000
 *    ori  r3, r3, 0xF000      -- r3 |= 0xF000 -> r3 = H_RTAS hcall #
 *    sc   1                   -- hypercall; QEMU intercepts
 *    blr                      -- return (r3 = hypercall return)
 *
 *  The token + args at *args_real are interpreted by QEMU's
 *  pegasos2_rtas() function, which knows how to power-off the
 *  machine, fetch the time-of-day, do PCI config access, etc.
 */

#include <stdint.h>
#include "defs.h"

/*
 * The RTAS trampoline body, hand-encoded so it doesn't depend on
 * the link-time layout of any .text section. Big-endian PowerPC
 * instruction words. instantiate-rtas memcpy's these to the
 * caller-supplied physical address and returns the address.
 */
static const uint32_t rtas_trampoline[] = {
	0x7c640378u,    /* mr   r4, r3                              */
	0x3c600000u,    /* lis  r3, 0                               */
	0x6063f000u,    /* ori  r3, r3, 0xF000   ; H_RTAS = 0xF000  */
	0x44000022u,    /* sc   1                ; LEV=1 hypercall  */
	0x4e800020u,    /* blr                                      */
};
#define RTAS_TRAMPOLINE_SIZE  ((Int)sizeof rtas_trampoline)

/*
 * instantiate-rtas ( base -- entry )
 *
 * IEEE-1275 / CHRP convention: the OS has claimed memory of size
 * `rtas-size` at physical address `base` and wants the firmware
 * to install RTAS there. We just memcpy the trampoline. Returns
 * the entry-point address (== base for our flat trampoline).
 */
static Retcode
f_instantiate_rtas(Environ *e)
{
	Cell base;

	IFCKSP(e, 1, 1);
	POP(e, base);

	uint8_t *dst = (uint8_t *)(uPtr)base;
	if (dst == NULL) {
		PUSH(e, 0);
		return E_BAD_ARGUMENT;
	}

	/*
	 * Use a 32-bit-store loop so we copy whole instructions in a
	 * single bus cycle each. memcpy would also work but pulls in
	 * the upstream stdlib symbol.
	 */
	const uint32_t *src = rtas_trampoline;
	uint32_t *d = (uint32_t *)dst;
	for (Int i = 0; i < (Int)(sizeof rtas_trampoline /
	                          sizeof rtas_trampoline[0]); i++)
		d[i] = src[i];

	/*
	 * Make sure the icache sees fresh bytes -- the OS will jump
	 * to `base` immediately after we return. dcbst flushes our
	 * stores out to coherent memory; icbi invalidates the icache
	 * line; sync + isync make the new bytes observable to the
	 * next instruction fetch.
	 */
	for (Int i = 0; i < RTAS_TRAMPOLINE_SIZE; i += 32) {
		__asm__ volatile ("dcbst 0, %0; sync; icbi 0, %0; sync; isync"
		                  :: "r"(dst + i) : "memory");
	}

	PUSH(e, base);   /* entry == base */
	return NO_ERROR;
}

/*
 * Trivial open/close methods. SF's open-package allocates an
 * Instance and calls the package's "open" word to let the driver
 * stash per-instance state. /rtas has none -- the trampoline is
 * stateless and the hypercall preserves no per-call context --
 * so we just push TRUE and return. Without an "open" method, SF
 * returns success with an ihandle of 0, and the OS's subsequent
 * call-method with that null handle fails.
 */
static Retcode
f_rtas_open(Environ *e)
{
	IFCKSP(e, 0, 1);
	PUSH(e, FTRUE);
	return NO_ERROR;
}

static Retcode
f_rtas_close(Environ *e)
{
	(void)e;
	return NO_ERROR;
}

static const Initentry rtas_methods[] = {
	{ (Byte *)"open",             f_rtas_open,
	  INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"close",            f_rtas_close,
	  INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"instantiate-rtas", f_instantiate_rtas,
	  INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ NULL, NULL, INVALID_FCODE, F_NONE, T_FUNC HELP("") },
};

/*
 * install_rtas: create the /rtas node with enough properties to
 * satisfy a CHRP-spec RTAS handshake. Run AFTER install_root_cells
 * (we depend on /'s cells being right) and ideally before
 * install_client_services so an OS calling finddevice("/rtas")
 * via the CI gets a hit.
 */
CC(install_rtas)
{
	Package *pkg = new_pkg_name(e->root, (char *)"rtas");
	if (pkg == NULL)
		return E_OUT_OF_MEMORY;

	Retcode ret = init_entries(e, pkg->dict, rtas_methods);
	if (ret != NO_ERROR)
		return ret;

	prop_set_str(pkg->props, (Byte *)"device_type", CSTR,
	             (Byte *)"rtas", CSTR);
	prop_set_int(pkg->props, (Byte *)"rtas-version", CSTR, 1);
	prop_set_int(pkg->props, (Byte *)"rtas-size", CSTR,
	             RTAS_TRAMPOLINE_SIZE);
	/*
	 * rtas-event-scan-rate: how often the OS should poll RTAS
	 * for asynchronous events. 0 = no polling needed. We don't
	 * generate events from hardware (everything is synchronous
	 * via the hypercall) so 0 is honest.
	 */
	prop_set_int(pkg->props, (Byte *)"rtas-event-scan-rate", CSTR, 0);

	/*
	 * Per-token availability properties. CHRP spec says each
	 * supported RTAS service appears as <name>=<token>. The
	 * tokens come from QEMU's pegasos2_rtas_tokens enum and the
	 * upstream Linux drivers/rtc/rtc-rtas.c / OS RTAS clients
	 * use the exact names below. We expose only the tokens
	 * QEMU's pegasos.c actually services -- the rest would
	 * silently fall to the default H_SUCCESS-with-zeroed-result
	 * branch, which can hide real bugs in OSes that probe by
	 * presence.
	 */
	prop_set_int(pkg->props, (Byte *)"get-time-of-day",   CSTR, 3);
	prop_set_int(pkg->props, (Byte *)"set-time-of-day",   CSTR, 4);
	prop_set_int(pkg->props, (Byte *)"read-pci-config",   CSTR, 8);
	prop_set_int(pkg->props, (Byte *)"write-pci-config",  CSTR, 9);
	prop_set_int(pkg->props, (Byte *)"display-character", CSTR, 10);
	prop_set_int(pkg->props, (Byte *)"power-off",         CSTR, 17);

	return NO_ERROR;
}
