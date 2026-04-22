/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Millisecond timer + MSR[EE] helpers.
 */

#include <stdint.h>
#include "timer.h"

/* Declared (and defined, in .bss) by exceptions.S. Volatile so the
 * compiler reloads it after any store that could be the decrementer
 * handler firing between adjacent reads. */
extern volatile uint32_t _ms_tick_count;

void timer_arm(uint32_t ticks)
{
	__asm__ volatile ("mtspr 22, %0" :: "r"(ticks) : "memory");
}

uint32_t get_msecs(void)
{
	return _ms_tick_count;
}

/*
 * MSR[EE] = bit 16 (MSB=0 numbering) = value 0x00008000.
 *
 * mtmsr alone is not context-synchronising on MPC7447A; the sync /
 * isync envelope guarantees that the store is globally visible and
 * that the next fetch uses the new MSR. Same pattern as reset.S.
 */
#define MSR_EE_MASK 0x00008000u

void enable_ei(void)
{
	uint32_t msr;
	__asm__ volatile ("mfmsr %0" : "=r"(msr));
	msr |= MSR_EE_MASK;
	__asm__ volatile ("sync; mtmsr %0; isync" :: "r"(msr) : "memory");
}

void disable_ei(void)
{
	uint32_t msr;
	__asm__ volatile ("mfmsr %0" : "=r"(msr));
	msr &= ~MSR_EE_MASK;
	__asm__ volatile ("sync; mtmsr %0; isync" :: "r"(msr) : "memory");
}
