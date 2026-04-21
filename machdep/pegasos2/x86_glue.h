/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Pegasos2 glue for the vendored x86emu core -- memory buffer
 *  placement, port-I/O routing, BDA/IVT initialisation. Keeps the
 *  x86emu tree unmodified (clean-room re-vendor path stays open).
 */

#ifndef X86_GLUE_H
#define X86_GLUE_H

#include <stdint.h>

/* Where the emulator's 1 MiB of real-mode memory lives in our
 * address space. DRAM-backed, placed past the 1 MiB mark so it
 * does not collide with the stack (ceiling 0x00100000) or with
 * Phase-1 scratch at 0x1000. */
#define X86EMU_MEM_PADDR   0x00200000u
#define X86EMU_MEM_SIZE    0x00100000u

/* Pegasos2-visible linear pointer into the emulator's 1 MiB. */
uint8_t *x86emu_mem(uint32_t x86_addr);

/* Initialise memory/pio callbacks + zero the 1 MiB buffer +
 * populate BDA and IVT per docs/09-known-bugs.md Bug 1 Required
 * Behaviour. Idempotent. */
void x86_glue_init(void);

/* Run a real-mode program already placed in emulator memory, with
 * CS:IP and register state set by the caller. Returns after HLT or
 * after the emulator's internal cycle budget is exhausted. */
void x86emu_run(void);

#endif
