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

void mv64361_enable_pci0_io_window(void)
{
	/* Remap PCI-side base of the CPU 0xF8000000 window to PCI I/O 0
	 * so ISA port 0x3F8 -> CPU 0xF80003F8. */
	mv64361_write32(MV_PCI0_IO_REMAP, 0u);

	/* BASE_ADDR_ENABLE bit N set => window N disabled. Reset
	 * default is 0x000FBFFF. Clear bit 9 to enable PCI0 I/O. */
	mv64361_write32(MV_BASE_ADDR_ENABLE, 0x000FBDFFu);
}
