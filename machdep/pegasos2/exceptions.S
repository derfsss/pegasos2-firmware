/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  PowerPC exception vectors for Phase 1+ of the Pegasos2 clean-room
 *  firmware.
 *
 *  Vector layout (per docs/01-cpu-init.md "Exception vectors"):
 *
 *    0x00000100  system reset
 *    0x00000200  machine check
 *    0x00000300  DSI (data storage interrupt)
 *    0x00000400  ISI (instruction storage interrupt)
 *    0x00000500  external interrupt
 *    0x00000600  alignment
 *    0x00000700  program (illegal instruction / trap)
 *    0x00000800  FPU unavailable
 *    0x00000900  decrementer
 *    0x00000C00  system call
 *    0x00000D00  trace
 *    0x00001300  instruction address breakpoint
 *
 *  This file's contents land in the .vectors output section (LMA in
 *  flash, VMA 0x00000000). reset.S copies it into place before
 *  clearing MSR[IP], so the stubs at the documented VMAs become live
 *  vectors.
 *
 *  Phase 1 handler behaviour is intentionally minimal: capture all
 *  registers plus SRR0/SRR1/DAR/DSISR/LR/CTR/XER/CR/MSR into a
 *  panic_frame in .bss, swap to a dedicated panic_stack, and call
 *  panic_dump() in C. Forth-runtime-era handlers (external-interrupt
 *  dispatch, decrementer tick, syscall for the client interface) are
 *  replaced later; today they just panic.
 *
 *  SPR numbers used (Power ISA "Book II" values):
 *     18 = DSISR       19 = DAR
 *     26 = SRR0        27 = SRR1
 *    272 = SPRG0      273 = SPRG1
 *  LR/CTR/XER/CR use the standard mfXX mnemonics.
 */

	.section .vectors, "ax", @progbits

/*
 * Each vector stub fits in at most 256 bytes (the gap between
 * exception origins). We only need three instructions: stash r3
 * into SPRG0, load the vector origin into r3, and branch to the
 * common save-state routine.
 */
	.macro VECTOR_STUB off
	. = \off
	mtspr   272, r3         /* SPRG0 = r3 (preserve caller's r3) */
	li      r3, \off        /* r3 = vector origin for the common path */
	b       common_trap
	.endm

	VECTOR_STUB 0x100
	VECTOR_STUB 0x200
	VECTOR_STUB 0x300
	VECTOR_STUB 0x400
	VECTOR_STUB 0x500
	VECTOR_STUB 0x600
	VECTOR_STUB 0x700
	VECTOR_STUB 0x800

	/*
	 * Decrementer (0x900): real handler, not a panic stub.
	 *
	 * On entry: SPR 22 (DEC) has just wrapped to negative.
	 *   SRR0/SRR1 = interrupted PC / MSR. rfi will restore both.
	 *   MSR at handler entry has IR=DR=EE=0 (HW forces those off).
	 *   SPRG0/SPRG1 are free scratch; all other regs must survive.
	 *
	 * Body:
	 *   ++_ms_tick_count
	 *   DEC <- _dec_reload       (written by timer_calibrate(), zero
	 *                             at power-on -- caller must calibrate
	 *                             before enabling MSR[EE])
	 *   rfi
	 */
	. = 0x900
	mtspr   272, r3              /* SPRG0 = r3 */
	mtspr   273, r4              /* SPRG1 = r4 */
	lis     r3, _ms_tick_count@ha
	lwz     r4, _ms_tick_count@l(r3)
	addi    r4, r4, 1
	stw     r4, _ms_tick_count@l(r3)
	lis     r3, _dec_reload@ha
	lwz     r4, _dec_reload@l(r3)
	mtspr   22, r4               /* SPR 22 = DEC; re-arm */
	mfspr   r4, 273
	mfspr   r3, 272
	rfi

	/*
	 * Syscall (0xC00): IEEE-1275 client-interface entry. `sc`
	 * triggers this vector with SRR0 = address of the instruction
	 * after `sc` and SRR1 = MSR at the time of the call. The
	 * trampoline is larger than 256 bytes (it saves all 32 GPRs
	 * plus SPRs to a frame), so this slot just branches to the
	 * body out-of-line. 24-bit PPC branch reaches ±32 MiB; the
	 * body lives a few hundred bytes later in .vectors.
	 */
	. = 0xC00
	b       syscall_trampoline

	VECTOR_STUB 0xD00
	VECTOR_STUB 0x1300

/*
 * Common trap handler at offset 0x1400 (outside the IEEE-exception
 * address range, still inside the 8 KiB .vectors region). Reached
 * by every vector stub with r3 = vector origin and the original
 * r3 stashed in SPRG0.
 *
 * Layout of the panic_frame (byte offsets, matching struct
 * panic_frame in panic.c):
 *      0  vector
 *      4  gpr[0..31]       (32 * 4 = 128 bytes, r0 .. r31 at 4..128)
 *    132  lr
 *    136  ctr
 *    140  xer
 *    144  cr
 *    148  srr0
 *    152  srr1
 *    156  dar
 *    160  dsisr
 *    164  msr
 */
	. = 0x1400
	.globl common_trap
	.type  common_trap, @function
