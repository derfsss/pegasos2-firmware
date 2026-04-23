/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Phase-1 C entry point. Called by reset.S after it has:
 *    - copied .data/.bss and initialised the stack in DRAM
 *    - copied the exception-vector table to 0x00000000..0x00002000
 *    - cleared MSR[IP] so faults route to 0x00000100..0x00001300
 *  Still runs in real mode (MSR[IR]=MSR[DR]=0) with no caches, no
 *  FP, no external interrupts. Brings up UART1, emits a banner and
 *  diagnostic output, runs the Phase-1 self-tests, and halts.
 */

#include <stdint.h>
#include "pegasos2.h"
#include "uart16550.h"
#include "mv64361.h"
#include "vt8231.h"
#include "pci.h"
#include "pci_walker.h"
#include "timer.h"
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

	/*
	 * By the time we get here, reset.S has already copied the
	 * exception vectors to 0x00000000 and cleared MSR[IP]. Any
	 * fault from now on lands in panic_dump.
	 */
	uart_puts(UART1_BASE, "Exceptions = installed at 0x00000100..0x00001300, MSR[IP]=0\n");

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
		0xB8, 0x42, 0x42,    /* MOV AX, 0x4242  (poison; overwritten below) */
		0x0F, 0xFE, 0xC0,    /* PADDB MM0, MM0  (spec 09 Bug 1 0F FE)       */
		0xB8, 0x34, 0x12,    /* MOV AX, 0x1234  (reachable iff 0F FE works) */
		0xBA, 0x78, 0x56,    /* MOV DX, 0x5678                              */
		0xF4                 /* HLT                                         */
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
	 * The PCI walker has already assigned this device's BARs and
	 * the expansion-ROM BAR, and enabled MEM+IO+MASTER in the
	 * command register. We just read the ROM BAR back from config
	 * space to find where it landed and copy from there. PCI1
	 * mem0 is direct-mapped on QEMU so the PCI-side base is also
	 * the CPU physical address.
	 */
	uart_puts(UART1_BASE, "\nOption ROM execution (bochs-VGA):\n");

	uint32_t rom_bar = pci_cfg_read32(1, 0, 1, 0, 0x30);
	uint32_t rom_addr = rom_bar & 0xFFFFF800u;
	if (!(rom_bar & PCI_ROM_BAR_ENABLE)) {
		uart_puts(UART1_BASE, "  ROM BAR not enabled -- walker skipped it\n");
	}

	uart_puts(UART1_BASE, "  ROM BAR    = 0x");
	uart_put_hex32(UART1_BASE, rom_bar);
	uart_puts(UART1_BASE, " (addr=0x");
	uart_put_hex32(UART1_BASE, rom_addr);
	uart_puts(UART1_BASE, ")\n");

	/* Verify signature at the ROM BAR's CPU-visible address. */
	uint8_t s0 = mmio_read8(rom_addr);
	uint8_t s1 = mmio_read8(rom_addr + 1);
	uint8_t size_units = mmio_read8(rom_addr + 2);
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
			emu_rom[i] = mmio_read8(rom_addr + i);

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

	/*
	 * Seed _dec_reload from Pegasos II board defaults before any
	 * interrupt can fire. Real-HW path will replace this with a
	 * W83194 SMBus probe; on QEMU the value is not wall-clock
	 * accurate but the 0x900 handler still re-arms with it and
	 * the tick counter advances monotonically.
	 */
	uart_puts(UART1_BASE, "\nClock calibration (Pegasos II defaults, W83194 probe TBD):\n");
	timer_calibrate();
	uart_puts(UART1_BASE, "  FSB = ");
	uart_put_hex32(UART1_BASE, timer_fsb_hz());
	uart_puts(UART1_BASE, " Hz  TB = ");
	uart_put_hex32(UART1_BASE, timer_tb_hz());
	uart_puts(UART1_BASE, " Hz  DEC reload = ");
	uart_put_hex32(UART1_BASE, timer_ms_reload());
	uart_puts(UART1_BASE, " ticks/ms\n");

	/*
	 * Decrementer self-test. Arm SPR 22 with one reload's worth,
	 * enable MSR[EE] briefly, busy-spin long enough for several
	 * fires, disable MSR[EE] again, and report the accumulated
	 * tick count. A non-zero delta proves the 0x900 vector runs
	 * and rfi's back cleanly. MSR[EE] is disabled again before
	 * the final halt so any latched ExtInt does not land in the
	 * panic stub at 0x500.
	 */
	uart_puts(UART1_BASE, "\nDecrementer test:\n");
	uart_puts(UART1_BASE, "  before spin: 0x");
	uart_put_hex32(UART1_BASE, pegasos2_get_msecs_ticks());
	uart_puts(UART1_BASE, "\n");

	timer_arm(timer_ms_reload());
	enable_ei();
	for (volatile uint32_t i = 0; i < 1000000u; i++)
		;
	disable_ei();

	uint32_t ticks = pegasos2_get_msecs_ticks();
	uart_puts(UART1_BASE, "  after  spin: 0x");
	uart_put_hex32(UART1_BASE, ticks);
	uart_puts(UART1_BASE, "\n");
	uart_puts(UART1_BASE,
		  ticks > 0 ? "  decrementer handler: OK\n"
			    : "  decrementer handler: FAIL (no ticks)\n");

	/*
	 * Syscall round-trip. Put a sentinel in r3, issue `sc`, read
	 * r3 back. The 0xC00 trampoline saves caller state, invokes
	 * syscall_dispatch which overwrites the saved r3 with 0xBABE,
	 * and rfi's us to the instruction after `sc`. Observing 0xBABE
	 * proves the save-dispatch-restore-rfi pipeline works end-to-end
	 * and that register modifications by the C dispatcher propagate
	 * back to the caller.
	 */
	uart_puts(UART1_BASE, "\nSyscall test:\n");
	uint32_t sc_result;
	__asm__ volatile (
		"li   3, 0x1337\n\t"
		"sc\n\t"
		"mr   %0, 3\n\t"
		: "=r"(sc_result)
		:
		: "r3", "lr", "ctr", "xer", "cc", "memory"
	);
	uart_puts(UART1_BASE, "  r3 after sc = 0x");
	uart_put_hex32(UART1_BASE, sc_result);
	uart_puts(UART1_BASE,
		  sc_result == 0x0000BABEu ? "  (expected 0xBABE) OK\n"
					   : "  (expected 0xBABE) FAIL\n");

	/*
	 * ExtInt preflight: read the MV64361 IC state at boot and print
	 * the raw values. All four registers should be 0 right after
	 * reset (no pending causes, all CPU-route masks cleared). Any
	 * other value would mean either QEMU set spurious bits or our
	 * register offsets are wrong.
	 */
	uart_puts(UART1_BASE, "\nExtInt preflight (MV64361 IC):\n");
	uart_puts(UART1_BASE, "  main cause L/H = 0x");
	uart_put_hex32(UART1_BASE, mv64361_read32(MV_IC_MAIN_CAUSE_LOW));
	uart_puts(UART1_BASE, " / 0x");
	uart_put_hex32(UART1_BASE, mv64361_read32(MV_IC_MAIN_CAUSE_HIGH));
	uart_puts(UART1_BASE, "\n  cpu0 mask L/H  = 0x");
	uart_put_hex32(UART1_BASE, mv64361_read32(MV_IC_CPU0_MASK_LOW));
	uart_puts(UART1_BASE, " / 0x");
	uart_put_hex32(UART1_BASE, mv64361_read32(MV_IC_CPU0_MASK_HIGH));
	uart_puts(UART1_BASE, "\n  gpp cause/mask = 0x");
	uart_put_hex32(UART1_BASE, mv64361_read32(MV_GPP_INT_CAUSE));
	uart_puts(UART1_BASE, " / 0x");
	uart_put_hex32(UART1_BASE, mv64361_read32(MV_GPP_INT_MASK0));
	uart_puts(UART1_BASE, "\n");

