/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Millisecond timer + MSR[EE] helpers.
 *
 *  The decrementer exception handler at 0x00000900 (see exceptions.S)
 *  increments a tick counter each time SPR 22 (DEC) underflows. This
 *  header exposes the C-side API to arm the decrementer, enable or
 *  disable the external-interrupt MSR bit, and read the counter.
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/*
 * Decrementer reload value. SPR 22 decrements at the time-base
 * frequency = bus_clock / 4. On QEMU's pegasos2 the TB runs somewhere
 * in the 25-100 MHz range depending on version; 25000 ticks gives
 * roughly a 1 ms period at a 25 MHz TB and ~0.25 ms at 100 MHz.
 * Either is fine for the Phase-1 self-test -- a real-HW build will
 * calibrate this from the W83194 / HID1[PLL_CFG] readout per
 * docs/01-cpu-init.md §"Clock detection".
 *
 * The same literal is hardcoded in exceptions.S's 0x900 handler.
 */
#define DEC_TICKS_PER_MS    25000u

/* Arm the decrementer (SPR 22) with `ticks`. It begins counting down
 * immediately; the handler at 0x900 re-arms itself after each fire. */
void timer_arm(uint32_t ticks);

/* Read the tick counter that the decrementer handler increments. */
uint32_t get_msecs(void);

/* Toggle MSR[EE]. Wraps mfmsr/mtmsr in a sync/isync envelope so the
 * change is context-synchronising on MPC7447. */
void enable_ei(void);
void disable_ei(void);

#endif
