/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  VT8231 PCI IDE driver attachment.
 *
 *  Upstream isa/atadisk.c provides the generic ATA/ATAPI driver; it
 *  is designed to be callable from any bus-layer glue that can supply
 *  (reg[4], read_callback, write_callback, parent-package) to
 *  probe_ata_disks(). Upstream isa/ata.c is NOT used -- it is the
 *  ISA-bus glue and hard-codes legacy ports + the Isa_device model
 *  we don't want to drag in.
 *
 *  This file is our pegasos2 replacement for isa/ata.c:
 *    - I/O callbacks that translate "PCI I/O port N" into a CPU MMIO
 *      access inside the PCI-host's I/O window (0xFE000000 for
 *      PCI_HOST_1 on QEMU pegasos2).
 *    - Controller-level method table (open/close/decode-unit/
 *      encode-unit/dma-alloc/dma-free/selftest).
 *    - install_ide_driver(): locates the IDE controller node built
 *      by install_pci_tree, reads its BARs to decide legacy vs
 *      native port layout, registers our methods, calls
 *      probe_ata_disks to create child cd@/disk@ nodes with the
 *      full ata_disk_methods method table (open/close/read-blocks/
 *      write-blocks/block-size/etc).
 *    - test-ide-probe Forth word: walks the disk children and
 *      prints identity + capacity for each.
 *
 *  VT8231 IDE operating modes on QEMU pegasos2:
 *    Progif = 0x8A puts both channels in PCI IDE "legacy-compat"
 *    mode (QEMU hw/ide/pci.c:pci_ide_update_mode "case 0xa"). BAR0-3
 *    are zero-sized; the legacy I/O ports 0x1F0/0x3F6/0x170/0x376 on
 *    the PCI I/O bus are where the controller actually decodes.
 *    Real hardware may run either legacy or native mode; we probe
 *    BAR0-3 and fall back to legacy ports only if a BAR is empty.
 *    Either way, the driver sees a PCI-I/O-space uint at which the
 *    ATA registers live; we translate via PCI1_IO_BASE at callback
 *    time.
 *
 *  Endianness: ATA registers are 8-bit, endianness-invariant; the
 *  DATA register is 16-bit and the PCI I/O bus is little-endian
 *  ordered. For DATA register transfers we use `lhbrx`/`sthbrx`
 *  (mmio_read16_le / mmio_write16_le), else plain `lbz`/`stb`.
 */

#include "defs.h"

#include "io.h"
#include "mv64361.h"
#include "pegasos2.h"
#include "uart16550.h"
#include "ide_dma.h"

/*
 * PCI I/O window base for host 1 (VT8231 bus). PCI I/O addresses
 * appear on the CPU side starting here. Port 0x1F0 on the PCI bus
 * is reachable via CPU memory address 0xFE0001F0.
 */
#define PCI1_IO_BASE  0xFE000000u

/*
 * atadisk.c I/O callback signature:
 *    Retcode (*read) (uInt addr, void *value, int size);
 *    Retcode (*write)(uInt addr, Int value,   int size);
 * `addr` is the ATA-register PCI I/O port (e.g. 0x1F0 + DATA); `size`
 * is 1 or 2 bytes. We add PCI1_IO_BASE to route through the host
 * bridge's I/O window.
 */
static Retcode
pegasos2_ide_io_read(uInt addr, void *value, int size)
{
	uint32_t mmio = PCI1_IO_BASE + (addr & 0xFFFFu);

	switch (size) {
	case 1:
		*(uByte *)value = mmio_read8(mmio);
		return NO_ERROR;
	case 2:
		*(uShort *)value = mmio_read16_le(mmio);
		return NO_ERROR;
	default:
		return E_ABORT;
	}
}

static Retcode
pegasos2_ide_io_write(uInt addr, Int value, int size)
{
	uint32_t mmio = PCI1_IO_BASE + (addr & 0xFFFFu);

	switch (size) {
	case 1:
		mmio_write8(mmio, (uint8_t)value);
		return NO_ERROR;
	case 2:
		mmio_write16_le(mmio, (uint16_t)value);
		return NO_ERROR;
	default:
		return E_ABORT;
	}
}

