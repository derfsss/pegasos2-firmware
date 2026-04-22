/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 */

#include "mv64361.h"
#include "io.h"
#include "pegasos2.h"

uint32_t mv64361_read32(uint32_t offset)
{
	return mmio_read32_le(MV64361_REG_BASE + offset);
}

void mv64361_write32(uint32_t offset, uint32_t val)
{
	mmio_write32_le(MV64361_REG_BASE + offset, val);
}

static uint32_t pci_cfg_addr_reg(int host)
{
	return host ? MV_PCI1_CFG_ADDR : MV_PCI0_CFG_ADDR;
}

static uint32_t pci_cfg_data_reg(int host)
{
	return host ? MV_PCI1_CFG_DATA : MV_PCI0_CFG_DATA;
}

static uint32_t pci_cfg_address(uint8_t bus, uint8_t dev,
				uint8_t fn, uint8_t reg)
{
	return 0x80000000u
	     | ((uint32_t)bus << 16)
	     | ((uint32_t)dev << 11)
	     | ((uint32_t)fn  <<  8)
	     | ((uint32_t)reg & 0xFCu);
}

uint32_t pci_cfg_read32(int host, uint8_t bus, uint8_t dev,
			uint8_t fn, uint8_t reg)
{
	mv64361_write32(pci_cfg_addr_reg(host),
			pci_cfg_address(bus, dev, fn, reg));
	return mv64361_read32(pci_cfg_data_reg(host));
}

void pci_cfg_write32(int host, uint8_t bus, uint8_t dev,
		     uint8_t fn, uint8_t reg, uint32_t val)
{
	mv64361_write32(pci_cfg_addr_reg(host),
			pci_cfg_address(bus, dev, fn, reg));
	mv64361_write32(pci_cfg_data_reg(host), val);
}

uint16_t pci_cfg_read16(int host, uint8_t bus, uint8_t dev,
			uint8_t fn, uint8_t reg)
{
	uint32_t v = pci_cfg_read32(host, bus, dev, fn, reg);
	return (uint16_t)((v >> ((reg & 2) * 8)) & 0xFFFFu);
}

uint8_t pci_cfg_read8(int host, uint8_t bus, uint8_t dev,
		      uint8_t fn, uint8_t reg)
{
	uint32_t v = pci_cfg_read32(host, bus, dev, fn, reg);
	return (uint8_t)((v >> ((reg & 3) * 8)) & 0xFFu);
}

void pci_cfg_write8(int host, uint8_t bus, uint8_t dev,
		    uint8_t fn, uint8_t reg, uint8_t val)
{
	uint32_t v = pci_cfg_read32(host, bus, dev, fn, reg);
	unsigned shift = (unsigned)(reg & 3) * 8;
	v = (v & ~(0xFFu << shift)) | ((uint32_t)val << shift);
	pci_cfg_write32(host, bus, dev, fn, reg, v);
}

void pci_cfg_write16(int host, uint8_t bus, uint8_t dev,
		     uint8_t fn, uint8_t reg, uint16_t val)
{
	/*
	 * Read-modify-write the 32-bit word containing this 16-bit slot.
	 * Critical for command-register writes: PCI_STATUS sits in bits
	 * 16..31 of the same dword and its bits are RW1C; a full-word
	 * write with 1 bits in the status half would clear latched
	 * errors as a side effect. RMW with an unchanged status half
	 * is the safe pattern.
	 */
	uint32_t v = pci_cfg_read32(host, bus, dev, fn, reg);
	unsigned shift = (unsigned)(reg & 2) * 8;
	v = (v & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
	pci_cfg_write32(host, bus, dev, fn, reg, v);
}

void mv64361_enable_pci0_io_window(void)
{
	/* Remap PCI-side base of the CPU 0xF8000000 window to PCI I/O 0
	 * so ISA port 0x3F8 -> CPU 0xF80003F8. */
	mv64361_write32(MV_PCI0_IO_REMAP, 0u);

	/* BASE_ADDR_ENABLE bit N set => window N disabled. Reset
	 * default is 0x000FBFFF (windows 14 and 20 already enabled).
	 * Clear bit 9 to enable PCI0 I/O and bit 15 for PCI1 mem0:
	 *   0x000FBFFF & ~((1<<9) | (1<<15)) = 0x000F3DFF. */
	mv64361_write32(MV_BASE_ADDR_ENABLE, 0x000F3DFFu);
}
