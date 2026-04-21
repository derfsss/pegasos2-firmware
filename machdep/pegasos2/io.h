/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  MMIO accessors for PowerPC bare metal.
 *
 *  The MV64361 register bank on QEMU pegasos2 is little-endian from
 *  the CPU's perspective (a PPC-native stw of 0x000FBDFF arrives as
 *  0xFFBD0F00 at the model) -- confirmed via -trace mv64361_reg_write.
 *  For that bank, use mmio_{read,write}32_le. For UART/SuperIO byte
 *  ports, byte access is endian-invariant so mmio_{read,write}8 is
 *  fine.
 */

#ifndef IO_H
#define IO_H

#include <stdint.h>

static inline uint8_t mmio_read8(uint32_t addr)
{
	uint8_t v;
	__asm__ volatile ("lbz %0, 0(%1); eieio"
			  : "=r"(v) : "r"(addr) : "memory");
	return v;
}

static inline void mmio_write8(uint32_t addr, uint8_t val)
{
	__asm__ volatile ("stb %0, 0(%1); eieio"
			  : : "r"(val), "r"(addr) : "memory");
}

static inline uint32_t mmio_read32_be(uint32_t addr)
{
	uint32_t v;
	__asm__ volatile ("lwz %0, 0(%1); eieio"
			  : "=r"(v) : "r"(addr) : "memory");
	return v;
}

static inline void mmio_write32_be(uint32_t addr, uint32_t val)
{
	__asm__ volatile ("stw %0, 0(%1); eieio"
			  : : "r"(val), "r"(addr) : "memory");
}

static inline uint32_t mmio_read32_le(uint32_t addr)
{
	uint32_t v;
	__asm__ volatile ("lwbrx %0, 0, %1; eieio"
			  : "=r"(v) : "r"(addr) : "memory");
	return v;
}

static inline void mmio_write32_le(uint32_t addr, uint32_t val)
{
	__asm__ volatile ("stwbrx %0, 0, %1; eieio"
			  : : "r"(val), "r"(addr) : "memory");
}

#endif /* IO_H */