/*
 * Simple controller-level methods. atadisk.c needs a parent package
 * whose methods table supports open/close and (for ATAPI DMA)
 * dma-alloc/dma-free. We don't do DMA yet, so those return E_ABORT
 * -- init_drive's path never consults them for PIO-mode IDENTIFY,
 * which is all M2 requires.
 */
C(f_ide_open)
{
	IFCKSP(e, 0, 1);
	PUSH(e, FTRUE);
	return NO_ERROR;
}

C(f_ide_close)
{
	return NO_ERROR;
}

/*
 * decode-unit (str len -- phys.lo phys.hi): parse "CTLR,ID" where
 * CTLR is 0 (primary) or 1 (secondary) and ID is 0 (master) or 1
 * (slave). This matches the unit-addr convention atadisk.c's
 * probe_ata_disk() encodes via its `reg = <ctlr id 0>` property.
 * Required for resolve_path to reach "/pci@.../ide@c,1/cd@0".
 */
C(f_ide_decode_unit)
{
	Byte *str;
	Int slen;
	Cell hi = 0, lo = 0, err;

	IFCKSP(e, 2, 2);
	POP(e, slen);
	POPT(e, str, Byte *);
	setstrlen(&str, &slen);

	parse_number(16, &str, &slen, &hi, &err, FALSE);

	if (slen && *str == ',') {
		str++; slen--;
		parse_number(16, &str, &slen, &lo, &err, FALSE);
	}

	PUSH(e, lo);
	PUSH(e, hi);
	return NO_ERROR;
}

/*
 * encode-unit (phys.lo phys.hi -- str len): inverse of decode-unit.
 * "CTLR,ID" in hex.
 */
C(f_ide_encode_unit)
{
	static Byte buf[32];
	Cell hi, lo;

	IFCKSP(e, 2, 2);
	POP(e, hi);
	POP(e, lo);

	bprintf((char *)buf, "%x,%x", (unsigned int)hi, (unsigned int)lo);

	PUSHP(e, buf);
	PUSH(e, strlen((char *)buf));
	return NO_ERROR;
}

/*
 * dma-alloc (size -- buf) / dma-free (buf size --)
 *
 * The deblocker (/packages/deblocker) and disk-label
 * (/packages/disk-label) both ask their grandparent (= this IDE
 * controller package) for a DMA-capable buffer when opening. On
 * Pegasos2 with no IOMMU and with MV64361-routed PCI DMA hitting
 * identity-mapped DRAM, "DMA-capable" is identical to "normal
 * malloc" -- the device sees the same physical address we see.
 * atadisk.c's PIO path never actually invokes DMA across the bus
 * (READ/WRITE commands transfer via the DATA register); the
 * buffer is just sector staging, so any reachable heap is fine.
 *
 * If we add real PCI bus-master DMA later (UDMA mode), this
 * becomes the right hook point for a cache-coherent allocator.
 */
C(f_ide_dma_alloc)
{
	Cell size;
	Byte *buf;

	IFCKSP(e, 1, 1);
	POP(e, size);
	buf = (Byte *)malloc((size_t)size);
	if (buf == NULL)
		return E_OUT_OF_MEMORY;
	PUSHP(e, buf);
	return NO_ERROR;
}

C(f_ide_dma_free)
{
	Cell size;
	Byte *buf;

	IFCKSP(e, 2, 0);
	POP(e, size);
	POPT(e, buf, Byte *);
	(void)size;
	free(buf);
	return NO_ERROR;
}

C(f_ide_selftest)
{
	IFCKSP(e, 0, 1);
	PUSH(e, FFALSE);  /* 0 = pass */
	return NO_ERROR;
}

