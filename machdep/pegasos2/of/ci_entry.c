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

#ifdef CI_TRACE_LIMITED
/* Trace each unique service name once, plus all calls until we
 * have seen N distinct services. After that, only print
 * 'unusual' return codes (errors). Keeps the log compact while
 * still showing the ABI shape amigaboot.of expects. */
#define CI_TRACE_DISTINCT_LIMIT 64
static int       g_ci_trace_counter;
static char      g_ci_seen[CI_TRACE_DISTINCT_LIMIT][24];
static int       g_ci_seen_n;

static int
ci_seen_or_remember(const char *name)
{
	for (int i = 0; i < g_ci_seen_n; i++)
		if (strcmp(g_ci_seen[i], name) == 0)
			return 1;
	if (g_ci_seen_n < CI_TRACE_DISTINCT_LIMIT) {
		int j = 0;
		while (j < (int)sizeof(g_ci_seen[0]) - 1 && name[j])
			g_ci_seen[g_ci_seen_n][j] = name[j], j++;
		g_ci_seen[g_ci_seen_n][j] = 0;
		g_ci_seen_n++;
	}
	return 0;
}
#endif

/* Track the most-recently-used valid ihandle from open() / non-zero
 * call-method args, for the amigaboot.of NULL-ihandle workaround. */
static uInt g_last_ihandle;

/* amigaboot.of's "scan block devices" pass calls call-method
 * "block-size" with a valid ihandle, immediately followed by call-
 * method "#blocks" with ihandle=NULL. Disassembly suggests amigaboot
 * stores the ihandle in a register that gets clobbered by the first
 * call-method's return value (the compiled code does not spill it
 * across the call). On real Pegasos2 firmware the same code works
 * because real SF's call-method falls back to e->currinst (the most
 * recently active instance) when given ihandle=NULL. Replicate that
 * by substituting g_last_ihandle for any NULL ihandle we see in a
 * call-method or read/write/seek service.
 */
static void
amigaboot_ihandle_fixup_in(Cell *a)
{
	uInt svc_ptr = (uInt)a[0];
	uInt nargs   = (uInt)a[1];
	if (svc_ptr == 0)
		return;
	const char *name = (const char *)(uPtr)svc_ptr;

	int is_callm = (name[0] == 'c' && name[1] == 'a' &&
	                name[2] == 'l' && name[3] == 'l' &&
	                name[4] == '-' && name[5] == 'm');
	int is_io    = ((name[0] == 'r' && name[1] == 'e' &&
	                 name[2] == 'a' && name[3] == 'd' &&
	                 name[4] == '\0') ||
	                (name[0] == 'w' && name[1] == 'r' &&
	                 name[2] == 'i' && name[3] == 't' &&
	                 name[4] == 'e' && name[5] == '\0') ||
	                (name[0] == 's' && name[1] == 'e' &&
	                 name[2] == 'e' && name[3] == 'k' &&
	                 name[4] == '\0'));

	if (is_callm && nargs >= 2) {
		/* For call-method, regardless of how many method-args
		 * follow, the input layout is always:
		 *   a[3]=method-name, a[4]=ihandle, a[5..]=method-args.
		 * SF's f_client_call_method pops method (TOP) then
		 * ihandle (NEXT), so ihandle is at a[4] not a[3+nargs-1].
		 */
		uInt ih = (uInt)a[4];
		if (ih == 0 && g_last_ihandle != 0)
			a[4] = (Cell)g_last_ihandle;
		else if (ih != 0)
			g_last_ihandle = ih;
	} else if (is_io && nargs >= 1) {
		/* read/write/seek services take ihandle as their LAST
		 * input (TOP of Forth stack), so ihandle is at the
		 * highest array slot for these. Per SF's do_method:
		 *   read/write: (addr len ihandle) -> POP order is
		 *               inst (TOP), addr, len -> ihandle = a[3]
		 *   seek      : (pos.lo pos.hi ihandle) -> ihandle = a[3]
		 * So ihandle is the FIRST slot (a[3]), regardless of
		 * nargs.
		 */
		uInt ih = (uInt)a[3];
		if (ih == 0 && g_last_ihandle != 0)
			a[3] = (Cell)g_last_ihandle;
		else if (ih != 0)
			g_last_ihandle = ih;
	}
}

