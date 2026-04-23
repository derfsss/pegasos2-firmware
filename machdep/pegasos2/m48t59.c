/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  M48T59 byte read/write over VT8231 ISA I/O. The chip exposes an
 *  address-indexed view of its 8 KiB SRAM via three ISA ports (spec
 *  08 §"M48T59 access"):
 *
 *    out 0x74, offset & 0xFF
 *    out 0x75, (offset >> 8) & 0x1F    -- 13-bit offset total
 *    in  al,  0x77
 *
 *  All three ports sit in the VT8231 PCI1 I/O window at 0xFE000000.
 *  Byte reads/writes on that window are endian-invariant (see io.h
 *  commentary), so the mmio_{read,write}8 helpers are the right
 *  layer.
 *
 *  No concurrency guard: the firmware is single-threaded and every
 *  caller runs with interrupts masked in phase1.
 */

#include "pegasos2.h"
#include "io.h"
#include "m48t59.h"

#define M48T59_ADDR_LO_MMIO   (PCI1_IO_BASE + M48T59_ADDR_LO_PORT)
#define M48T59_ADDR_HI_MMIO   (PCI1_IO_BASE + M48T59_ADDR_HI_PORT)
#define M48T59_DATA_MMIO      (PCI1_IO_BASE + M48T59_DATA_PORT)

static inline void
m48t59_set_addr(unsigned offset)
{
	mmio_write8(M48T59_ADDR_LO_MMIO, (uint8_t)(offset & 0xFFu));
	mmio_write8(M48T59_ADDR_HI_MMIO, (uint8_t)((offset >> 8) & 0x1Fu));
}

uint8_t
m48t59_read_byte(unsigned offset)
{
	m48t59_set_addr(offset);
	return mmio_read8(M48T59_DATA_MMIO);
}

void
m48t59_write_byte(unsigned offset, uint8_t val)
{
	m48t59_set_addr(offset);
	mmio_write8(M48T59_DATA_MMIO, val);
}