static const Initentry ide_ctlr_methods[] = {
	{ (Byte *)"open",        f_ide_open,        INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"close",       f_ide_close,       INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"decode-unit", f_ide_decode_unit, INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"encode-unit", f_ide_encode_unit, INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"dma-alloc",   f_ide_dma_alloc,   INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"dma-free",    f_ide_dma_free,    INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"selftest",    f_ide_selftest,    INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ NULL, NULL, INVALID_FCODE, F_NONE, T_FUNC HELP("") },
};

/*
 * Walk /pci@80000000's children searching for a class-code 0x01 01 XX
 * package -- that's the IDE controller. Return NULL if absent.
 * Iteration is deliberately by class-code rather than by path lookup
 * (finddevice "/pci@.../ide@c,1") because resolve_path requires a
 * decode-unit method on the /pci@ bus package, which M1 deliberately
 * didn't install (the PCI-level decode-unit is SF's f_pci_decode_unit
 * from pci.c, ~100 LOC; deferred until a milestone that needs
 * path-level PCI navigation from OS clients).
 */
Package *
find_ide_controller(Environ *e)
{
	Package *pci_bus =
		find_device(e, (Byte *)"/pci@80000000", CSTR);
	if (pci_bus == NULL)
		return NULL;

	Package *c;
	for (c = pci_bus->children; c != NULL; c = c->link) {
		Int classcode;
		if (prop_get_int(c->props, (Byte *)"class-code", CSTR,
				 &classcode) != NO_ERROR)
			continue;
		/* Class 01 (storage), subclass 01 (IDE). Progif (low 8
		 * bits) varies between legacy-compat/native-mode/bus-
		 * master so we mask it out. */
		if (((uInt)classcode & 0xFFFF00u) == 0x010100u)
			return c;
	}
	return NULL;
}

/*
 * Read the BAR at `offset` on (bus, dev, fn) and return the PCI-I/O
 * base it resolves to, stripped of flag bits, masked to 16 bits
 * (the PCI I/O window on Pegasos2 is 64 KiB). Returns `fallback` if
 * the BAR is zero or not an I/O BAR.
 */
static uint32_t
read_io_bar(int host, uint8_t bus, uint8_t dev, uint8_t fn,
	    uint8_t offset, uint32_t fallback)
{
	uint32_t bar = pci_cfg_read32(host, bus, dev, fn, offset);
	if ((bar & 1u) == 0)              /* not an I/O BAR */
		return fallback;
	uint32_t port = bar & 0xFFFFFFFCu;
	if (port == 0)                     /* unimplemented / legacy mode */
		return fallback;
	return port & 0xFFFFu;
}

/*
 * probe_ata_disks signature is external to atadisk.c; atadisk.c
 * doesn't ship a header for it. Keep this prototype in sync with
 * upstream/smartfirmware/bin/of/isa/atadisk.c:1222.
 */
extern Retcode probe_ata_disks(Environ *e, uInt reg[4], Package *pkg,
	Retcode (*read)(uInt addr, void *value, int size),
	Retcode (*write)(uInt addr, Int value, int size));

/*
 * install_ide_driver: called from install_list[] after
 * install_pci_tree. Attaches to the VT8231 IDE controller node and
 * instantiates child cd@/disk@ packages for each populated drive.
 *
 * On QEMU pegasos2 with `-cdrom foo.iso`, the CD appears as a child
 * of the secondary channel (reg[2]/reg[3]) because QEMU's
 * pci_ide_create_devs() places the first -cdrom on ide.1 = secondary
 * master. Primary channel typically has no drives in the default
 * configuration (bare QEMU) and probe_ata_disks returns E_NO_DEVICE
 * for each, moving on.
 */
