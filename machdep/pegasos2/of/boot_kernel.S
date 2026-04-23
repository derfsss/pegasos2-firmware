/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Spec 07 §"Boot-time register state" asm trampoline. Clears
 *  MSR[EE] (per spec: "interrupts disabled"), sets up the IEEE-1275
 *  client-program register convention, and transfers control to
 *  the loaded image. Does not return.
 *
 *  Entry:
 *      r3 = image entry point (PC we want to reach)
 *      r4 = ci_handler address (will land in r5 for the OS)
 *      r5 = stack_top (will land in r1 for the OS)
 *
 *  The register shuffle must happen in asm because r5 is not a
 *  natural argument register on PPC -- the C ABI uses r3..r10 for
 *  integer args and we need r5 specifically for spec 07.
 */

	.section .text
	.globl  machine_jump_os
	.type   machine_jump_os, @function
machine_jump_os:
	/*
	 * Stash entry + ci_handler in SPRG0/SPRG1 before we touch the
	 * GPRs we need to load per spec 07. r5 already holds the new
	 * r1 value, so move it directly; r1 is a GPR we never use in
	 * this function (no stack ops, no callee-saves).
	 */
	mtspr   272, 3                  /* SPRG0 = entry */
	mtspr   273, 4                  /* SPRG1 = ci_handler */
	mr      1, 5                    /* r1 = stack_top (from arg 3) */

	/* Clear MSR[EE] (bit 16, 0x00008000). Leave MSR[ME]/[RI]
	 * alone so machine-check and recoverable bits stay set. */
	mfmsr   0
	rlwinm  0, 0, 0, 17, 15         /* mask out bit 16 (EE) */
	mtmsr   0
	isync                            /* context synch per 7447A PEM */

	/*
	 * IEEE-1275 client-program register convention (spec 07 §"Boot-
	 * time register state"):
	 *     r1 = small stack above image (set above, before MSR update)
	 *     r3 = 0
	 *     r4 = 0
	 *     r5 = CI handler entry pointer
	 *     r6 = 0 (reserved)
	 *     r7 = e_entry (some OSes check it)
	 */
	mfspr   7, 272                  /* r7 = entry */
	mfspr   5, 273                  /* r5 = ci_handler */
	li      3, 0
	li      4, 0
	li      6, 0

	mtctr   7
	bctr                             /* branch to entry; does not return */

	.size machine_jump_os, . - machine_jump_os

	.section .note.GNU-stack, "", @progbits
