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
 *  M48T59 NVRAM/RTC chip at the spec 08 §RTC offsets.
 *
 *  Also handles install_pegasos2_ci_services -- a post-
 *  install_client_services hook that adds the pegasos2-specific
 *  time-of-day services to /openprom/client-services so an OS
 *  asking for them via the CI name dispatcher gets a hit (upstream
 *  SF's client_services_methods[] only includes `milliseconds`).
 */

#include "defs.h"

extern int m48t59_read_rtc(int *year, int *month, int *day,
                           int *hour, int *minute, int *second);
extern int m48t59_write_rtc(int year, int month, int day,
                            int hour, int minute, int second);

/*
 * get-time-of-day ( -- second minute hour day month year )
 *
 * Spec 06 §"Time of day" CI service. Reads the M48T59 RTC and
 * pushes six integers: second/minute/hour/day/month/year (year
 * is the full 4-digit value, e.g. 2026). If the chip is absent
 * (QEMU pegasos2 -- no M48T59 model) we push an epoch-like
 * fallback (1970-01-01 00:00:00). An OS that expects this CI
 * service will still receive well-formed values; it can detect
 * the fallback by recognising the epoch.
 */
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
 * Writes a new wall clock to the M48T59 (if present). Silently
 * does nothing when the chip is absent; on QEMU this lets test
 * harnesses run without spurious errors from a platform
 * limitation rather than a bug in the caller.
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

	return init_entries(e, e->client->dict, init_pegasos2_ci);
}
