/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  U-Boot <asm/io.h> shim. The core x86emu does not use readl/writel
 *  -- those appear only in the besys.c/biosemu.c wrappers we are
 *  replacing. This header stays empty so the core compiles cleanly.
 */

#ifndef COMPAT_ASM_IO_H
#define COMPAT_ASM_IO_H

#endif
