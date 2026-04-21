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
#include "pci_walker.h"
#include "x86_glue.h"
#include "x86emu.h"

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

	uart_puts(UART1_BASE, "\nPCI enumeration:\n");
	pci_walk();

	/*
	 * Synthetic x86 self-test: hand-assemble a tiny real-mode
	 * program at CS:IP = 0x1000:0x0000 that sets AX=0x1234,
	 * DX=0x5678, HLT. Running it proves the vendored x86emu is
	 * alive, that our sys glue and memory buffer are correctly
	 * set up, and that we can read registers back out.
	 */
	uart_puts(UART1_BASE, "\nx86 emulator self-test:\n");
	x86_glue_init();

	static const uint8_t test_prog[] = {
		0xB8, 0x34, 0x12,    /* MOV AX, 0x1234 */
		0xBA, 0x78, 0x56,    /* MOV DX, 0x5678 */
		0xF4                 /* HLT            */
	};
	uint8_t *dst = x86emu_mem(((uint32_t)0x1000 << 4) + 0);
	for (unsigned i = 0; i < sizeof test_prog; i++)
		dst[i] = test_prog[i];

	M.x86.R_CS = 0x1000;
	M.x86.R_IP = 0x0000;
	M.x86.R_AX = 0;
	M.x86.R_DX = 0;

	x86emu_run();

	uart_puts(UART1_BASE, "  AX = 0x");
	uart_put_hex16(UART1_BASE, M.x86.R_AX);
	uart_puts(UART1_BASE, "  (expect 0x1234)\n");
	uart_puts(UART1_BASE, "  DX = 0x");
	uart_put_hex16(UART1_BASE, M.x86.R_DX);
	uart_puts(UART1_BASE, "  (expect 0x5678)\n");
	uart_puts(UART1_BASE,
		  (M.x86.R_AX == 0x1234 && M.x86.R_DX == 0x5678)
		  ? "  x86emu self-test: OK\n"
		  : "  x86emu self-test: FAIL\n");

	uart_puts(UART1_BASE, "\nPhase 1 complete. Halting.\n");

	for (;;)
		;
}
