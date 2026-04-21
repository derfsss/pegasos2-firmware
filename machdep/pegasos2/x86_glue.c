/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Pegasos2-specific wiring for the vendored x86 realmode emulator.
 *
 *  Memory model:
 *     A single 1 MiB buffer in DRAM at X86EMU_MEM_PADDR models the
 *     entire real-mode address space (0x00000..0xFFFFF). The
 *     emulator's M.mem_base points to it so the default sys_rdX
 *     and sys_wrX helpers in upstream/x86emu/x86emu/sys.c can index
 *     it directly without indirection. We do NOT override those
 *     callbacks yet -- they access M.mem_base[addr] which is exactly
 *     what we want.
 *
 *  Port I/O:
 *     The x86 world the Option ROM expects is PC-compatible ISA
 *     legacy I/O (VGA ports 0x3C0..0x3DF, CRTC 0x3D4/5, etc.). On
 *     Pegasos2 that maps onto PCI1 I/O at CPU 0xFE000000. Our
 *     handlers forward x86 port numbers 1:1 to CPU physical
 *     0xFE000000 + port.
 *
 *  BDA / IVT:
 *     Spec 09 requires BIOS Data Area at 0x400..0x4FF zeroed, with
 *     BDA+0x485 = 16 (character-cell height). IVT slots 0x00..0x1F
 *     must point at IRET stubs so an Option ROM that fires an
 *     unexpected interrupt returns cleanly. Both are programmed in
 *     x86_glue_init().
 */

#include <stdint.h>
#include <stddef.h>

#include "x86_glue.h"
#include "io.h"
#include "pegasos2.h"
#include "uart16550.h"

#include "x86emu.h"

uint8_t *x86emu_mem(uint32_t x86_addr)
{
	return (uint8_t *)(X86EMU_MEM_PADDR + (x86_addr & 0xFFFFFu));
}

/* ---------- port I/O routing --------------------------------- */
/*
 * The x86 world expects little-endian byte order on I/O ports; PCI
 * I/O is also little-endian. Our byte accessor uses plain mmio_read8
 * (byte reads are endian-invariant); our word/long accessors must
 * use the byte-reversed helpers so a 16-bit OUT becomes an LE-32 on
 * the PCI-I/O-side bus.
 */

#define X86_PIO_PADDR(port)  (PCI1_IO_BASE + ((port) & 0xFFFFu))

static uint8_t sys_inb_pegasos2(X86EMU_pioAddr port)
{
	return mmio_read8(X86_PIO_PADDR(port));
}

static uint16_t sys_inw_pegasos2(X86EMU_pioAddr port)
{
	uint32_t a = X86_PIO_PADDR(port);
	uint16_t lo = mmio_read8(a);
	uint16_t hi = mmio_read8(a + 1);
	return (uint16_t)(lo | (hi << 8));
}

static uint32_t sys_inl_pegasos2(X86EMU_pioAddr port)
{
	uint32_t a = X86_PIO_PADDR(port);
	uint32_t v = 0;
	v |= (uint32_t)mmio_read8(a    );
	v |= (uint32_t)mmio_read8(a + 1) << 8;
	v |= (uint32_t)mmio_read8(a + 2) << 16;
	v |= (uint32_t)mmio_read8(a + 3) << 24;
	return v;
}

static void sys_outb_pegasos2(X86EMU_pioAddr port, uint8_t val)
{
	mmio_write8(X86_PIO_PADDR(port), val);
}

static void sys_outw_pegasos2(X86EMU_pioAddr port, uint16_t val)
{
	uint32_t a = X86_PIO_PADDR(port);
	mmio_write8(a,     (uint8_t)(val & 0xFF));
	mmio_write8(a + 1, (uint8_t)(val >> 8));
}

static void sys_outl_pegasos2(X86EMU_pioAddr port, uint32_t val)
{
	uint32_t a = X86_PIO_PADDR(port);
	mmio_write8(a,     (uint8_t)(val      ));
	mmio_write8(a + 1, (uint8_t)(val >>  8));
	mmio_write8(a + 2, (uint8_t)(val >> 16));
	mmio_write8(a + 3, (uint8_t)(val >> 24));
}

/* ---------- BDA and IVT initialisation ---------------------- */
/*
 * Docs 09 Bug 1 "Required behaviour":
 *   BDA zeroed, BDA+0x485 = 16 (character-cell height). Option ROMs
 *   read that byte and treat garbage as "bad font", bailing.
 *
 * IVT is the first 1 KiB of real-mode memory -- 256 vectors, 4 bytes
 * each (seg:off). Slots 0x00..0x1F are required to point at stubs
 * that return cleanly (CF=0, IRET). We park IRET stubs in an unused
 * high corner of the 1 MiB buffer (segment 0xF000) and seed all 32
 * slots with the same stub address.
 */

