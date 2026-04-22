/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Exception-handler tail: register dump on UART1 and halt.
 *
 *  Called by common_trap in exceptions.S with r3 = &_panic_frame.
 *  The struct layout below MUST match the byte offsets used in
 *  exceptions.S's save sequence.
 */

#include <stdint.h>
#include "pegasos2.h"
#include "uart16550.h"

struct panic_frame {
	uint32_t vector;    /* byte  0 */
	uint32_t gpr[32];   /* byte  4..128  (r0..r31) */
	uint32_t lr;        /* byte 132 */
	uint32_t ctr;       /* byte 136 */
	uint32_t xer;       /* byte 140 */
	uint32_t cr;        /* byte 144 */
	uint32_t srr0;      /* byte 148 */
	uint32_t srr1;      /* byte 152 */
	uint32_t dar;       /* byte 156 */
	uint32_t dsisr;     /* byte 160 */
	uint32_t msr;       /* byte 164 */
};

static const char *vector_name(uint32_t v)
{
	switch (v) {
	case 0x100:  return "System Reset";
	case 0x200:  return "Machine Check";
	case 0x300:  return "DSI (Data Storage)";
	case 0x400:  return "ISI (Instruction Storage)";
	case 0x500:  return "External Interrupt";
	case 0x600:  return "Alignment";
	case 0x700:  return "Program";
	case 0x800:  return "FP Unavailable";
	case 0x900:  return "Decrementer";
	case 0xC00:  return "System Call";
	case 0xD00:  return "Trace";
	case 0x1300: return "Instruction Breakpoint";
	default:     return "UNKNOWN";
	}
}

static void print_reg(const char *name, uint32_t v)
{
	uart_puts(UART1_BASE, name);
	uart_puts(UART1_BASE, " = 0x");
	uart_put_hex32(UART1_BASE, v);
}

void panic_dump(struct panic_frame *f)
{
	uart_puts(UART1_BASE, "\n\n!! PANIC: exception 0x");
	uart_put_hex32(UART1_BASE, f->vector);
	uart_puts(UART1_BASE, " (");
	uart_puts(UART1_BASE, vector_name(f->vector));
	uart_puts(UART1_BASE, ")\n");

	print_reg("   SRR0 ", f->srr0);
	print_reg("   SRR1 ", f->srr1);
	uart_puts(UART1_BASE, "\n");
	print_reg("   DAR  ", f->dar);
	print_reg("   DSISR", f->dsisr);
	uart_puts(UART1_BASE, "\n");
	print_reg("   MSR  ", f->msr);
	print_reg("   LR   ", f->lr);
	uart_puts(UART1_BASE, "\n");
	print_reg("   CTR  ", f->ctr);
	print_reg("   XER  ", f->xer);
	uart_puts(UART1_BASE, "\n");
	print_reg("   CR   ", f->cr);
	uart_puts(UART1_BASE, "\n");

	static const char *const gpr_labels[8] = {
		"r0 ..r3 ", "r4 ..r7 ", "r8 ..r11", "r12..r15",
		"r16..r19", "r20..r23", "r24..r27", "r28..r31",
	};
	for (int row = 0; row < 8; row++) {
		uart_puts(UART1_BASE, "   ");
		uart_puts(UART1_BASE, gpr_labels[row]);
		uart_puts(UART1_BASE, ": ");
		for (int j = 0; j < 4; j++) {
			uart_put_hex32(UART1_BASE, f->gpr[row * 4 + j]);
			uart_putc(UART1_BASE, ' ');
		}
		uart_puts(UART1_BASE, "\n");
	}

	uart_puts(UART1_BASE, "!! halted.\n");

	for (;;)
		;
}