static void
amigaboot_ihandle_track_open(Cell *a, int rc)
{
	uInt svc_ptr = (uInt)a[0];
	uInt nargs   = (uInt)a[1];
	uInt nrets   = (uInt)a[2];
	if (rc != 0 || svc_ptr == 0 || nrets < 1)
		return;
	const char *name = (const char *)(uPtr)svc_ptr;
	if (name[0] == 'o' && name[1] == 'p' &&
	    name[2] == 'e' && name[3] == 'n') {
		uInt ih = (uInt)a[3 + nargs];
		if (ih != 0)
			g_last_ihandle = ih;
	}
}

/* Body of CI dispatch -- separated so the outer ci_dispatch can
 * wrap it in MSR[EE] enable/restore without complicating the body's
 * many return paths. */
static int
ci_dispatch_body(void *args)
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
#elif defined(CI_TRACE_LIMITED)
	Cell *a = (Cell *)args;
	uInt svc_ptr = (uInt)a[0];
	uInt nargs   = (uInt)a[1];
	uInt nrets   = (uInt)a[2];

	char name[24] = "<unmapped>";
	int  in_dram  = (svc_ptr < 0x10000000u);
	int  in_flash = (svc_ptr >= 0xF0000000u);
	if ((in_dram || in_flash) && svc_ptr != 0) {
		const uByte *p = (const uByte *)(uPtr)svc_ptr;
		int j = 0;
		while (j < (int)sizeof(name) - 1 &&
		       p[j] >= 0x20 && p[j] < 0x7Fu)
			name[j] = p[j], j++;
		name[j] = 0;
	}
	int seen = ci_seen_or_remember(name);
	int call_n = ++g_ci_trace_counter;

	amigaboot_ihandle_fixup_in(a);

	/* Workaround for SF f_client_nextprop bug: when prev-cstr names
	 * the LAST property on a node, SF takes a code path with an
	 * unguarded NULL deref that GCC compiles into an unconditional
	 * trap. Intercept "nextprop" service and handle it ourselves
	 * with the spec-compliant return:
	 *   - prev-cstr == NULL or "":   return first property
	 *   - prev-cstr matches an entry: return the next one (link)
	 *     -> if there's no next, return -1 (no more)
	 *   - prev-cstr doesn't match:    return -1 (per IEEE 1275)
	 * Spec: nextprop(buf-addr, prev-cstr, phandle -- size). Array
	 * layout: a[3]=phandle, a[4]=prev-cstr, a[5]=buf, a[6]=size.
	 */
	int rc;
	if (name[0] == 'n' && name[1] == 'e' && name[2] == 'x' &&
	    name[3] == 't' && name[4] == 'p' && name[5] == 'r' &&
	    nargs == 3 && nrets == 1) {
		Package *pkg = (Package *)(uPtr)(uInt)a[3];
		const Byte *prev = (const Byte *)(uPtr)(uInt)a[4];
		Byte *buf = (Byte *)(uPtr)(uInt)a[5];
		Entry *ent;

		if (pkg == NULL || pkg->props == NULL) {
			pkg = g_e->root;
		}
		if (pkg == NULL || pkg->props == NULL) {
			a[3 + nargs] = (Cell)-1;
			rc = 0;
			goto nextprop_done;
		}

		if (prev == NULL || *prev == 0) {
			ent = pkg->props->list;
		} else {
			ent = find_table(pkg->props, (Byte *)prev, CSTR);
			if (ent != NULL)
				ent = ent->link;
		}

		if (ent == NULL || ent->name == NULL) {
			a[3 + nargs] = (Cell)-1;   /* no more props */
			rc = 0;
			goto nextprop_done;
		}

		int len = (int)(uByte)ent->name[0];
		if (len > 31) len = 31;
		if (buf != NULL) {
			for (int i = 0; i < len; i++)
				buf[i] = ent->name[1 + i];
			buf[len] = 0;
		}
		a[3 + nargs] = (Cell)(uInt)len;
		rc = 0;
		goto nextprop_done;
	}

	rc = (int)client_interface((Cell *)args);
