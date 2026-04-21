/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Pegasos II firmware reset vector - Phase 0 placeholder.
 *
 *  On hard reset the MPC7447 starts execution at 0xFFF00100 (see
 *  docs/01-cpu-init.md section "Reset vector"). This file will grow
 *  into the full CPU init sequence specified in that chapter. For
 *  now it exists only to give the build system something to compile
 *  and to prove that the toolchain emits a correctly-placed 512 KiB
 *  firmware-raw.bin.
 */

	.section .reset, "ax", @progbits
	.globl   _reset_vector
	.type    _reset_vector, @function

_reset_vector:
	/* TODO: CPU init per docs/01-cpu-init.md. Phase 0: spin in place. */
	b       _reset_vector

	.size _reset_vector, . - _reset_vector
