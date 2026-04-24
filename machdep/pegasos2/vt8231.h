/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  VIA VT8231 southbridge -- Phase-1 subset (UART1 enable chain).
 */

#ifndef VT8231_H
#define VT8231_H

/* QEMU places VT8231 at PCI1 bus 0 dev 0x0C fn 0. Spec says PCI0,
 * but that slot is empty on QEMU -- see SPEC-QUESTIONS.md Q1. */
#define VT8231_HOST        1
#define VT8231_BUS         0u
#define VT8231_DEV         0x0Cu
#define VT8231_FN_ISA      0u
#define VT8231_FN_IDE      1u
#define VT8231_FN_USB1     2u
#define VT8231_FN_USB2     3u
#define VT8231_FN_PMU      4u   /* ACPI + SMBus + hardware monitor */
#define VT8231_FN_AC97     5u
#define VT8231_FN_MC97     6u

/* Bring UART1 up at ISA 0x3F8: enable I/O decode on the ISA bridge,
 * unlock the SuperIO config window, flip the UART1 enable bit. */
void vt8231_enable_uart1(void);

/* --------------------------------------------------------------- *
 *  i8259 PIC (master+slave) -- standard ISA ports                  *
 * --------------------------------------------------------------- */

#define VT8231_PIC_MASTER_CMD    0x20u  /* command/status */
#define VT8231_PIC_MASTER_DATA   0x21u  /* data/mask      */
#define VT8231_PIC_SLAVE_CMD     0xA0u
#define VT8231_PIC_SLAVE_DATA    0xA1u

/* Standard ICW1..ICW4 + OCW1(=0xFF) on master and slave. Edge-
 * triggered, cascade master/slave on IR2, 8086 mode, no auto-EOI.
 * All IRQs masked at OCW1 until a caller unmasks a specific line. */
void vt8231_pic_init(void);

/* Clear the OCW1 mask bit for an IRQ on the master PIC (0..7). */
void vt8231_pic_unmask_master(int irq);

/* Non-specific EOI to the master PIC (OCW2 = 0x20). Pairs with
 * handler completion after the MV_PCI1_INTA_VIRT read-based
 * pic_intack has advanced ISR. */
void vt8231_pic_eoi_master(void);

/* --------------------------------------------------------------- *
 *  VT8231 PMU (fn 4) -- SMBus host controller                      *
 * --------------------------------------------------------------- *
 *
 * The VT8231 PMU function houses an i2c-compatible SMBus host
 * controller at PM_BASE + 0x10 (the VT8231 datasheet names the
 * register block SMB_STS..SMB_BLK at bytes 0..7 of a 16-byte
 * window starting at PM_BASE+0x10). The I/O base is programmed
 * via PCI config reg 0x90 on fn 4; phase-1 code has historically
 * trusted the BIOS-programmed value on real HW.
 *
 * QEMU pegasos2 does NOT model VT8231 fn 4 at all -- PCI config
 * reads on (host 1, bus 0, dev 0x0C, fn 4) return 0xFFFFFFFF,
 * and any SMBus transaction we attempt goes to an unmapped I/O
 * window. The probe helpers below detect the absent-function
 * case and return -1 so the caller can fall back to board-default
 * values (see timer_calibrate()).
 */
#define VT8231_PMU_PM_BASE_REG      0x48u  /* u16 -- PMU I/O base */
#define VT8231_PMU_SMB_IO_BASE_REG  0x90u  /* u16 -- SMBus I/O base */

/* SMBus register offsets relative to the SMBus I/O base. */
#define VT8231_SMB_HST_STS     0x00u
#define VT8231_SMB_HST_CNT     0x02u
#define VT8231_SMB_HST_CMD     0x03u
#define VT8231_SMB_HST_ADD     0x04u
#define VT8231_SMB_HST_DAT0    0x05u
#define VT8231_SMB_HST_DAT1    0x06u

/* HST_CNT bits */
#define VT8231_SMB_CNT_START   0x40u       /* start transaction */
#define VT8231_SMB_CNT_BYTE    0x04u       /* read-byte protocol */

/* HST_STS bits */
#define VT8231_SMB_STS_BUSY    0x01u
#define VT8231_SMB_STS_INTR    0x02u       /* transaction complete */
#define VT8231_SMB_STS_DEVERR  0x04u
#define VT8231_SMB_STS_BUSCOL  0x08u
#define VT8231_SMB_STS_FAIL    0x10u

/*
 * Probe VT8231 fn 4 + SMBus I/O base. Returns -1 if fn 4 is
 * absent (QEMU) or if its PM/SMBus BARs are zero; otherwise
 * returns 0 and fills *smbus_io_base with the I/O-port base
 * (a 16-bit address inside the PCI1 I/O window, suitable for
 * ISA-style mmio_write8 via PCI1_IO_BASE + base + offset).
 */
int vt8231_smbus_probe(unsigned *smbus_io_base);

/*
 * Issue an SMBus read-byte transaction: slave at `address` (7-bit
 * form, no R/W bit), register `reg`. Returns the byte value on
 * success, -1 on bus error or timeout. On QEMU this always
 * returns -1 because vt8231_smbus_probe() fails first.
 */
int vt8231_smbus_read_byte(unsigned smbus_io_base,
                           unsigned address, unsigned reg);

/*
 * Read the W83194 clock generator's FSB configuration and return
 * the derived CPU bus frequency in Hz. W83194 is on SMBus at
 * address 0x69 on Pegasos II. Returns 0 when no reading is
 * possible (SMBus absent, bad checksum, or unrecognised PLL
 * setting) so the caller can fall back to board defaults.
 */
unsigned vt8231_w83194_fsb_hz(void);

#endif
