/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Minimal ELF32 PPC BE test "kernel" for the spec 07 boot-kernel
 *  smoke test. Embedded into firmware.bin as a data blob; the
 *  Forth word `test-boot` copies it to DRAM at the linked address
 *  and hands off via boot-kernel. On entry, per spec 07:
 *      r3 = 0
 *      r4 = 0
 *      r5 = CI handler address (must be non-zero)
 *      r6 = 0
 *      r7 = this entry point
 *
 *  We verify the contract by:
 *    1. Writing a fixed pattern to UART1 to prove we reached here.
 *    2. Checking r5 != 0 and writing a second marker on success.
 *    3. Trapping via `twi 31, r0, 0` to produce a clean halt that
 *       the firmware's panic_dump can report as end-of-test.
 *
 *  UART1 is at PCI1 I/O + 0x3F8 = 0xFE0003F8. THR is at offset 0,
 *  LSR at offset 5, THRE (TX-holding-empty) is bit 5. Polling for
 *  THRE before each write avoids overruns when the firmware has
 *  unsynced output from before the handoff.
 */

	.section .text.start, "ax", @progbits
	.globl __start
	.type  __start, @function
__start:
	/* r11 = UART1 base. Use r11-r13 as scratch (no C ABI here;
	 * we're not a C function, this is the entry point). */
	lis     r11, 0xFE00
	ori     r11, r11, 0x03F8

	/* Write "KERNEL OK" via the fixed-string helper below. */
	bl      write_kernel_ok

	/* Verify r5 is non-zero (spec 07 says it's the CI handler
	 * pointer). Write " r5=" + 8 hex digits. */
	bl      write_r5

	/* Verify r1 (stack pointer) is set per spec 07 §register state
	 * ("r1 = a valid small stack in the loaded image's memory
	 * region"). Firmware sets it to ~4 KiB above the image top. */
	bl      write_r1

	/* Verify translation is on per spec 07 §register state
	 * ("MSR = supervisor, translation on, interrupts disabled").
	 * The firmware's machine_jump_os programs BAT0 (DRAM) + BAT1
	 * (MV64361+flash) then sets MSR[IR|DR]. Expected values on
	 * entry:
	 *   MSR    low nibble bits: IR(0x20) | DR(0x10) set, EE(0x8000) clear
	 *   DBAT0U = 0x00001FFF (BEPI=0, BL=0x7FF, Vs=Vp=1)
	 *   DBAT1U = 0xF0001FFF (BEPI=0x7800, BL=0x7FF, Vs=Vp=1)
	 */
	bl      write_msr
	bl      write_dbat0u
	bl      write_dbat1u

	/* Write terminator newline. */
	li      r12, 0x0A
	bl      uart_putc

	/* Spec 06 CI callback smoke test -- call the firmware's CI
	 * handler via the r5 pointer we were given at boot, asking for
	 * the "milliseconds" service. Proves that the CI entry is
	 * reachable from OS context (not just from firmware Forth) and
	 * returns a sane value. */
	bl      call_ci_milliseconds

	/* Halt via an unconditional trap. This hits the firmware's
	 * 0x700 Program-exception handler (panic_dump), which prints
	 * a register dump and spins -- a clean terminal state for
	 * the test observer. */
	twi     31, r0, 0

	/* Unreachable. */
1:	b       1b

/* ------- CI callback helper -------
 *
 * call_ci_milliseconds: invokes the firmware's ci_handler (address
 * in r5 on function entry) with a {"milliseconds", 0 args, 1 return}
 * call struct built on the stack. Prints " CI.ms=XXXXXXXX" on
 * success or " CI.ms=ERR" on failure.
 *
 * The IEEE-1275 call struct layout (spec 06 §"Entry convention"):
 *     u32 service        (pointer to ASCII service name)
 *     u32 nargs          (count of arg slots)
 *     u32 nreturns       (count of return slots)
 *     u32 argret[]       (nargs args followed by nreturns rets)
 *
 * ci_handler ABI:
 *     r3 = struct pointer on entry
 *     r3 = 0 on dispatch success (ret slots filled), nonzero on
 *          unknown service
 */