CC(install_ide_driver)
{
	Package *ide = find_ide_controller(e);
	if (ide == NULL) {
		/* No IDE controller -- not an error. Some future Pegasos2
		 * variant may not have a VT8231, and users may disable
		 * the IDE function via nvedit. */
		return NO_ERROR;
	}

	/*
	 * Annotate the IDE node and set its address/size cells. ATA
	 * disk packages use reg = <ctlr id 0> (2 address cells + 1
	 * size cell) per atadisk.c:probe_ata_disk line 1198-1200.
	 * Setting these explicitly matches the reg shape so
	 * do_decode_phys/do_decode_reg in SF's resolve_path chain
	 * extract the right number of cells.
	 */
	prop_set_str(ide->props, (Byte *)"device_type", CSTR,
		     (Byte *)"ide", CSTR);
	prop_set_int(ide->props, (Byte *)"#address-cells", CSTR, 2);
	prop_set_int(ide->props, (Byte *)"#size-cells",    CSTR, 1);
	Retcode mr = init_entries(e, ide->dict, ide_ctlr_methods);
	if (mr != NO_ERROR) return mr;

	/*
	 * Read BAR0-3 to determine port layout. In native mode BARs are
	 * programmed; in legacy-compat mode (QEMU default for VT8231)
	 * BAR0-3 read as 0 and we use the ATA legacy port numbers.
	 *
	 * We look up the controller's bus/dev/fn by decoding its reg
	 * property's first entry (the config-space descriptor whose
	 * physhi encodes BDF per IEEE-1275 PCI binding).
	 */
	/*
	 * reg is a binary property (not a string), so prop_get_str
	 * truncates at any NUL byte -- notably the leading 0x00 of
	 * the config-space physhi for a type-0 PCI device. Go
	 * straight to the Entry instead.
	 */
	Entry *regent = find_table(ide->props, (Byte *)"reg", CSTR);
	if (regent == NULL || regent->len < (Int)sizeof(Int))
		return E_ABORT;
	Byte *regp = (Byte *)regent->v.array;
	Int reglen = regent->len;

	uint32_t physhi = 0;
	{
		/* First cell of reg = physhi of config-space descriptor. */
		physhi = ((uInt)regp[0] << 24) | ((uInt)regp[1] << 16) |
			 ((uInt)regp[2] << 8)  | (uInt)regp[3];
	}
	uint8_t bus = (physhi >> 16) & 0xFFu;
	uint8_t dev = (physhi >> 11) & 0x1Fu;
	uint8_t fn  = (physhi >> 8)  & 0x07u;
	int host = PCI_HOST_1;   /* /pci@80000000 is always host 1 on Pegasos2 */

	uInt reg[4];
	reg[0] = read_io_bar(host, bus, dev, fn, 0x10, 0x1F0u);  /* BAR0 or 0x1F0 */
	reg[1] = read_io_bar(host, bus, dev, fn, 0x14, 0x3F6u);  /* BAR1 or 0x3F6 */
	reg[2] = read_io_bar(host, bus, dev, fn, 0x18, 0x170u);  /* BAR2 or 0x170 */
	reg[3] = read_io_bar(host, bus, dev, fn, 0x1C, 0x376u);  /* BAR3 or 0x376 */

	/*
	 * probe_ata_disks (upstream/.../atadisk.c:1222) walks all four
	 * slots: (channel 0, master), (channel 0, slave), and -- when
	 * reg[2] is non-zero, which our BAR-fallback always makes true
	 * -- (channel 1, master), (channel 1, slave). Each responding
	 * slot gets a disk@CTL,ID or cd@CTL,ID child node attached to
	 * `ide`. Slots that don't respond are silently skipped.
	 */
	Retcode pret = probe_ata_disks(e, reg, ide,
	                               pegasos2_ide_io_read,
	                               pegasos2_ide_io_write);

	/*
	 * Print the resulting drive inventory so users can see at a
	 * glance what the firmware decided was attached. Especially
	 * useful for multi-drive configs where smart-boot's pick may
	 * not be the user's expectation.
	 */
	/*
	 * Push a one-line per-device summary to UART1 directly. We
	 * can't go via cprintf here: install_packages runs before SF
	 * opens the output console (e->screen == NULL until
	 * `install-console` later in main()), and although display_text
	 * does fall back to failsafe_write in that state, the SF
	 * paginator hooks (e->paginate / e->debug) and capture buffer
	 * race with the early install ordering and silently drop the
	 * output on QEMU. Going straight to UART1 sidesteps the lot --
	 * users see the inventory regardless of console state.
	 */
	int found = 0;
	for (Package *c = ide->children; c != NULL; c = c->link) {
		Byte *nm; Int nl;
		if (prop_get_str(c->props, (Byte *)"name", CSTR,
		                 &nm, &nl) != NO_ERROR)
			continue;
		Int ctlr = -1, id = -1;
		Entry *re = find_table(c->props, (Byte *)"reg", CSTR);
		if (re != NULL && re->len >= 2 * (Int)sizeof(Int)) {
			Byte *r = (Byte *)re->v.array;
			ctlr = ((Int)r[0] << 24) | ((Int)r[1] << 16) |
			       ((Int)r[2] << 8)  |  (Int)r[3];
			id   = ((Int)r[4] << 24) | ((Int)r[5] << 16) |
			       ((Int)r[6] << 8)  |  (Int)r[7];
		}
		Int is_atapi =
			(find_table(c->props, (Byte *)"atapi", CSTR) != NULL);
		if (found == 0)
			uart_puts(UART1_BASE, "IDE devices found:\n");
		uart_puts(UART1_BASE, "  ");
		for (Int j = 0; j < nl; j++)
			uart_putc(UART1_BASE, (char)nm[j]);
		uart_putc(UART1_BASE, '@');
		uart_put_hex32(UART1_BASE, (uint32_t)ctlr);
		uart_putc(UART1_BASE, ',');
		uart_put_hex32(UART1_BASE, (uint32_t)id);
		uart_puts(UART1_BASE, is_atapi ? "  (ATAPI)\n" : "  (ATA)\n");
		found++;
	}
	if (found == 0)
		uart_puts(UART1_BASE, "IDE devices: none attached\n");

	/* M5: enable bus-master DMA on every ATA child. ATAPI keeps
	 * the upstream PIO method (DMA-aware ATAPI is more involved
	 * and isn't on the v0.6 plan). */
	(void)ide_dma_install(e, ide);

	return pret;
}