common_trap:
	/* Free r4 via SPRG1 so we have two scratches. */
	mtspr   273, r4         /* SPRG1 = r4 */

	/* r4 = &panic_frame (physical; MSR[DR]=0 post-exception). */
	lis     r4, _panic_frame@ha
	addi    r4, r4, _panic_frame@l

	/* vector number (currently in r3) */
	stw     r3, 0(r4)

	/* GPRs 0..2 are live as passed. r3 and r4 come from SPRGs. */
	stw     r0, 4(r4)
	stw     r1, 8(r4)
	stw     r2, 12(r4)
	mfspr   r3, 272          /* saved r3 */
	stw     r3, 16(r4)
	mfspr   r3, 273          /* saved r4 */
	stw     r3, 20(r4)
	stw     r5,  24(r4)
	stw     r6,  28(r4)
	stw     r7,  32(r4)
	stw     r8,  36(r4)
	stw     r9,  40(r4)
	stw     r10, 44(r4)
	stw     r11, 48(r4)
	stw     r12, 52(r4)
	stw     r13, 56(r4)
	stw     r14, 60(r4)
	stw     r15, 64(r4)
	stw     r16, 68(r4)
	stw     r17, 72(r4)
	stw     r18, 76(r4)
	stw     r19, 80(r4)
	stw     r20, 84(r4)
	stw     r21, 88(r4)
	stw     r22, 92(r4)
	stw     r23, 96(r4)
	stw     r24, 100(r4)
	stw     r25, 104(r4)
	stw     r26, 108(r4)
	stw     r27, 112(r4)
	stw     r28, 116(r4)
	stw     r29, 120(r4)
	stw     r30, 124(r4)
	stw     r31, 128(r4)

	mflr    r3
	stw     r3, 132(r4)
	mfctr   r3
	stw     r3, 136(r4)
	mfxer   r3
	stw     r3, 140(r4)
	mfcr    r3
	stw     r3, 144(r4)
	mfspr   r3, 26           /* SRR0 */
	stw     r3, 148(r4)
	mfspr   r3, 27           /* SRR1 */
	stw     r3, 152(r4)
	mfspr   r3, 19           /* DAR */
	stw     r3, 156(r4)
	mfspr   r3, 18           /* DSISR */
	stw     r3, 160(r4)
	mfmsr   r3
	stw     r3, 164(r4)

	/*
	 * Swap to a dedicated panic stack. The panic path never returns,
	 * so the caller's stack is irrelevant -- but if the exception
	 * itself was triggered by stack corruption, reusing r1 would
	 * recurse into the same fault. Lay down one EABI stack frame
	 * with a NULL back chain.
	 */
	lis     r1, _panic_stack_top@ha
	addi    r1, r1, _panic_stack_top@l
	li      r0, 0
	stwu    r0, -16(r1)
	mtlr    r0

	/* r3 = &panic_frame for the C handler. */
	mr      r3, r4
	bl      panic_dump

	/* panic_dump() never returns, but defensively loop if it does. */
1:	b       1b

	.size common_trap, . - common_trap

/*
 * Syscall trampoline. Reached from the 0xC00 vector via a 24-bit
 * relative branch. Responsibilities:
 *
 *   1. Save caller's full register state to _syscall_frame in .bss.
 *      Layout matches struct sc_frame in syscall.c:
 *          0   gpr[0..31]   (32 words, r0 at 0, r31 at 124)
 *        128   lr
 *        132   ctr
 *        136   xer
 *        140   cr
 *        144   srr0          (= PC after the `sc` instruction)
 *        148   srr1          (= MSR at time of `sc`)
 *
 *   2. Call syscall_dispatch(&frame) in C. The caller's r1 is still
 *      valid (real mode, no translation flip needed) and the
 *      dispatcher can use it for its own stack frame.
 *
 *   3. Reload all state from the (possibly modified) frame. The
 *      dispatcher may have written return values into gpr[3]..[4]
 *      or advanced SRR0 to skip over a helper sequence.
 *
 *   4. rfi. Hardware restores PC from SRR0 and MSR from SRR1.
 *
 * SPRG0/SPRG1 are used as scratch (same as common_trap). This is
 * safe because exceptions can't nest during a syscall: MSR[EE] is
 * cleared by hardware on exception entry and we don't re-enable
 * it here. A panic from inside syscall_dispatch would still use
 * SPRG0/1 -- correct, the syscall state is being abandoned anyway.
 */
	.globl syscall_trampoline
	.type  syscall_trampoline, @function
