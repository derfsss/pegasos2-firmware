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
 *    * The memory pool starts at 0x00200000 (right after firmware
 *      bss/stack at 0x00100000..0x001FFFFF), 2 MiB long so it ends
 *      at 0x003FFFFF -- spec 07 §Load-address compliant, leaving
 *      0x00400000+ clear for OS kernel loads. The x86 emulator
 *      buffer moved to 0x01000000 (16 MiB mark) to free this range.
 *    * machine_initialize() does NOT memcpy SF handlers over our
 *      already-installed exception vectors (unlike bebox). Our 0x100..
 *      0x1300 table with panic_dump / decrementer / syscall trampoline
 *      is retained.
 *    * machine_probe_read/write do the access unconditionally for now;
 *      wiring them through a "probe active" flag in the DSI handler is
 *      a later commit (risk #6 from the planning doc).
 *    * NVRAM (M48T59) driver is wired in. The machine_nvram_* hooks
 *      expose the 1 KiB "system partition" from spec 08 (M48T59 offsets
 *      0x0200..0x05FF) to SF. Firmware-private state, the OS-specific
 *      partition, and RTC access are deferred to later commits.
 *
 *      QEMU caveat: pegasos2 in QEMU 10.2 does NOT instantiate an
 *      M48T59 (see hw/ppc/pegasos.c -- only a VT8231 built-in RTC +
 *      RTAS NVRAM hypercalls for guest Linux). Our reads land on
 *      unmapped VT8231 I/O space and return 0, so the magic check
 *      fails and SF falls back to compile-time defaults every boot
 *      -- same observable behaviour as the pre-driver stubs. The
 *      code is correct per spec 08 for real Pegasos II hardware.
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
#include "../m48t59.h"
#include "../extint.h"

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
 *   0x00200000..0x003FFFFF  OF malloc pool (init_malloc backing, 2 MiB;
 *                            spec 07 §Load-address compliant)
 *   0x00400000..            default OS kernel load area per spec 07
 *   0x01000000..0x010FFFFF  x86 emulator buffer (X86EMU_MEM_PADDR);
 *                            only active during phase1, OS may reuse
 *   rest of DRAM..0x1FFFFFFF  OS-available DRAM (reported by /memory
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
/*
 * Malloc pool base. Historically parked at 0x00200000 (the first
 * 2 MiB mark) per docs/07 §Load-address, which reasons about the
 * AmigaOS kernel proper loading at 0x00400000. That layout was
 * wrong for the real world: the AOS bootstrap `amigaboot.of` is
 * itself an ELF linked at 0x00200000 -- it loads at the SAME
 * address our pool used to sit at, and its PT_LOAD stomps the
 * first ~60 KiB of SF's heap (Forth dict, device-tree nodes,
 * caller stacks). The first CI callback from the just-loaded
 * amigaboot.of then walks a corrupted dict and faults.
 *
 * Moving the pool past every conventional OS load region avoids
 * the collision entirely:
 *     0x00200000..0x003FFFFF  free for amigaboot.of + bootstraps
 *     0x00400000..0x00FFFFFF  free for the AOS kernel / Linux
 *     0x01000000..0x010FFFFF  x86emu buffer (Boot 4/N+2)
 *     0x01100000..0x012FFFFF  <-- SF malloc pool (2 MiB here)
 *
 * heap-info's spec-07 verdict check was updated in lockstep; the
 * window it asserts is now 0x01100000..0x012FFFFF.
 */
#define PEGASOS2_MEM_POOL_BASE   0x01100000u
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
 * Read up to `len` bytes from UART1 without blocking.
 *
 * SmartFirmware's "serial" console contract (cmdio.c:468-471) is:
 *   "input devices may return -2 if no input is pending
 *    the read call should always be non blocking
 *    ref. Annex A, 'serial'"
 *
 * get_key() loops on this read until a byte arrives, so blocking
 * semantics at the driver level would deadlock any call site that
 * only wanted a non-blocking poll -- notably key_down(), called
 * from display_text() whenever e->paginate is TRUE. That path fires
 * on every banner line and would freeze the banner at its first
 * cprintf with a file-backed serial port (no keys arriving).
 *
 * So we just drain whatever the UART FIFO holds and return. If
 * empty, return 0; SF's f_failsafe_read translates that to -2 on
 * the Forth stack, and get_key loops until the next poll finds a
 * byte.
 */
Int
failsafe_read(Byte *buf, Int len)
{
	Int n = 0;
	int c;

	if (len <= 0 || buf == NULL)
		return 0;

	/* Primary path: interrupt-fed ring populated by uart_rx_handler
	 * in extint.c. With extint_uart_install + MSR[EE]=1 every
	 * keystroke reaches the ring before SF even gets here. */
	while (n < len && (c = extint_uart_pop()) >= 0)
		buf[n++] = (Byte)c;

	/* Fallback: direct poll. Covers an unexpected config where
	 * interrupts are disabled or the cascade was never armed --
	 * cheap, and keeps the contract working in every state. */
	while (n < len && (c = uart_poll_rx(UART1_BASE)) >= 0)
		buf[n++] = (Byte)c;

	return n;
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
 *  NVRAM (M48T59) -- system partition (spec 08 §NVRAM partitioning) *
 * --------------------------------------------------------------- */

/*
 * SF's nvram.c wraps every stored OF environment variable in a
 * 6-byte header:
 *     [0..1]  magic 0xBE 0xEF
 *     [2..3]  payload length, big-endian (bytes after the header,
 *             excluding the trailing NUL)
 *     [4..5]  two's-complement checksum of the first 6+payload bytes
 *             (the payload sum plus these two bytes must equal 0
 *             modulo 256 per byte lane, i.e. the running 16-bit sum
 *             of every byte must equal 0 mod 0x10000).
 * After the header comes the ASCII "name=value" blob and a trailing
 * NUL. nvram.c's NVRAM_HEADER is 6, NVRAM_MAX is g_nvram_size - 8.
 *
 * machine_nvram_size reports the window we expose to SF: the 1 KiB
 * system partition per spec 08 (M48T59 offsets 0x0200..0x05FF).
 *
 * machine_nvram_read loads header + payload + trailing NUL into `buf`
 * and writes the total byte count to `*len`. SF subtracts NVRAM_HEADER
 * + 1 to recover the useful payload length. If the magic is missing
 * or the checksum fails, return any non-NO_ERROR retcode; SF's
 * load_nvram() then prints "NVRAM is corrupt, reseting to default
 * values" and proceeds with compile-time defaults.
 *
 * machine_nvram_write stores the prepared buffer verbatim. SF writes
 * at most NVRAM_HEADER + payload + 1 bytes; bounds-check here so a
 * corrupted caller can't scribble past the partition.
 */

uInt
machine_nvram_size(Environ *e)
{
	(void)e;
	return M48T59_SYSTEM_SIZE;
}

Retcode
machine_nvram_read(Environ *e, uChar *buf, uInt *len)
{
	(void)e;

	if (buf == NULL || len == NULL)
		return E_NO_DEVICE;

	*len = 0;

	/* Peek the header to determine the payload length before reading
	 * the rest. Lets us return early on a fresh / corrupt NVRAM
	 * without slurping 1 KiB of 0xFF. */
	uChar hdr0 = m48t59_read_byte(M48T59_SYSTEM_OFFSET + 0);
	uChar hdr1 = m48t59_read_byte(M48T59_SYSTEM_OFFSET + 1);

	if (hdr0 != 0xBEu || hdr1 != 0xEFu)
		return E_NO_DEVICE;

	uChar hdr2 = m48t59_read_byte(M48T59_SYSTEM_OFFSET + 2);
	uChar hdr3 = m48t59_read_byte(M48T59_SYSTEM_OFFSET + 3);
	uInt  payload_len = ((uInt)hdr2 << 8) | (uInt)hdr3;

	/* Total bytes SF expects: 6 header + payload + 1 trailing NUL. */
	uInt total = 6u + payload_len + 1u;

	if (total > M48T59_SYSTEM_SIZE)
		return E_NO_DEVICE;

	for (uInt i = 0; i < total; i++)
		buf[i] = m48t59_read_byte(M48T59_SYSTEM_OFFSET + i);

	/* Validate checksum: sum of all header+payload bytes (excluding
	 * the trailing NUL) should fold to zero in the low 16 bits. The
	 * two checksum bytes were set to -sum at write time. */
	uInt sum = 0;
	for (uInt i = 0; i < 6u + payload_len; i++)
		sum += buf[i];

	if ((sum & 0xFFFFu) != 0u)
		return E_NO_DEVICE;

	*len = total;
	return NO_ERROR;
}

Retcode
machine_nvram_write(Environ *e, uChar *buf, uInt len)
{
	(void)e;

	if (buf == NULL || len == 0u)
		return E_NO_DEVICE;

	if (len > M48T59_SYSTEM_SIZE)
		return E_OUT_OF_PROM;

	for (uInt i = 0; i < len; i++)
		m48t59_write_byte(M48T59_SYSTEM_OFFSET + i, buf[i]);

	return NO_ERROR;
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
