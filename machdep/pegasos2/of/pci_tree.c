/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  pegasos2 PCI device-tree installer.
 *
 *  install_pci_tree creates /pci@80000000 (VT8231 primary bus, QEMU
 *  PCI_HOST_1) and /pci@c0000000 (secondary expansion bus, QEMU
 *  PCI_HOST_0) as top-level children of /, following the shape of
 *  QEMU's reverse-engineered pegasos2.dts (pc-bios/dtb/pegasos2.dts).
 *
 *  Phase-1's pci_walker.c has already sized BARs, assigned addresses,
 *  enabled command-register bits, and programmed bridge windows. This
 *  installer just re-reads the live config space to build the device-
 *  tree nodes that OS clients (amigaboot.of, linux, ...) expect via
 *  the IEEE-1275 client interface. It does NOT touch BAR assignments.
 *
 *  Per-device properties match IEEE-1275 PCI binding + the QEMU DTS
 *  shape:
 *      vendor-id, device-id, class-code, revision-id
 *      subsystem-id, subsystem-vendor-id (swapped, per the Pegasos2
 *        firmware convention preserved in QEMU)
 *      interrupts    (PCI_INTERRUPT_PIN value, if non-zero)
 *      reg           (1 config-space entry + 1 entry per implemented BAR)
 *      assigned-addresses  (current BAR values decoded as PCI addrs)
 *
 *  Node naming follows the QEMU convention:
 *      <class-derived-name>@<slot>[,<func>]
 *  where the suffix ",<func>" is only present when func != 0.
 *  class-derived-name comes from class_to_name() below.
 *
 *  Per IEEE-1275 PCI binding §2.2.1.1, the /pci@X bus itself has:
 *      device_type = "pci"
 *      #address-cells = 3, #size-cells = 2
 *      clock-frequency = 33.3 or 66.7 MHz
 *      bus-range = <0 0>   (single-bus, PPB inspection is a later M)
 *      ranges = <I/O window, MEM window>   (6 cells each, since root
 *                                           has #address-cells = 1)
 *      reg = <cpu_base cpu_size>
 *      pci-bridge-number = 0 or 1
 *      8259-interrupt-acknowledge = 0xF1000CB4 (PCI_HOST_1 only;
 *                                   GPP31 cascade read for i8259 ack)
 */

#include "defs.h"

#include "mv64361.h"
#include "pegasos2.h"
#include "pci.h"

/* PCI config register offsets we read during tree-building. These are
 * the standard type-0 header layout per PCI 2.3 §6.1. */
#define CFG_VENDOR_ID        0x00u
#define CFG_DEVICE_ID        0x02u
#define CFG_CLASS_REVISION   0x08u
#define CFG_HEADER_TYPE      0x0Eu
#define CFG_BAR0             0x10u
#define CFG_SUBSYS_VENDOR    0x2Cu
#define CFG_SUBSYS_ID        0x2Eu
#define CFG_ROM_BASE         0x30u
#define CFG_INTERRUPT_LINE   0x3Cu
#define CFG_INTERRUPT_PIN    0x3Du

#define HEADER_TYPE_MASK     0x7Fu
#define HEADER_TYPE_MULTIFN  0x80u
#define HTYPE_NORMAL         0x00u
#define HTYPE_BRIDGE         0x01u

#define PCI_VENDOR_INVALID   0xFFFFu

#define NUM_STD_BARS         6

/* BAR decoding. */
#define BAR_SPACE_IO         0x1u
#define BAR_MEM_TYPE_MASK    0x6u
#define BAR_MEM_TYPE_32      0x0u
#define BAR_MEM_TYPE_64      0x4u
#define BAR_MEM_PREFETCH     0x8u

/*
 * IEEE-1275 PCI binding physhi layout, bit-for-bit:
 *   bit 31    = n  (non-relocatable)
 *   bit 30    = p  (prefetchable)
 *   bit 29    = t  (aliased)
 *   bits 25-24 = ss (space: 0=cfg, 1=IO, 2=MEM32, 3=MEM64)
 *   bits 23-16 = bus
 *   bits 15-11 = dev
 *   bits 10-8  = func
 *   bits  7-0  = register offset
 */
static uint32_t physhi_mk(unsigned n, unsigned p, unsigned ss,
			  unsigned bus, unsigned dev, unsigned func,
			  unsigned reg)
{
	return ((n & 1u) << 31) | ((p & 1u) << 30) | ((ss & 7u) << 24) |
	       ((bus & 0xFFu) << 16) | ((dev & 0x1Fu) << 11) |
	       ((func & 7u) << 8) | (reg & 0xFFu);
}

/*
 * class_to_name -- map the 24-bit class-code (+ vendor/device for a
 * few VT8231 sub-functions) to the canonical device-tree node name.
 * Falls back to the generic "pciVVVV,DDDD" form when nothing matches.
 * Matches QEMU's pegasos.c device_map + class-based fallback.
 *
 * Buffer must be at least 16 bytes for the fallback form.
 */
static void class_to_name(uint32_t classcode, uint16_t vendid,
			  uint16_t devid, char *buf, Int buflen)
{
	const char *name = NULL;

	/* VT8231 sub-function matches first (QEMU device_map shape). */
	if (vendid == 0x1106) {
		switch (devid) {
		case 0x0571: name = "ide";      break;
		case 0x3038: name = "usb";      break;
		case 0x3044: name = "firewire"; break;
		case 0x3058: name = "sound";    break;
		case 0x8235: name = "other";    break;  /* PM / SMBus fn4 */
		default: break;
		}
	}

	/* Class-based fallback: (base << 16) | (sub << 8) | progif. */
	if (!name) {
		uint32_t base = (classcode >> 16) & 0xFFu;
		uint32_t sub  = (classcode >> 8)  & 0xFFu;
		switch (base) {
		case 0x01:   /* mass storage */
			if (sub == 0x01)      name = "ide";
			else if (sub == 0x07) name = "scsi";
			else                  name = "storage";
			break;
		case 0x02:   /* network */
			name = "ethernet";
			break;
		case 0x03:   /* display */
			name = "display";
			break;
		case 0x04:   /* multimedia */
			name = "sound";
			break;
		case 0x06:   /* bridge */
			if (sub == 0x01)      name = "isa";
			else if (sub == 0x04) name = "pci";
			else                  name = "other";
			break;
		case 0x0C:   /* serial bus */
			if (sub == 0x03)      name = "usb";
			else if (sub == 0x00) name = "firewire";
			else                  name = "other";
			break;
		default:
			name = NULL;
		}
	}

	if (name) {
		Int n = 0;
		while (name[n] && n < buflen - 1) { buf[n] = name[n]; n++; }
		buf[n] = '\0';
	} else {
		bprintf(buf, "pci%Cx,%Cx", vendid, devid);
	}
}

/*
 * Unit-address per PCI binding: <dev>[,<func>]. Matches QEMU DTS
 * which omits ",0" when func == 0.
 */
static void format_unit_addr(char *buf, Int buflen, uint8_t dev, uint8_t fn)
{
	if (fn == 0)
		bprintf(buf, "%Cx", dev);
	else
		bprintf(buf, "%Cx,%Cx", dev, fn);
}

/*
 * Build the genus-only node name: "ide", "isa", "usb", etc. The
 * unit-addr suffix ("@c,1") is NOT baked into the `name` property;
 * it is derived at display/match time by SF's package_name() which
 * calls decode-phys(reg) + encode-unit(parent) -- our f_pci_encode_unit
 * returns "c,1" from the reg property's config-space physhi, and
 * package_name appends "@" between them to produce the canonical
 * "ide@c,1" form.
 *
 * An earlier draft baked the unit-addr into the name ("ide@c,1"),
 * which caused package_name to emit "ide@c,1@c,1" (doubled suffix)
 * AND broke path navigation (name_match compared requested "ide"
 * to stored "ide@c,1" and failed). IEEE-1275 §3.5 mandates that
 * the name property is the genus only.
 */
static void build_node_name(uint32_t classcode, uint16_t vendid,
			    uint16_t devid, uint8_t dev, uint8_t fn,
			    char *buf, Int buflen)
{
	(void)dev; (void)fn;
	class_to_name(classcode, vendid, devid, buf, buflen);
}

/*
 * Non-destructive BAR-size probe. Same idea as pci_walker.c's
 * pci_bar_probe but read-only where possible: we preserve the
 * current BAR value around a write-0xFFFFFFFF / read-back / restore
 * dance. Returns (size, original-BAR-low-bits, is-64).
 *
 * Important: this happens AFTER phase-1 assignment, so the "original"
 * value we restore is the live-programmed address. A bus glitch
 * during the probe would break the device temporarily; we accept
 * that -- running at the ok prompt with no in-flight PCI I/O.
 */
static uint32_t bar_probe(int host, uint8_t bus, uint8_t dev, uint8_t fn,
			  uint8_t offset, uint32_t *flags_out, int *is_64_out)
{
	uint32_t saved, set, addr_mask, size;

	*is_64_out = 0;

	saved = pci_cfg_read32(host, bus, dev, fn, offset);

	/* Detect space + type before overwriting. */
	uint32_t low_bits;
	if (saved & BAR_SPACE_IO) {
		low_bits  = saved & 0x3u;
		addr_mask = 0xFFFFFFFCu;
	} else {
		low_bits  = saved & 0xFu;
		addr_mask = 0xFFFFFFF0u;
		if ((saved & BAR_MEM_TYPE_MASK) == BAR_MEM_TYPE_64)
			*is_64_out = 1;
	}

	/* Preserve the low non-address bits during the probe (PCI 2.3
	 * §6.2.5.1). Restore saved value afterwards. */
	pci_cfg_write32(host, bus, dev, fn, offset,
			(saved & ~addr_mask) | addr_mask);
	set = pci_cfg_read32(host, bus, dev, fn, offset);
	pci_cfg_write32(host, bus, dev, fn, offset, saved);

	if (set == 0) {
		*flags_out = 0;
		return 0;   /* BAR unimplemented */
	}

	size = (~(set & addr_mask)) + 1u;
	*flags_out = low_bits;
	return size;
}

/*
 * prop_encode_int writes 4 big-endian bytes starting at `arr` and
 * increments `*len` by 4, but does NOT advance `arr` itself. Callers
 * are expected to pass &arr[*len] as the base for successive writes
 * (see pci.c:pci_encode_reg_prop for the canonical pattern). Wrap
 * that here for clarity.
 */
static void enc_int(Byte *base, Int *plen, Int val)
{
	prop_encode_int(&base[*plen], plen, val);
}

/*
 * Encode a single 5-cell PCI reg/assigned-address entry into the
 * buffer, big-endian, advancing *plen.
 */
static void encode_pci_entry(Byte *arr, Int *plen,
			     uint32_t physhi, uint32_t physmid,
			     uint32_t physlo, uint32_t sizehi,
			     uint32_t sizelo)
{
	enc_int(arr, plen, (Int)physhi);
	enc_int(arr, plen, (Int)physmid);
	enc_int(arr, plen, (Int)physlo);
	enc_int(arr, plen, (Int)sizehi);
	enc_int(arr, plen, (Int)sizelo);
}

/*
 * Build the `reg` property: one config-space entry + one entry per
 * implemented BAR describing the BAR's PCI space type + size. This
 * matches QEMU's add_pci_device shape in hw/ppc/pegasos.c.
 *
 * On a 32-bit BAR entry, physmid + physlo are zero (the BAR's
 * position is captured in the register-offset field of physhi, not
 * in the phys.mid/lo cells -- the PCI binding uses the latter for
 * actual addresses, which reg doesn't carry).
 */
static Retcode set_reg_prop(Environ *e, Package *pkg, int host,
			    uint8_t bus, uint8_t dev, uint8_t fn,
			    uint8_t header_type_kind, uint8_t *bars_present)
{
	/* Max entries: 1 config + 6 standard BARs + 1 ROM BAR. */
	Byte buf[8 * 5 * sizeof(Int)];
	Int plen = 0;
	int i;

	*bars_present = 0;

	/* Entry 0: config-space descriptor. ss=0 (config), BDF encoded. */
	encode_pci_entry(buf, &plen,
			 physhi_mk(0, 0, 0, bus, dev, fn, 0),
			 0, 0, 0, 0);

	if (header_type_kind == HTYPE_NORMAL) {
		/* Walk 6 standard BARs. */
		for (i = 0; i < NUM_STD_BARS; i++) {
			uint8_t off = (uint8_t)(CFG_BAR0 + i * 4);
			uint32_t flags;
			int is_64;
			uint32_t sz = bar_probe(host, bus, dev, fn, off,
						&flags, &is_64);
			if (sz == 0) {
				if (is_64) i++;  /* skip high half anyway */
				continue;
			}
			uint32_t ss, pbit = 0;
			if (flags & BAR_SPACE_IO) {
				ss = 1;  /* I/O */
			} else if (is_64) {
				ss = 3;  /* MEM64 */
				if (flags & BAR_MEM_PREFETCH) pbit = 1;
			} else {
				ss = 2;  /* MEM32 */
				if (flags & BAR_MEM_PREFETCH) pbit = 1;
			}
			encode_pci_entry(buf, &plen,
					 physhi_mk(0, pbit, ss, bus, dev, fn,
						   off),
					 0, 0, 0, sz);
			*bars_present |= (uint8_t)(1u << i);
			if (is_64) i++;  /* skip BAR high-half slot */
		}

		/* ROM BAR, if implemented and non-zero size. */
		{
			uint32_t flags;
			int is_64;
			uint32_t sz = bar_probe(host, bus, dev, fn,
						CFG_ROM_BASE, &flags, &is_64);
			(void)flags; (void)is_64;
			if (sz != 0) {
				/* ROM BAR is MEM32 space. */
				encode_pci_entry(buf, &plen,
						 physhi_mk(0, 0, 2, bus, dev,
							   fn, CFG_ROM_BASE),
						 0, 0, 0, sz);
				*bars_present |= 0x80u;
			}
		}
	}

	Byte *prop_buf = prop_alloc(e, plen);
	if (prop_buf == NULL)
		return E_OUT_OF_MEMORY;
	memcpy(prop_buf, buf, plen);
	return add_property(pkg->props, (Byte *)"reg", CSTR, prop_buf, plen);
}

/*
 * Build the `assigned-addresses` property: one entry per implemented
 * BAR whose live config-space value is non-zero. physmid + physlo
 * carry the actual assigned PCI address.
 */
static Retcode set_assigned_addresses(Environ *e, Package *pkg,
				      int host, uint8_t bus, uint8_t dev,
				      uint8_t fn, uint8_t bars_present)
{
	Byte buf[8 * 5 * sizeof(Int)];
	Int plen = 0;
	int i;

	for (i = 0; i < NUM_STD_BARS; i++) {
		if (!(bars_present & (1u << i)))
			continue;
		uint8_t off = (uint8_t)(CFG_BAR0 + i * 4);
		uint32_t low = pci_cfg_read32(host, bus, dev, fn, off);
		uint32_t high = 0;
		uint32_t ss, pbit = 0;
		int is_64 = 0;
		/* Compute size again so we can emit size.hi/lo. Cheap;
		 * this runs once at OF init, not in a hot path. */
		uint32_t flags;
		uint32_t sz = bar_probe(host, bus, dev, fn, off,
					&flags, &is_64);
		if (low & BAR_SPACE_IO) {
			ss = 1;
			low &= 0xFFFFFFFCu;
		} else {
			if (is_64) {
				ss = 3;
				if (flags & BAR_MEM_PREFETCH) pbit = 1;
				high = pci_cfg_read32(host, bus, dev, fn,
						      (uint8_t)(off + 4));
			} else {
				ss = 2;
				if (flags & BAR_MEM_PREFETCH) pbit = 1;
			}
			low &= 0xFFFFFFF0u;
		}
		encode_pci_entry(buf, &plen,
				 physhi_mk(0, pbit, ss, bus, dev, fn, off),
				 high, low, 0, sz);
		if (is_64) i++;
	}

	/* ROM BAR */
	if (bars_present & 0x80u) {
		uint32_t rom = pci_cfg_read32(host, bus, dev, fn, CFG_ROM_BASE);
		uint32_t flags;
		int is_64;
		uint32_t sz = bar_probe(host, bus, dev, fn, CFG_ROM_BASE,
					&flags, &is_64);
		(void)flags; (void)is_64;
		rom &= 0xFFFFF800u;   /* mask off enable bit + reserved */
		if (rom != 0 && sz != 0) {
			encode_pci_entry(buf, &plen,
					 physhi_mk(0, 0, 2, bus, dev, fn,
						   CFG_ROM_BASE),
					 0, rom, 0, sz);
		}
	}

	if (plen == 0)
		return add_property(pkg->props, (Byte *)"assigned-addresses",
				    CSTR, NULL, 0);

	Byte *prop_buf = prop_alloc(e, plen);
	if (prop_buf == NULL)
		return E_OUT_OF_MEMORY;
	memcpy(prop_buf, buf, plen);
	return add_property(pkg->props, (Byte *)"assigned-addresses", CSTR,
			    prop_buf, plen);
}

/*
 * Populate the device-level properties (vendor/device/class/etc) on
 * the already-created child package. Reads are live config-space
 * reads via mv64361 PCI cfg cycle helpers.
 */
static Retcode populate_device_props(Environ *e, Package *pkg,
				     int host, uint8_t bus, uint8_t dev,
				     uint8_t fn, uint32_t class_rev,
				     uint16_t vendid, uint16_t devid)
{
	Retcode r;

	if ((r = prop_set_int(pkg->props, (Byte *)"vendor-id", CSTR,
			      vendid)) != NO_ERROR) return r;
	if ((r = prop_set_int(pkg->props, (Byte *)"device-id", CSTR,
			      devid)) != NO_ERROR) return r;
	if ((r = prop_set_int(pkg->props, (Byte *)"revision-id", CSTR,
			      class_rev & 0xFFu)) != NO_ERROR) return r;
	/* class-code is the upper 24 bits of CLASS_REVISION. */
	if ((r = prop_set_int(pkg->props, (Byte *)"class-code", CSTR,
			      (class_rev >> 8) & 0xFFFFFFu)) != NO_ERROR)
		return r;

	uint16_t subsys_vend = pci_cfg_read16(host, bus, dev, fn,
					      CFG_SUBSYS_VENDOR);
	uint16_t subsys_id   = pci_cfg_read16(host, bus, dev, fn,
					      CFG_SUBSYS_ID);
	/*
	 * Pegasos firmware swap convention (preserved by QEMU):
	 *   subsystem-id         = <what hw SUBSYS_ID reports>
	 *   subsystem-vendor-id  = <what hw SUBSYS_VENDOR_ID reports>
	 * are reported in the NATURAL order on the spec and in the
	 * real firmware, but QEMU's add_pci_device line 925-928
	 * explicitly swaps them (comment: "Pegasos firmware has
	 * subsystem-id and subsystem-vendor-id swapped"). We follow
	 * the firmware's documented behaviour so OS clients see the
	 * expected values.
	 */
	if ((r = prop_set_int(pkg->props, (Byte *)"subsystem-vendor-id",
			      CSTR, subsys_id)) != NO_ERROR) return r;
	if ((r = prop_set_int(pkg->props, (Byte *)"subsystem-id",
			      CSTR, subsys_vend)) != NO_ERROR) return r;

	uint8_t intr_pin = pci_cfg_read8(host, bus, dev, fn,
					 CFG_INTERRUPT_PIN);
	if (intr_pin != 0) {
		if ((r = prop_set_int(pkg->props, (Byte *)"interrupts",
				      CSTR, intr_pin)) != NO_ERROR)
			return r;
	}

	return NO_ERROR;
}

/*
 * Create and populate one child package under buspkg for a given BDF.
 * Returns NO_ERROR even if the device is absent (so callers can
 * unconditionally iterate dev/fn ranges).
 */
static Retcode install_one_device(Environ *e, Package *buspkg,
				  int host, uint8_t bus, uint8_t dev,
				  uint8_t fn)
{
	uint16_t vendid = pci_cfg_read16(host, bus, dev, fn, CFG_VENDOR_ID);
	if (vendid == PCI_VENDOR_INVALID)
		return NO_ERROR;

	uint16_t devid     = pci_cfg_read16(host, bus, dev, fn, CFG_DEVICE_ID);
	uint32_t class_rev = pci_cfg_read32(host, bus, dev, fn,
					    CFG_CLASS_REVISION);
	uint8_t header     = pci_cfg_read8 (host, bus, dev, fn,
					    CFG_HEADER_TYPE);
	uint8_t ht_kind    = header & HEADER_TYPE_MASK;
	uint32_t classcode = (class_rev >> 8) & 0xFFFFFFu;

	char nodename[40];
	build_node_name(classcode, vendid, devid, dev, fn,
			nodename, sizeof nodename);

	Package *cpkg = new_pkg_name(buspkg, nodename);
	if (cpkg == NULL)
		return E_OUT_OF_MEMORY;

	Retcode r = populate_device_props(e, cpkg, host, bus, dev, fn,
					  class_rev, vendid, devid);
	if (r != NO_ERROR) return r;

	uint8_t bars_present = 0;
	if ((r = set_reg_prop(e, cpkg, host, bus, dev, fn, ht_kind,
			      &bars_present)) != NO_ERROR) return r;

	if ((r = set_assigned_addresses(e, cpkg, host, bus, dev, fn,
					bars_present)) != NO_ERROR) return r;

	return NO_ERROR;
}

/*
 * Walk bus 0 on the given host, installing every function of every
 * populated slot as a child of buspkg. PPB recursion is a future
 * milestone -- for now we cover the flat bus 0 case that the
 * default QEMU pegasos2 boot (and the bridge-test -device path)
 * both exercise at the top level.
 */
static Retcode walk_bus(Environ *e, Package *buspkg, int host, uint8_t bus)
{
	uint8_t dev;
	for (dev = 0; dev < 32; dev++) {
		uint16_t vendor = pci_cfg_read16(host, bus, dev, 0,
						 CFG_VENDOR_ID);
		if (vendor == PCI_VENDOR_INVALID)
			continue;

		uint8_t header = pci_cfg_read8(host, bus, dev, 0,
					       CFG_HEADER_TYPE);
		uint8_t max_fn = (header & HEADER_TYPE_MULTIFN) ? 8 : 1;

		uint8_t fn;
		for (fn = 0; fn < max_fn; fn++) {
			Retcode r = install_one_device(e, buspkg, host, bus,
						       dev, fn);
			if (r != NO_ERROR)
				return r;
		}
	}
	return NO_ERROR;
}

/*
 * PCI bus decode-unit / encode-unit -- the minimum subset needed for
 * SF's resolve_path to navigate /pci@<host>/<child>@<unit> paths and
 * for package_name() to build display names like "ide@c,1".
 *
 * Our children all use config-space physhi (space=0) in their `reg`
 * property's first entry (see install_one_device -> set_reg_prop,
 * which always encodes the config-space descriptor first). That
 * means decode-unit only needs to handle the config-space form:
 *     "DD[,FF[,RR]]"  ->  (physhi=PCI_PHYSHI_MK(cfg, bus, DD, FF, RR),
 *                           physmid=0, physlo=0)
 * where bus is read from the parent bus's first `bus-range` cell
 * (we always set bus-range=<0 0> in create_pci_host_package, so
 * bus=0 for now; honour the property anyway for future PPB children).
 *
 * We deliberately skip the full IEEE-1275 PCI binding form (the
 * optional "n", "i|m|x|u", "t", "p" prefixes for non-relocatable +
 * space + aliased + prefetch flags) -- those only matter when
 * clients reference memory/IO BAR regions by path, which nothing
 * in our M3..M7 arc does. If a future milestone needs them, port
 * SF's f_pci_decode_unit from pci/pci.c:280-433 verbatim.
 */

static Int parse_hex_field(Byte **ps, Int *pn)
{
	Byte *s = *ps;
	Int n = *pn;
	Int val = 0;

	while (n > 0) {
		int c = *s;
		int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
		else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
		else break;
		val = (val << 4) | d;
		s++; n--;
	}
	*ps = s;
	*pn = n;
	return val;
}

static Int get_parent_bus_number(Environ *e)
{
	/* bus-range's first cell is the primary bus number of this bus.
	 * Decoded from the binary property's first 4 big-endian bytes;
	 * don't use prop_get_str (NUL-byte truncation -- see M2 gotcha). */
	Entry *br = find_table(e->currpkg->props,
			       (Byte *)"bus-range", CSTR);
	if (br == NULL || br->len < (Int)sizeof(Int))
		return 0;
	Byte *p = (Byte *)br->v.array;
	return ((Int)p[0] << 24) | ((Int)p[1] << 16) |
	       ((Int)p[2] << 8)  | (Int)p[3];
}

/* decode-unit (str len -- phys.lo phys.mid phys.hi) */
CC(f_pci_decode_unit)
{
	Byte *str;
	Int slen;

	IFCKSP(e, 2, 3);
	POP(e, slen);
	POPT(e, str, Byte *);
	setstrlen(&str, &slen);

	Int bus  = get_parent_bus_number(e);
	Int dev  = parse_hex_field(&str, &slen);
	Int func = 0;
	Int reg  = 0;

	if (slen > 0 && *str == ',') {
		str++; slen--;
		func = parse_hex_field(&str, &slen);
	}
	if (slen > 0 && *str == ',') {
		str++; slen--;
		reg  = parse_hex_field(&str, &slen);
	}

	PUSH(e, 0);                                              /* phys.lo */
	PUSH(e, 0);                                              /* phys.mid */
	PUSH(e, (Cell)physhi_mk(0, 0, 0, bus, dev, func, reg));  /* phys.hi */
	return NO_ERROR;
}

/* encode-unit (phys.lo phys.mid phys.hi -- str len) */
CC(f_pci_encode_unit)
{
	static Byte buf[32];
	Cell physhi;
	Cell physmid;
	Cell physlo;

	IFCKSP(e, 3, 2);
	POP(e, physhi);
	POP(e, physmid);
	POP(e, physlo);
	(void)physmid;
	(void)physlo;

	unsigned dev   = (unsigned)((physhi >> 11) & 0x1Fu);
	unsigned func  = (unsigned)((physhi >> 8)  & 0x07u);
	unsigned reg   = (unsigned)((physhi >> 0)  & 0xFFu);
	unsigned space = (unsigned)((physhi >> 24) & 0x07u);

	if (space == 0) {                    /* config-space */
		if (reg != 0)
			bprintf((char *)buf, "%x,%x,%x", dev, func, reg);
		else if (func != 0)
			bprintf((char *)buf, "%x,%x", dev, func);
		else
			bprintf((char *)buf, "%x", dev);
	} else {
		/* Non-config paths shouldn't occur for our children; fall
		 * back to a bare dev,func rendering so nothing explodes. */
		bprintf((char *)buf, "%x,%x", dev, func);
	}

	PUSHP(e, buf);
	PUSH(e, (Cell)strlen((char *)buf));
	return NO_ERROR;
}

/*
 * open / close for the /pci@X bus package. open-dev walks the
 * requested path and calls `open` on every intermediate node; if
 * /pci@X has no open method the whole open-dev fails at the first
 * segment with E_NO_METHOD. The bus doesn't have any real setup
 * to do here (phase-1's pci_walker already programmed BARs / cmd
 * bits / bridge windows), so push FTRUE and return.
 */
CC(f_pci_bus_open)
{
	IFCKSP(e, 0, 1);
	PUSH(e, FTRUE);
	return NO_ERROR;
}

CC(f_pci_bus_close)
{
	(void)e;
	return NO_ERROR;
}

static const Initentry pci_bus_methods[] = {
	{ (Byte *)"open",        f_pci_bus_open,    INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"close",       f_pci_bus_close,   INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"decode-unit", f_pci_decode_unit, INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ (Byte *)"encode-unit", f_pci_encode_unit, INVALID_FCODE, F_NONE, T_FUNC HELP("") },
	{ NULL, NULL, INVALID_FCODE, F_NONE, T_FUNC HELP("") },
};

/*
 * Create a top-level /pci@<cpu_base> package with all the bus-level
 * properties CHRP clients expect. Parent = / so the ranges property
 * encodes #a(pci=3) + #a(root=1) + #s(pci=2) = 6 cells per entry.
 *
 * Note on naming: per IEEE-1275 §3.5 and SF's exact_match
 * (device.c:1086), a child package identifies itself via its literal
 * `name` property plus a unit-addr derived from its `reg` property.
 * resolve_path's incoming segment "pci@80000000" is split into
 * name="pci" + unit="80000000"; then the matcher wants a package
 * whose `name` == "pci" and whose reg address == 0x80000000. So we
 * set name = "pci" (NOT "pci@80000000") and let the `reg` property
 * <cpu_base cpu_size> supply the unit-addr disambiguator between
 * our two sibling /pci@X hosts.
 */
static Package *create_pci_host_package(Environ *e, const char *genus,
					uint32_t reg_base, uint32_t reg_size,
					uint32_t clock_hz,
					int pci_bridge_number,
					uint32_t io_cpu,  uint32_t io_size,
					uint32_t mem_cpu, uint32_t mem_size,
					uint32_t inta_virt)
{
	Package *pkg = new_pkg_name(e->root, (char *)genus);
	if (pkg == NULL) return NULL;

	prop_set_str(pkg->props, (Byte *)"device_type", CSTR,
		     (Byte *)"pci", CSTR);
	prop_set_int(pkg->props, (Byte *)"#address-cells", CSTR, 3);
	prop_set_int(pkg->props, (Byte *)"#size-cells",    CSTR, 2);
	prop_set_int(pkg->props, (Byte *)"clock-frequency", CSTR,
		     (Int)clock_hz);
	prop_set_int(pkg->props, (Byte *)"pci-bridge-number", CSTR,
		     pci_bridge_number);

	/* bus-range = <0 0> -- one bus. Encoded as 2 big-endian cells. */
	{
		Byte *arr = prop_alloc(e, 2 * sizeof(Int));
		Int len = 0;
		if (arr != NULL) {
			enc_int(arr, &len, 0);
			enc_int(arr, &len, 0);
			add_property(pkg->props, (Byte *)"bus-range", CSTR,
				     arr, len);
		}
	}

	/* reg = <cpu_base cpu_size> per root #a=1 #s=1. */
	{
		Byte *arr = prop_alloc(e, 2 * sizeof(Int));
		Int len = 0;
		if (arr != NULL) {
			enc_int(arr, &len, (Int)reg_base);
			enc_int(arr, &len, (Int)reg_size);
			add_property(pkg->props, (Byte *)"reg", CSTR,
				     arr, len);
		}
	}

	/* ranges: I/O + MEM windows. 6 cells per entry. */
	{
		Byte *arr = prop_alloc(e, 2 * 6 * sizeof(Int));
		Int len = 0;
		if (arr != NULL) {
			/* IO: physhi ss=1 bus=0 dev=0, physmid=0, physlo=0,
			   parent=io_cpu, size=(0, io_size). */
			enc_int(arr, &len,
				(Int)physhi_mk(0, 0, 1, 0, 0, 0, 0));
			enc_int(arr, &len, 0);
			enc_int(arr, &len, 0);
			enc_int(arr, &len, (Int)io_cpu);
			enc_int(arr, &len, 0);
			enc_int(arr, &len, (Int)io_size);

			/* MEM32: physhi ss=2, physlo=mem_cpu,
			   parent=mem_cpu (identity), size=(0, mem_size). */
			enc_int(arr, &len,
				(Int)physhi_mk(0, 0, 2, 0, 0, 0, 0));
			enc_int(arr, &len, 0);
			enc_int(arr, &len, (Int)mem_cpu);
			enc_int(arr, &len, (Int)mem_cpu);
			enc_int(arr, &len, 0);
			enc_int(arr, &len, (Int)mem_size);

			add_property(pkg->props, (Byte *)"ranges", CSTR,
				     arr, len);
		}
	}

	if (inta_virt != 0) {
		prop_set_int(pkg->props,
			     (Byte *)"8259-interrupt-acknowledge", CSTR,
			     (Int)inta_virt);
	}

	/* Register decode-unit / encode-unit on this bus so SF's
	 * resolve_path can navigate to children like ide@c,1. */
	(void)init_entries(e, pkg->dict, pci_bus_methods);

	return pkg;
}

/*
 * install_pci_tree: Forth word registered in install_list[]. Creates
 * both /pci@... top-level nodes and populates them with children for
 * every PCI device found on bus 0 of each host bridge.
 *
 * Constants chosen to match QEMU's pegasos2.dts verbatim:
 *
 *   /pci@80000000  (QEMU "host 1" = our PCI_HOST_1, VT8231 primary):
 *       reg             = 0x80000000  0x40000000
 *       ranges IO       = CPU 0xFE000000..0xFE010000 (64 KiB)
 *       ranges MEM32    = CPU 0x80000000..0xC0000000 (1 GiB, identity)
 *       clock-frequency = 33_333_333
 *       pci-bridge-number = 0
 *       8259-interrupt-acknowledge = 0xF1000CB4
 *
 *   /pci@c0000000  (QEMU "host 0" = our PCI_HOST_0, secondary slots):
 *       reg             = 0xC0000000  0x20000000
 *       ranges IO       = CPU 0xF8000000..0xF8010000 (64 KiB)
 *       ranges MEM32    = CPU 0xC0000000..0xE0000000 (512 MiB, identity)
 *       clock-frequency = 66_666_666
 *       pci-bridge-number = 1
 *       (no 8259-interrupt-acknowledge; only PCI_HOST_1 cascades
 *        through the VT8231 i8259.)
 */
/*
 * Set the root node's #address-cells / #size-cells to the CHRP
 * pegasos2 convention (1 each) BEFORE install_memory runs. SF's
 * install_root defaults them to 2 (per get_address_cells()'s
 * DEFAULT_*_CELLS); install_memory reads them when encoding
 * /memory/reg, so if we leave them at 2 the reg blob is 3 cells
 * per entry (2 address + 1 size, 12 bytes) -- but Linux's PPC32
 * boot wrapper reads /memory/reg expecting 1+1 (8 bytes), so it
 * sees address=our_base_low_cell, size=our_base_high_cell=0.
 * That gave the wrapper ram_top=0x200000 size=0 and a "Can't
 * allocate initial device-tree chunk" abort.
 *
 * Wired into install_list[] BEFORE install_memory so the cells
 * are set when /memory/reg gets encoded.
 */
CC(install_root_cells)
{
	prop_set_int(e->root->props, (Byte *)"#address-cells", CSTR, 1);
	prop_set_int(e->root->props, (Byte *)"#size-cells",    CSTR, 1);
	return NO_ERROR;
}

CC(install_pci_tree)
{

	/* Both bus packages use name="pci"; the `reg` property provides
	 * the unit-addr ("80000000" vs "c0000000") that disambiguates
	 * them when resolve_path walks the tree. */
	Package *pci1 = create_pci_host_package(
		e, "pci",
		0x80000000u, 0x40000000u,
		33333333u, 0,
		0xFE000000u, 0x00010000u,
		0x80000000u, 0x40000000u,
		0xF1000CB4u);
	if (pci1 == NULL) return E_OUT_OF_MEMORY;

	Retcode r = walk_bus(e, pci1, PCI_HOST_1, 0);
	if (r != NO_ERROR) return r;

	Package *pci0 = create_pci_host_package(
		e, "pci",
		0xC0000000u, 0x20000000u,
		66666666u, 1,
		0xF8000000u, 0x00010000u,
		0xC0000000u, 0x20000000u,
		0u);
	if (pci0 == NULL) return E_OUT_OF_MEMORY;

	return walk_bus(e, pci0, PCI_HOST_0, 0);
}

/* ---------- ls-pci Forth word (M1 smoke test) ------------------ */

/*
 * Traverse a host-bridge package's child list and print each with
 * name + vendor-id, device-id, class-code. Indent by two for visual
 * clarity. Used by the ls-pci Forth word below.
 */
static void ls_pci_host(Environ *e, Package *host, const char *label)
{
	Package *child;
	Byte namebuf[64];

	cprintf(e, "%s:\n", label);

	for (child = host->children; child != NULL; child = child->link) {
		Int vendid, devid, classcode;

		if (!get_device_name(e, child, namebuf))
			strcpy((char *)namebuf, "?");

		if (prop_get_int(child->props, (Byte *)"vendor-id", CSTR,
				 &vendid) != NO_ERROR) vendid = 0xFFFF;
		if (prop_get_int(child->props, (Byte *)"device-id", CSTR,
				 &devid) != NO_ERROR)  devid  = 0xFFFF;
		if (prop_get_int(child->props, (Byte *)"class-code", CSTR,
				 &classcode) != NO_ERROR) classcode = 0;

		/*
		 * get_device_name returns the full path "/pci@.../ide@c,1";
		 * print just the final component for readability.
		 */
		char *base = strrchr((char *)namebuf, '/');
		base = base ? base + 1 : (char *)namebuf;

		cprintf(e, "  %-14s vendor=0x%04X device=0x%04X class=0x%06X\n",
			base, (Int)(vendid & 0xFFFFu),
			(Int)(devid & 0xFFFFu), (Int)(classcode & 0xFFFFFFu));
	}
}

CC(f_ls_pci)
{
	Package *pci1;
	Package *pci0;

	/*
	 * finddevice returns the Package* for a path. For the top-level
	 * host-bridge nodes we created in install_pci_tree, a direct
	 * traversal of e->root->firstchild would work too but using
	 * the path keeps this self-documenting and robust against
	 * reordering.
	 */
	pci1 = find_device(e, (Byte *)"/pci@80000000", CSTR);
	if (pci1 != NULL)
		ls_pci_host(e, pci1, "/pci@80000000");
	else
		cprintf(e, "ls-pci: /pci@80000000 not found\n");

	pci0 = find_device(e, (Byte *)"/pci@c0000000", CSTR);
	if (pci0 != NULL)
		ls_pci_host(e, pci0, "/pci@c0000000");
	else
		cprintf(e, "ls-pci: /pci@c0000000 not found\n");

	return NO_ERROR;
}
