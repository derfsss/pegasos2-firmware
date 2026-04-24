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
 * Dedicated firmware stack for CI callbacks. The OS hands us
 * whatever r1 it chooses when it calls through the saved CI
 * entry pointer; some callers (classic Amiga amigaboot.of in
 * particular) are linked at a low address (0x00200000) with
 * .bss immediately above the text and a tiny stack -- the
 * caller's r1 is only ~2.7 KiB above .bss. SF's request
 * processing is not stack-tiny: a typical package-to-path
 * triggers four recursive get_device_name() frames, each with
 * its own 256-byte `name[]` local. That pushes our C stack
 * down past amigaboot.of's initial sp and into its .bss area,
 * corrupting whatever amigaboot.of keeps there (including its
 * saved CI handler pointer, so the NEXT CI call bctrl's to
 * garbage).
 *
 * The fix is to switch r1 to firmware-owned memory on the way
 * in and restore it on the way out. Size: 64 KiB is generous
 * -- SF's deepest call chains stay under 8 KiB. Placement in
 * .bss so it doesn't bloat the ROM image.
 */
#define CI_STACK_SIZE   (64 * 1024)
static uByte            ci_stack[CI_STACK_SIZE] __attribute__((aligned(16)));
#define CI_STACK_TOP    ((uByte *)&ci_stack[CI_STACK_SIZE - 64])

/*
 * The actual dispatch, entered with r1 already switched to
 * ci_stack. Kept in a separate function (non-static so the asm
 * shim can reference it by name) so the caller wrapper can do
 * the stack swap in asm without the compiler moving locals
 * around.
 */
int ci_dispatch(void *args);

int
ci_dispatch(void *args)
{

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

	/* Watch amigaboot.of's saved-ci_handler slot at 0x20F290.
	 * If it changes during a CI call we know THIS call corrupted
	 * it; the next bctrl would then jump to garbage. */
	volatile uInt *watch = (volatile uInt *)(uPtr)0x0020F290u;
	uInt before = *watch;

	/* Capture r1 (the OS-provided stack pointer) at CI entry,
	 * plus the Forth stack base/top, so we can see whether either
	 * touches 0x20F290 during the call. */
	register uInt r1_entry __asm__("r1");
	uInt r1_at_entry = r1_entry;
	uInt fstk_base = (uInt)(uPtr)g_e->stack;
	uInt fstk_top  = (uInt)(uPtr)g_e->sp;
	cprintf(g_e, "[ci]   entry: r1=0x%X fstk=0x%X..0x%X (used %d)\n",
	        (unsigned)r1_at_entry,
	        (unsigned)fstk_base, (unsigned)fstk_top,
	        (int)(fstk_top - fstk_base));

	int rc = (int)client_interface((Cell *)args);

	uInt after = *watch;
	cprintf(g_e, "[ci]   -> rc=%d", rc);
	for (uInt i = 0; i < nrets && i < 8; i++)
		cprintf(g_e, " r%d=0x%X", (int)i,
		        (unsigned)a[3 + nargs + i]);
	if (before != after)
		cprintf(g_e, "  [WATCH 0x20F290 CHANGED 0x%X -> 0x%X]",
		        (unsigned)before, (unsigned)after);
	cprintf(g_e, "\n");
	return rc;
#else
	return (int)client_interface((Cell *)args);
#endif
}

/*
 * Public CI entry point: stash caller's r1, switch to
 * ci_stack, call ci_dispatch, restore. A tiny asm shim because
 * C can't safely change r1 without the compiler re-ordering
 * accesses to it.
 */
int
ci_handler(void *args)
{
	if (args == NULL)
		return -1;

	int rc;
	register void *args_r3 __asm__("r3") = args;
	register int rc_out __asm__("r3");

	__asm__ volatile (
		/* save caller's r1 on our private stack, then jump in */
		"mr      11, 1\n\t"
		"lis     1, %[top]@ha\n\t"
		"la      1, %[top]@l(1)\n\t"
		"stwu    11, -16(1)\n\t"      /* reserve linkage + saved-r1 */
		"mflr    0\n\t"
		"stw     0, 20(1)\n\t"
		"bl      ci_dispatch\n\t"
		"lwz     0, 20(1)\n\t"
		"mtlr    0\n\t"
		"lwz     1, 0(1)\n\t"         /* restore caller's r1 */
		: "=r" (rc_out)
		: [top] "i" (&ci_stack[CI_STACK_SIZE - 64]),
		  "r" (args_r3)
		: "r0", "r11", "lr", "cr0", "cr1", "cr5", "cr6", "cr7",
		  "memory"
	);
	rc = rc_out;
	return rc;
}
