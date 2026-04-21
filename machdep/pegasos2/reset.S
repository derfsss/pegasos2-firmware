/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Pegasos II Phase-1 bootstrap.
 *
 *  Runs in real mode (MSR[IR]=MSR[DR]=0) directly from flash. No
 *  stack is available yet (DRAM init lands in a later milestone);
 *  this file is therefore entirely assembly, using only leaf-call
 *  subroutines that save LR in a caller register slot.
 *
 *  Behaviour for this milestone:
 *    1. Initialise VT8231 UART1 (16550 at CPU physical 0xF80003F8).
 *    2. Emit a banner on UART1.
 *    3. Read PVR (SPR 287) and emit it as eight hex digits.
 *    4. Halt.
 *
 *  Spec references:
 *    - docs/01-cpu-init.md  "Reset vector", "PVR detection"
 *    - docs/04-southbridge.md  "SuperIO" table (UART1 at 0x3F8, IRQ 4)
 *    - docs/00-overview.md  System memory map (PCI0 I/O at 0xF8000000)
 */

	.section .reset, "ax", @progbits
	.globl   _reset_vector
	.type    _reset_vector, @function
	.align   2

/*
 * Register usage across this file:
 *   r3  = UART base address (0xF80003F8), preserved across all helpers
 *   r4  = byte to send (tx helpers' arg / scratch)
 *   r5  = string pointer (tx_string arg)
 *   r6  = LSR scratch
 *   r7  = 32-bit value to print (tx_hex32 arg)
 *   r8  = loop counter
 *   r9  = nibble scratch
 *   r30 = saved LR across bl calls (manual one-level return saving;
 *         needed because bl clobbers LR and we have no stack for it)
 */