nextprop_done:
	amigaboot_ihandle_track_open(a, rc);

	/* Print this call iff it's the first occurrence of its service
	 * name, OR returned a non-zero rc (an error worth seeing), OR
	 * we're still in the first 5000 calls (during which the trace
	 * shows the full ABI shape; later calls are mostly stdin polls). */
	int is_error = (rc != 0);
	int print_it = !seen || is_error || (call_n <= 5000);
	if (print_it) {
		cprintf(g_e, "[ci#%d] %s nargs=%d nrets=%d",
		        call_n, name, (int)nargs, (int)nrets);
		for (uInt i = 0; i < nargs && i < 6; i++) {
			uInt v = (uInt)a[3 + i];
			cprintf(g_e, " a%d=0x%X", (int)i, (unsigned)v);
			/* If arg looks like a pointer into amigaboot's
			 * text/data (0x00200000..0x00400000), try to
			 * print as a NUL-terminated ASCII string. */
			if (v >= 0x00200000u && v < 0x00400000u) {
				const uByte *p = (const uByte *)(uPtr)v;
				char strbuf[32];
				int j = 0;
				while (j < (int)sizeof(strbuf) - 1 &&
				       p[j] >= 0x20 && p[j] < 0x7Fu)
					strbuf[j] = p[j], j++;
				strbuf[j] = 0;
				if (j >= 1)
					cprintf(g_e, "(\"%s\")", strbuf);
			}
		}
		cprintf(g_e, " -> rc=%d", rc);
		for (uInt i = 0; i < nrets && i < 6; i++)
			cprintf(g_e, " r%d=0x%X", (int)i,
				(unsigned)a[3 + nargs + i]);
		cprintf(g_e, "\n");
	}
	return rc;
#else
	{
		Cell *a = (Cell *)args;
		uInt svc_ptr = (uInt)a[0];
		uInt nargs   = (uInt)a[1];
		uInt nrets   = (uInt)a[2];
		const char *sname = (svc_ptr != 0)
			? (const char *)(uPtr)svc_ptr
			: "";

		amigaboot_ihandle_fixup_in(a);

		/* Same nextprop bug-fix as the CI_TRACE_LIMITED branch. */
		if (sname[0] == 'n' && sname[1] == 'e' &&
		    sname[2] == 'x' && sname[3] == 't' &&
		    sname[4] == 'p' && sname[5] == 'r' &&
		    nargs == 3 && nrets == 1) {
			Package *pkg = (Package *)(uPtr)(uInt)a[3];
			const Byte *prev = (const Byte *)(uPtr)(uInt)a[4];
			Byte *buf = (Byte *)(uPtr)(uInt)a[5];
			Entry *ent;

			if (pkg == NULL || pkg->props == NULL)
				pkg = g_e->root;
			if (pkg == NULL || pkg->props == NULL) {
				a[3 + nargs] = (Cell)-1;
				amigaboot_ihandle_track_open(a, 0);
				return 0;
			}
			if (prev == NULL || *prev == 0) {
				ent = pkg->props->list;
			} else {
				ent = find_table(pkg->props,
				                 (Byte *)prev, CSTR);
				if (ent != NULL)
					ent = ent->link;
			}
			if (ent == NULL || ent->name == NULL) {
				a[3 + nargs] = (Cell)-1;
				amigaboot_ihandle_track_open(a, 0);
				return 0;
			}
			int len = (int)(uByte)ent->name[0];
			if (len > 31) len = 31;
			if (buf != NULL) {
				for (int i = 0; i < len; i++)
					buf[i] = ent->name[1 + i];
				buf[len] = 0;
			}
			a[3 + nargs] = (Cell)(uInt)len;
			amigaboot_ihandle_track_open(a, 0);
			return 0;
		}

		int rc = (int)client_interface((Cell *)args);
		amigaboot_ihandle_track_open(a, rc);
		return rc;
	}
#endif
}

/* Wrapper around ci_dispatch_body that re-enables MSR[EE] for the
 * duration of CI processing.
 *
 * The OS (amigaboot.of in particular) hands control back to firmware
 * via the CI handler with MSR[EE]=0 (interrupts off). Our atadisk
 * driver calls u_sleep(N) which busy-spins waiting for the
 * decrementer-driven _ms_tick_count to advance; with EE cleared, the
 * decrementer can never fire, so u_sleep hangs forever. Re-enable
 * EE while we run, restore on the way out so we don't disturb
 * OS-visible state. */
int
ci_dispatch(void *args)
{
	uInt saved_msr;
	__asm__ volatile (
		"mfmsr  %0\n\t"
		"ori    11, %0, 0x8000\n\t"   /* set MSR[EE] */
		"mtmsr  11\n\t"
		"isync\n\t"
		: "=r"(saved_msr) :: "r11", "memory"
	);

	int rc = ci_dispatch_body(args);

	__asm__ volatile (
		"mtmsr  %0\n\t"
		"isync\n\t"
		: : "r"(saved_msr) : "memory"
	);
	return rc;
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