/* ---------- test-ide-probe Forth word ---------- */

static const char *
atapi_type_str(Int classcode)
{
	/* Not currently used -- the atapi flag from atadisk.c is what
	 * matters; keep as a future hook. */
	(void)classcode;
	return "";
}

/*
 * Read the (ctlr, id) pair from a disk child's `reg` property,
 * which atadisk.c's probe_ata_disk encodes as <ctlr id 0> per
 * isa/atadisk.c:1198-1200. reg is a binary property and its first
 * byte is typically 0, so go straight to the Entry rather than
 * prop_get_str (which would truncate at the leading NUL).
 */
static int disk_reg(Package *c, Int *ctlr, Int *id)
{
	Entry *re = find_table(c->props, (Byte *)"reg", CSTR);
	if (re == NULL || re->len < 2 * (Int)sizeof(Int))
		return 0;
	Byte *r = (Byte *)re->v.array;
	*ctlr = ((Int)r[0] << 24) | ((Int)r[1] << 16) |
	        ((Int)r[2] << 8)  | (Int)r[3];
	*id   = ((Int)r[4] << 24) | ((Int)r[5] << 16) |
	        ((Int)r[6] << 8)  | (Int)r[7];
	return 1;
}

/*
 * Walk the IDE controller package's children (disk@ / cd@ packages
 * created by probe_ata_disks) and print identity info pulled from
 * each package's properties.
 */
CC(f_test_ide_probe)
{
	Package *ide = find_ide_controller(e);
	if (ide == NULL) {
		cprintf(e, "test-ide-probe: no IDE controller found\n");
		return NO_ERROR;
	}

	Package *c;
	int found = 0;
	for (c = ide->children; c != NULL; c = c->link) {
		Byte *name;
		Int namelen;
		if (prop_get_str(c->props, (Byte *)"name", CSTR,
				 &name, &namelen) != NO_ERROR) {
			name = (Byte *)"?";
			namelen = 1;
		}

		Int ctlr = -1, id = -1;
		disk_reg(c, &ctlr, &id);

		Int is_atapi =
			(find_table(c->props, (Byte *)"atapi", CSTR) != NULL)
			? 1 : 0;

		cprintf(e, "  %S@%x,%x  %s\n",
			name, namelen, (Int)ctlr, (Int)id,
			is_atapi ? "ATAPI" : "ATA");

		(void)atapi_type_str(0);
		found++;
	}

	if (!found)
		cprintf(e, "test-ide-probe: no drives attached\n");
	return NO_ERROR;
}

