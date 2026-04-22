/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Standard PCI 2.3 configuration-space register map and related
 *  constants. All offsets are bytes inside a device's 256-byte
 *  type-0 or type-1 configuration-space region.
 */

#ifndef PCI_H
#define PCI_H

/* Header registers common to type 0 and type 1. */
#define PCI_VENDOR_ID           0x00u
#define PCI_DEVICE_ID           0x02u
#define PCI_COMMAND             0x04u
#define PCI_STATUS              0x06u
#define PCI_REVISION_ID         0x08u
#define PCI_PROG_IF             0x09u
#define PCI_SUBCLASS            0x0Au
#define PCI_BASECLASS           0x0Bu
#define PCI_CACHE_LINE_SIZE     0x0Cu
#define PCI_LATENCY_TIMER       0x0Du
#define PCI_HEADER_TYPE         0x0Eu
#define PCI_BIST                0x0Fu

/* Type-1 (PCI-to-PCI bridge) header fields. */
#define PCI_BRIDGE_BUS_PRIMARY      0x18u
#define PCI_BRIDGE_BUS_SECONDARY    0x19u
#define PCI_BRIDGE_BUS_SUBORDINATE  0x1Au
#define PCI_BRIDGE_LATENCY_TIMER    0x1Bu
#define PCI_BRIDGE_IO_BASE          0x1Cu
#define PCI_BRIDGE_IO_LIMIT         0x1Du
#define PCI_BRIDGE_SEC_STATUS       0x1Eu
#define PCI_BRIDGE_MEM_BASE         0x20u
#define PCI_BRIDGE_MEM_LIMIT        0x22u
#define PCI_BRIDGE_PREFETCH_BASE    0x24u
#define PCI_BRIDGE_PREFETCH_LIMIT   0x26u

/* PCI_COMMAND bits used by this firmware. */
#define PCI_CMD_IO              (1u << 0)
#define PCI_CMD_MEM             (1u << 1)
#define PCI_CMD_MASTER          (1u << 2)

/* PCI_HEADER_TYPE encoding. */
#define PCI_HEADER_TYPE_MASK    0x7Fu
#define PCI_HEADER_TYPE_MULTIFN 0x80u

#define PCI_HTYPE_DEVICE        0x00u
#define PCI_HTYPE_BRIDGE        0x01u
#define PCI_HTYPE_CARDBUS       0x02u

/* Class codes the walker cares about. Format: (base<<8) | subclass. */
#define PCI_CLASS_BRIDGE_PCI    0x0604u    /* PCI-to-PCI bridge */
#define PCI_CLASS_BRIDGE_ISA    0x0601u    /* PCI-to-ISA bridge (VT8231 fn 0) */

/* Sentinel vendor ID for "no device at this slot". */
#define PCI_VENDOR_INVALID      0xFFFFu

/* Base Address Register layout (PCI 2.3 §6.2.5). */
#define PCI_BAR_0               0x10u
#define PCI_BAR_COUNT_TYPE0     6        /* type-0 devices expose BAR0..BAR5 */
#define PCI_BAR_COUNT_TYPE1     2        /* bridges expose BAR0..BAR1 only  */

#define PCI_BAR_SPACE_IO        0x00000001u   /* bit 0: 1=I/O, 0=memory      */
#define PCI_BAR_MEM_TYPE_MASK   0x00000006u   /* bits 2..1 for memory BARs   */
#define PCI_BAR_MEM_TYPE_32     0x00000000u
#define PCI_BAR_MEM_TYPE_1M     0x00000002u   /* deprecated "below 1 MiB"    */
#define PCI_BAR_MEM_TYPE_64     0x00000004u
#define PCI_BAR_MEM_PREFETCH    0x00000008u   /* bit 3: prefetchable memory  */
#define PCI_BAR_MEM_ADDR_MASK   0xFFFFFFF0u
#define PCI_BAR_IO_ADDR_MASK    0xFFFFFFFCu

/* Expansion-ROM BAR: type-0 at 0x30, type-1 (bridge) at 0x38. */
#define PCI_ROM_BAR_TYPE0       0x30u
#define PCI_ROM_BAR_TYPE1       0x38u
#define PCI_ROM_BAR_ENABLE      0x00000001u
#define PCI_ROM_BAR_ADDR_MASK   0xFFFFF800u   /* bits 31..11 are the address */

#endif
