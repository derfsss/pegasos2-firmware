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
#include "extint.h"

#define EI_MAX_BIT  64

static ei_handler_t g_ei_handlers[EI_MAX_BIT];

void
ei_init(void)
{
	/* Mask every cause from reaching CPU0. */
	mv64361_write32(MV_IC_CPU0_MASK_LOW,  0u);
	mv64361_write32(MV_IC_CPU0_MASK_HIGH, 0u);

	/* Clear all GPP pending causes (RW1C: writing 1 clears the
	 * pending bit on the MV64361; harmless if already clear). */
	mv64361_write32(MV_GPP_INT_CAUSE, 0xFFFFFFFFu);

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