syscall_trampoline:
	mtspr   272, r3                  /* SPRG0 = r3 */
	mtspr   273, r4                  /* SPRG1 = r4 */

	lis     r4, _syscall_frame@ha
	addi    r4, r4, _syscall_frame@l

	stw     r0,  0(r4)
	stw     r1,  4(r4)
	stw     r2,  8(r4)
	mfspr   r3, 272                  /* saved r3 */
	stw     r3, 12(r4)
	mfspr   r3, 273                  /* saved r4 */
	stw     r3, 16(r4)
	stw     r5,  20(r4)
	stw     r6,  24(r4)
	stw     r7,  28(r4)
	stw     r8,  32(r4)
	stw     r9,  36(r4)
	stw     r10, 40(r4)
	stw     r11, 44(r4)
	stw     r12, 48(r4)
	stw     r13, 52(r4)
	stw     r14, 56(r4)
	stw     r15, 60(r4)
	stw     r16, 64(r4)
	stw     r17, 68(r4)
	stw     r18, 72(r4)
	stw     r19, 76(r4)
	stw     r20, 80(r4)
	stw     r21, 84(r4)
	stw     r22, 88(r4)
	stw     r23, 92(r4)
	stw     r24, 96(r4)
	stw     r25, 100(r4)
	stw     r26, 104(r4)
	stw     r27, 108(r4)
	stw     r28, 112(r4)
	stw     r29, 116(r4)
	stw     r30, 120(r4)
	stw     r31, 124(r4)

	mflr    r3
	stw     r3, 128(r4)
	mfctr   r3
	stw     r3, 132(r4)
	mfxer   r3
	stw     r3, 136(r4)
	mfcr    r3
	stw     r3, 140(r4)
	mfspr   r3, 26                   /* SRR0 */
	stw     r3, 144(r4)
	mfspr   r3, 27                   /* SRR1 */
	stw     r3, 148(r4)

	mr      r3, r4                   /* r3 = &frame */
	bl      syscall_dispatch

	/* Reload state from frame. Walk SPRs first so r3/r4 are not
	 * yet their final values while we still need them as scratch. */
	lis     r4, _syscall_frame@ha
	addi    r4, r4, _syscall_frame@l

	lwz     r3, 144(r4)
	mtspr   26, r3                   /* SRR0 */
	lwz     r3, 148(r4)
	mtspr   27, r3                   /* SRR1 */
	lwz     r3, 140(r4)
	mtcr    r3
	lwz     r3, 136(r4)
	mtxer   r3
	lwz     r3, 132(r4)
	mtctr   r3
	lwz     r3, 128(r4)
	mtlr    r3

	lwz     r31, 124(r4)
	lwz     r30, 120(r4)
	lwz     r29, 116(r4)
	lwz     r28, 112(r4)
	lwz     r27, 108(r4)
	lwz     r26, 104(r4)
	lwz     r25, 100(r4)
	lwz     r24, 96(r4)
	lwz     r23, 92(r4)
	lwz     r22, 88(r4)
	lwz     r21, 84(r4)
	lwz     r20, 80(r4)
	lwz     r19, 76(r4)
	lwz     r18, 72(r4)
	lwz     r17, 68(r4)
	lwz     r16, 64(r4)
	lwz     r15, 60(r4)
	lwz     r14, 56(r4)
	lwz     r13, 52(r4)
	lwz     r12, 48(r4)
	lwz     r11, 44(r4)
	lwz     r10, 40(r4)
	lwz     r9,  36(r4)
	lwz     r8,  32(r4)
	lwz     r7,  28(r4)
	lwz     r6,  24(r4)
	lwz     r5,  20(r4)
	lwz     r0,  0(r4)
	lwz     r1,  4(r4)
	lwz     r2,  8(r4)
	/* r3 and r4 last -- they were our frame-pointer scratch. */
	lwz     r3, 12(r4)               /* frame[r3] -> r3 */
	lwz     r4, 16(r4)               /* frame[r4] -> r4 (via old r4) */

	rfi

	.size syscall_trampoline, . - syscall_trampoline

/*
 * .bss allocations for the panic frame and the panic stack.
 * Declared here (rather than panic.c) so the asm side has stable
 * linker-visible symbols for `lis/addi`.
 */

	.section .bss
	.align   4
	.globl   _panic_frame
_panic_frame:
	.skip    256

	.align   4
	.globl   _panic_stack
_panic_stack:
	.skip    4096
	.globl   _panic_stack_top
_panic_stack_top:

	/*
	 * Millisecond tick counter, incremented by the decrementer
	 * handler at 0x900. 32-bit so aligned reads are atomic on
	 * PPC32; wraps every ~49 days which is irrelevant for a
	 * boot-only firmware.
	 */
	.align   4
	.globl   _ms_tick_count
_ms_tick_count:
	.skip    4

	/*
	 * Decrementer reload value. Loaded by the 0x900 handler each
	 * time the decrementer underflows. Must be non-zero before
	 * MSR[EE] is enabled -- otherwise the handler writes 0 to DEC
	 * and the counter fires again immediately, looping forever.
	 * timer_calibrate() in timer.c seeds this from the detected /
	 * assumed TB frequency.
	 */
	.align   4
	.globl   _dec_reload
_dec_reload:
	.skip    4

	/*
	 * Syscall frame used by syscall_trampoline and read by
	 * syscall_dispatch (struct sc_frame). 152 bytes live, rounded
	 * to 256 for slack if the C struct grows.
	 */
	.align   4
	.globl   _syscall_frame
_syscall_frame:
	.skip    256

	/* Silence a linker GNU-stack note for this asm file. */
	.section .note.GNU-stack, "", @progbits