#ifdef EXCEPTION_TEST
	/*
	 * Compile-time-gated exception self-test. Builds with
	 * -DEXCEPTION_TEST=1 trigger a deliberate trap via
	 * `twi 31, r0, 0` (unconditional trap) to verify the
	 * Program-exception (0x700) path reaches panic_dump. Not enabled
	 * in the default build -- the default build must halt cleanly.
	 */
	uart_puts(UART1_BASE, "\nEXCEPTION_TEST: triggering `twi 31, r0, 0` (should panic)...\n");
	__asm__ volatile ("twi 31, 0, 0");
	uart_puts(UART1_BASE, "(!!! test returned from trap -- should not happen)\n");
#endif

	/*
	 * Hand off to the SmartFirmware OF runtime.  Its main() calls
	 * machine_initialize() (our machdep/pegasos2/of/machdep.c) to
	 * set up the malloc pool, then builds the Environ, runs the
	 * default startup script (probe-all / install-console / banner),
	 * and enters the interpret() read-eval loop.
	 *
	 * If main() ever returns (e.g. SF decides to exit), we fall
	 * through to an infinite loop rather than returning to the
	 * caller -- reset.S did not set up a back-chain for us.
	 */
	extern int main(int argc, char **argv);
	(void)main(0, (char **)0);

	uart_puts(UART1_BASE, "\n(OF main() returned -- halting)\n");
	for (;;)
		;
}
