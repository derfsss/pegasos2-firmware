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
#include "io.h"
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

	/*
	 * Load and execute the bochs-VGA Option ROM (Bug 1 target).
	 *
	 * QEMU's pegasos2 has bochs-VGA at PCI1 00:01.0 with a 64 KiB
	 * Option ROM exposed via config-offset 0x30. We assign it a
	 * PCI address inside the PCI1 mem0 window (CPU 0x80000000 is
	 * a direct-mapped alias), enable mem-decode + ROM-decode,
	 * then copy the ROM to emulator memory at 0xC0000 and run
	 * from CS:IP = 0xC000:0x0003 per Option-ROM POST convention.
	 */
	uart_puts(UART1_BASE, "\nOption ROM execution (bochs-VGA):\n");

	/* Enable memory decode on the VGA device (was only set for
	 * I/O + MASTER earlier). */
	pci_cfg_write32(1, 0, 1, 0, 0x04, 0x00000007u);

	/* Park the ROM at PCI mem 0x80000000 and enable its decode
	 * (bit 0 of ROM BAR). Address also == CPU physical 0x80000000. */
	pci_cfg_write32(1, 0, 1, 0, 0x30, 0x80000001u);

	/* Verify signature at CPU 0x80000000. */
	uint8_t s0 = mmio_read8(0x80000000u);
	uint8_t s1 = mmio_read8(0x80000001u);
	uint8_t size_units = mmio_read8(0x80000002u);
	uart_puts(UART1_BASE, "  ROM signature = 0x");
	uart_put_hex8(UART1_BASE, s0);
	uart_putc(UART1_BASE, ' ');
	uart_put_hex8(UART1_BASE, s1);
	uart_puts(UART1_BASE, "  size units = 0x");
	uart_put_hex8(UART1_BASE, size_units);
	uart_puts(UART1_BASE, "\n");

	if (s0 != 0x55 || s1 != 0xAA) {
		uart_puts(UART1_BASE, "  ROM signature invalid -- skipping execution\n");
	} else {
		uint32_t rom_bytes = (uint32_t)size_units * 512u;
		if (rom_bytes == 0 || rom_bytes > 0x10000u)
			rom_bytes = 0x10000u;
		uart_puts(UART1_BASE, "  copying 0x");
		uart_put_hex32(UART1_BASE, rom_bytes);
		uart_puts(UART1_BASE, " bytes to emulator 0xC0000\n");

		uint8_t *emu_rom = x86emu_mem(0xC0000u);
		for (uint32_t i = 0; i < rom_bytes; i++)
			emu_rom[i] = mmio_read8(0x80000000u + i);

		/* Option-ROM POST entry per PnP spec:
		 *   AH = ?, AL = bus number (unused by legacy ROMs)
		 *   CS:IP = 0xC000:0x0003
		 *   DS = 0x0040 (BDA seg), ES = 0
		 *   Stack provided by the caller; we use SS:SP = 0x0000:0xFFF0.
		 * Legacy VGA VBIOS does not need a PCI BIOS call trap chain;
		 * it just POSTs and RETFs back. We watch for HLT or cycle-
		 * limit exit.
		 */
		M.x86.R_CS = 0xC000;
		M.x86.R_IP = 0x0003;
		M.x86.R_SS = 0x0000;
		M.x86.R_SP = 0xFFF0;
		M.x86.R_DS = 0x0040;
		M.x86.R_ES = 0x0000;
		M.x86.R_AX = 0x0000;
		M.x86.R_BX = 0x0000;
		M.x86.R_CX = 0x0000;
		M.x86.R_DX = 0x0000;

		/* Seed top-of-stack return target: when the ROM's init
		 * code executes RETF, pop CS:IP = 0x0050:0x0000, which
		 * we pre-load with a single HLT (0xF4). That exits the
		 * emulator cleanly. */
		uint8_t *hlt_landing = x86emu_mem(0x00500u);
		hlt_landing[0] = 0xF4;
		uint8_t *stack_top = x86emu_mem(0x0FFF0u);
		/* LE: IP first, then CS, as pushed by a FAR CALL. */
		stack_top[0] = 0x00; stack_top[1] = 0x00;  /* return IP = 0 */
		stack_top[2] = 0x50; stack_top[3] = 0x00;  /* return CS = 0x0050 */

		uart_puts(UART1_BASE, "  running ROM POST...\n");
		x86emu_run();

		uart_puts(UART1_BASE, "  POST returned. AX=0x");
		uart_put_hex16(UART1_BASE, M.x86.R_AX);
		uart_puts(UART1_BASE, " final CS:IP=0x");
		uart_put_hex16(UART1_BASE, M.x86.R_CS);
		uart_putc(UART1_BASE, ':');
		uart_put_hex16(UART1_BASE, M.x86.R_IP);
		uart_puts(UART1_BASE, "\n");
	}

	uart_puts(UART1_BASE, "\nPhase 1 complete. Halting.\n");

	for (;;)
		;
}
