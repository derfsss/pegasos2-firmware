/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 */

#include "vt8231.h"
#include "mv64361.h"
#include "io.h"
#include "pegasos2.h"

/*
 * Minimum chain to make UART1 respond at ISA 0x3F8:
 *
 *   1. Enable I/O + MEM + MASTER decode on the VT8231 ISA-bridge
 *      function by setting PCI_COMMAND bits 0..2.
 *
 *   2. Unlock the SuperIO config window by setting bit 2 of the
 *      VT8231 fn-0 PCI-config register 0x50. Without this, writes
 *      to ISA ports 0x3F0/0x3F1 are silently dropped by the
 *      emulation rather than routed into the SuperIO config window.
 *
 *   3. Set SuperIO index 0xF2 (Function Select) bit 2 to enable
 *      UART1 decode. Reset default of 0xF2 is 0x03, so 0x07 keeps
 *      existing enables and adds UART1.
 *
 *   4. Leave SuperIO index 0xF4 at its reset default 0xFE -- the
 *      VT8231 model interprets that as UART1 I/O base = 0x3F8.
 *
 * See SPEC-QUESTIONS.md Q3 for the spec-clarity follow-up.
 */

void vt8231_enable_uart1(void)
{
	pci_cfg_write32(VT8231_HOST, VT8231_BUS, VT8231_DEV, VT8231_FN_ISA,
			0x04, 0x00000007u);

	pci_cfg_write32(VT8231_HOST, VT8231_BUS, VT8231_DEV, VT8231_FN_ISA,
			0x50, 0x00000004u);

	mmio_write8(SUPERIO_IDX_ADDR, 0xF2);
	mmio_write8(SUPERIO_DAT_ADDR, 0x07);
}

/* --------------------------------------------------------------- *
 *  i8259 PIC init + EOI helpers                                     *
 * --------------------------------------------------------------- */

static inline uint32_t pic_master_cmd (void) { return PCI1_IO_BASE + VT8231_PIC_MASTER_CMD; }
static inline uint32_t pic_master_data(void) { return PCI1_IO_BASE + VT8231_PIC_MASTER_DATA; }
static inline uint32_t pic_slave_cmd  (void) { return PCI1_IO_BASE + VT8231_PIC_SLAVE_CMD; }
static inline uint32_t pic_slave_data (void) { return PCI1_IO_BASE + VT8231_PIC_SLAVE_DATA; }

void vt8231_pic_init(void)
{
	/* Master ICW1..ICW4 + OCW1. ICW2 irq_base=0x20 follows PC
	 * convention; the vector number is returned by the
	 * MV_PCI1_INTA_VIRT read but we do not use it (we dispatch by
	 * IRQ index already known from the handler registration). */
	mmio_write8(pic_master_cmd(),  0x11);   /* ICW1: ICW4 needed, cascade */
	mmio_write8(pic_master_data(), 0x20);   /* ICW2: vector base */
	mmio_write8(pic_master_data(), 0x04);   /* ICW3: slave on IR2 */
	mmio_write8(pic_master_data(), 0x01);   /* ICW4: 8086 mode */
	mmio_write8(pic_master_data(), 0xFF);   /* OCW1: mask all */

	mmio_write8(pic_slave_cmd(),   0x11);
	mmio_write8(pic_slave_data(),  0x28);
	mmio_write8(pic_slave_data(),  0x02);
	mmio_write8(pic_slave_data(),  0x01);
	mmio_write8(pic_slave_data(),  0xFF);
}

void vt8231_pic_unmask_master(int irq)
{
	uint8_t mask = mmio_read8(pic_master_data());
	mask &= (uint8_t)~(1u << (irq & 7));
	mmio_write8(pic_master_data(), mask);
}

void vt8231_pic_eoi_master(void)
{
	mmio_write8(pic_master_cmd(), 0x20);    /* OCW2: non-specific EOI */
}

/* --------------------------------------------------------------- *
 *  SMBus host controller + W83194 clock-generator probe            *
 * --------------------------------------------------------------- *
 *
 * Pegasos2's FSB (133 MHz default) is programmable via the W83194
 * clock synthesizer on SMBus at address 0x69. Phase 1 currently
 * hard-codes PEGASOS2_FSB_HZ_DEFAULT; this code path lets us
 * query the real setting. Whether or not the query succeeds,
 * pegasos2 always reports a decrementer tick that is useful for
 * coarse timing; the W83194 value only improves accuracy.
 *
 * VT8231 PMU function 4 is not modelled by QEMU, so on QEMU
 * every probe here returns -1 / 0 and the timer code falls
 * back to PEGASOS2_FSB_HZ_DEFAULT. Real-HW validation is
 * deferred per the "QEMU first" directive.
 */

