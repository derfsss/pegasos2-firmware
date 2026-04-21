/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Marvell MV64361 (Discovery II) register accessors + PCI config.
 */

#ifndef MV64361_H
#define MV64361_H

#include <stdint.h>

/* MV64361 register-bank offsets used by this firmware.
 * Offset names are ours; values come from the Marvell Discovery II
 * Programmer's Reference (and, for the PCI1 config pair, empirical
 * verification against QEMU -- see SPEC-QUESTIONS.md Q2). */
#define MV_PCI0_IO_REMAP       0x0F0u
#define MV_BASE_ADDR_ENABLE    0x278u
#define MV_PCI0_CFG_ADDR       0xCF8u
#define MV_PCI0_CFG_DATA       0xCFCu
#define MV_PCI1_CFG_ADDR       0xC78u    /* != spec 03's 0x8CF8 */
#define MV_PCI1_CFG_DATA       0xC7Cu    /* != spec 03's 0x8CFC */

/* Which host bridge a PCI config cycle targets. */
enum {
	PCI_HOST_0 = 0,
	PCI_HOST_1 = 1,
};

/* Register-bank access. The register bank is little-endian viewed
 * from the CPU (on QEMU; real HW TBD) -- mv64361_write32 handles
 * the byte reverse. */
uint32_t mv64361_read32(uint32_t offset);
void     mv64361_write32(uint32_t offset, uint32_t val);

/* PCI configuration-space access routed through the appropriate
 * host bridge. Values on the PCI side are little-endian; uint32_t
 * return values have byte-0 at PCI cfg offset `reg` in the LSB. */
uint32_t pci_cfg_read32(int host, uint8_t bus, uint8_t dev,
			uint8_t fn, uint8_t reg);
void     pci_cfg_write32(int host, uint8_t bus, uint8_t dev,
			 uint8_t fn, uint8_t reg, uint32_t val);

uint16_t pci_cfg_read16(int host, uint8_t bus, uint8_t dev,
			uint8_t fn, uint8_t reg);
uint8_t  pci_cfg_read8 (int host, uint8_t bus, uint8_t dev,
			uint8_t fn, uint8_t reg);
void     pci_cfg_write8(int host, uint8_t bus, uint8_t dev,
			uint8_t fn, uint8_t reg, uint8_t val);

/* Open the CPU->PCI0 I/O decode window (CPU 0xF8000000..0xF8FFFFFF
 * -> PCI I/O 0). PCI1 I/O is already open from QEMU reset default. */
void mv64361_enable_pci0_io_window(void);

#endif