/*
 * test-read-block (--)
 *
 * Finds the first ATAPI CD under the VT8231 IDE controller, opens
 * it via open-dev on the full device path, seeks to LBA 16 (byte
 * offset 0x8000), reads 2048 bytes via the deblocker, and verifies
 * the ISO9660 primary volume descriptor signature: byte 0 = 0x01
 * (VD type 1), bytes 1-5 = "CD001", byte 6 = 0x01 (VD version).
 *
 * This exercises the full block I/O chain that M4's ISO9660 and
 * M6's boot-from-file will depend on:
 *    open-dev  -> atadisk's f_ata_disk_open
 *       -> $open-package deblocker (requires /packages/deblocker)
 *       -> $open-package disk-label (requires /packages/disk-label)
 *    seek      -> deblocker's f_deblock_seek (byte cursor)
 *    read      -> deblocker's f_deblock_read
 *       -> atadisk's f_ata_disk_read_blocks (ATAPI READ_BIG CDB)
 *       -> pegasos2_ide_io_read / pegasos2_ide_io_write
 *
 * The CD's bus position depends on what the user passed to QEMU
 * (-cdrom goes to the first free slot, usually secondary master on
 * pegasos2 = cd@1,0), so we search rather than hard-code.
 *
 * Success line:  "test-read-block: OK CD001 @ LBA 16 on <path>"
 * Failure line:  "test-read-block: FAIL <reason>"
 */

static int cdsig_ok(const uByte *b)
{
	return b[0] == 0x01 &&
	       b[1] == 'C'  && b[2] == 'D'  && b[3] == '0' &&
	       b[4] == '0'  && b[5] == '1'  && b[6] == 0x01;
}

/*
 * Locate the first ATAPI (CD) child under the IDE controller.
 * Returns NULL if no CD attached.
 */
/* Non-static so smart_boot.c can iterate ATAPI media directly. */
Package *find_first_cd(Environ *e)
{
	Package *ide = find_ide_controller(e);
	if (ide == NULL) return NULL;
	Package *c;
	for (c = ide->children; c != NULL; c = c->link) {
		if (find_table(c->props, (Byte *)"atapi", CSTR) != NULL)
			return c;
	}
	return NULL;
}

/*
 * Locate the first non-ATAPI (i.e. ATA hard disk) child under the
 * IDE controller. Returns NULL if no HD attached.
 */
static Package *find_first_hd(Environ *e)
{
	Package *ide = find_ide_controller(e);
	if (ide == NULL) return NULL;
	Package *c;
	for (c = ide->children; c != NULL; c = c->link) {
		if (find_table(c->props, (Byte *)"atapi", CSTR) == NULL)
			return c;
	}
	return NULL;
}

