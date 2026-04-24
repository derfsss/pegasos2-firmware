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
#include "vt8231.h"

/* Declared (and defined, in .bss) by exceptions.S. Volatile so the
 * compiler reloads them after any store that could be the decrementer
 * handler firing between adjacent reads. */
extern volatile uint32_t _ms_tick_count;
extern volatile uint32_t _dec_reload;

static uint32_t s_fsb_hz;
static uint32_t s_tb_hz;

void timer_calibrate(void)
{
	/*
	 * Try the W83194 clock-synthesizer probe first. On real HW
	 * this returns the actual FSB the board was strapped to (one
	 * of 66/75/83/100/120/133/150/166 MHz). On QEMU the probe
	 * fails (no VT8231 fn 4 model) and we fall back to the board
	 * default of 133 MHz. Either way the decrementer tick is
	 * monotonically increasing, which is what the self-test
	 * requires; the wall-clock accuracy delta only matters on
	 * real HW.
	 */
	unsigned probed = vt8231_w83194_fsb_hz();
	s_fsb_hz     = probed ? probed : PEGASOS2_FSB_HZ_DEFAULT;
	s_tb_hz      = s_fsb_hz / 4u;
	_dec_reload  = s_tb_hz / 1000u;
}

uint32_t timer_fsb_hz   (void) { return s_fsb_hz; }
uint32_t timer_tb_hz    (void) { return s_tb_hz;  }
uint32_t timer_ms_reload(void) { return _dec_reload; }

void timer_arm(uint32_t ticks)
{
	__asm__ volatile ("mtspr 22, %0" :: "r"(ticks) : "memory");
}

uint32_t pegasos2_get_msecs_ticks(void)
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
