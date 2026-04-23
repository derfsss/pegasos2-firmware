/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  External-interrupt (0x500) dispatcher for the MV64361 main
 *  interrupt controller. Spec 02 §"Interrupt controller" describes
 *  the overall model; QEMU's cascade wiring is laid out in
 *  mv64361.h and the E0 preflight commit.
 *
 *  Usage:
 *
 *    ei_init()        -- masks every cause, zeroes the handler
 *                        table, clears any pending GPP state. Must
 *                        be called before MSR[EE] is first enabled.
 *    ei_install(b,f)  -- register `f` as the handler for main-IC
 *                        cause bit b (0..63). Sets the CPU0 route
 *                        mask bit so the interrupt reaches the CPU.
 *                        Returns 0 on success, -1 on bounds error.
 *    ei_uninstall(b)  -- inverse of install.
 *
 *  Handlers run with MSR[EE]=0 (hardware clears it on exception
 *  entry). They must:
 *    - Acknowledge / clear their own cause at the device (the main
 *      IC cause bits are RO; the source device's cause register is
 *      where the ack happens -- GPP_CAUSE for GPP lines, the i8259
 *      specific-EOI for VT8231 PIC sources, etc.).
 *    - Return quickly. The dispatcher's asm trampoline rfi's back
 *      to the interrupted instruction with MSR[EE] restored.
 *    - Not themselves re-enable EE (no nested-interrupt support).
 */

#ifndef EXTINT_H
#define EXTINT_H

#include <stdint.h>

typedef void (*ei_handler_t)(void);

void ei_init(void);
int  ei_install(int cause_bit, ei_handler_t handler);
int  ei_uninstall(int cause_bit);

/* Called from the 0x500 asm trampoline after it has saved caller
 * state. Walks the cause registers and invokes registered handlers
 * for any bit that is both set in cause and registered. */
void ei_dispatch(void);

/* --------------------------------------------------------------- *
 *  UART1 RX consumer -- first real ExtInt client                   *
 * --------------------------------------------------------------- */

/* Arm the VT8231 i8259 + UART IER + MV64361 GPP31 cascade chain
 * and register an RX handler on main cause bit 59. Programs the
 * CUnit arbiter to level-triggered GPP, which is required for
 * the MV_PCI1_INTA_VIRT read in the handler to advance i8259
 * state. After this call, MSR[EE]=1 arms delivery. */
void extint_uart_install(void);

/* Pop one byte from the interrupt-fed RX ring. Returns -1 if
 * empty. Lock-free single-producer (handler) / single-consumer
 * (failsafe_read); handlers run with MSR[EE]=0 so no interleave
 * is possible on UP. */
int extint_uart_pop(void);

#endif /* EXTINT_H */
