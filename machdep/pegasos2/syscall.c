/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  System-call (0xC00) tail: receives the saved register state from
 *  syscall_trampoline in exceptions.S, does whatever work the caller
 *  asked for, optionally writes back return registers, then returns.
 *  The trampoline rfis us back to the instruction after `sc`.
 *
 *  Today this is a stub. When the IEEE-1275 client interface (spec
 *  06) lands, dispatch will grow to decode the caller's call-structure
 *  pointer in r3 and route to the appropriate service. The stub
 *  contract is: whatever value the caller put in r3 before `sc`, they
 *  get 0x0000BABE back as a return value.
 *
 *  The struct layout below MUST match the byte offsets saved and
 *  restored by syscall_trampoline.
 */

#include <stdint.h>
#include "pegasos2.h"
#include "uart16550.h"

struct sc_frame {
	uint32_t gpr[32];   /* 0..124   r0..r31 */
	uint32_t lr;        /* 128 */
	uint32_t ctr;       /* 132 */
	uint32_t xer;       /* 136 */
	uint32_t cr;        /* 140 */
	uint32_t srr0;      /* 144  = PC of instruction after `sc` */
	uint32_t srr1;      /* 148  = MSR at time of `sc` */
};

void syscall_dispatch(struct sc_frame *f)
{
	uart_puts(UART1_BASE, "[syscall r3=0x");
	uart_put_hex32(UART1_BASE, f->gpr[3]);
	uart_puts(UART1_BASE, " srr0=0x");
	uart_put_hex32(UART1_BASE, f->srr0);
	uart_puts(UART1_BASE, "]\n");

	/* Stub return value. Real dispatch will come from spec 06. */
	f->gpr[3] = 0x0000BABEu;
}
