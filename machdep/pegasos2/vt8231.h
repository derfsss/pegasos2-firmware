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

#endif