_reset_vector:
	/*
	 * Step 0 -- MSR is already IR=0, DR=0, EE=0, IP=1 after hard
	 * reset. Real mode, vectors at 0xFFFxxxxx. We're on solid
	 * ground to execute from flash.
	 */

	/*
	 * Step 0.5 -- Enough MV64361 + VT8231 setup to reach UART1.
	 * On QEMU pegasos2 with -bios (no VOF), the MV64361 PCI0 I/O
	 * window is disabled at reset and the VT8231 ISA bridge has
	 * its PCI_COMMAND I/O decode bit clear. Without these writes
	 * our stores to 0xF80003F8 are silently dropped.
	 */

	/*
	 * MV64361 register bank base (CPU physical 0xF1000000).
	 *
	 * The MV64361 MMIO region is little-endian from the CPU's
	 * perspective on QEMU (confirmed by -trace mv64361_reg_write
	 * showing a PPC-native stw of 0x000FBDFF arriving as
	 * 0xFFBD0F00). All 32-bit register writes therefore use
	 * stwbrx to pre-byte-reverse.
	 */
	lis	r10, 0xF100

	/* PCI0 I/O ADDR REMAP (offset 0x0F0) = 0: maps CPU 0xF8000000
	 * onto PCI I/O 0x00000000 so ISA port 0x3F8 -> CPU 0xF80003F8.
	 * Value 0 is byte-symmetric; stw or stwbrx both work. */
	li	r4, 0
	li	r11, 0x0F0
	stwbrx	r4, r10, r11
	eieio

	/* BASE_ADDR_ENABLE (offset 0x278). Reset default = 0x000FBFFF
	 * (bit N set => window N disabled). Clear bit 9 to enable the
	 * PCI0 I/O window: 0x000FBFFF & ~(1<<9) = 0x000FBDFF. */
	lis	r4, 0x000F
	ori	r4, r4, 0xBDFF
	li	r11, 0x278
	stwbrx	r4, r10, r11
	eieio

	/* VT8231 I/O decode enable via PCI1 config cycle.
	 *
	 * On QEMU pegasos2, VT8231 lives on PCI1 (not PCI0 as
	 * docs/04-southbridge.md currently states). Spec 03 says
	 * PCI1's config register pair is at MV64361 offsets
	 * 0x8CF8/0x8CFC; QEMU's model puts them at 0xC78/0xC7C
	 * instead (and treats 0x8CF8 as unimplemented). Two
	 * spec-vs-QEMU divergences noted for maintainer follow-up;
	 * for now we match the emulator.
	 *
	 * Target: bus 0, dev 0x0C, fn 0, reg 0x04.
	 * CA = 0x80000000 | (0x0C << 11) | 0x04 = 0x80006004.
	 */
	lis	r4, 0x8000
	ori	r4, r4, 0x6004
	li	r11, 0xC78
	stwbrx	r4, r10, r11		/* PCI1 CA <- 0x80006004 */
	eieio
	li	r4, 0x07
	li	r11, 0xC7C
	stwbrx	r4, r10, r11		/* PCI1 CD <- 0x00000007 */
	eieio

	/*
	 * VT8231 SuperIO unlock: set bit 2 of PCI fn 0 cfg register
	 * 0x50 so writes to ISA ports 0x3F0/0x3F1 are routed into the
	 * SuperIO config window. Other bits of reg 0x50 are "don't
	 * care" for Phase 0. CA = 0x80006050.
	 */
	lis	r4, 0x8000
	ori	r4, r4, 0x6050
	li	r11, 0xC78
	stwbrx	r4, r10, r11
	eieio
	li	r4, 0x04		/* bit 2: SuperIO config ports enable */
	li	r11, 0xC7C
	stwbrx	r4, r10, r11
	eieio

	/*
	 * Enable UART1 via SuperIO config. Index 0xF2 "Function
	 * select", bit 2 = UART1 enable. Reset default is 0x03, so
	 * write 0x07 to preserve reset bits and add UART1. Base-
	 * address select at index 0xF4 already reads 0xFE at reset
	 * (port 0x3F8) -- leave alone.
	 *
	 * SuperIO config access: ISA I/O port 0x3F0 = index,
	 * 0x3F1 = data. ISA lives on PCI1, so CPU physical is
	 * 0xFE000000 + port.
	 */
	lis	r8, 0xFE00
	ori	r8, r8, 0x03F0		/* r8 = SuperIO index port */
	li	r4, 0xF2
	stb	r4, 0(r8)
	eieio
	li	r4, 0x07
	stb	r4, 1(r8)		/* data port at 0x3F1 */
	eieio

	/* Load UART1 base into r3 once; keep it there for this file.
	 * UART1 at ISA I/O 0x3F8, sitting on the VT8231 (PCI1). PCI1
	 * I/O window is at CPU 0xFE000000 so UART1 = 0xFE0003F8. */
	lis	r3, 0xFE00
	ori	r3, r3, 0x03F8

	/*
	 * Step 1 -- Initialise 16550. Sequence:
	 *   LCR = 0x80    enable divisor latch access (DLAB=1)
	 *   DLL = 0x01    low byte of divisor  (115200 @ 1.8432 MHz)
	 *   DLM = 0x00    high byte of divisor
	 *   LCR = 0x03    8N1, DLAB=0
	 *   FCR = 0x07    FIFO enable + reset RX/TX FIFOs
	 *   MCR = 0x03    DTR + RTS
	 * QEMU's 16550 ignores the divisor; this sequence is chosen to
	 * also work on real hardware without code changes.
	 */
	li	r4, 0x80
	stb	r4, 3(r3)		/* LCR */
	li	r4, 0x01
	stb	r4, 0(r3)		/* DLL */
	li	r4, 0x00
	stb	r4, 1(r3)		/* DLM */
	li	r4, 0x03
	stb	r4, 3(r3)		/* LCR = 8N1 */
	li	r4, 0x07
	stb	r4, 2(r3)		/* FCR */
	li	r4, 0x03
	stb	r4, 4(r3)		/* MCR */
	eieio				/* flush MMIO writes */

	/* Step 2 -- Banner. */
	lis	r5, banner@ha
	addi	r5, r5, banner@l
	bl	tx_string

	/* Step 3 -- PVR. */
	lis	r5, msg_pvr@ha
	addi	r5, r5, msg_pvr@l
	bl	tx_string
	mfspr	r7, 287			/* PVR */
	bl	tx_hex32
	bl	tx_crlf

	/*
	 * Step 3.5 -- DRAM stuck-bit test at physical 0x00001000.
	 *
	 * On QEMU pegasos2 the DRAM is already live from machine init
	 * (no real SPD probe / DDR init sequence needed -- the real-HW
	 * sequence per docs/02-memory-controller.md section "DDR init
	 * sequence" is a later milestone). This test just proves the
	 * CPU can round-trip values to DRAM.
	 *
	 * Two complementary patterns cover every data-line stuck-at
	 * bit. Address 0x1000 is past the exception-vector area
	 * (0x100..0xFFF) so the test does not trample a later load.
	 */
	li	r7, 0x1000
	lis	r8, 0xAAAA
	ori	r8, r8, 0x5555
	stw	r8, 0(r7)
	sync
	lwz	r9, 0(r7)
	cmpw	r8, r9
	bne	dram_bad
	lis	r8, 0x5555
	ori	r8, r8, 0xAAAA
	stw	r8, 0(r7)
	sync
	lwz	r9, 0(r7)
	cmpw	r8, r9
	beq	dram_good
