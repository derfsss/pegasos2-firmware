/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Phase-1 C entry point. Called by reset.S after the stack has been
 *  set up in DRAM. Runs in real mode (MSR[IR]=MSR[DR]=0), no caches,
 *  no exception vectors installed, no FP. Brings up UART1 and emits
 *  a banner plus a DRAM round-trip check, then halts.
 */

#include <stdint.h>
#include "pegasos2.h"
#include "uart16550.h"
#include "mv64361.h"
#include "vt8231.h"

static uint32_t read_pvr(void)
{
	uint32_t pvr;
	__asm__ volatile ("mfspr %0, 287" : "=r"(pvr));
	return pvr;
}

static int dram_round_trip_test(void)
{
	volatile uint32_t *p = (volatile uint32_t *)0x00001000u;
	*p = 0xAAAA5555u;
	__asm__ volatile ("sync" ::: "memory");
	if (*p != 0xAAAA5555u)
		return 0;
	*p = 0x5555AAAAu;
	__asm__ volatile ("sync" ::: "memory");
	if (*p != 0x5555AAAAu)
		return 0;
	return 1;
}

void phase1_c_main(void)
{
	/* Hardware bring-up to light up UART1. */
	mv64361_enable_pci0_io_window();
	vt8231_enable_uart1();
	uart_init(UART1_BASE);

	uart_puts(UART1_BASE,
		  "\r\n\r\nPegasos II BIOS (clean-room rewrite) - Phase 1\n");

	uart_puts(UART1_BASE, "CPU PVR    = 0x");
	uart_put_hex32(UART1_BASE, read_pvr());
	uart_puts(UART1_BASE, "\n");

	uart_puts(UART1_BASE, "DRAM test  = ");
	uart_puts(UART1_BASE, dram_round_trip_test() ? "OK\n" : "FAIL\n");

	uart_puts(UART1_BASE,
		  "Console    = UART1 @ 0x");
	uart_put_hex32(UART1_BASE, UART1_BASE);
	uart_puts(UART1_BASE, " (VT8231 PCI1 0:0C.0)\n");

	uart_puts(UART1_BASE, "Stack      = 0x");
	uint32_t sp;
	__asm__ volatile ("mr %0, 1" : "=r"(sp));
	uart_put_hex32(UART1_BASE, sp);
	uart_puts(UART1_BASE, " (DRAM)\n");

	uart_puts(UART1_BASE, "Phase 1 complete. Halting.\n");

	for (;;)
		;
}