CC(f_test_read_block)
{
	Instance *inst = NULL;
	Retcode r;
	Cell status;
	Cell actual;
	static uByte buf[2048];
	Byte pathbuf[STR_SIZE];

	Package *cd = find_first_cd(e);
	if (cd == NULL) {
		cprintf(e, "test-read-block: FAIL no ATAPI drive attached\n");
		return NO_ERROR;
	}
	if (!get_device_name(e, cd, pathbuf)) {
		cprintf(e, "test-read-block: FAIL could not get device name\n");
		return NO_ERROR;
	}

	/* open-dev ( path-str path-len -- ihandle | 0 )  */
	PUSHP(e, pathbuf);
	PUSH(e, (Cell)strlen((char *)pathbuf));
	r = execute_word(e, "open-dev");
	if (r != NO_ERROR) {
		cprintf(e, "test-read-block: FAIL open-dev %s rc=%d\n",
			(char *)pathbuf, (Int)r);
		return NO_ERROR;
	}
	POPT(e, inst, Instance *);
	if (inst == NULL) {
		cprintf(e, "test-read-block: FAIL open returned NULL "
			"(path=%s)\n", (char *)pathbuf);
		return NO_ERROR;
	}

	/* seek ( pos.lo pos.hi -- status )
	 * LBA 16 * 2048 bytes/block = 32768 = 0x8000 bytes. */
	PUSH(e, (Cell)0x8000);
	PUSH(e, (Cell)0);
	r = execute_method_name(e, inst, (Byte *)"seek", CSTR);
	if (r != NO_ERROR) {
		cprintf(e, "test-read-block: FAIL seek rc=%d\n", (Int)r);
		goto close_and_return;
	}
	POP(e, status);
	if (status != 0 && status != 1) {
		cprintf(e, "test-read-block: FAIL seek status=%d\n",
			(Int)status);
		goto close_and_return;
	}

	/* read ( addr len -- actual ) */
	memset(buf, 0, sizeof buf);
	PUSHP(e, buf);
	PUSH(e, (Cell)sizeof buf);
	r = execute_method_name(e, inst, (Byte *)"read", CSTR);
	if (r != NO_ERROR) {
		cprintf(e, "test-read-block: FAIL read rc=%d\n", (Int)r);
		goto close_and_return;
	}
	POP(e, actual);

	if (actual != (Cell)sizeof buf) {
		cprintf(e, "test-read-block: FAIL read actual=%d "
			"(expected 2048)\n", (Int)actual);
		goto close_and_return;
	}

	if (cdsig_ok(buf)) {
		cprintf(e, "test-read-block: OK CD001 @ LBA 16 on %s\n",
			(char *)pathbuf);
	} else {
		cprintf(e,
			"test-read-block: FAIL bad signature "
			"%02X %02X%02X%02X%02X%02X %02X on %s\n",
			(Int)buf[0], (Int)buf[1], (Int)buf[2], (Int)buf[3],
			(Int)buf[4], (Int)buf[5], (Int)buf[6],
			(char *)pathbuf);
	}

close_and_return:
	PUSHP(e, inst);
	(void)execute_word(e, "close-dev");
	return NO_ERROR;
}

/*
 * test-iso-ls (--)
 *
 * Finds the first ATAPI CD, open-devs it, then invokes the
 * `list-files` method on the disk instance with "/" as the
 * directory argument. disklbl's f_disklbl_list_files
 * (disklbl.c:124) passes that through file_system(FS_LIST, ...)
 * which iterates g_filesys[] and calls iso9660's action with
 * FS_LIST, which walks the ISO root directory and prints one
 * line per entry:
 *     "[NAME]"     for subdirectories
 *     "SIZE NAME"  for files
 * plus a one-line header:
 *     "ISO-9660 filesystem:  System-ID: "..."  Volume-ID: "..."
 *
 * Success line (printed after the listing):
 *     "test-iso-ls: OK listed ISO9660 root on <path>"
 * Failure line:
 *     "test-iso-ls: FAIL <reason>"
 */
