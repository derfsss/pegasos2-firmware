/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Minimal U-Boot <common.h> replacement, just enough to satisfy the
 *  vendored upstream/x86emu/ source tree when compiled outside
 *  U-Boot. Provides the u8/u16/u32/u64 and s8/s16/s32 typedefs,
 *  plus the CONFIG_PPC define the emulator uses for its asm-
 *  comment-char selection.
 */

#ifndef COMPAT_COMMON_H
#define COMPAT_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* Architecture flags sampled by x86emu headers. */
#define CONFIG_PPC  1

/* U-Boot's fixed-width integer aliases. */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef unsigned int uint;
typedef unsigned long ulong;

/* Freestanding libc shims. Implementations live in x86emu_stubs.c. */
void *memset(void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);

/* Minimal printf/sprintf surface used only by x86emu debug paths.
 * Current impl is a no-op; full UART-routed printf is a later task. */
int printf (const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);

/* U-Boot env-var access. decode.c calls getenv("x86emu") to pick a
 * cycle-count limit; returning NULL leaves the default. */
char *getenv(const char *name);
int   atoi  (const char *s);

#endif
