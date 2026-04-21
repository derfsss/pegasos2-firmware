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

#endif
