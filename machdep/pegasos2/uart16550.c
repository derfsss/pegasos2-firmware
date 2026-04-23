/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Polled 16550 UART driver. No interrupts, no FIFO management beyond
 *  the reset sequence. Sufficient for Phase-1 console output.
 */

#include "uart16550.h"
#include "io.h"
#include "pegasos2.h"

void uart_init(uint32_t base)
{
	/* 115200 8N1 @ 1.8432 MHz reference; divisor = 1. QEMU ignores
	 * the divisor but real hardware needs it. */
	mmio_write8(base + UART16550_LCR, 0x80);  /* DLAB = 1 */
	mmio_write8(base + UART16550_DLL, 0x01);
	mmio_write8(base + UART16550_DLM, 0x00);
	mmio_write8(base + UART16550_LCR, 0x03);  /* 8N1, DLAB = 0 */
	mmio_write8(base + UART16550_FCR, 0x07);  /* FIFO on, reset RX+TX */
	mmio_write8(base + UART16550_MCR, 0x03);  /* DTR + RTS */
}

void uart_putc(uint32_t base, char c)
{
	while ((mmio_read8(base + UART16550_LSR) & UART16550_LSR_THRE) == 0)
		;
	mmio_write8(base + UART16550_THR, (uint8_t)c);
}

void uart_puts(uint32_t base, const char *s)
{
	while (*s) {
		if (*s == '\n')
			uart_putc(base, '\r');
		uart_putc(base, *s++);
	}
}

static const char hex_digits[] = "0123456789ABCDEF";

static void put_hex_n(uint32_t base, uint32_t v, int nibbles)
{
	int i;
	for (i = (nibbles - 1) * 4; i >= 0; i -= 4)
		uart_putc(base, hex_digits[(v >> i) & 0xF]);
}

void uart_put_hex8 (uint32_t base, uint8_t  v) { put_hex_n(base, v, 2); }
void uart_put_hex16(uint32_t base, uint16_t v) { put_hex_n(base, v, 4); }
void uart_put_hex32(uint32_t base, uint32_t v) { put_hex_n(base, v, 8); }

int uart_poll_rx(uint32_t base)
{
	if ((mmio_read8(base + UART16550_LSR) & UART16550_LSR_DR) == 0)
		return -1;
	return mmio_read8(base + UART16550_RBR);
}

uint8_t uart_getc(uint32_t base)
{
	int c;
	do {
		c = uart_poll_rx(base);
	} while (c < 0);
	return (uint8_t)c;
}

void uart_enable_rx_irq(uint32_t base)
{
	/* IER bit 0 = ERBFI (Enable Received Data Available Interrupt).
	 * Leave THRE / RLSI / MSI interrupts off. */
	mmio_write8(base + UART16550_IER, 0x01);
}
