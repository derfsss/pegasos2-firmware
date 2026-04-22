/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Recursive PCI enumeration for Bug 2 (docs/09-known-bugs.md).
 *
 *  Algorithm per docs/03-pci.md section "Required behaviour":
 *    1. For each primary bus, iterate (dev, fn) with header-type bit 7
 *       gating fn 1..7 as multi-function.
 *    2. For each discovered device, read class-code and header-type.
 *    3. If the device is a PCI-to-PCI bridge (class 0x0604, header
 *       type 0x01):
 *         a. Allocate the next available bus number as secondary.
 *         b. Program BUS_PRIMARY / BUS_SECONDARY / BUS_SUBORDINATE.
 *         c. Temporarily set SUBORDINATE = 0xFF so Type-1 cycles to
 *            the new bus are accepted during enumeration.
 *         d. Recurse into the secondary bus.
 *         e. After recursion, clamp SUBORDINATE to the highest bus
 *            number actually assigned in the subtree.
 *
 *  Type selection: bit 0 of the Configuration Address word. The
 *  MV64361 host bridge emits Type 0 when bus == its primary bus and
 *  Type 1 otherwise. The mv64361.c config helpers already set bit 0
 *  correctly based on the bus argument because bit 0 of the encoded
 *  CA word is always 0 here -- wait, actually that's Type 0 only.
 *
 *  NOTE: this initial revision emits Type 0 only. MV64361 forwards a
 *  Type-0 cycle with non-zero bus as a Type-1 when the bus number
 *  does not match its primary -- verified empirically on QEMU by
 *  observing that pci_cfg_write reaches downstream-bridge devices.
 *  If a future QEMU revision or real HW requires us to set the type
 *  bit explicitly, the generic helper in mv64361.c gains a parameter.
 */

#include <stdint.h>

#include "pci_walker.h"
#include "pci.h"
#include "mv64361.h"
#include "uart16550.h"
#include "pegasos2.h"

/* ---------- printing helpers --------------------------------- */

static void put_indent(int depth)
{
	int i;
	for (i = 0; i < depth; i++)
		uart_puts(UART1_BASE, "  ");
}

static void print_bdf(uint8_t bus, uint8_t dev, uint8_t fn)
{
	uart_put_hex8(UART1_BASE, bus);
	uart_putc(UART1_BASE, ':');
	uart_put_hex8(UART1_BASE, dev);
	uart_putc(UART1_BASE, '.');
	uart_putc(UART1_BASE, '0' + (fn & 7));
}

static void print_device(int depth, uint8_t bus, uint8_t dev, uint8_t fn,
			 uint16_t vendor, uint16_t device, uint16_t class,
			 uint8_t header_type)
{
	put_indent(depth);
	uart_puts(UART1_BASE, "  ");
	print_bdf(bus, dev, fn);
	uart_puts(UART1_BASE, "  ");
	uart_put_hex16(UART1_BASE, vendor);
	uart_putc(UART1_BASE, ':');
	uart_put_hex16(UART1_BASE, device);
	uart_puts(UART1_BASE, "  class=0x");
	uart_put_hex16(UART1_BASE, class);
	uart_puts(UART1_BASE, "  ht=0x");
	uart_put_hex8(UART1_BASE, header_type);
	uart_puts(UART1_BASE, "\n");
}

/* ---------- BAR sizing -------------------------------------- */

/*
 * Non-destructive BAR probe per PCI 2.3 §6.2.5.1:
 *    saved     <- read BAR
 *    probe_val <- (saved & ~addr_mask) | addr_mask   (poke address bits)
 *    write BAR <- probe_val
 *    mask      <- read BAR
 *    restore BAR <- saved
 *    size      <- ~(mask & addr_mask) + 1
 *
 * Preserving the non-address bits during the probe matters for the
 * ROM BAR (bit 0 = enable), where writing 0xFFFFFFFF would briefly
 * enable ROM decode on an unpredictable address. Memory/I/O BARs
 * have their low bits hardwired per PCI 2.3 §6.2.5, so the mask is
 * a no-op for them.
 *
 * Returns size in bytes (0 if the BAR is unimplemented). On return,
 * *flags_out holds the original BAR's low bits (type + prefetch);
 * *is_64_out is 1 for 64-bit memory BARs (the caller skips the
 * next BAR slot, which is the 64-bit high half).
 */
