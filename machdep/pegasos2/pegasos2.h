/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Pegasos II physical-address constants. Values sourced from the
 *  paraphrased memory map in docs/00-overview.md and the VT8231
 *  SuperIO table in docs/04-southbridge.md.
 */

#ifndef PEGASOS2_H
#define PEGASOS2_H

/*
 * Flash / firmware ROM.
 *
 *   QEMU `-M pegasos2`: 0xFFF00000..0xFFF7FFFF (512 KiB, our build target).
 *   Real hardware:      0xFFF80000..0xFFFFFFFF plus mirrors for the
 *                       MPC7447 reset vector at 0xFFF00100.
 *   See docs/00-overview.md section "System memory map".
 */
#define FLASH_BASE         0xFFF00000u
#define FLASH_SIZE_BYTES   0x00080000u       /* 512 KiB */
#define RESET_VECTOR_ADDR  0xFFF00100u       /* MPC7447 HRESET_HIGH entry */

/* Marvell MV64361 internal register bank (64 KiB). */
#define MV64361_REG_BASE   0xF1000000u

/*
 * MV64361 PCI host bridge windows.
 *
 * NB: On QEMU pegasos2, the VT8231 southbridge (and therefore UART1)
 * lives on PCI1, not PCI0 as docs/04-southbridge.md states. See
 * SPEC-QUESTIONS.md Q1. We follow the emulator.
 */
#define PCI0_IO_BASE       0xF8000000u
#define PCI0_IO_SIZE       0x01000000u       /* 16 MiB */
#define PCI1_IO_BASE       0xFE000000u
#define PCI1_IO_SIZE       0x01000000u       /* 16 MiB */

/* VT8231 SuperIO legacy UART ports (docs/04-southbridge.md §SuperIO). */
#define ISA_IO_UART1       0x000003F8u
#define ISA_IO_UART2       0x000002F8u

/* UART1 as a CPU-visible physical address (VT8231 on PCI1). */
#define UART1_BASE         (PCI1_IO_BASE + ISA_IO_UART1)  /* 0xFE0003F8 */
#define UART2_BASE         (PCI1_IO_BASE + ISA_IO_UART2)  /* 0xFE0002F8 */

/* SuperIO index/data ports on the VT8231 (same PCI1 I/O window). */
#define ISA_IO_SUPERIO_IDX 0x000003F0u
#define ISA_IO_SUPERIO_DAT 0x000003F1u
#define SUPERIO_IDX_ADDR   (PCI1_IO_BASE + ISA_IO_SUPERIO_IDX)
#define SUPERIO_DAT_ADDR   (PCI1_IO_BASE + ISA_IO_SUPERIO_DAT)

/* 16550 register offsets (DLAB=0 view unless noted). */
#define UART16550_THR      0x0                /* W: transmit holding */
#define UART16550_RBR      0x0                /* R: receive buffer */
#define UART16550_DLL      0x0                /* DLAB=1: divisor low */
#define UART16550_IER      0x1                /* interrupt enable */
#define UART16550_DLM      0x1                /* DLAB=1: divisor high */
#define UART16550_FCR      0x2                /* W: FIFO control */
#define UART16550_IIR      0x2                /* R: interrupt id */
#define UART16550_LCR      0x3                /* line control */
#define UART16550_MCR      0x4                /* modem control */
#define UART16550_LSR      0x5                /* line status */
#define UART16550_MSR      0x6                /* modem status */

/* LSR bits. */
#define UART16550_LSR_DR   0x01               /* data ready (RX) */
#define UART16550_LSR_THRE 0x20               /* transmit holding empty */
#define UART16550_LSR_TEMT 0x40               /* transmitter empty */

#endif /* PEGASOS2_H */