int vt8231_smbus_probe(unsigned *smbus_io_base)
{
	/* Vendor-ID probe: a non-present function reads as all-ones. */
	uint32_t vid = pci_cfg_read32(VT8231_HOST, VT8231_BUS, VT8231_DEV,
	                              VT8231_FN_PMU, 0x00);
	if (vid == 0xFFFFFFFFu || vid == 0)
		return -1;

	/* The SMBus I/O base lives at fn4 config offset 0x90 per the
	 * VT8231 datasheet. Low bit is the IO-space marker; mask it
	 * off and take the 16-bit port base. */
	uint32_t bar = pci_cfg_read32(VT8231_HOST, VT8231_BUS, VT8231_DEV,
	                              VT8231_FN_PMU,
	                              VT8231_PMU_SMB_IO_BASE_REG);
	unsigned base = bar & 0xFFF0u;
	if (base == 0)
		return -1;

	*smbus_io_base = base;
	return 0;
}

static inline uint32_t
smb_mmio(unsigned smbus_io_base, unsigned reg)
{
	return PCI1_IO_BASE + smbus_io_base + reg;
}

int vt8231_smbus_read_byte(unsigned smbus_io_base,
                           unsigned address, unsigned reg)
{
	if (smbus_io_base == 0)
		return -1;

	/* Clear any stale status bits (write-1-to-clear). */
	mmio_write8(smb_mmio(smbus_io_base, VT8231_SMB_HST_STS),
	            VT8231_SMB_STS_INTR   | VT8231_SMB_STS_DEVERR |
	            VT8231_SMB_STS_BUSCOL | VT8231_SMB_STS_FAIL);

	/* Spin if the controller is already busy. Bounded so a dead
	 * controller can't hang the boot. */
	for (int i = 0; i < 1000; i++) {
		uint8_t sts = mmio_read8(smb_mmio(smbus_io_base,
		                                   VT8231_SMB_HST_STS));
		if (!(sts & VT8231_SMB_STS_BUSY))
			break;
	}

	/* Address byte: 7-bit slave + read bit (1). */
	mmio_write8(smb_mmio(smbus_io_base, VT8231_SMB_HST_ADD),
	            (uint8_t)((address << 1) | 1u));
	/* Command byte: target register on the slave. */
	mmio_write8(smb_mmio(smbus_io_base, VT8231_SMB_HST_CMD),
	            (uint8_t)reg);
	/* Kick a byte-read transaction. */
	mmio_write8(smb_mmio(smbus_io_base, VT8231_SMB_HST_CNT),
	            VT8231_SMB_CNT_START | VT8231_SMB_CNT_BYTE);

	/* Poll for completion or error. */
	for (int i = 0; i < 100000; i++) {
		uint8_t sts = mmio_read8(smb_mmio(smbus_io_base,
		                                   VT8231_SMB_HST_STS));
		if (sts & (VT8231_SMB_STS_DEVERR |
		           VT8231_SMB_STS_BUSCOL |
		           VT8231_SMB_STS_FAIL))
			return -1;
		if (sts & VT8231_SMB_STS_INTR) {
			uint8_t val = mmio_read8(smb_mmio(smbus_io_base,
			                                   VT8231_SMB_HST_DAT0));
			/* Clear INTR so subsequent reads don't see stale
			 * completion. */
			mmio_write8(smb_mmio(smbus_io_base,
			                      VT8231_SMB_HST_STS),
			            VT8231_SMB_STS_INTR);
			return (int)val;
		}
	}
	return -1;  /* timeout */
}

#define W83194_SMBUS_ADDR   0x69u
#define W83194_REG_FSB_CFG  0x03u  /* Pegasos2 reads CPU-FSB select here
                                    * per Genesi documentation. */

unsigned vt8231_w83194_fsb_hz(void)
{
	unsigned base;
	if (vt8231_smbus_probe(&base) != 0)
		return 0;

	int fsb_cfg = vt8231_smbus_read_byte(base, W83194_SMBUS_ADDR,
	                                     W83194_REG_FSB_CFG);
	if (fsb_cfg < 0)
		return 0;

	/* Bits 2..0 of register 0x03 select the FSB frequency on the
	 * W83194 variants used by Pegasos II. Values per the Winbond
	 * datasheet:
	 *   0b000 ->  66 MHz
	 *   0b001 ->  75 MHz
	 *   0b010 ->  83 MHz
	 *   0b011 -> 100 MHz
	 *   0b100 -> 120 MHz
	 *   0b101 -> 133 MHz  (Pegasos II default)
	 *   0b110 -> 150 MHz
	 *   0b111 -> 166 MHz
	 * A future revision could read the full frequency table from
	 * a Pegasos2-specific I2C leaf; for now we support the values
	 * documented in the boardsource Genesi material. */
	static const unsigned fsb_table[8] = {
		 66666667u,  75000000u,  83000000u, 100000000u,
		120000000u, 133000000u, 150000000u, 166666667u
	};
	return fsb_table[fsb_cfg & 0x07u];
}