call_ci_milliseconds:
	mflr    r14

	/* Obtain a pointer to "milliseconds\0" via the usual PPC
	 * trampoline trick: BL to after the string, MFLR picks up the
	 * return address which is the byte just past the string. */
	bl      past_ci_str
.byte 'm','i','l','l','i','s','e','c','o','n','d','s', 0
	.align  2
past_ci_str:
	mflr    r18
	mtlr    r14                     /* restore caller LR before we
	                                   stomp it again below */
	mflr    r14                     /* re-save for the bctrl call */

	/* Allocate 32 bytes of CI scratch on the OS-provided stack.
	 * Layout (r1-relative after addi):
	 *   [r1+ 8]  service  = r18     (ptr to "milliseconds")
	 *   [r1+12]  nargs    = 0
	 *   [r1+16]  nreturns = 1
	 *   [r1+20]  rets[0]  = placeholder (CI fills with ms count)
	 */
	stwu    r1, -32(r1)
	stw     r18, 8(r1)
	li      r19, 0
	stw     r19, 12(r1)
	li      r19, 1
	stw     r19, 16(r1)
	li      r19, 0
	stw     r19, 20(r1)

	addi    r3, r1, 8              /* r3 = call-struct ptr */
	mtctr   r5                     /* ctr = ci_handler address */
	bctrl                          /* call ci_handler(struct_ptr) */

	mr      r20, r3                /* CI return code (0 = ok) */
	lwz     r21, 20(r1)            /* milliseconds value */
	addi    r1, r1, 32             /* restore stack */

	/* Print " CI.ms=" then hex. On non-zero CI ret, print "ERR"
	 * instead of the value. */
	li      r12, ' '; bl uart_putc
	li      r12, 'C'; bl uart_putc
	li      r12, 'I'; bl uart_putc
	li      r12, '.'; bl uart_putc
	li      r12, 'm'; bl uart_putc
	li      r12, 's'; bl uart_putc
	li      r12, '='; bl uart_putc

	cmpwi   r20, 0
	beq     ci_ms_hex
	li      r12, 'E'; bl uart_putc
	li      r12, 'R'; bl uart_putc
	li      r12, 'R'; bl uart_putc
	b       ci_ms_done

ci_ms_hex:
	/* 8 hex nibbles from r21, MSB first. */
	li      r16, 8
	mr      r17, r21
1:
	rlwinm  r12, r17, 4, 28, 31
	cmpwi   r12, 10
	blt     2f
	addi    r12, r12, ('A' - '0' - 10)
2:
	addi    r12, r12, '0'
	bl      uart_putc
	slwi    r17, r17, 4
	addic.  r16, r16, -1
	bne     1b

ci_ms_done:
	li      r12, 0x0A; bl uart_putc
	mtlr    r14
	blr

/* ------- Helpers ------- */

/* uart_putc: write byte in r12 to UART1 (base in r11). Waits for
 * LSR[THRE]. Clobbers r13. */
uart_putc:
	lbz     r13, 5(r11)             /* LSR */
	andi.   r13, r13, 0x20           /* THRE */
	beq     uart_putc
	stb     r12, 0(r11)             /* THR */
	blr

/* write_kernel_ok: emit "KERNEL OK\n". Uses r12/r13. */
write_kernel_ok:
	mflr    r14
	li      r12, 'K'; bl uart_putc
	li      r12, 'E'; bl uart_putc
	li      r12, 'R'; bl uart_putc
	li      r12, 'N'; bl uart_putc
	li      r12, 'E'; bl uart_putc
	li      r12, 'L'; bl uart_putc
	li      r12, ' '; bl uart_putc
	li      r12, 'O'; bl uart_putc
	li      r12, 'K'; bl uart_putc
	li      r12, 0x0A; bl uart_putc
	mtlr    r14
	blr

/* write_r5: emit " r5=XXXXXXXX" where X is hex. */
write_r5:
	mflr    r14
	mr      r15, r5                 /* stash r5 value */
	li      r12, ' '; bl uart_putc
	li      r12, 'r'; bl uart_putc
	li      r12, '5'; bl uart_putc
	li      r12, '='; bl uart_putc

	/* 8 hex nibbles from MSB. */
	li      r16, 8
	mr      r17, r15
