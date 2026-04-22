/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Pegasos II Phase-1 reset vector.
 *
 *  The MPC7447 begins executing at 0xFFF00100 after hard reset
 *  (HRESET_HIGH, MSR[IP]=1). On entry MSR[IR]=MSR[DR]=0 (real
 *  mode), MSR[EE]=0 (interrupts disabled). This file does the
 *  minimum setup needed to call into C:
 *
 *    1. Set r1 = initial stack pointer in DRAM.
 *    2. Lay down one dummy PPC-EABI frame with NULL back chain.
 *    3. Branch to phase1_c_main().
 *
 *  DRAM was proved live in the previous milestone's stuck-bit
 *  test. On QEMU with -m 512 MiB the DRAM covers 0..0x1FFFFFFF,
 *  so a stack ceiling at 0x00100000 (1 MiB) is safely inside.
 *  The reset-vector page (0x0..0x1FFF) is left untouched; the
 *  stack grows downward from 0x00100000 and won't reach it.
 *
 *  Real-HW CPU init per docs/01-cpu-init.md (cache invalidate,
 *  BAT setup, MSR bits, clock-gen probe) is still pending. QEMU
 *  does not require any of it to reach this entry path.
 */

	.section .reset, "ax", @progbits
	.globl   _reset_vector
	.type    _reset_vector, @function
	.align   2

_reset_vector:
	/*
	 * Copy .data from its LMA in flash into its VMA in DRAM.
	 *   r3 = source = _data_lma (in flash)
	 *   r4 = dest   = _data_start (in DRAM)
	 *   r5 = end    = _data_end
	 * Word copy; lengths are already 4-aligned by the linker.
	 */
	lis	r3, _data_lma@ha
	addi	r3, r3, _data_lma@l
	lis	r4, _data_start@ha
	addi	r4, r4, _data_start@l
	lis	r5, _data_end@ha
	addi	r5, r5, _data_end@l
copy_data:
	cmpw	r4, r5
	bge	copy_data_done
	lwz	r0, 0(r3)
	stw	r0, 0(r4)
	addi	r3, r3, 4
	addi	r4, r4, 4
	b	copy_data
copy_data_done:

	/*
	 * Zero .bss.
	 *   r4 = dest = _bss_start
	 *   r5 = end  = _bss_end
	 */
	lis	r4, _bss_start@ha
	addi	r4, r4, _bss_start@l
	lis	r5, _bss_end@ha
	addi	r5, r5, _bss_end@l
	li	r0, 0
zero_bss:
	cmpw	r4, r5
	bge	zero_bss_done
	stw	r0, 0(r4)
	addi	r4, r4, 4
	b	zero_bss
zero_bss_done:

	/*
	 * Copy .vectors from its LMA in flash to its VMA at 0x00000000.
	 *   r3 = source = _vectors_lma (in flash)
	 *   r4 = dest   = _vectors_start (= 0)
	 *   r5 = end    = _vectors_end
	 * Same word-copy pattern as .data above. .vectors length is
	 * ALIGN(4) so the word loop is exact.
	 */
	lis	r3, _vectors_lma@ha
	addi	r3, r3, _vectors_lma@l
	lis	r4, _vectors_start@ha
	addi	r4, r4, _vectors_start@l
	lis	r5, _vectors_end@ha
	addi	r5, r5, _vectors_end@l
copy_vec:
	cmpw	r4, r5
	bge	copy_vec_done
	lwz	r0, 0(r3)
	stw	r0, 0(r4)
	addi	r3, r3, 4
	addi	r4, r4, 4
	b	copy_vec
copy_vec_done:
	/*
	 * Ensure the handler stores are globally visible and any
	 * prefetched instructions are flushed before we let the CPU
	 * route an exception through 0x00000000.
	 */
	sync
	isync

	/*
	 * MSR fixups for Phase 1, per docs/01-cpu-init.md §MSR setup:
	 *    clear IP  (bit 25 MSB, mask 0x00000040) -- route exceptions
	 *              to the table we just copied at 0x00000100..
	 *    set   ME  (bit 19 MSB, mask 0x00001000) -- machine checks
	 *              become recoverable instead of CHECKSTOP
	 *    set   RI  (bit 30 MSB, mask 0x00000002) -- "recoverable
	 *              interrupt", signals that SRR0/SRR1 do not hold
	 *              critical in-flight state (steady-state value)
	 * mtmsr is context-synchronising only when paired with
	 * sync/isync around it per MPC7447 user manual.
	 */
	mfmsr	r3
	ori	r3, r3, 0x1002              /* set ME | RI */
	li	r4, 0x40                    /* IP mask */
	andc	r3, r3, r4                  /* clear IP */
	sync
	mtmsr	r3
	isync

	/* r1 = 0x00100000 (stack ceiling). Stack grows down toward 0. */
	lis	r1, 0x0010
	li	r0, 0
	stwu	r0, -16(r1)

	/* Clear LR -- there is nobody to return to from phase1_c_main. */
	mtlr	r0

	bl	phase1_c_main

	/* phase1_c_main spins forever; if control ever returns, halt. */
1:	b	1b

	.size _reset_vector, . - _reset_vector

	/* Mark the stack as non-executable (silences a linker warning;
	 * bare-metal firmware never uses this attribute). */
	.section .note.GNU-stack, "", @progbits
