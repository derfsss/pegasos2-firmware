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

/* ---------- BAR sizing + assignment -------------------------- */

/*
 * Running high-water allocators, one triple per host. PCI1 memory
 * uses the direct-mapped MV64361 mem0 alias starting at CPU (and
 * PCI-side) 0x80000000 -- a BAR value here is both the PCI-bus
 * address and the CPU physical address.
 *
 * The 1 GiB mem0 window splits in half to keep prefetchable BARs
 * in their own range:
 *   0x80000000..0x9FFFFFFF  non-prefetchable  (s_alloc_mem_next)
 *   0xA0000000..0xBFFFFFFF  prefetchable      (s_alloc_pref_next)
 * This matters on real HW: bridge prefetch forwarding should only
 * cover addresses that the downstream device has marked prefetch,
 * so the bridge's non-prefetch and prefetch windows must not
 * overlap. On QEMU the split is cosmetic -- QEMU's model forwards
 * whatever BASE/LIMIT claims -- but keeping them apart avoids
 * future drift.
 *
 * PCI I/O on both hosts uses the PCI-side number space starting
 * at 0x1000 so legacy-ISA ports (UART1 at 0x3F8, SuperIO index at
 * 0x3F0, etc.) stay free for the VT8231.
 *
 * PCI0 on QEMU has only the host bridge itself populated, so none
 * of these allocators are exercised on PCI0 today; they are kept
 * symmetric for when real HW (or a future QEMU) places devices
 * there.
 */
static uint32_t s_alloc_mem_next [2] = { 0x80000000u, 0x80000000u };
static uint32_t s_alloc_pref_next[2] = { 0xA0000000u, 0xA0000000u };
static uint32_t s_alloc_io_next  [2] = { 0x00001000u, 0x00001000u };

static uint32_t align_up(uint32_t v, uint32_t a)
{
	/* a is a power of two (BAR sizes are always 2^N by PCI spec). */
	return (v + (a - 1u)) & ~(a - 1u);
}

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

/*
 * Probe every BAR on a device, assign an address, and print the
 * per-BAR line. Returns a bitmask of PCI_CMD_MEM/PCI_CMD_IO bits
 * indicating which command-register decoders the caller should
 * enable for this device.
 *
 * BAR assignment policy:
 *   - Memory BAR: base = align_up(host's mem allocator, size)
 *   - I/O BAR   : base = align_up(host's io  allocator, size)
 *   - ROM BAR   : base = align_up(host's mem allocator, size),
 *                  enable bit set so the ROM is visible.
 *   - 64-bit BAR: low dword gets the base, high dword gets 0. We
 *                  stay within the 4 GiB PCI memory window.
 */
