/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Freestanding libc stubs for the vendored x86emu core.
 *
 *  Scope: the minimum set of symbols the emulator requires to LINK.
 *  Current milestone does not execute the emulator at runtime, so
 *  printf/sprintf are no-ops. When Phase 2 wires the emulator to a
 *  real Option ROM loader we will route printf to the UART and give
 *  sprintf a real implementation (or remove its callers).
 */

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

void *memset(void *s, int c, size_t n)
{
	unsigned char *p = (unsigned char *)s;
	while (n--)
		*p++ = (unsigned char)c;
	return s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char       *d = (unsigned char       *)dst;
	const unsigned char *s = (const unsigned char *)src;
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char       *d = (unsigned char       *)dst;
	const unsigned char *s = (const unsigned char *)src;
	if (d == s || n == 0)
		return dst;
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

int printf(const char *fmt, ...)
{
	(void)fmt;
	return 0;
}

int sprintf(char *buf, const char *fmt, ...)
{
	(void)fmt;
	if (buf)
		buf[0] = '\0';
	return 0;
}

char *getenv(const char *name)
{
	(void)name;
	return (char *)0;
}

int atoi(const char *s)
{
	(void)s;
	return 0;
}
