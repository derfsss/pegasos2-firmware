/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 */

#ifndef UART16550_H
#define UART16550_H

#include <stdint.h>

void uart_init(uint32_t base);
void uart_putc(uint32_t base, char c);
void uart_puts(uint32_t base, const char *s);
void uart_put_hex8 (uint32_t base, uint8_t  v);
void uart_put_hex16(uint32_t base, uint16_t v);
void uart_put_hex32(uint32_t base, uint32_t v);

/* Non-blocking polled RX. Returns the byte in [0..255] if one is
 * available, or -1 if the UART has no data pending. */
int  uart_poll_rx(uint32_t base);

/* Blocking polled RX. Spins until one byte is available, returns it. */
uint8_t uart_getc(uint32_t base);

#endif
