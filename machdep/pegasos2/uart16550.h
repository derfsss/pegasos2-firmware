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
void uart_put_hex32(uint32_t base, uint32_t v);

#endif
