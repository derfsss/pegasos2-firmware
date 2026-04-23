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

/* --------------------------------------------------------------- *
 *  Interrupt controller (Discovery II §Interrupt Controller)       *
 * --------------------------------------------------------------- */

/*
 * 64-bit main cause register, split into two 32-bit halves. Cause
 * bit N is the N-th internal interrupt source (0..31 = LOW,
 * 32..63 = HIGH).
 *
 * On QEMU pegasos2 the VT8231 southbridge's aggregated PIC output
 * is wired to MV64361 GPP line 31, which fires main cause bit 59
 * (P0_GPP24_31). PCI INTA..D from both PCI0+PCI1 cascade through
 * GPP 12..15, which fires bit 57 (P0_GPP8_15). These routings are
 * documented in hw/ppc/pegasos.c pegasos2_setup_pci_irq().
 */
#define MV_IC_MAIN_CAUSE_LOW   0x004u  /* RO, bits 0..31   */
#define MV_IC_MAIN_CAUSE_HIGH  0x00Cu  /* RO, bits 32..63  */
#define MV_IC_CPU0_MASK_LOW    0x014u  /* RW, 1=enabled    */
#define MV_IC_CPU0_MASK_HIGH   0x01Cu  /* RW               */
#define MV_IC_CPU0_SEL_CAUSE   0x024u  /* RO, highest-prio pending */

/* GPP (general-purpose I/O repurposed as PCI-IRQ input) controls.
 * GPP 0..31 map to main cause bits 56..59 (P0_GPP0_7..P0_GPP24_31)
 * in blocks of 8. Any enabled GPP pin going high will set its
 * block's main cause bit. */
#define MV_GPP_INT_CAUSE       0xF108u /* RW1C, bit N = GPP pin N */
#define MV_GPP_INT_MASK0       0xF10Cu /* RW, mask for CPU0 route  */
#define MV_GPP_INT_MASK1       0xF114u /* RW, mask for CPU1 route  */

/* Main-cause bit numbers used by this firmware (full enum lives in
 * hw/pci-host/mv64361.c MV64361_IRQ_*). Cast to the high word when
 * >= 32. */
#define MV_IC_BIT_P0_GPP8_15   57u     /* PCI INTA..D cascade      */
#define MV_IC_BIT_P0_GPP24_31  59u     /* VT8231 PIC cascade       */

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
void     pci_cfg_write16(int host, uint8_t bus, uint8_t dev,
			 uint8_t fn, uint8_t reg, uint16_t val);
uint8_t  pci_cfg_read8 (int host, uint8_t bus, uint8_t dev,
			uint8_t fn, uint8_t reg);
void     pci_cfg_write8(int host, uint8_t bus, uint8_t dev,
			uint8_t fn, uint8_t reg, uint8_t val);

/* Open the CPU->PCI0 I/O decode window (CPU 0xF8000000..0xF8FFFFFF
 * -> PCI I/O 0). PCI1 I/O is already open from QEMU reset default. */
void mv64361_enable_pci0_io_window(void);

#endif
