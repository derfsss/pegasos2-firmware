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

#endif
