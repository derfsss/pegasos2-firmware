/*
 * Pegasos II firmware linker script.
 *
 * Flash ROM maps at the top 512 KiB of the 32-bit address space;
 * the MPC7447 begins executing at 0xFFF00100 after hard reset.
 *
 * DRAM is the runtime home of writable data: .data (with an LMA
 * in flash so initial values ship in the binary) and .bss (NOBITS,
 * zeroed by the reset trampoline). Everything below 0x00100000 is
 * left for the Phase-1 stack.
 *
 * Layout (runtime view):
 *   0x00000000..0x000FFFFF  scratch + stack (stack grows down from 1 MiB)
 *   0x00100000..             .data then .bss
 *   0x00200000..0x002FFFFF   1 MiB x86 emulator buffer (carved out in C)
 *   0xFFF00000..0xFFF7FFFF   flash: .text + .rodata + LMA(.data)
 */

OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(_reset_vector)

MEMORY
{
    flash (rx)  : ORIGIN = 0xFFF00000, LENGTH = 512K
    dram  (rwx) : ORIGIN = 0x00100000, LENGTH = 1M
}

SECTIONS
{
    .rom 0xFFF00000 : {
        FILL(0xffffffff);

        . = . + 0x100;

        _text_start = .;
        KEEP(*(.reset))
        *(.text .text.*)
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
        _text_end = .;

        _data_lma = .;
    } > flash = 0xffffffff

    .data : AT(_data_lma) {
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        *(.sdata .sdata.*)
        . = ALIGN(4);
        *(.sdata2 .sdata2.*)
        . = ALIGN(4);
        _data_end = .;
    } > dram

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(.sbss2 .sbss2.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > dram

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame)
        *(.gnu.attributes)
    }

    /* Code + read-only must fit in 512 KiB flash. */
    ASSERT(_text_end - 0xFFF00000 + (_data_end - _data_start) <= 0x80000,
           "ERROR: firmware image exceeds 512 KiB flash.")
}