CC(f_test_iso_ls)
{
	Instance *inst = NULL;
	Retcode r;
	Byte pathbuf[STR_SIZE];

	Package *cd = find_first_cd(e);
	if (cd == NULL) {
		cprintf(e, "test-iso-ls: FAIL no ATAPI drive attached\n");
		return NO_ERROR;
	}
	if (!get_device_name(e, cd, pathbuf)) {
		cprintf(e, "test-iso-ls: FAIL could not get device name\n");
		return NO_ERROR;
	}

	PUSHP(e, pathbuf);
	PUSH(e, (Cell)strlen((char *)pathbuf));
	r = execute_word(e, "open-dev");
	if (r != NO_ERROR) {
		cprintf(e, "test-iso-ls: FAIL open-dev rc=%d\n", (Int)r);
		return NO_ERROR;
	}
	POPT(e, inst, Instance *);
	if (inst == NULL) {
		cprintf(e, "test-iso-ls: FAIL open returned NULL\n");
		return NO_ERROR;
	}

	/* list-files (args alen --) -- disklbl's f_disklbl_list_files
	 * inserted into the disk package's dict by disklbl's open.
	 * Pass "/" as the directory path; disklbl passes it to
	 * file_system(FS_LIST, ...) which fires iso9660's FS_LIST. */
	PUSHP(e, (Byte *)"/");
	PUSH(e, (Cell)1);
	r = execute_method_name(e, inst, (Byte *)"list-files", CSTR);
	if (r != NO_ERROR) {
		cprintf(e, "test-iso-ls: FAIL list-files rc=%d\n", (Int)r);
	} else {
		cprintf(e, "test-iso-ls: OK listed ISO9660 root on %s\n",
			(char *)pathbuf);
	}

	PUSHP(e, inst);
	(void)execute_word(e, "close-dev");
	return NO_ERROR;
}

/*
 * install_aliases (--)
 *
 * Populates the canonical Pegasos2 device aliases (/aliases/cd
 * and /aliases/hd) with the paths of whichever CD / HD the IDE
 * driver actually found. AOS4's `boot cd amigaboot.of` and
 * Linux's `boot hd vmlinux` reference these aliases via SF's
 * resolve_path alias-expansion (device.c:1359-1387: when the
 * leading path component doesn't start with '/', look it up in
 * /aliases->props before treating it as a literal path).
 *
 * /aliases is created by SF's install_packages BEFORE install_list
 * runs (table.c:1344), so we just call prop_set_str on
 * e->aliases->props -- no install_aliases() Forth word, no
 * install-list scaffolding.
 *
 * Run AFTER install_ide_driver so the cd@/disk@ children exist
 * to derive paths from. If no drive is present we silently skip
 * that alias rather than installing a broken one (resolve_path
 * would fail at the missing leaf with "no such device").
 */
CC(install_aliases)
{
	Byte pathbuf[STR_SIZE];

	Package *cd = find_first_cd(e);
	if (cd != NULL && get_device_name(e, cd, pathbuf)) {
		prop_set_str(e->aliases->props, (Byte *)"cd", CSTR,
			     pathbuf, CSTR);
		/* `cdrom` is a widely-used alternate spelling some
		 * bootloaders look for; alias to the same path. */
		prop_set_str(e->aliases->props, (Byte *)"cdrom", CSTR,
			     pathbuf, CSTR);
	}

	Package *hd = find_first_hd(e);
	if (hd != NULL && get_device_name(e, hd, pathbuf)) {
		prop_set_str(e->aliases->props, (Byte *)"hd", CSTR,
			     pathbuf, CSTR);
		/* `disk` -- generic alias used by some OS loaders. */
		prop_set_str(e->aliases->props, (Byte *)"disk", CSTR,
			     pathbuf, CSTR);
	}

	return NO_ERROR;
}

/*
 * test-aliases (--)
 *
 * Print every property on /aliases (skipping the implicit `name`).
 * Smoke test for install_aliases: we expect to see "cd" and
 * "cdrom" on a -cdrom QEMU run, plus "hd"/"disk" if a HD is also
 * attached.
 */
CC(f_test_aliases)
{
	Entry *ent;
	int n = 0;

	cprintf(e, "test-aliases:\n");
	for (ent = e->aliases->props->list; ent != NULL; ent = ent->link) {
		if (compare_strs(ent->name, PSTR, (Byte *)"name", CSTR))
			continue;
		Byte *str;
		Int len;
		if (prop_get_str(e->aliases->props, ent->name, PSTR,
				 &str, &len) == NO_ERROR) {
			cprintf(e, "  %-12P %S\n",
				ent->name, str, len);
			n++;
		}
	}
	if (n == 0)
		cprintf(e, "  (none)\n");
	return NO_ERROR;
}