static unsigned probe_assign_and_print_bars(int host, uint8_t bus, uint8_t dev,
					    uint8_t fn, uint8_t header_type,
					    int depth)
{
	const uint8_t ht_kind = header_type & PCI_HEADER_TYPE_MASK;
	const int num_bars =
		(ht_kind == PCI_HTYPE_BRIDGE) ? PCI_BAR_COUNT_TYPE1
					      : PCI_BAR_COUNT_TYPE0;
	unsigned cmd_bits = 0;

	for (int i = 0; i < num_bars; i++) {
		const uint8_t  offset    = PCI_BAR_0 + (uint8_t)(i * 4);
		const uint32_t first_val = pci_cfg_read32(host, bus, dev, fn,
							  offset);
		const int      is_io     = (first_val & PCI_BAR_SPACE_IO) != 0;
		const uint32_t addr_mask = is_io ? PCI_BAR_IO_ADDR_MASK
						 : PCI_BAR_MEM_ADDR_MASK;

		uint32_t flags;
		int      is_64;
		const uint32_t size = pci_bar_probe(host, bus, dev, fn, offset,
						    addr_mask, &flags, &is_64);

		if (size != 0) {
			/*
			 * Route mem BARs into prefetch vs non-prefetch
			 * based on the device's own flag (bit 3). IO BARs
			 * go to the IO allocator regardless.
			 */
			const int pref = !is_io
				      && (flags & PCI_BAR_MEM_PREFETCH) != 0;
			uint32_t *hw =
				is_io ? &s_alloc_io_next  [host] :
				pref  ? &s_alloc_pref_next[host] :
					&s_alloc_mem_next [host];
			const uint32_t base = align_up(*hw, size);
			*hw = base + size;

			const uint32_t bar_val = (flags & ~addr_mask) | base;
			pci_cfg_write32(host, bus, dev, fn, offset, bar_val);
			if (is_64)
				pci_cfg_write32(host, bus, dev, fn, offset + 4,
						0u);

			cmd_bits |= is_io ? PCI_CMD_IO : PCI_CMD_MEM;

			put_indent(depth);
			uart_puts(UART1_BASE, "      BAR");
			uart_putc(UART1_BASE, '0' + i);
			uart_puts(UART1_BASE, ": ");
			if (is_io)
				uart_puts(UART1_BASE, "io        ");
			else
				print_mem_type(flags, is_64);
			uart_puts(UART1_BASE, " size=0x");
			uart_put_hex32(UART1_BASE, size);
			uart_puts(UART1_BASE, " -> 0x");
			uart_put_hex32(UART1_BASE, base);
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
		uint32_t *hw = &s_alloc_mem_next[host];
		const uint32_t base = align_up(*hw, rom_size);
		*hw = base + rom_size;

		pci_cfg_write32(host, bus, dev, fn, rom_offset,
				base | PCI_ROM_BAR_ENABLE);

		cmd_bits |= PCI_CMD_MEM;

		put_indent(depth);
		uart_puts(UART1_BASE, "      ROM : rom        size=0x");
		uart_put_hex32(UART1_BASE, rom_size);
		uart_puts(UART1_BASE, " -> 0x");
		uart_put_hex32(UART1_BASE, base);
		uart_puts(UART1_BASE, " (enabled)\n");
	}

	return cmd_bits;
}

/* ---------- bridge-window programming ------------------------ */

/*
 * Granularities mandated by the PCI 2.3 bridge header:
 *   MEM_BASE/LIMIT (0x20/0x22, 16-bit each): 1 MiB.
 *   IO_BASE/LIMIT  (0x1C/0x1D, 8-bit each):  4 KiB.
 *   The register value holds the upper bits of the address; the
 *   implied low bits are 0 for BASE and 1 for LIMIT, which is why
 *   a single-byte allocation inflates the bridge window to a full
 *   granularity block.
 */
#define BRIDGE_MEM_GRANULE   0x00100000u
#define BRIDGE_IO_GRANULE    0x00001000u

static uint16_t mem_window_encode(uint32_t addr)
{
	/* addr bits 31..20 go into reg bits 15..4; reg bits 3..0 are
	 * reserved-zero. */
	return (uint16_t)((addr >> 16) & 0xFFF0u);
}

static uint8_t io_window_encode(uint32_t addr)
{
	/* addr bits 15..12 go into reg bits 7..4; reg bits 3..0 hold
	 * the IO-capability flag (0 = 16-bit IO, which is our mode). */
	return (uint8_t)((addr >> 8) & 0xF0u);
}

/*
 * Shared helper: program a 16-bit MEM-shaped base/limit pair
 * (either PCI_BRIDGE_MEM_* or PCI_BRIDGE_PREFETCH_*) at 1 MiB
 * granularity. Emits one "<label> <range>" chunk to UART1.
 */
static void bridge_mem_window_write(int host, uint8_t bus, uint8_t dev,
				    uint8_t fn,
				    uint8_t base_off, uint8_t limit_off,
				    uint32_t start, uint32_t end,
				    const char *label)
{
	uart_puts(UART1_BASE, label);
	uart_puts(UART1_BASE, " ");

	if (end > start) {
		const uint16_t mb = mem_window_encode(start);
		const uint16_t ml = mem_window_encode(end - 1u);
		pci_cfg_write16(host, bus, dev, fn, base_off,  mb);
		pci_cfg_write16(host, bus, dev, fn, limit_off, ml);
		uart_puts(UART1_BASE, "0x");
		uart_put_hex32(UART1_BASE, start);
		uart_puts(UART1_BASE, "..0x");
		uart_put_hex32(UART1_BASE,
			       (uint32_t)((uint32_t)ml << 16) | 0xFFFFFu);
	} else {
		pci_cfg_write16(host, bus, dev, fn, base_off,  0xFFF0u);
		pci_cfg_write16(host, bus, dev, fn, limit_off, 0x0000u);
		uart_puts(UART1_BASE, "<disabled>");
	}
	uart_puts(UART1_BASE, " ");
}

static void bridge_program_window(int host, uint8_t bus, uint8_t dev,
				  uint8_t fn,
				  uint32_t mem_start,  uint32_t mem_end,
				  uint32_t pref_start, uint32_t pref_end,
				  uint32_t io_start,   uint32_t io_end,
				  int depth)
{
	put_indent(depth);
	uart_puts(UART1_BASE, "  -> bridge windows:");

	bridge_mem_window_write(host, bus, dev, fn,
				PCI_BRIDGE_MEM_BASE,  PCI_BRIDGE_MEM_LIMIT,
				mem_start,  mem_end,  " mem");
	bridge_mem_window_write(host, bus, dev, fn,
				PCI_BRIDGE_PREFETCH_BASE,
				PCI_BRIDGE_PREFETCH_LIMIT,
				pref_start, pref_end, "pref");

	if (io_end > io_start) {
		const uint8_t ib = io_window_encode(io_start);
		const uint8_t il = io_window_encode(io_end - 1u);
		pci_cfg_write8(host, bus, dev, fn, PCI_BRIDGE_IO_BASE,  ib);
		pci_cfg_write8(host, bus, dev, fn, PCI_BRIDGE_IO_LIMIT, il);
		uart_puts(UART1_BASE, "io 0x");
		uart_put_hex16(UART1_BASE, (uint16_t)io_start);
		uart_puts(UART1_BASE, "..0x");
		uart_put_hex16(UART1_BASE,
			       (uint16_t)(((uint16_t)il << 8) | 0x0FFFu));
	} else {
		pci_cfg_write8(host, bus, dev, fn, PCI_BRIDGE_IO_BASE,  0xF0u);
		pci_cfg_write8(host, bus, dev, fn, PCI_BRIDGE_IO_LIMIT, 0x00u);
		uart_puts(UART1_BASE, "io <disabled>");
	}

	uart_puts(UART1_BASE, "\n");
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

	unsigned cmd_bits = probe_assign_and_print_bars(ctx->host, bus, dev, fn,
							header_type, depth);
	if (cmd_bits != 0) {
		/*
		 * Enable the appropriate decoder(s) plus bus-master.
		 * PCI_CMD_MASTER is set unconditionally so devices can
		 * initiate DMA; this is the standard firmware policy.
		 * pci_cfg_write16 does a read-modify-write so PCI_STATUS
		 * (in the high half of the same dword) stays untouched.
		 */
		uint16_t cmd = pci_cfg_read16(ctx->host, bus, dev, fn,
					      PCI_COMMAND);
		cmd |= (uint16_t)cmd_bits | PCI_CMD_MASTER;
		pci_cfg_write16(ctx->host, bus, dev, fn, PCI_COMMAND, cmd);

		put_indent(depth);
		uart_puts(UART1_BASE, "      cmd: ");
		if (cmd_bits & PCI_CMD_MEM) uart_puts(UART1_BASE, "MEM ");
		if (cmd_bits & PCI_CMD_IO ) uart_puts(UART1_BASE, "IO ");
		uart_puts(UART1_BASE, "MASTER\n");
	}

	if (ht_kind == PCI_HTYPE_BRIDGE && class == PCI_CLASS_BRIDGE_PCI) {
		uint8_t secondary = ctx->next_bus++;

		pci_cfg_write8(ctx->host, bus, dev, fn,
			       PCI_BRIDGE_BUS_PRIMARY,    bus);
		pci_cfg_write8(ctx->host, bus, dev, fn,
			       PCI_BRIDGE_BUS_SECONDARY,  secondary);
		pci_cfg_write8(ctx->host, bus, dev, fn,
			       PCI_BRIDGE_BUS_SUBORDINATE, 0xFFu);

		/*
		 * Pad all three allocators to the bridge-window
		 * granularity BEFORE descending, so the subtree's first
		 * allocation lines up with a window boundary. The saved
		 * *_start values become the bridge's BASE; the allocator
		 * values after the recursive walk become the bridge's
		 * LIMIT. Prefetch uses the same 1 MiB MEM granularity.
		 */
		uint32_t mem_start  = align_up(s_alloc_mem_next [ctx->host],
					       BRIDGE_MEM_GRANULE);
		uint32_t pref_start = align_up(s_alloc_pref_next[ctx->host],
					       BRIDGE_MEM_GRANULE);
		uint32_t io_start   = align_up(s_alloc_io_next  [ctx->host],
					       BRIDGE_IO_GRANULE);
		s_alloc_mem_next [ctx->host] = mem_start;
		s_alloc_pref_next[ctx->host] = pref_start;
		s_alloc_io_next  [ctx->host] = io_start;

		put_indent(depth);
		uart_puts(UART1_BASE, "  -> recurse into bus 0x");
		uart_put_hex8(UART1_BASE, secondary);
		uart_puts(UART1_BASE, "\n");

		pci_scan_bus(ctx, secondary, depth + 1);

		uint32_t mem_end  = s_alloc_mem_next [ctx->host];
		uint32_t pref_end = s_alloc_pref_next[ctx->host];
		uint32_t io_end   = s_alloc_io_next  [ctx->host];

		bridge_program_window(ctx->host, bus, dev, fn,
				      mem_start,  mem_end,
				      pref_start, pref_end,
				      io_start,   io_end,
				      depth);

		/*
		 * Enable the corresponding forwarding bits on the
		 * bridge's own command register. Without this, the
		 * bridge does not claim matching cycles on the primary
		 * bus no matter what BASE/LIMIT says. MASTER is already
		 * set from the earlier probe_assign path; only MEM / IO
		 * might still be clear if the bridge had no mem/io BAR
		 * of its own. Prefetch forwarding is also gated by
		 * PCI_CMD_MEM (the same bit controls both windows).
		 */
		uint16_t fwd_cmd = pci_cfg_read16(ctx->host, bus, dev, fn,
						  PCI_COMMAND);
		if (mem_end  > mem_start ) fwd_cmd |= PCI_CMD_MEM;
		if (pref_end > pref_start) fwd_cmd |= PCI_CMD_MEM;
		if (io_end   > io_start  ) fwd_cmd |= PCI_CMD_IO;
		pci_cfg_write16(ctx->host, bus, dev, fn, PCI_COMMAND, fwd_cmd);

		/*
		 * Bridge LIMIT rounds the subtree's end UP to the next
		 * granule boundary (that's just how the encoding works:
		 * the low 20 or 12 implied bits are all-1s). Advance our
		 * allocator to match, otherwise a sibling device on the
		 * parent bus might be assigned an address that falls
		 * inside this bridge's now-inflated window.
		 */
		if (mem_end > mem_start)
			s_alloc_mem_next[ctx->host] =
				align_up(mem_end, BRIDGE_MEM_GRANULE);
		if (pref_end > pref_start)
			s_alloc_pref_next[ctx->host] =
				align_up(pref_end, BRIDGE_MEM_GRANULE);
		if (io_end > io_start)
			s_alloc_io_next[ctx->host] =
				align_up(io_end, BRIDGE_IO_GRANULE);

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
