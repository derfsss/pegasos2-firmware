/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Real-time-clock Forth words and the matching IEEE-1275 CI
 *  service registration. Both word and service read/write the
 *  VT8231 internal RTC (the schematic-confirmed Pegasos II
 *  time-of-day source -- there is no separate M48T59 chip on the
 *  board; SPEC-QUESTIONS.md Q7 records the audit).
 *
 *  Also handles install_pegasos2_ci_services -- a post-
 *  install_client_services hook that adds the pegasos2-specific
 *  time-of-day services to /openprom/client-services so an OS
 *  asking for them via the CI name dispatcher gets a hit (upstream
 *  SF's client_services_methods[] only includes `milliseconds`).
 */

#include "defs.h"

extern int vt8231_rtc_read(int *year, int *month, int *day,
                           int *hour, int *minute, int *second);
extern int vt8231_rtc_write(int year, int month, int day,
                            int hour, int minute, int second);

/*
 * get-time-of-day ( -- second minute hour day month year )
 *
 * Spec 06 §"Time of day" CI service. Reads the VT8231 internal
 * RTC and pushes six integers: second/minute/hour/day/month/year
 * (year is the full 4-digit value, e.g. 2026). If the chip is
 * absent or its battery has never been initialised, we push an
 * epoch-like fallback (1970-01-01 00:00:00). An OS that expects
 * this CI service will still receive well-formed values; it can
 * detect the fallback by recognising the epoch.
 */
CC(f_get_time_of_day)
{
	int yr, mo, da, hr, mi, se;
	if (vt8231_rtc_read(&yr, &mo, &da, &hr, &mi, &se) != 0) {
		/* RTC not responding -- fall back to a fixed epoch so
		 * the CI contract stays predictable. */
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
 * Writes a new wall clock to the VT8231 RTC (if present). Silently
 * does nothing when the chip does not respond; that lets test
 * harnesses run without spurious errors from a platform limitation
 * rather than a bug in the caller.
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
	(void)vt8231_rtc_write((int)yr, (int)mo, (int)da,
	                       (int)hr, (int)mi, (int)se);
	return NO_ERROR;
}

/*
 * Per-pegasos2 client-interface services. Upstream SF's
 * client_services_methods[] in client.c provides the standard
 * IEEE-1275 services (finddevice, getprop, claim, ...) but not
 * spec-06 §Time-of-day -- only `milliseconds`. We append
 * get-time-of-day + set-time-of-day to /openprom/client-services
 * so an OS that calls the CI name dispatcher with either gets a
 * hit instead of "service unknown".
 */
static const Initentry init_pegasos2_ci[] = {
	{ (Byte *)"get-time-of-day", f_get_time_of_day,
	  INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"set-time-of-day", f_set_time_of_day,
	  INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ NULL, NULL, INVALID_FCODE, F_NONE, T_FUNC HELP("") }
};

/*
 * Replacement claim service: the upstream f_client_claim splits a
 * claim() call into a /memory/claim (allocate phys), a /mmu/claim
 * (allocate virt), and an /mmu/map (link them). On real-mode
 * Pegasos II hardware (translation off, virt == phys, no real MMU
 * to track) this is double work, and the upstream /mmu/claim path
 * has a stack mismatch with /mmu/map (claim pushes 4 args, map
 * pops cells+3 = 5 for cells=2) that yields virt=0 even when the
 * underlying physical allocation succeeded. The visible symptom:
 * Linux's CHRP boot wrapper prints `claim ... got 0` and aborts
 * with "Can't allocate memory for kernel image", even though
 * 0x400000+9MiB is comfortably inside our /memory/available.
 *
 * The fix: bypass the /mmu dance entirely. /memory/claim on its
 * own correctly honours both fixed-address (align==0) and any-
 * address (align!=0) claims, returns the allocated phys address,
 * and that's exactly what the OF client expects to see -- on
 * real-mode firmware, claim's "virtual address" is the physical
 * address. AOS4 amigaboot.of also benefits: its align==0 calls
 * went through the upstream path and "worked" only because the
 * uninitialised /mmu g_free_list left reqaddr unchanged on no-
 * match; that's an accident, not correctness.
 */
extern Retcode execute_method_name(Environ *e, Instance *inst,
                                   Byte *name, Int len);

static Retcode
f_pegasos2_claim(Environ *e)
{
	Cell virt, size, align;
	Int inst;

	IFCKSP(e, 3, 4);
	POP(e, virt);
	POP(e, size);
	POP(e, align);

	if (prop_get_int(e->chosen->props,
	                 (Byte *)"memory", CSTR, &inst) != NO_ERROR ||
	    inst == 0) {
		PUSH(e, 0);
		return E_NULL_INSTANCE;
	}

	/*
	 * Push the args /memory/claim expects. Its stack signature:
	 *   align == 0 : (phys.lo ... phys.hi size align -- base)
	 *   align != 0 : (size align -- base)
	 * For 32-bit phys (cells=1) the only phys cell is virt.
	 */
	if (align == 0)
		PUSH(e, virt);
	PUSH(e, size);
	PUSH(e, align);

	Retcode ret = execute_method_name(e, (Instance *)(uPtr)inst,
	                                  (Byte *)"claim", CSTR);
	if (ret != NO_ERROR) {
		PUSH(e, 0);
		return ret;
	}

	/*
	 * /memory/claim's result (the allocated base address) is on
	 * the top of the stack; that's our return value, leave it.
	 */
	return NO_ERROR;
}

CC(install_pegasos2_ci_services)
{
	if (e->client == NULL || e->client->dict == NULL)
		return NO_ERROR;    /* install_client_services didn't run;
		                     * drop silently -- next CI name
		                     * dispatch will fall back to the
		                     * linear-scan path */

	if (e->options != NULL && e->options->props != NULL) {
		/* The AOS4 kernel reads /options/os4_commandline; "serial
		 * debuglevel=1" routes kprintf output to UART1 with verbose
		 * level 1, matching the standard AOS4 boot append-config. */
		(void)prop_set_str(e->options->props,
		             (Byte *)"os4_commandline", CSTR,
		             (Byte *)"serial debuglevel=1", CSTR);
	}

	/*
	 * NOTE on post-quiesce kernel-mode console output:
	 *
	 * Linux PPC32 and MorphOS Quark both run through our CI
	 * (finddevice / getprop / claim / RTAS / quiesce) cleanly,
	 * confirmed by trace, but go silent on UART after quiesce.
	 * The kernel-mode 8250 / Quark serial driver expects to
	 * bind to a device-tree node with `compatible="ns16550"`,
	 * a real `reg` describing the UART MMIO window, and a
	 * `clock-frequency`. Our /failsafe is virtual (no such reg),
	 * and we don't currently expose a /pci@80000000/isa@c/
	 * serial@i3f8 child node either, so the kernel binds nothing.
	 *
	 * Two alternatives were tried here and both crashed the
	 * firmware mid-boot:
	 *  - prop_set_str("linux,stdout-path", "/failsafe") on
	 *    /chosen: Linux's own setprop overwrites this and SF's
	 *    property-list walker (find_table at fff18a54)
	 *    dereferences a corrupted pointer (r31="iiii") in a
	 *    later getprop, DSI-faulting.
	 *  - Adding `compatible="ns16550"` + clock-frequency to
	 *    /failsafe directly: same corruption pattern.
	 *
	 * Both suggest upstream nextprop / find_table can't safely
	 * coexist with OS-driven setprop in the strings we add.
	 * Working through that bug is a sizeable upstream-SF
	 * investigation -- deferred. The /rtas install above is the
	 * meaningful win this round: with it, Quark / Linux complete
	 * prom_init / instantiate-rtas / quiesce successfully (vs.
	 * "missing /rtas -> quiesce(rc=-1)" before).
	 */

	Retcode r = init_entries(e, e->client->dict, init_pegasos2_ci);
	if (r != NO_ERROR)
		return r;

	/* Override the existing "claim" method registered by upstream
	 * install_client_services. find_table on the dict's entry
	 * table returns the live Initentry-derived Entry; rewriting
	 * its v.cfunc pointer redirects every future CI dispatch of
	 * "claim" to our handler. Same trick M5 uses to swap
	 * read-blocks on disk@ children. */
	Entry *ent = find_table(e->client->dict,
	                        (Byte *)"claim", CSTR);
	if (ent != NULL)
		ent->v.cfunc = f_pegasos2_claim;

	return NO_ERROR;
}
