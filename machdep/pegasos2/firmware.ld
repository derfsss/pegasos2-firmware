/*
 * Pegasos II firmware linker script.
 *
 * Flash ROM lives at the top 512 KiB of the 32-bit address space
 * (0xFFF00000..0xFFFFFFFF). After hard reset the MPC7447 begins
 * executing at 0xFFF00100 - see docs/01-cpu-init.md.
 *
 * The first 256 bytes of ROM (0xFFF00000..0xFFF000FF) are unused
 * in this layout and filled with 0xFF (unprogrammed flash).
 */

OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(_reset_vector)

MEMORY
{
    flash (rx) : ORIGIN = 0xFFF00000, LENGTH = 512K
}

SECTIONS
{
    .rom 0xFFF00000 : {
        FILL(0xffffffff);

        /* 0xFFF00000..0xFFF000FF: unused head of ROM, padded with 0xFF. */
        . = . + 0x100;

        /* 0xFFF00100: reset vector. */
        _text_start = .;
        KEEP(*(.reset))

        /* Firmware body. */
        *(.text .text.*)
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
        *(.data .data.*)
        . = ALIGN(4);
        _text_end = .;
    } > flash = 0xffffffff

    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    } > flash

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame)
        *(.gnu.attributes)
    }

    /* Firmware must fit in the 512 KiB flash. */
    ASSERT(_end - 0xFFF00000 <= 0x80000,
           "ERROR: firmware image exceeds 512 KiB flash.")
}
