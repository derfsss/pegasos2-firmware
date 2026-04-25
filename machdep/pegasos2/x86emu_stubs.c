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
 *  Scope: the minimum set of symbols the emulator requires to LINK
 *  that nobody else provides. memset/memcpy/memmove/strlen come from
 *  SmartFirmware's stdlib.c (used firmware-wide); we only stub here
 *  what's left. The x86emu never calls printf/sprintf at runtime in
 *  our use case -- the option-ROM path emits output via the ROM's
 *  own BIOS services -- so these stubs are no-ops, just enough to
 *  keep the emulator linkable.
 */

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

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