static void bda_init(void)
{
	uint8_t *bda = x86emu_mem(0x400);
	for (uint32_t i = 0; i < 0x100; i++)
		bda[i] = 0;
	bda[0x485 - 0x400] = 16;   /* character-cell height */
}

static void ivt_init(void)
{
	/* Put IRET stubs at 0xF000:0x1000 (physical 0xF1000) -- a
	 * 256-byte pad area unlikely to be clobbered by an Option ROM
	 * that loads at 0xC0000..0xC7FFF. */
	enum { STUB_SEG = 0xF000u, STUB_OFF_BASE = 0x1000u };

	uint8_t *ivt = x86emu_mem(0x000);
	uint32_t i;
	for (i = 0; i < 0x20; i++) {
		uint16_t off = (uint16_t)(STUB_OFF_BASE + i);
		uint8_t *stub = x86emu_mem(((uint32_t)STUB_SEG << 4) + off);
		stub[0] = 0xCF;    /* IRET */

		ivt[i * 4 + 0] = (uint8_t)(off        & 0xFF);
		ivt[i * 4 + 1] = (uint8_t)((off >> 8) & 0xFF);
		ivt[i * 4 + 2] = (uint8_t)(STUB_SEG   & 0xFF);
		ivt[i * 4 + 3] = (uint8_t)(STUB_SEG >> 8);
	}
}

/* ---------- top-level init ---------------------------------- */

static X86EMU_pioFuncs pegasos2_pio_funcs = {
	.inb  = sys_inb_pegasos2,
	.inw  = sys_inw_pegasos2,
	.inl  = sys_inl_pegasos2,
	.outb = sys_outb_pegasos2,
	.outw = sys_outw_pegasos2,
	.outl = sys_outl_pegasos2,
};

/* ---------- memory callbacks ------------------------------- */
/*
 * The default sys_rdb / sys_wrb in upstream sys.c are no-ops
 * (return 0 / drop). We replace them with real accessors backed
 * by the 1 MiB buffer at X86EMU_MEM_PADDR. x86 is little-endian,
 * so word and long accesses are composed from byte accesses (which
 * keeps MMIO-style semantics if we later route specific sub-ranges
 * somewhere other than the buffer).
 */

static uint8_t rdb_pegasos2(uint32_t addr)
{
	return *x86emu_mem(addr);
}

static uint16_t rdw_pegasos2(uint32_t addr)
{
	uint16_t lo = *x86emu_mem(addr);
	uint16_t hi = *x86emu_mem(addr + 1);
	return (uint16_t)(lo | (hi << 8));
}

static uint32_t rdl_pegasos2(uint32_t addr)
{
	uint32_t v = 0;
	v |= (uint32_t)*x86emu_mem(addr    );
	v |= (uint32_t)*x86emu_mem(addr + 1) <<  8;
	v |= (uint32_t)*x86emu_mem(addr + 2) << 16;
	v |= (uint32_t)*x86emu_mem(addr + 3) << 24;
	return v;
}

static void wrb_pegasos2(uint32_t addr, uint8_t val)
{
	*x86emu_mem(addr) = val;
}

static void wrw_pegasos2(uint32_t addr, uint16_t val)
{
	*x86emu_mem(addr    ) = (uint8_t)(val     );
	*x86emu_mem(addr + 1) = (uint8_t)(val >> 8);
}

static void wrl_pegasos2(uint32_t addr, uint32_t val)
{
	*x86emu_mem(addr    ) = (uint8_t)(val      );
	*x86emu_mem(addr + 1) = (uint8_t)(val >>  8);
	*x86emu_mem(addr + 2) = (uint8_t)(val >> 16);
	*x86emu_mem(addr + 3) = (uint8_t)(val >> 24);
}

static X86EMU_memFuncs pegasos2_mem_funcs = {
	.rdb = rdb_pegasos2,
	.rdw = rdw_pegasos2,
	.rdl = rdl_pegasos2,
	.wrb = wrb_pegasos2,
	.wrw = wrw_pegasos2,
	.wrl = wrl_pegasos2,
};

static int glue_initialised = 0;

void x86_glue_init(void)
{
	if (glue_initialised)
		return;
	glue_initialised = 1;

	/* Zero the entire 1 MiB buffer. */
	uint8_t *base = (uint8_t *)X86EMU_MEM_PADDR;
	for (uint32_t i = 0; i < X86EMU_MEM_SIZE; i++)
		base[i] = 0;

	/* Point the emulator at our buffer. */
	M.mem_base = base;
	M.mem_size = X86EMU_MEM_SIZE;

	/* Memory + port I/O callbacks. */
	X86EMU_setupMemFuncs(&pegasos2_mem_funcs);
	X86EMU_setupPioFuncs(&pegasos2_pio_funcs);

	bda_init();
	ivt_init();
}

void x86emu_run(void)
{
	X86EMU_exec();
}