static uint32_t pci_bar_probe(int host, uint8_t bus, uint8_t dev, uint8_t fn,
			      uint8_t offset, uint32_t addr_mask,
			      uint32_t *flags_out, int *is_64_out)
{
	*is_64_out = 0;

	uint32_t saved     = pci_cfg_read32(host, bus, dev, fn, offset);
	uint32_t probe_val = (saved & ~addr_mask) | addr_mask;
	pci_cfg_write32(host, bus, dev, fn, offset, probe_val);
	uint32_t mask      = pci_cfg_read32(host, bus, dev, fn, offset);
	pci_cfg_write32(host, bus, dev, fn, offset, saved);

	*flags_out = saved & ~addr_mask;

	if ((mask & addr_mask) == 0)
		return 0;                  /* unimplemented BAR */

	if (!(saved & PCI_BAR_SPACE_IO)
	    && (saved & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64)
		*is_64_out = 1;

	return (uint32_t)(~(mask & addr_mask) + 1u);
}

static void print_mem_type(uint32_t flags, int is_64)
{
	const int pref = (flags & PCI_BAR_MEM_PREFETCH) != 0;
	uart_puts(UART1_BASE, is_64 ? "mem64" : "mem32");
	uart_puts(UART1_BASE, pref ? " pref" : "     ");
}

static void probe_and_print_bars(int host, uint8_t bus, uint8_t dev, uint8_t fn,
				 uint8_t header_type, int depth)
{
	const uint8_t ht_kind = header_type & PCI_HEADER_TYPE_MASK;
	const int num_bars =
		(ht_kind == PCI_HTYPE_BRIDGE) ? PCI_BAR_COUNT_TYPE1
					      : PCI_BAR_COUNT_TYPE0;

	for (int i = 0; i < num_bars; i++) {
		const uint8_t  offset    = PCI_BAR_0 + (uint8_t)(i * 4);
		const uint32_t first_val = pci_cfg_read32(host, bus, dev, fn,
							  offset);
		const uint32_t addr_mask = (first_val & PCI_BAR_SPACE_IO)
						? PCI_BAR_IO_ADDR_MASK
						: PCI_BAR_MEM_ADDR_MASK;

		uint32_t flags;
		int      is_64;
		const uint32_t size = pci_bar_probe(host, bus, dev, fn, offset,
						    addr_mask, &flags, &is_64);

		if (size != 0) {
			put_indent(depth);
			uart_puts(UART1_BASE, "      BAR");
			uart_putc(UART1_BASE, '0' + i);
			uart_puts(UART1_BASE, ": ");
			if (flags & PCI_BAR_SPACE_IO)
				uart_puts(UART1_BASE, "io        ");
			else
				print_mem_type(flags, is_64);
			uart_puts(UART1_BASE, " size=0x");
			uart_put_hex32(UART1_BASE, size);
			uart_puts(UART1_BASE, "\n");
		}

		/* 64-bit BAR consumes the next slot as its high dword. */
		if (is_64)
			i++;
	}

	/* Expansion-ROM BAR. Offset differs between type-0 and type-1. */
	const uint8_t rom_offset = (ht_kind == PCI_HTYPE_BRIDGE)
					? PCI_ROM_BAR_TYPE1
					: PCI_ROM_BAR_TYPE0;
	uint32_t rom_flags;
	int      rom_is_64;
	const uint32_t rom_size = pci_bar_probe(host, bus, dev, fn, rom_offset,
						PCI_ROM_BAR_ADDR_MASK,
						&rom_flags, &rom_is_64);
	if (rom_size != 0) {
		put_indent(depth);
		uart_puts(UART1_BASE, "      ROM : rom        size=0x");
		uart_put_hex32(UART1_BASE, rom_size);
		uart_puts(UART1_BASE, "\n");
	}
}

/* ---------- enumeration ------------------------------------- */

/* Shared per-host bus-number allocator. Handed to every nested call
 * in a walk and post-incremented when a new secondary bus is needed. */
struct walk_ctx {
	int      host;
	uint8_t  next_bus;
};

static void pci_scan_bus(struct walk_ctx *ctx, uint8_t bus, int depth);

static void pci_scan_function(struct walk_ctx *ctx, uint8_t bus,
			      uint8_t dev, uint8_t fn, int depth)
{
	uint16_t vendor = pci_cfg_read16(ctx->host, bus, dev, fn, PCI_VENDOR_ID);
	if (vendor == PCI_VENDOR_INVALID)
		return;

	uint16_t device      = pci_cfg_read16(ctx->host, bus, dev, fn,
					      PCI_DEVICE_ID);
	/* Read 16 bits at offset 0x0A: low byte = subclass, high byte =
	 * base class. As a PCI-byte-order uint16_t this is already
	 * (base<<8) | subclass -- i.e. 0x0604 for a PCI-PCI bridge. */
	uint16_t class       = pci_cfg_read16(ctx->host, bus, dev, fn,
					      PCI_SUBCLASS);
	uint8_t  header_type = pci_cfg_read8 (ctx->host, bus, dev, fn,
					      PCI_HEADER_TYPE);
	uint8_t  ht_kind     = header_type & PCI_HEADER_TYPE_MASK;

	print_device(depth, bus, dev, fn, vendor, device, class, header_type);
	probe_and_print_bars(ctx->host, bus, dev, fn, header_type, depth);

	if (ht_kind == PCI_HTYPE_BRIDGE && class == PCI_CLASS_BRIDGE_PCI) {
		uint8_t secondary = ctx->next_bus++;

		pci_cfg_write8(ctx->host, bus, dev, fn,
			       PCI_BRIDGE_BUS_PRIMARY,    bus);
		pci_cfg_write8(ctx->host, bus, dev, fn,
			       PCI_BRIDGE_BUS_SECONDARY,  secondary);
		pci_cfg_write8(ctx->host, bus, dev, fn,
			       PCI_BRIDGE_BUS_SUBORDINATE, 0xFFu);

		put_indent(depth);
		uart_puts(UART1_BASE, "  -> recurse into bus 0x");
		uart_put_hex8(UART1_BASE, secondary);
		uart_puts(UART1_BASE, "\n");

		pci_scan_bus(ctx, secondary, depth + 1);

		uint8_t highest = (uint8_t)(ctx->next_bus - 1u);
		pci_cfg_write8(ctx->host, bus, dev, fn,
			       PCI_BRIDGE_BUS_SUBORDINATE, highest);
	}
}

static void pci_scan_bus(struct walk_ctx *ctx, uint8_t bus, int depth)
{
	uint8_t dev;
	for (dev = 0; dev < 32; dev++) {
		uint16_t vendor = pci_cfg_read16(ctx->host, bus, dev, 0,
						 PCI_VENDOR_ID);
		if (vendor == PCI_VENDOR_INVALID)
			continue;

		uint8_t header_type = pci_cfg_read8(ctx->host, bus, dev, 0,
						    PCI_HEADER_TYPE);
		int multi_fn = (header_type & PCI_HEADER_TYPE_MULTIFN) != 0;
		uint8_t max_fn = multi_fn ? 8u : 1u;

		uint8_t fn;
		for (fn = 0; fn < max_fn; fn++)
			pci_scan_function(ctx, bus, dev, fn, depth);
	}
}

void pci_walk(void)
{
	int host;
	for (host = 0; host <= 1; host++) {
		uart_puts(UART1_BASE, "PCI");
		uart_putc(UART1_BASE, '0' + host);
		uart_puts(UART1_BASE, ":\n");

		struct walk_ctx ctx = { .host = host, .next_bus = 1 };
		pci_scan_bus(&ctx, 0, 0);
	}
}
