/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  IEEE-1275 client interface entry (spec 06).
 *
 *  Spec 06 §"Entry convention": at boot handoff, firmware publishes
 *  the CI entry-point address to the OS in register r5, with r3
 *  cleared. The OS saves r5 to a well-known kernel global and later
 *  calls the handler like a C function:
 *
 *      int handler(struct ci_call *args);
 *      r3 = (uintptr_t)args     on entry
 *      r3 = 0                   on successful dispatch
 *      r3 = nonzero error code  on failure (-1 = unknown service)
 *
 *  The call-struct points to:
 *
 *      struct ci_call {
 *          u32 service;          // null-terminated ASCII service name
 *          u32 nargs;            // count of u32 arguments
 *          u32 nreturns;         // count of u32 return slots
 *          u32 argret[];         // nargs args followed by nreturns rets
 *      };
 *
 *  SmartFirmware's client.c already implements the dispatcher body
 *  as `client_interface(Cell array[])`, taking the call-struct as a
 *  Cell array with the same layout spec 06 specifies. We wrap it in
 *  our own symbol `ci_handler` so:
 *
 *    1. The publication / ABI contract is named by our project,
 *       not buried in upstream SmartFirmware code.
 *    2. There is a stable hook point for pegasos2-specific preamble
 *       (quiesce-state check, statistics, future reentrancy guard)
 *       without editing upstream files.
 *    3. The symbol that a future boot loader loads into r5 is
 *       ours, so reviewers reading spec 06 see the expected name.
 *
 *  Today the wrapper is thin. It's not yet published to any OS:
 *  the boot loader (spec 07) that would populate r5 at handoff is
 *  still stubbed. Until then, ci_handler is callable only from
 *  within the firmware (see CI/3's synthetic Forth-word test).
 */

#include "defs.h"

extern Int client_interface(Cell array[]);

extern Environ *g_e;

/*
 * Public CI entry point. Thin wrapper -- no state tracking yet.
 * Returns 0 on dispatch success, -1 when the service name is
 * unknown. On dispatch failure (bad arg count, service error),
 * client_interface returns -1 with the stack restored.
 */
int
ci_handler(void *args)
{
	if (args == NULL)
		return -1;

#ifdef CI_TRACE
	Cell *a = (Cell *)args;
	uInt svc_ptr = (uInt)a[0];
	uInt nargs   = (uInt)a[1];
	uInt nrets   = (uInt)a[2];

	cprintf(g_e, "[ci] svc=0x%X nargs=%d nrets=%d",
	        (unsigned)svc_ptr, (int)nargs, (int)nrets);

	int in_dram  = (svc_ptr >= 0x00000000u && svc_ptr < 0x10000000u);
	int in_flash = (svc_ptr >= 0xF0000000u);
	if ((in_dram || in_flash) && svc_ptr != 0) {
		const uByte *p = (const uByte *)(uPtr)svc_ptr;
		cprintf(g_e, " name=\"");
		for (int i = 0; i < 40 && p[i] >= 0x20 && p[i] < 0x7Fu; i++)
			cprintf(g_e, "%c", p[i]);
		cprintf(g_e, "\"");
	} else {
		cprintf(g_e, " name=<unmapped>");
	}

	for (uInt i = 0; i < nargs && i < 8; i++)
		cprintf(g_e, " a%d=0x%X", (int)i, (unsigned)a[3 + i]);
	cprintf(g_e, "\n");

	int rc = (int)client_interface((Cell *)args);

	cprintf(g_e, "[ci]   -> rc=%d", rc);
	for (uInt i = 0; i < nrets && i < 8; i++)
		cprintf(g_e, " r%d=0x%X", (int)i,
		        (unsigned)a[3 + nargs + i]);
	cprintf(g_e, "\n");
	return rc;
#else
	return (int)client_interface((Cell *)args);
#endif
}