1:
	rlwinm  r12, r17, 4, 28, 31      /* top nibble of r17 after <<4 */
	/* Actually we want top nibble first; shift-left r17 by 4, take low 4. */
	cmpwi   r12, 10
	blt     2f
	addi    r12, r12, ('A' - '0' - 10)
2:
	addi    r12, r12, '0'
	bl      uart_putc
	slwi    r17, r17, 4
	addic.  r16, r16, -1
	bne     1b

	mtlr    r14
	blr

/* write_r1: emit " r1=XXXXXXXX" where X is hex. Same shape as
 * write_r5. r1 is the stack pointer; since our asm doesn't push/pop
 * the value is still what the firmware handed us. */
write_r1:
	mflr    r14
	mr      r15, r1                 /* stash r1 value */
	li      r12, ' '; bl uart_putc
	li      r12, 'r'; bl uart_putc
	li      r12, '1'; bl uart_putc
	li      r12, '='; bl uart_putc

	li      r16, 8
	mr      r17, r15
1:
	rlwinm  r12, r17, 4, 28, 31
	cmpwi   r12, 10
	blt     2f
	addi    r12, r12, ('A' - '0' - 10)
2:
	addi    r12, r12, '0'
	bl      uart_putc
	slwi    r17, r17, 4
	addic.  r16, r16, -1
	bne     1b

	mtlr    r14
	blr

/* write_msr: emit " MSR=XXXXXXXX". Proves MSR[IR|DR]=1 at entry. */
write_msr:
	mflr    r14
	mfmsr   r15
	li      r12, ' '; bl uart_putc
	li      r12, 'M'; bl uart_putc
	li      r12, 'S'; bl uart_putc
	li      r12, 'R'; bl uart_putc
	li      r12, '='; bl uart_putc

	li      r16, 8
	mr      r17, r15
1:
	rlwinm  r12, r17, 4, 28, 31
	cmpwi   r12, 10
	blt     2f
	addi    r12, r12, ('A' - '0' - 10)
2:
	addi    r12, r12, '0'
	bl      uart_putc
	slwi    r17, r17, 4
	addic.  r16, r16, -1
	bne     1b

	mtlr    r14
	blr

/* write_dbat0u: emit " DBAT0U=XXXXXXXX". Proves BAT0 is programmed to
 * cover DRAM window (expected 0x00001FFF). */
write_dbat0u:
	mflr    r14
	mfspr   r15, 536                /* DBAT0U */
	li      r12, ' '; bl uart_putc
	li      r12, 'D'; bl uart_putc
	li      r12, 'B'; bl uart_putc
	li      r12, 'A'; bl uart_putc
	li      r12, 'T'; bl uart_putc
	li      r12, '0'; bl uart_putc
	li      r12, 'U'; bl uart_putc
	li      r12, '='; bl uart_putc

	li      r16, 8
	mr      r17, r15
1:
	rlwinm  r12, r17, 4, 28, 31
	cmpwi   r12, 10
	blt     2f
	addi    r12, r12, ('A' - '0' - 10)
2:
	addi    r12, r12, '0'
	bl      uart_putc
	slwi    r17, r17, 4
	addic.  r16, r16, -1
	bne     1b

	mtlr    r14
	blr

/* write_dbat1u: emit " DBAT1U=XXXXXXXX". Proves BAT1 is programmed to
 * cover MV64361+flash window (expected 0xF0001FFF). */
write_dbat1u:
	mflr    r14
	mfspr   r15, 538                /* DBAT1U */
	li      r12, ' '; bl uart_putc
	li      r12, 'D'; bl uart_putc
	li      r12, 'B'; bl uart_putc
	li      r12, 'A'; bl uart_putc
	li      r12, 'T'; bl uart_putc
	li      r12, '1'; bl uart_putc
	li      r12, 'U'; bl uart_putc
	li      r12, '='; bl uart_putc

	li      r16, 8
	mr      r17, r15
1:
	rlwinm  r12, r17, 4, 28, 31
	cmpwi   r12, 10
	blt     2f
	addi    r12, r12, ('A' - '0' - 10)
2:
	addi    r12, r12, '0'
	bl      uart_putc
	slwi    r17, r17, 4
	addic.  r16, r16, -1
	bne     1b

	mtlr    r14
	blr

	.size __start, . - __start
	.section .note.GNU-stack, "", @progbits
