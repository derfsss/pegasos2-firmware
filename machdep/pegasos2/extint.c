/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  External-interrupt dispatcher. See extint.h for the API
 *  contract and mv64361.h for the cascade wiring. The 0x500
 *  vector stub in exceptions.S saves caller state and branches
 *  to ei_dispatch(); we consult cause + mask, walk a 64-entry
 *  handler table, and return. The asm restores state and rfi's.
 *
 *  No handler is registered by default: ei_init() is called from
 *  phase1_c_main() before any MSR[EE]=1, so even if a spurious
 *  cause fires, CPU0_MASK is zero and nothing reaches us.
 *
 *  Today no consumer registers a handler. ei_install() is ready
 *  for a future commit that wires a specific source (UART RX,
 *  timer ticks, PCI MSI, ...). Left in this state deliberately
 *  per the "infrastructure lands, first real use comes later"
 *  decision.
 */

#include "pegasos2.h"
#include "io.h"
#include "mv64361.h"
#include "vt8231.h"
#include "uart16550.h"
#include "extint.h"

#define EI_MAX_BIT  64

static ei_handler_t g_ei_handlers[EI_MAX_BIT];

void
ei_init(void)
{
	/* Mask every cause from reaching CPU0. */
	mv64361_write32(MV_IC_CPU0_MASK_LOW,  0u);
	mv64361_write32(MV_IC_CPU0_MASK_HIGH, 0u);

	/* Mask every GPP pin from the main IC. Even if a downstream
	 * device asserts its GPP pin, it won't reach main cause bits. */
	mv64361_write32(MV_GPP_INT_MASK0, 0u);

	/* Clear our handler table. */
	for (int i = 0; i < EI_MAX_BIT; i++)
		g_ei_handlers[i] = (ei_handler_t)0;
}

int
ei_install(int cause_bit, ei_handler_t handler)
{
	if (cause_bit < 0 || cause_bit >= EI_MAX_BIT || handler == (ei_handler_t)0)
		return -1;

	g_ei_handlers[cause_bit] = handler;

	uint32_t mask_reg = (cause_bit < 32) ? MV_IC_CPU0_MASK_LOW
					     : MV_IC_CPU0_MASK_HIGH;
	uint32_t bit = 1u << (cause_bit & 31);
	mv64361_write32(mask_reg, mv64361_read32(mask_reg) | bit);

	return 0;
}

int
ei_uninstall(int cause_bit)
{
	if (cause_bit < 0 || cause_bit >= EI_MAX_BIT)
		return -1;

	uint32_t mask_reg = (cause_bit < 32) ? MV_IC_CPU0_MASK_LOW
					     : MV_IC_CPU0_MASK_HIGH;
	uint32_t bit = 1u << (cause_bit & 31);
	mv64361_write32(mask_reg, mv64361_read32(mask_reg) & ~bit);

	g_ei_handlers[cause_bit] = (ei_handler_t)0;
	return 0;
}

void
ei_dispatch(void)
{
	/* AND cause with CPU0 mask: we only service bits enabled to
	 * reach CPU0, matching the cascade hardware already decided
	 * was worth firing the external-interrupt line. */
	uint32_t lo = mv64361_read32(MV_IC_MAIN_CAUSE_LOW) &
		      mv64361_read32(MV_IC_CPU0_MASK_LOW);
	uint32_t hi = mv64361_read32(MV_IC_MAIN_CAUSE_HIGH) &
		      mv64361_read32(MV_IC_CPU0_MASK_HIGH);

	for (int b = 0; b < 32; b++) {
		if ((lo & (1u << b)) && g_ei_handlers[b] != (ei_handler_t)0)
			g_ei_handlers[b]();
	}
	for (int b = 0; b < 32; b++) {
		if ((hi & (1u << b)) && g_ei_handlers[32 + b] != (ei_handler_t)0)
			g_ei_handlers[32 + b]();
	}
}

/* --------------------------------------------------------------- *
 *  UART1 RX consumer                                                *
 * --------------------------------------------------------------- */

#define RX_RING_SIZE  128u

static volatile uint8_t  g_rx_ring[RX_RING_SIZE];
static volatile uint32_t g_rx_head;
static volatile uint32_t g_rx_tail;

static void
uart_rx_handler(void)
{
	/*
	 * Pegasos2 cascade handler discipline:
	 *
	 *   1. Read MV_PCI1_INTA_VIRT. In level mode this calls
	 *      pic_read_irq on the master i8259, which sets ISR for
	 *      the pending IRQ, clears IRR (for edge-sensitive PIC
	 *      inputs), and re-evaluates int_out. The returned value
	 *      is IRQ_BASE + irq_number but we don't use it -- the
	 *      side effect is what matters. Without this step the
	 *      i8259 keeps "intr" asserted, GPP31 stays high, cause
	 *      bit 59 stays latched, and we storm.
	 *   2. Device-specific ack: drain UART RBR until LSR[DR]=0.
	 *      Each RBR read clears the UART's own int line.
	 *   3. i8259 EOI (OCW2 = 0x20) to clear the ISR bit we set
	 *      in step 1; in level-triggered PIC mode this also lets
	 *      "intr" track the underlying IRR state post-drain.
	 */
	(void)mv64361_read32(MV_PCI1_INTA_VIRT);

	int c;
	while ((c = uart_poll_rx(UART1_BASE)) >= 0) {
		uint32_t next = (g_rx_head + 1u) % RX_RING_SIZE;
		if (next != g_rx_tail) {
			g_rx_ring[g_rx_head] = (uint8_t)c;
			g_rx_head = next;
		}
		/* Ring full: drop byte. Shouldn't happen for interactive
		 * typing; only matters for scripted mon:stdio floods. */
	}

	vt8231_pic_eoi_master();
}

void
extint_uart_install(void)
{
	/* Level-triggered GPP plane. Required for (a) auto-clear of
	 * main cause 59 on falling edge and (b) the INTA-virtual
	 * register's pic_read_irq gating. Must precede the cascade
	 * arming below. */
	mv64361_write32(MV_CUNIT_ARB_CTRL,
	                mv64361_read32(MV_CUNIT_ARB_CTRL) |
	                MV_CUNIT_ARB_CTRL_GPP_LEVEL);

	/* Master + slave i8259 init, UART IRQ 4 unmasked on master. */
	vt8231_pic_init();
	vt8231_pic_unmask_master(4);

	/* UART IER[0] raises an interrupt on RX-ready transitions. */
	uart_enable_rx_irq(UART1_BASE);

	/* GPP31 cascade from VT8231 "intr" -> main cause bit 59. */
	mv64361_write32(MV_GPP_INT_MASK0,
	                mv64361_read32(MV_GPP_INT_MASK0) | (1u << 31));

	/* Register the handler + unmask cause 59 in CPU0 route.
	 * Final gate is MSR[EE] -- enabled by the caller afterwards. */
	(void)ei_install(MV_IC_BIT_P0_GPP24_31, uart_rx_handler);
}

int
extint_uart_pop(void)
{
	if (g_rx_head == g_rx_tail)
		return -1;
	uint8_t c = g_rx_ring[g_rx_tail];
	g_rx_tail = (g_rx_tail + 1u) % RX_RING_SIZE;
	return c;
}
