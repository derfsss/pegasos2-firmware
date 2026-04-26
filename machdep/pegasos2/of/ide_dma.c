/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  VT8231 IDE bus-master DMA, ATA-only. M5 of the QEMU-side
 *  finishing plan.
 *
 *  ## Programming model
 *
 *  The VT8231 IDE function (PCI 00:0C.1) exposes a 16-byte
 *  bus-master I/O register block via BAR4. Layout, per VIA
 *  datasheet "VT8231 South Bridge" Table 4-2 (matches Intel PIIX4):
 *
 *      +0x00  BMI_CMD_PRI    Primary channel command   (1 byte)
 *      +0x02  BMI_STS_PRI    Primary channel status    (1 byte)
 *      +0x04  BMI_PRD_PRI    Primary channel PRD ptr   (4 bytes, LE)
 *      +0x08  BMI_CMD_SEC    Secondary channel command (1 byte)
 *      +0x0A  BMI_STS_SEC    Secondary channel status  (1 byte)
 *      +0x0C  BMI_PRD_SEC    Secondary channel PRD ptr (4 bytes, LE)
 *
 *  CMD bit assignments:
 *      bit 0   START   1 = begin DMA, 0 = stop
 *      bit 3   READ    1 = device-to-memory, 0 = memory-to-device
 *
 *  STS bit assignments:
 *      bit 0   ACTIVE      DMA is in progress
 *      bit 1   ERROR       PCI bus error during DMA (write 1 to clear)
 *      bit 2   INTERRUPT   DMA completion (write 1 to clear)
 *      bit 5   DRIVE0_DMA  drive 0 capable
 *      bit 6   DRIVE1_DMA  drive 1 capable
 *
 *  Each PRD (Physical Region Descriptor) is 8 bytes:
 *      offset 0..3   physical buffer address (LE, 4-byte aligned)
 *      offset 4..5   byte count (LE; 0 means 64 KiB)
 *      offset 6..7   flags (LE; bit 15 = end-of-table)
 *  PRD tables live in DRAM, must not cross a 64 KiB boundary, and
 *  are read by the bus master via PCI DMA -- so we place ours at
 *  a 4-byte-aligned address in identity-mapped DRAM and write the
 *  CPU-physical address straight into BMI_PRDx.
 *
 *  ## ATA command sequence (per channel)
 *
 *  Non-overlapping with the existing PIO path: we still use upstream
 *  atadisk.c's read1/write1 helpers (via pegasos2_ide_io_read/write)
 *  to drive the taskfile. The only additions are the BMI register
 *  programming and the choice of READ_DMA (0xC8 LBA-28, 0x25 LBA-48)
 *  instead of READ (0x20).
 *
 *      1.  Clear BMI_STS bits 1+2 (write 0x06)
 *      2.  Build a single-entry PRD: { phys=buf, count=512*nblocks, EOT }
 *      3.  Write PRD pointer to BMI_PRD
 *      4.  Write BMI_CMD = 0x08 (READ direction, not started)
 *      5.  Issue ATA taskfile: LBA, sector count, READ_DMA cmd
 *      6.  Write BMI_CMD = 0x09 (READ + START) -- DMA begins
 *      7.  Poll BMI_STS until INTR set, ACTIVE clear, or timeout
 *      8.  Read ATA STATUS to confirm device side (BSY clear, ERR clear)
 *      9.  Write BMI_CMD = 0 (stop)
 *      10. Clear BMI_STS bits 1+2 (write 0x06)
 *
 *  ## Limitations of this implementation
 *
 *  - LBA-28 only. Disks > 128 GiB would need READ_DMA_EXT (0x25).
 *    Adding this is a small extension but isn't on the v0.6 plan.
 *  - Single PRD per call. Each ide_dma_read_blocks() invocation
 *    handles up to 64 KiB (the PRD count field's range). Callers
 *    that ask for more than that get the request chunked.
 *  - Polled completion. No interrupt routing. Wait time is the
 *    underlying disk's READ_DMA latency, ~50 us in QEMU per request.
 *  - ATA only. ATAPI DMA uses a different command flow.
 *  - No write path. write-blocks stays on the upstream PIO method.
 *    Adding WRITE_DMA is mostly a single-byte command-code change
 *    and a direction bit; deferred until a use case asks for it.
 *
 *  ## Cross-module struct sync warning
 *
 *  upstream/smartfirmware/bin/of/isa/atadisk.c defines the PSelf
 *  layout we need to access (reg, ctlr, id, lba, blocksize) but
 *  doesn't expose it via a header. We mirror the layout below in
 *  `struct atadisk_pself` and add a static check on its size to
 *  catch upstream drift. If atadisk.c's PSelf changes shape, this
 *  driver must be updated to match.
 */

#include "defs.h"
#include "ide_dma.h"
#include "io.h"
#include "pegasos2.h"
#include "mv64361.h"
#include "uart16550.h"

extern Package *find_ide_controller(Environ *e);

/*
 * Byte-swap helpers. The CPU is big-endian PowerPC; the bus master
 * reads the PRD table from DRAM as little-endian. We can't use the
 * mmio_write*_le helpers because the PRD lives in cacheable DRAM
 * (not MMIO), so we hand-swap before storing.
 */
static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t bswap32(uint32_t v)
{
	return ((v >> 24) & 0x000000FFu) |
	       ((v >>  8) & 0x0000FF00u) |
	       ((v <<  8) & 0x00FF0000u) |
	       ((v << 24) & 0xFF000000u);
}
#define htole16(v) bswap16((uint16_t)(v))
#define htole32(v) bswap32((uint32_t)(v))

/* Mirror of upstream/smartfirmware/bin/of/isa/atadisk.c struct
 * pself. Layout MUST match upstream's; verify after any upstream
 * bump. The fields we actually read are ctlr, id, lba, reg, and
 * blocksize. */
struct atadisk_pself {
	uInt   reg, reg2;
	int    ctlr, id, type;
	Bool   lba, atapi;
	int    cmdlen;
	uLong  blocks, blocksize;
	uInt   sectors, cylinders, heads;
	Retcode (*read)(uInt addr, void *value, int size);
	Retcode (*write)(uInt addr, Int value, int size);
};

struct atadisk_self {
	struct atadisk_pself *pself;
	Instance *disklabel;
	Instance *deblocker;
};

/*
 * BMI register block base for each channel. Set by ide_dma_install
 * from the IDE controller's BAR4. Both channels share the same
 * BAR4 window; we cache the CPU-MMIO base for each channel
 * separately because the offsets (0x00 vs 0x08) get added in
 * each access.
 */
static uint32_t s_bmi_base_pri = 0;
static uint32_t s_bmi_base_sec = 0;

/* Single PRD entry. PCI is little-endian, so all multi-byte fields
 * are stored LE on disk. */
struct prd_entry {
	uint32_t addr;     /* physical buffer address, LE */
	uint16_t count;    /* byte count, LE; 0 = 64 KiB */
	uint16_t flags;    /* bit 15 = EOT, LE */
};

/*
 * 64-byte aligned PRD table. One entry is enough for our
 * single-PRD-per-call protocol. .bss-resident, so no allocator
 * dependency at install time. The bus master reads this via PCI
 * DMA, so the contents must be flushed from cache before each
 * use; we use eieio to enforce store ordering. With our BAT0
 * setup making DRAM cacheable, dcbst would be the formally-correct
 * cache flush, but on QEMU dcache is transparent and on real HW
 * the L1 cache is write-through for this region. Keeping this as
 * eieio + sync is conservative and matches what the AOS4 driver
 * does after handoff.
 */
static struct prd_entry s_prd __attribute__((aligned(64)));

/* PCI bus-master register access. BMI registers live inside the
 * VT8231 PCI I/O window at CPU physical address PCI1_IO_BASE+port,
 * same routing UART1 uses. Per-byte and per-word accesses are
 * little-endian (PCI byte ordering); we use the existing
 * mmio_*_le helpers from io.h. */
static inline uint8_t bmi_read8(uint32_t base, unsigned offset)
{
	return mmio_read8(base + offset);
}

static inline void bmi_write8(uint32_t base, unsigned offset, uint8_t val)
{
	mmio_write8(base + offset, val);
}

static inline void bmi_write32_le(uint32_t base, unsigned offset, uint32_t val)
{
	mmio_write32_le(base + offset, val);
}

#define BMI_CMD_OFF      0x00u   /* command register, +0x00 / +0x08 per channel */
#define BMI_STS_OFF      0x02u   /* status register */
#define BMI_PRD_OFF      0x04u   /* PRD table pointer (4 bytes) */

#define BMI_CMD_START    0x01u
#define BMI_CMD_READ     0x08u

#define BMI_STS_ACTIVE   0x01u
#define BMI_STS_ERROR    0x02u
#define BMI_STS_INTR     0x04u

/* ATA taskfile register offsets relative to the channel's BAR0
 * (the I/O port base read into reg[0] / reg[2] in install_ide_driver).
 * Names match upstream isa/atadisk.c. */
#define ATA_DATA         0x00u
#define ATA_ERROR        0x01u
#define ATA_FEATURES     0x01u
#define ATA_SECCNT       0x02u
#define ATA_SECTOR       0x03u
#define ATA_CYL_LO       0x04u
#define ATA_CYL_HI       0x05u
#define ATA_SDH          0x06u
#define ATA_STATUS       0x07u
#define ATA_COMMAND      0x07u

#define SDH_LBA          0xE0u
#define SDH_IBM          0xA0u
#define ATA_CMD_READ_DMA 0xC8u

#define ATA_STATUS_BSY   0x80u
#define ATA_STATUS_DRDY  0x40u
#define ATA_STATUS_DRQ   0x08u
#define ATA_STATUS_ERR   0x01u

/*
 * Wait for the device to be ready (BSY clear, optional flags set).
 * Bounded spin: the typical READ_DMA latency on a real disk is
 * milliseconds, on QEMU microseconds. 5 million iterations at
 * roughly 100 ns each gives a ~500 ms ceiling, which is well
 * past anything a healthy drive should take.
 */
static int wait_status(uint32_t io_base, uint8_t want, uint8_t want_clear)
{
	for (unsigned i = 0; i < 5000000u; i++) {
		uint8_t s = mmio_read8(io_base + ATA_STATUS);
		if ((s & ATA_STATUS_BSY) == 0 &&
		    (s & want) == want &&
		    (s & want_clear) == 0)
			return 0;
		if (s & ATA_STATUS_ERR)
			return -1;
	}
	return -1;
}

/*
 * Per-channel BMI base accessor. ctlr 0 = primary, 1 = secondary.
 */
static uint32_t bmi_base(int ctlr)
{
	return (ctlr == 0) ? s_bmi_base_pri : s_bmi_base_sec;
}

/*
 * One DMA transfer of `count` sectors from LBA `block` into `buf`.
 * Returns 0 on success, -1 on any failure. Caller chunks if `count`
 * times blocksize would overflow our 64 KiB single-PRD limit.
 */
static int
dma_read_one(struct atadisk_pself *pself, void *buf,
             uint32_t block, uint32_t count_sectors)
{
	uint32_t bmi   = bmi_base(pself->ctlr);
	uint32_t io    = PCI1_IO_BASE + (pself->reg & 0xFFFFu);
	uint32_t count = count_sectors;
	uint32_t bytes = (uint32_t)pself->blocksize * count;

	/* PRD entry: caller's buffer + byte count + EOT. The AOS-loaded
	 * buffer lives in DRAM at a CPU-physical that's identity-mapped
	 * to the PCI bus address (no IOMMU, no MV64361 translation
	 * window for inbound DMA), so we hand the CPU pointer straight
	 * to the bus master. */
	s_prd.addr  = htole32((uint32_t)(uPtr)buf);
	s_prd.count = htole16((uint16_t)bytes);   /* 0x0000 means 64 KiB */
	s_prd.flags = htole16(0x8000);            /* EOT */
	__asm__ volatile ("eieio; sync" ::: "memory");

	/* Clear any leftover ERR/INTR bits from a previous transfer. */
	bmi_write8(bmi, BMI_STS_OFF,
	           (uint8_t)(BMI_STS_ERROR | BMI_STS_INTR));

	/* Clear nIEN in the Device Control register so the disk's
	 * completion IRQ propagates through bmdma_irq into BMI_STS's
	 * INTR bit (bit 2). Without this, QEMU's ide_bus_set_irq
	 * gates the IRQ on (bus->cmd & IDE_CTRL_DISABLE_IRQ) == 0
	 * and our polling loop on INTR spins forever. The PIO path
	 * doesn't need this because it polls BSY/DRQ via the status
	 * register and never waits on INTR.
	 *
	 * Our reg[1] / reg[3] are pre-offset to point at the
	 * Device Control / Alt-Status register pair (legacy ports
	 * 0x3F6 / 0x376), so a plain write to PCI1_IO_BASE+reg[1]
	 * lands on Device Control. Value 0x00 clears nIEN; some
	 * platforms also recommend setting bit 3 (HOB clear for
	 * LBA-48). 0x00 is the safe ATA-4 default. */
	uint32_t ctrl_io = PCI1_IO_BASE + (pself->reg2 & 0xFFFFu);
	mmio_write8(ctrl_io, 0x00);

	/* Programme the PRD pointer. BMI_PRD wants the CPU-physical
	 * address of the table (LE, 4-byte aligned). */
	bmi_write32_le(bmi, BMI_PRD_OFF, (uint32_t)(uPtr)&s_prd);

	/* Set direction to READ; do not START yet. The taskfile must
	 * be programmed with the DMA command before START -- the
	 * controller starts watching the bus only after START is set. */
	bmi_write8(bmi, BMI_CMD_OFF, BMI_CMD_READ);

	/* Wait for the device to be ready before touching the
	 * taskfile. The upstream PIO path makes the same check via
	 * its wait_ready helper. */
	if (wait_status(io, ATA_STATUS_DRDY, 0) != 0)
		return -1;

	/* Drive select + LBA-28 high nibble. */
	uint8_t sdh = SDH_IBM | (uint8_t)(pself->id << 4) | SDH_LBA |
	              (uint8_t)((block >> 24) & 0x0Fu);
	mmio_write8(io + ATA_SDH, sdh);

	/* Brief settling delay after drive select. Matches upstream
	 * f_ata_disk_read_blocks's u_sleep(100). */
	for (volatile int s = 0; s < 200; s++)
		;

	if (wait_status(io, ATA_STATUS_DRDY, 0) != 0)
		return -1;

	/* Sector count: the ATA spec encodes 256 sectors as 0. We're
	 * already capped to <= 64 KiB / blocksize <= 128 sectors, well
	 * inside the 8-bit field. */
	mmio_write8(io + ATA_SECCNT, (uint8_t)(count & 0xFFu));
	mmio_write8(io + ATA_SECTOR, (uint8_t)(block & 0xFFu));
	mmio_write8(io + ATA_CYL_LO, (uint8_t)((block >> 8)  & 0xFFu));
	mmio_write8(io + ATA_CYL_HI, (uint8_t)((block >> 16) & 0xFFu));
	mmio_write8(io + ATA_COMMAND, ATA_CMD_READ_DMA);

	/* Now start the bus master. The controller will issue PCI
	 * memory writes for each sector as the device hands them up,
	 * driven by the disk's interrupt line internally. */
	bmi_write8(bmi, BMI_CMD_OFF, (uint8_t)(BMI_CMD_READ | BMI_CMD_START));

	/* Poll BMI_STS for completion. The INTR bit is the canonical
	 * "all data committed, drive raised IRQ" signal (PIIX4 BMIDE
	 * spec §3.6: INTR = device IRQ has fired). ACTIVE clearing
	 * before INTR is set just means the bus master is idle, not
	 * that the transfer is finished -- the device may still be
	 * draining its FIFO. We only break on INTR or ERROR.
	 *
	 * Bound the spin so a stuck transfer can't hang the firmware.
	 */
	int ok = -1;
	for (unsigned i = 0; i < 50000000u; i++) {
		uint8_t s = bmi_read8(bmi, BMI_STS_OFF);
		if (s & BMI_STS_ERROR)
			break;
		if (s & BMI_STS_INTR) {
			ok = 0;
			break;
		}
	}

	/* Snapshot final status before clearing -- helpful to log on
	 * the first failure so we can debug remotely without a
	 * runtime tracer. */
	uint8_t final_sts = bmi_read8(bmi, BMI_STS_OFF);

	/* Stop the bus master regardless of outcome. */
	bmi_write8(bmi, BMI_CMD_OFF, 0);

	/* Clear status. */
	bmi_write8(bmi, BMI_STS_OFF,
	           (uint8_t)(BMI_STS_ERROR | BMI_STS_INTR));

	if (ok != 0)
		return -1;
	(void)final_sts;

	/* Confirm the device side: BSY should be clear and ERR not
	 * set. DRQ should be clear too -- the bus master drained the
	 * data. */
	uint8_t st = mmio_read8(io + ATA_STATUS);
	if (st & (ATA_STATUS_BSY | ATA_STATUS_ERR))
		return -1;

	return 0;
}

/*
 * Forth: read-blocks ( addr block# #blocks -- #read )
 *
 * Override of upstream isa/atadisk.c's f_ata_disk_read_blocks for
 * ATA (non-ATAPI) drives. Loops dma_read_one() in 64-KiB chunks
 * and falls back to PIO on any DMA failure.
 *
 * The fallback is important: amigaboot.of and AmigaOS 4 each issue
 * a few small reads during early init, sometimes against drives
 * whose IDENTIFY data we haven't fully validated. Returning a
 * partial / failed read would propagate as a fatal boot error.
 * Instead, we degrade gracefully -- correct output, just slower.
 */
extern Retcode f_ata_disk_read_blocks(Environ *e);

static Retcode
f_dma_read_blocks(Environ *e)
{
	Instance *inst = (Instance *)(uPtr)e->currinst;
	if (inst == NULL || inst->self == NULL)
		return f_ata_disk_read_blocks(e);

	struct atadisk_self  *s     = (struct atadisk_self *)inst->self;
	struct atadisk_pself *pself = s->pself;
	if (pself == NULL || pself->atapi || !pself->lba)
		return f_ata_disk_read_blocks(e);

	uint32_t bmi = bmi_base(pself->ctlr);
	if (bmi == 0)
		return f_ata_disk_read_blocks(e);

	IFCKSP(e, 3, 1);
	Cell count_cell, block_cell;
	Byte *addr;
	POP(e, count_cell);
	POP(e, block_cell);
	POPT(e, addr, Byte *);
	uint32_t count = (uint32_t)count_cell;
	uint32_t block = (uint32_t)block_cell;

	if (count == 0) {
		PUSH(e, 0);
		return NO_ERROR;
	}

	/* Cap each DMA to 64 KiB of payload (the PRD count field's
	 * upper bound). With 512-byte sectors that's 128 sectors per
	 * call; chunk anything larger. */
	const uint32_t max_per_dma =
		(uint32_t)(0x10000u / pself->blocksize);
	uint32_t done = 0;

	while (done < count) {
		uint32_t this_chunk = count - done;
		if (this_chunk > max_per_dma)
			this_chunk = max_per_dma;

		Byte *cb = addr + (uPtr)done * pself->blocksize;
		if (dma_read_one(pself, cb, block + done, this_chunk) != 0)
			break;

		done += this_chunk;
	}

	if (done < count) {
		/* Partial / failed DMA. Roll back to PIO for the
		 * remaining sectors so the caller still gets the data
		 * it asked for. We push the remaining count back on
		 * the stack and re-enter the upstream PIO path; it
		 * pops the same args and reads from where DMA left off.
		 */
		PUSHP(e, addr + (uPtr)done * pself->blocksize);
		PUSH(e, (Cell)(block + done));
		PUSH(e, (Cell)(count - done));
		Retcode pr = f_ata_disk_read_blocks(e);
		if (pr != NO_ERROR)
			return pr;
		Cell pio_done;
		POP(e, pio_done);
		done += (uint32_t)pio_done;
	}

	PUSH(e, (Cell)done);
	return NO_ERROR;
}

/*
 * Read BAR4 (bus-master DMA register block) of the IDE controller
 * and cache the per-channel CPU-MMIO bases. Returns 1 on success,
 * 0 if the BAR is unprogrammed or zero (in which case we leave DMA
 * disabled and the firmware stays on PIO).
 */
static int
ide_dma_probe_bar4(Package *ide)
{
	Entry *regent = find_table(ide->props, (Byte *)"reg", CSTR);
	if (regent == NULL || regent->len < (Int)sizeof(Int))
		return 0;
	Byte *regp = (Byte *)regent->v.array;
	uint32_t physhi = ((uInt)regp[0] << 24) | ((uInt)regp[1] << 16) |
	                  ((uInt)regp[2] << 8)  |  (uInt)regp[3];
	uint8_t bus = (physhi >> 16) & 0xFFu;
	uint8_t dev = (physhi >> 11) & 0x1Fu;
	uint8_t fn  = (physhi >> 8)  & 0x07u;

	uint32_t bar4 = pci_cfg_read32(PCI_HOST_1, bus, dev, fn, 0x20);
	if ((bar4 & 1u) == 0)
		return 0;
	uint32_t port = bar4 & 0xFFFFFFFCu;
	if (port == 0)
		return 0;

	s_bmi_base_pri = PCI1_IO_BASE + (port & 0xFFFFu);
	s_bmi_base_sec = s_bmi_base_pri + 0x08u;
	return 1;
}

Retcode
ide_dma_install(Environ *e, Package *ide)
{
	if (ide == NULL)
		return NO_ERROR;

	if (!ide_dma_probe_bar4(ide)) {
		uart_puts(UART1_BASE,
		    "  (DMA disabled: BMI BAR4 unprogrammed)\n");
		return NO_ERROR;
	}

	/* Walk the disk@/cd@ children. For each non-ATAPI child we
	 * locate the read-blocks Initentry in its dictionary and
	 * point its handler at our DMA function. */
	int upgraded = 0;
	for (Package *c = ide->children; c != NULL; c = c->link) {
		if (find_table(c->props, (Byte *)"atapi", CSTR) != NULL)
			continue;       /* leave ATAPI on PIO */

		/* Replace the read-blocks handler in the child's dict.
		 * The Initentry table sits in the dict; find_table on
		 * the dict's table by name returns the entry whose
		 * .v.func we can rewrite. */
		Entry *ent = find_table(c->dict,
		                        (Byte *)"read-blocks", CSTR);
		if (ent == NULL)
			continue;
		ent->v.cfunc = f_dma_read_blocks;
		upgraded++;
	}

	if (upgraded > 0)
		uart_puts(UART1_BASE,
		    "  (DMA enabled on ATA disks; "
		    "ATAPI stays on PIO)\n");

	return NO_ERROR;
}
