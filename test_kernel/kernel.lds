/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Linker script for the spec-07 boot-kernel smoke-test image.
 *
 *  Links at 0x00800000 (8 MiB). This address is:
 *    - past the firmware's Phase-1 stack (0x000FFFC0) and .data/.bss
 *      (below 0x00200000);
 *    - past the x86 emulator buffer (0x00200000..0x002FFFFF);
 *    - past SmartFirmware's malloc pool (0x00300000..0x006FFFFF);
 *    - inside QEMU's -m 512 DRAM ceiling (0x20000000).
 *
 *  No dependencies on firmware memory layout beyond the UART1 MMIO
 *  address (hard-coded in kernel.S). The Forth `test-boot` word
 *  copies this ELF to its linked address before calling boot-kernel.
 */

OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(__start)

SECTIONS
{
    . = 0x00800000;

    .text : {
        *(.text.start)
        *(.text .text.*)
        . = ALIGN(4);
    }

    .data : {
        *(.data .data.*)
        . = ALIGN(4);
    }

    .bss (NOLOAD) : {
        *(.bss .bss.*)
        . = ALIGN(4);
    }

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.eh_frame)
    }
}
