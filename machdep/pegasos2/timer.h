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
 * Pegasos II board-standard clock rates. Used by timer_calibrate()
 * as the reset-time assumption when no probe has run. The real-HW
 * path for detecting an overridden FSB is a W83194 SMBus probe,
 * per docs/01-cpu-init.md §"Clock detection"; that probe is TBD
 * and will overwrite these via timer_set_fsb().
 *
 * On MPC7447A the time-base counter increments at bus_clock / 4,
 * a fixed ratio (no HID0 / L2CR configuration to check).
 */
#define PEGASOS2_FSB_HZ_DEFAULT   133000000u        /* 133 MHz board default  */
#define PEGASOS2_TB_HZ_DEFAULT    (PEGASOS2_FSB_HZ_DEFAULT / 4u)
#define PEGASOS2_DEC_TICKS_PER_MS (PEGASOS2_TB_HZ_DEFAULT / 1000u)

/*
 * Seed the decrementer reload value from the assumed clock rates.
 * Must be called at least once BEFORE MSR[EE] is enabled; otherwise
 * the 0x900 handler reads _dec_reload == 0 from zero-initialised
 * .bss and loops forever.
 */
void timer_calibrate(void);

/* Read-back accessors for the calibration state (diagnostic). */
uint32_t timer_fsb_hz(void);
uint32_t timer_tb_hz(void);
uint32_t timer_ms_reload(void);

/* Arm the decrementer (SPR 22) with `ticks`. It begins counting down
 * immediately; the handler at 0x900 re-arms itself after each fire
 * using the calibrated reload value. */
void timer_arm(uint32_t ticks);

/* Read the tick counter that the decrementer handler increments. */
uint32_t get_msecs(void);

/* Toggle MSR[EE]. Wraps mfmsr/mtmsr in a sync/isync envelope so the
 * change is context-synchronising on MPC7447. */
void enable_ei(void);
void disable_ei(void);

#endif
