/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Pegasos2 machdep implementation for the SmartFirmware OF runtime
 *  (upstream/smartfirmware/bin/of). Provides the machine_*, failsafe_*,
 *  u_sleep, dprintf, and get_msecs hooks that defs.h §machine-dependent
 *  interfaces declares.
 *
 *  This is Commit 2 of the multi-commit OF bring-up. The stubs here
 *  are deliberately minimal: enough to link, not yet enough to run.
 *  The `of-test` Makefile target partial-links the SF subset against
 *  this file and prints remaining undefined symbols -- those are the
 *  inputs for Commit 3 (expand the SF subset to satisfy them).
 *
 *  Notable decisions:
 *    * The memory pool starts at 0x00300000 (past our x86 emulator's
 *      1 MiB buffer at 0x00200000), 4 MiB long. Phase-1 stack at
 *      0x00100000 grows down, bss lives 0x00100000+, so 0x00300000+
 *      is safely unowned DRAM.
 *    * machine_initialize() does NOT memcpy SF handlers over our
 *      already-installed exception vectors (unlike bebox). Our 0x100..
 *      0x1300 table with panic_dump / decrementer / syscall trampoline
 *      is retained.
 *    * machine_probe_read/write do the access unconditionally for now;
 *      wiring them through a "probe active" flag in the DSI handler is
 *      a later commit (risk #6 from the planning doc).
 *    * NVRAM (M48T59) is not implemented. The machine_nvram_* hooks
 *      return errors so SF's nvram.c cleanly reports "nvram unavailable"
 *      rather than looping or crashing.
 */

#include <stdarg.h>

#include "defs.h"

/*
 * Our own hardware glue from outside the SF include path. machdep.c
 * is the bridge between SF's world (Environ, Retcode, Byte *) and our
 * bare-metal drivers.
 */
#include "../pegasos2.h"
#include "../uart16550.h"
#include "../io.h"

/* vbprintf lives in SF's stdlib.c; declared in its stdlib.h but not
 * via defs.h's include chain directly. Forward-declare locally. */
extern int vbprintf(char *buf, const char *fmt, va_list args);

/* _ms_tick_count is the decrementer-handler counter in exceptions.S. */
extern volatile uint32_t _ms_tick_count;

/* --------------------------------------------------------------- *
 *  Globals expected by memory.c to publish /memory node properties  *
 * --------------------------------------------------------------- */

/*
 * Memory layout on QEMU -m 512 (other sizes TBD):
 *
 *   0x00000000..0x001FFFFF  exception vectors, stack, .data, .bss
 *   0x00200000..0x002FFFFF  x86 emulator buffer (X86EMU_MEM_PADDR)
 *   0x00300000..0x006FFFFF  OF malloc pool (init_malloc backing)
 *   0x00700000..0x1FFFFFFF  OS-available DRAM (reported by /memory
 *                            through the available/claim allocator)
 *
 * g_machine_memory points at the malloc-pool start and also serves as
 * the base of the /memory node's "reg". g_machine_memory_size is
 * total /memory extent (pool + OS-available), and g_machine_memory_used
 * is the amount install_memory's memory-claim reserves -- i.e. the
 * malloc pool itself, so the OS free list excludes it.
 *
 * Value choices hard-coded to the 512 MiB QEMU DRAM until real HW
 * brings in a DDR-probe driver; machine_initialize() can be made
 * size-adaptive later.
 */
#define PEGASOS2_MEM_POOL_BASE   0x00300000u
#define PEGASOS2_DRAM_TOP        0x20000000u     /* QEMU -m 512 ceiling */
#define PEGASOS2_MEM_REPORT_SIZE (PEGASOS2_DRAM_TOP - PEGASOS2_MEM_POOL_BASE)

Byte *g_machine_memory         = NULL;
uInt  g_machine_memory_size    = 0;
uInt  g_machine_memory_offset  = 0;
uInt  g_machine_memory_used    = 0;

/* --------------------------------------------------------------- *
 *  failsafe_read / failsafe_write -- always-available UART console  *
 * --------------------------------------------------------------- */

/*
 * Write `len` bytes to UART1. Used by SF when the proper console
 * package is not yet installed, or as a fallback when DPRINTF is
 * enabled. Byte-at-a-time; the uart_putc helper in uart16550.c
 * polls the LSR for THRE, so this blocks the CPU but has no
 * concurrent-access hazard.
 */
Int
failsafe_write(Byte *buf, Int len)
{
	Int i;
	for (i = 0; i < len; i++)
		uart_putc(UART1_BASE, (char)buf[i]);
	return len;
}

/*
 * Read from UART1. We don't have a UART RX path yet (polled RX via
 * LSR[DR] is trivial but not wired; and an interrupt-driven RX needs
 * the ExtInt dispatcher which is deferred per PROGRESS.md). Return
 * zero-bytes-available so callers that want input exit cleanly.
 */
Int
failsafe_read(Byte *buf, Int len)
{
	(void)buf;
	(void)len;
	return 0;
}

/* --------------------------------------------------------------- *
 *  machine_initialize and friends                                   *
 * --------------------------------------------------------------- */

/*
 * Called first from main(). Must set up the malloc pool and leave
 * the system in a state where SF can allocate Environ, build the
 * device tree, etc.
 *
 * We intentionally do NOT:
 *   - init UART1 (already done by phase1_c_main before it hands off)
 *   - memcpy SF's DSI/alignment/timer handlers to 0x0300/0x0600/0x0500
 *     (we keep our panic_dump, decrementer, and syscall trampoline)
 *   - set MSR[ME]/[EE] (MSR[ME]=1 already set by reset.S; EE stays off
 *     until the ExtInt dispatcher lands)
 */
Retcode
machine_initialize(void)
{
	g_machine_memory        = (Byte *)PEGASOS2_MEM_POOL_BASE;
	g_machine_memory_size   = PEGASOS2_MEM_REPORT_SIZE;
	g_machine_memory_used   = MALLOC_POOL;
	g_machine_memory_offset = 0;

	return init_malloc(g_machine_memory, MALLOC_POOL);
}

/*
 * machine_init_args is NOT defined here -- SmartFirmware's nvram.c
 * provides a portable implementation that decodes a "-parameter value"
 * string vector into nvram overrides.  Redefining it would collide.
 */

/*
 * "reset-all" word target. `ba 0xFFF00100` jumps to the MPC7447 reset
 * entry point; the hardware re-runs reset.S which re-initialises
 * everything including a fresh malloc pool. Note: on real HW this
 * would want to issue a HRESET_HIGH via a hardware path first.
 */
Retcode
machine_reset_all(Environ *e)
{
	(void)e;
	__asm__ volatile ("ba 0xFFF00100");
	for (;;) /* unreachable */ ;
	return NO_ERROR;
}

/* LED / display-status tail. We don't have LEDs; echo on UART1. */
void
machine_led_write(Environ *e, Int n)
{
	(void)e;
	uart_puts(UART1_BASE, "LED=");
	uart_put_hex32(UART1_BASE, (uint32_t)n);
	uart_puts(UART1_BASE, "\n");
}

/* --------------------------------------------------------------- *
 *  probe / reg / unalign  --  memory probes from Forth           *
 * --------------------------------------------------------------- */

/*
 * Forth "probe-X" words. These should catch an access fault and
 * return FALSE without panicking. Today we do the access blindly:
 *   - if the address is real memory, the read/write succeeds
 *   - if the address is unmapped MMIO, QEMU logs "-d unimp" but
 *     does not raise a machine check (returns 0xFF..), so the
 *     caller sees a plausible-looking value.
 * Wiring probe-read/write through a "probe_active" flag that the
 * DSI/machine-check handler checks (to advance SRR0 and rfi) is
 * tracked in PROGRESS.md gotcha #6; a later commit lands it.
 */
Bool
machine_probe_read(Environ *e, Cell addr, Cell *value, int size)
{
	(void)e;
	switch (size) {
	case 1:  *value = *(volatile uChar  *)(uPtr)addr; break;
	case 2:  *value = *(volatile uShort *)(uPtr)addr; break;
	case 4:  *value = *(volatile uInt   *)(uPtr)addr; break;
#ifdef SF_64BIT
	case 8:  *value = *(volatile uLong  *)(uPtr)addr; break;
#endif
	default: return FALSE;
	}
	return TRUE;
}

Bool
machine_probe_write(Environ *e, Cell addr, Cell value, int size)
{
	(void)e;
	switch (size) {
	case 1:  *(volatile uChar  *)(uPtr)addr = (uChar) value; break;
	case 2:  *(volatile uShort *)(uPtr)addr = (uShort)value; break;
	case 4:  *(volatile uInt   *)(uPtr)addr = (uInt)  value; break;
#ifdef SF_64BIT
	case 8:  *(volatile uLong  *)(uPtr)addr = (uLong) value; break;
#endif
	default: return FALSE;
	}
	return TRUE;
}

/*
 * Translate a CPU-physical address into a PCI-bus address for the
 * IEEE-1275 "config-addr" handling path. On Pegasos2 our mem windows
 * are direct-mapped (CPU 0x80000000 == PCI1 mem-bus 0x80000000, etc.)
 * so the translation is identity. The "io" flag tells the caller
 * whether the address is in the I/O aperture; we return FALSE
 * (memory) for everything above 0xF0000000 except the PCI I/O
 * windows at 0xF8000000 (PCI0) and 0xFE000000 (PCI1).
 */
Bool
machine_pci_translate(Cell addr, Int *pci_addr, Bool *io)
{
	const uInt a = (uInt)addr;

	if ((a >= PCI1_IO_BASE && a < PCI1_IO_BASE + PCI1_IO_SIZE) ||
	    (a >= PCI0_IO_BASE && a < PCI0_IO_BASE + PCI0_IO_SIZE)) {
		*pci_addr = (Int)(a & 0x00FFFFFFu);  /* strip window base */
		*io = TRUE;
		return TRUE;
	}
	*pci_addr = (Int)a;
	*io = FALSE;
	return TRUE;
}

/* Big-endian native reg access.  MMIO helpers in io.h already do
 * sync barriers; just pick the right width. */
void
machine_reg_read(Environ *e, Cell addr, Cell *value, int size)
{
	(void)e;
	switch (size) {
	case 1:  *value = mmio_read8   ((uint32_t)addr); break;
	case 2:  *value = *(volatile uShort *)(uPtr)addr; break;
	case 4:  *value = *(volatile uInt   *)(uPtr)addr; break;
	default: *value = 0;
	}
}

void
machine_reg_write(Environ *e, Cell addr, Cell value, int size)
{
	(void)e;
	switch (size) {
	case 1:  *(volatile uChar  *)(uPtr)addr = (uChar) value; break;
	case 2:  *(volatile uShort *)(uPtr)addr = (uShort)value; break;
	case 4:  *(volatile uInt   *)(uPtr)addr = (uInt)  value; break;
	default: break;
	}
}

/*
 * Unaligned access. PowerPC does NOT trap on unaligned loads for most
 * integer ops; the alignment exception fires only on lwarx/stwcx,
 * stmw, stswi, and similar. For normal lwz/stw on a 7447, the CPU
 * does the access in two bus cycles silently. So a simple byte-by-byte
 * copy is correct; the compiler will NOT inline it into a single
 * load and bypass the intent.
 */
void
machine_unalign_read(Environ *e, Cell addr, Cell *value, int size)
{
	(void)e;
	volatile uByte *p = (volatile uByte *)(uPtr)addr;
	uInt v = 0;
	int i;
	for (i = 0; i < size; i++)
		v = (v << 8) | p[i];     /* big-endian assembly */
	*value = v;
}

void
machine_unalign_write(Environ *e, Cell addr, Cell value, int size)
{
	(void)e;
	volatile uByte *p = (volatile uByte *)(uPtr)addr;
	int i;
	for (i = size - 1; i >= 0; i--) {
		p[i] = (uByte)value;
		value >>= 8;
	}
}

/* --------------------------------------------------------------- *
 *  time + memory tests + diag                                       *
 * --------------------------------------------------------------- */

void
machine_gettime(Environ *e, Time_value *tv)
{
	(void)e;
	/*
	 * 32-bit arithmetic only: PPC's divwu covers this natively.
	 * If we upcast _ms_tick_count to uLong (64-bit under __LONGLONG)
	 * the compiler emits __udivdi3 / __umoddi3 which live in libgcc,
	 * and our bare-metal link is -nostdlib. The tick counter wraps
	 * every ~49 days; sec overflow at 2^32 / 1000 s == ~1600 hours.
	 */
	uInt ms  = (uInt)_ms_tick_count;
	tv->sec  = ms / 1000u;
	tv->nsec = (ms % 1000u) * 1000000u;
}

/*
 * Memory test for the Forth "test-memory" word. We have a simpler
 * stuck-bit test in phase1's DRAM check; promote that concept here.
 * `mask` tells which bits to exercise; `diag` toggles verbose
 * reporting via test_begin/pass/fail callbacks.
 */
Bool
machine_memory_test(Environ *e, Cell addr, Cell len, Cell mask, Bool diag)
{
	(void)e; (void)diag;
	volatile uByte *p = (volatile uByte *)(uPtr)addr;
	Cell i;
	const uByte m = (uByte)(mask ? mask : 0xFFu);

	for (i = 0; i < len; i++) {
		p[i] = (uByte)(i & m);
	}
	for (i = 0; i < len; i++) {
		if ((p[i] & m) != (uByte)(i & m))
			return FALSE;
	}
	return TRUE;
}

Bool machine_diag_switch(Environ *e)          { (void)e; return FALSE; }
void machine_test_begin  (Environ *e)         { (void)e; }
void machine_test_pass   (Environ *e)         { (void)e; }
void machine_test_fail   (Environ *e)         { (void)e; }

/* --------------------------------------------------------------- *
 *  NVRAM (M48T59) -- not implemented yet                            *
 * --------------------------------------------------------------- */

uInt
machine_nvram_size(Environ *e)
{
	(void)e;
	/* 0 signals "no NVRAM" to SF's nvram.c, which then treats every
	 * getenv as "not found" and every setenv as a no-op. Once we
	 * write an M48T59 driver, return the real 8 KiB. */
	return 0u;
}

Retcode
machine_nvram_read(Environ *e, uChar *buf, uInt *len)
{
	(void)e; (void)buf;
	if (len) *len = 0;
	return E_NO_DEVICE;
}

Retcode
machine_nvram_write(Environ *e, uChar *buf, uInt len)
{
	(void)e; (void)buf; (void)len;
	return E_NO_DEVICE;
}

/* --------------------------------------------------------------- *
 *  debugging + sleeping + time API for C drivers                    *
 * --------------------------------------------------------------- */

/*
 * dprintf() is the DPRINTF macro's expansion target when DEBUG is
 * on. Format + go to UART1. We use SF's vbprintf into a local buffer
 * (also provided by SF's stdlib.c) so no hosted <stdio.h>.
 */
void
dprintf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	Int i;

	/*
	 * vbprintf's return value is unreliable: its last line is
	 * `return strlen(buf);` where `buf` has been advanced through
	 * the formatting loop, so it points at the NUL terminator and
	 * strlen always returns 0.  SF's own cprintf / bprintf / etc.
	 * workarounds for this by walking buf themselves until '\0'.
	 * We do the same: NUL-terminate defensively before the call
	 * (vbprintf also writes a NUL on exit), then walk buf for the
	 * true byte count.
	 */
	buf[0] = '\0';
	va_start(ap, fmt);
	(void)vbprintf(buf, fmt, ap);
	va_end(ap);

	for (i = 0; i < (Int)sizeof buf && buf[i]; i++)
		;

	if (i > 0)
		failsafe_write((Byte *)buf, i);
}

/*
 * u_sleep: micro-seconds. Implemented as a busy-spin with a
 * tick-granularity lower bound. Our decrementer fires on an interval
 * of ~TB_TICKS_PER_MS ticks, so sub-millisecond sleeps round up to
 * one tick. Good enough for driver settling delays; refine when a
 * microsecond-resolution time source exists.
 */
void
u_sleep(uInt us)
{
	uInt ms = (us + 999u) / 1000u;
	uInt start = _ms_tick_count;
	while ((_ms_tick_count - start) < ms)
		/* busy-spin */ ;
}

/*
 * get_msecs(Environ *) is NOT defined here -- SF's other.c provides a
 * portable implementation on top of machine_gettime().  Redefining it
 * would collide at link time.  Our own 32-bit get_msecs(void) in
 * timer.c has a different signature; the name still collides, tracked
 * as gotcha #12 in PROGRESS.md.
 */

/* Client-interface callback hook; spec 06 wires this later. */
int
machine_callback(Environ *e, Callback *func, Cell *array)
{
	(void)e; (void)func; (void)array;
	return (int)E_NO_CALLBACK;
}
