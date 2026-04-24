/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Spec 07 §"Boot-time register state" asm trampoline. Programs
 *  BATs covering DRAM + MV64361 (spec "MMU = firmware-established
 *  BATs still covering DRAM and MV64361 registers"), clears MSR[EE]
 *  and sets MSR[IR|DR] (spec "translation on, interrupts disabled"),
 *  sets up the IEEE-1275 client-program register convention, and
 *  transfers control to the loaded image. Does not return.
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

	/*
	 * Spec 07 §"Boot-time register state" requires "translation on"
	 * at handoff, with "firmware-established BATs still covering DRAM
	 * and MV64361 registers". The firmware itself runs MMU-off
	 * (MSR[IR]=MSR[DR]=0) from reset through here, so we program BATs
	 * and flip MSR[IR|DR] as a final act of the trampoline.
	 *
	 * BAT layout chosen:
	 *   BAT0: 0x00000000..0x0FFFFFFF (256 MiB), cacheable, R/W.
	 *         Covers all of QEMU's -m 512 DRAM (the first 256 MiB),
	 *         identity-mapped. The loaded kernel's image and our r1
	 *         stack sit inside this window.
	 *   BAT1: 0xF0000000..0xFFFFFFFF (256 MiB), cache-inhibited +
	 *         guarded, R/W. Covers the MV64361 register bank
	 *         (0xF1000000), PCI I/O window carrying UART1 via VT8231
	 *         (0xFE000000), and flash (0xFFF00000). Identity-mapped;
	 *         MMIO accesses go straight through with no speculation.
	 *
	 * BATs 2-3 are left invalidated. BATs 4-7 (7447A HIGH_BAT_EN) we
	 * don't touch -- default-disabled until HID0[7] is set, which we
	 * never set. The OS may re-map freely after entry.
	 *
	 * BAT register numbering (IBAT0U=528..IBAT3L=535, DBAT0U=536..
	 * DBAT3L=543). Sequence: clear every U register first to invalidate
	 * (Vs=Vp=0 in BATU means "not valid in any mode"), isync, then
	 * program BAT0 + BAT1 pairs (L before U by convention, though for
	 * a virgin BAT it doesn't matter), isync. Per 7447A PEM §3.2 the
	 * final isync synchronises instruction fetch against the BAT
	 * writes so the subsequent mtmsr that sets IR/DR sees them.
	 */

	/* Invalidate all IBAT / DBAT entries. */
	li      0, 0
	mtspr   528, 0                  /* IBAT0U */
	mtspr   529, 0                  /* IBAT0L */
	mtspr   530, 0                  /* IBAT1U */
	mtspr   531, 0                  /* IBAT1L */
	mtspr   532, 0                  /* IBAT2U */
	mtspr   533, 0                  /* IBAT2L */
	mtspr   534, 0                  /* IBAT3U */
	mtspr   535, 0                  /* IBAT3L */
	mtspr   536, 0                  /* DBAT0U */
	mtspr   537, 0                  /* DBAT0L */
	mtspr   538, 0                  /* DBAT1U */
	mtspr   539, 0                  /* DBAT1L */
	mtspr   540, 0                  /* DBAT2U */
	mtspr   541, 0                  /* DBAT2L */
	mtspr   542, 0                  /* DBAT3U */
	mtspr   543, 0                  /* DBAT3L */
	isync

	/*
	 * BAT0: DRAM window.
	 *   BATU = BEPI(0) | BL(0x7FF=256MiB) | Vs=1 | Vp=1 = 0x00001FFF
	 *   BATL = BRPN(0) | WIMG(0000)       | PP(10)       = 0x00000002
	 */
	lis     0, 0x0000
	ori     0, 0, 0x1FFF
	mtspr   528, 0                  /* IBAT0U */
	mtspr   536, 0                  /* DBAT0U */
	li      0, 0x0002
	mtspr   529, 0                  /* IBAT0L */
	mtspr   537, 0                  /* DBAT0L */

	/*
	 * BAT1: MV64361 + PCI I/O + flash window.
	 *   BATU = BEPI(0x7800) | BL(0x7FF) | Vs=1 | Vp=1 = 0xF0001FFF
	 *   BATL = BRPN(0x7800) | WIMG(0101, I+G) | PP(10) = 0xF000002A
	 * I (cache-inhibited) + G (guarded) is the standard WIMG for MMIO
	 * on PPC: stops the cache stealing MMIO reads and prevents the
	 * core from speculatively accessing device registers.
	 */
	lis     0, 0xF000
	ori     0, 0, 0x1FFF
	mtspr   530, 0                  /* IBAT1U */
	mtspr   538, 0                  /* DBAT1U */
	lis     0, 0xF000
	ori     0, 0, 0x002A
	mtspr   531, 0                  /* IBAT1L */
	mtspr   539, 0                  /* DBAT1L */
	isync

	/*
	 * MSR: clear EE (bit 16, 0x00008000), set IR (bit 26, 0x20),
	 * DR (bit 27, 0x10), and FP (bit 18, 0x2000). Leave MSR[ME]/
	 * [RI] alone so machine-check and recoverable bits stay set.
	 *
	 * FP matters because real OSes use floating-point from the
	 * bootstrap onwards -- AmigaOS 4's amigaboot.of performs fsub
	 * /fmadd/frsp during early device-tree parsing (integer-to-
	 * float-to-integer conversions), which take an 0x800 "FP
	 * Unavailable" interrupt if MSR[FP] is clear. The spec 07
	 * register-state table doesn't spell this out, but every
	 * stock Pegasos2 bootloader expects FP enabled at handoff.
	 *
	 * 0x2030 is FP | IR | DR.
	 *
	 * After mtmsr the next instruction fetch is a translated fetch;
	 * IBAT1 covers 0xFFF00000 (the VA of this code, running from flash)
	 * identity-mapped, so execution continues cleanly. The mandatory
	 * isync synchronises instruction fetch against the translation
	 * state change per 7447A PEM §2.3.2.
	 */
	mfmsr   0
	rlwinm  0, 0, 0, 17, 15         /* clear EE */
	ori     0, 0, 0x2030            /* set FP | IR | DR */
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