dram_bad:
	lis	r5, msg_dram_fail@ha
	addi	r5, r5, msg_dram_fail@l
	b	dram_print
dram_good:
	lis	r5, msg_dram_ok@ha
	addi	r5, r5, msg_dram_ok@l
dram_print:
	bl	tx_string

	/* Step 4 -- Halt. */
	lis	r5, msg_halt@ha
	addi	r5, r5, msg_halt@l
	bl	tx_string

halt:
	b	halt

	.size _reset_vector, . - _reset_vector


/*
 * tx_string: write the NUL-terminated string at r5 to the UART at r3.
 *   in:  r3 = UART base, r5 = string pointer
 *   out: r5 = past terminator
 *   clobbers: r4, r6
 *   LR: standard — bl/blr nesting NOT allowed from callers that are
 *       themselves called via bl (this is a leaf).
 */
	.type tx_string, @function
	.align 2
tx_string:
1:	lbz	r4, 0(r5)
	cmpwi	r4, 0
	beq	2f
	addi	r5, r5, 1
	/* Wait for THRE (LSR bit 5). */
3:	lbz	r6, 5(r3)
	andi.	r6, r6, 0x20
	beq	3b
	stb	r4, 0(r3)
	eieio
	b	1b
2:	blr
	.size tx_string, . - tx_string


/*
 * tx_hex32: write r7 to the UART as 8 hex digits, MSB first.
 *   in:  r3 = UART base, r7 = 32-bit value
 *   clobbers: r4, r6, r7 (rotates), r8, r9
 *   leaf.
 */
	.type tx_hex32, @function
	.align 2
tx_hex32:
	li	r8, 8
hex_loop:
	rotlwi	r7, r7, 4		/* move top nibble into low 4 */
	andi.	r9, r7, 0x000F
	cmpwi	r9, 10
	blt	digit
	addi	r9, r9, 7		/* 10+'0'+7 = 'A' */
digit:
	addi	r4, r9, 0x30		/* '0' */
	/* Wait for THRE. */
3:	lbz	r6, 5(r3)
	andi.	r6, r6, 0x20
	beq	3b
	stb	r4, 0(r3)
	eieio
	subi	r8, r8, 1
	cmpwi	r8, 0
	bne	hex_loop
	blr
	.size tx_hex32, . - tx_hex32


/*
 * tx_crlf: write "\r\n" to the UART.
 *   in:  r3 = UART base
 *   clobbers: r4, r6
 *   leaf.
 */
	.type tx_crlf, @function
	.align 2
tx_crlf:
	li	r4, 0x0D		/* CR */
3:	lbz	r6, 5(r3)
	andi.	r6, r6, 0x20
	beq	3b
	stb	r4, 0(r3)
	li	r4, 0x0A		/* LF */
3:	lbz	r6, 5(r3)
	andi.	r6, r6, 0x20
	beq	3b
	stb	r4, 0(r3)
	eieio
	blr
	.size tx_crlf, . - tx_crlf


	.section .rodata, "a"
	.align 2
banner:
	.asciz "\r\n\r\nPegasos II BIOS (clean-room rewrite) - Phase 1\r\n"

msg_pvr:
	.asciz "CPU PVR = 0x"

msg_dram_ok:
	.asciz "DRAM test: OK (stuck-bit patterns verified at 0x00001000)\r\n"

msg_dram_fail:
	.asciz "DRAM test: FAIL\r\n"

msg_halt:
	.asciz "Phase 1 halt.\r\n"
