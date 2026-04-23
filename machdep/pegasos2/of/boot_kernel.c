/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Minimal spec 07 boot loader -- QEMU-first slice. Assumes the
 *  target image has already been placed in DRAM, typically via
 *  QEMU's `-kernel <elf-path>` which load_elf()'s the ELF into
 *  DRAM at its PT_LOAD addresses before the firmware runs. Our
 *  job is reduced to: validate the ELF at the caller-supplied
 *  load base, extract e_entry, set up the spec 07 register
 *  contract, and jump.
 *
 *  Filesystem + block-device support belongs to a later milestone
 *  that adds a real `boot` command per the full spec 07 flow.
 *  This file ships the smallest thing that proves the register
 *  handoff end-to-end on QEMU.
 */

#include "defs.h"

/*
 * Minimal Elf32 Ehdr layout we need. Avoids pulling in <elf.h>;
 * this file builds under SF_CFLAGS which has its own include
 * chain. Values are spec-standard and won't change.
 */
#define ELF_MAGIC_0   0x7Fu
#define ELF_MAGIC_1   'E'
#define ELF_MAGIC_2   'L'
#define ELF_MAGIC_3   'F'
#define ELF_CLASS_32  1u
#define ELF_DATA_MSB  2u
#define ELF_VERSION_1 1u
#define ELF_TYPE_EXEC 2u
#define ELF_MACHINE_PPC 20u

extern void machine_jump_os(uInt entry, uInt ci_handler_addr);
extern int  ci_handler(void *args);

/* Embedded test kernel image, linked in from build/test_kernel.elf
 * via objcopy. See Makefile $(TK_OBJ) rule. */
extern uChar _test_kernel_start[];
extern uChar _test_kernel_end[];

/*
 * Read a big-endian 16 or 32-bit field from a byte pointer.
 * The PPC CPU is BE natively; a direct load would work, but
 * open-coding the fetch keeps us alignment-safe and avoids the
 * compiler second-guessing the memory model on an arbitrary
 * user-supplied address.
 */
static uInt
be16(const uChar *p)
{
	return ((uInt)p[0] << 8) | (uInt)p[1];
}

static uInt
be32(const uChar *p)
{
	return ((uInt)p[0] << 24) | ((uInt)p[1] << 16) |
	       ((uInt)p[2] << 8)  | (uInt)p[3];
}

/*
 * `boot-kernel ( load-addr -- )`
 *
 * Validates the ELF header at load-addr, extracts e_entry, and
 * hands off per spec 07. On any validation failure, prints a
 * diagnostic and returns to the `ok` prompt (no transfer).
 *
 * Interactive usage (with `qemu ... -bios firmware.bin -kernel bboot`):
 *
 *     ok 200000 boot-kernel
 *     [kernel output from 0x200000 onward]
 *
 * The load-addr must match the ELF's PT_LOAD p_vaddr (0x200000 for
 * bboot). QEMU's load_elf() has already placed the bytes there;
 * we only parse the header to find e_entry.
 */
CC(f_boot_kernel)
{
	uInt addr;

	IFCKSP(e, 1, 0);
	POP(e, addr);

	const uChar *eh = (const uChar *)(uPtr)addr;

	/* e_ident[EI_MAG0..3]: 0x7F 'E' 'L' 'F' */
	if (eh[0] != ELF_MAGIC_0 || eh[1] != ELF_MAGIC_1 ||
	    eh[2] != ELF_MAGIC_2 || eh[3] != ELF_MAGIC_3) {
		cprintf(e, "boot-kernel: no ELF magic at 0x%X\n",
		        (unsigned)addr);
		return NO_ERROR;
	}

	/* e_ident[EI_CLASS]=1 (32-bit), [EI_DATA]=2 (MSB),
	 * [EI_VERSION]=1 */
	if (eh[4] != ELF_CLASS_32) {
		cprintf(e, "boot-kernel: not ELFCLASS32 (got %d)\n", eh[4]);
		return NO_ERROR;
	}
	if (eh[5] != ELF_DATA_MSB) {
		cprintf(e, "boot-kernel: not ELFDATA2MSB (got %d)\n", eh[5]);
		return NO_ERROR;
	}
	if (eh[6] != ELF_VERSION_1) {
		cprintf(e, "boot-kernel: bad ELF version (got %d)\n", eh[6]);
		return NO_ERROR;
	}

	/* e_type (ET_EXEC=2) and e_machine (EM_PPC=20) at offsets 16, 18. */
	uInt e_type    = be16(eh + 16);
	uInt e_machine = be16(eh + 18);
	if (e_type != ELF_TYPE_EXEC) {
		cprintf(e, "boot-kernel: not ET_EXEC (got %d)\n",
		        (int)e_type);
		return NO_ERROR;
	}
	if (e_machine != ELF_MACHINE_PPC) {
		cprintf(e, "boot-kernel: not EM_PPC (got %d)\n",
		        (int)e_machine);
		return NO_ERROR;
	}

	/* e_entry at offset 24 (32-bit BE). */
	uInt entry = be32(eh + 24);

	/* Walk program headers (spec 07 says PT_LOAD) and memcpy each
	 * loadable segment to its p_vaddr. We're the loader -- the bytes
	 * at `addr` are the raw ELF file; PT_LOAD tells us where to
	 * place them. Zero-fill from p_filesz to p_memsz for BSS. */
	uInt phoff    = be32(eh + 28);   /* e_phoff   */
	uInt phentsz  = be16(eh + 42);   /* e_phentsize */
	uInt phnum    = be16(eh + 44);   /* e_phnum   */

	if (phoff == 0 || phnum == 0) {
		cprintf(e, "boot-kernel: no program headers\n");
		return NO_ERROR;
	}

	for (uInt i = 0; i < phnum; i++) {
		const uChar *ph   = eh + phoff + i * phentsz;
		uInt p_type       = be32(ph + 0);
		uInt p_offset     = be32(ph + 4);
		uInt p_vaddr      = be32(ph + 8);
		uInt p_filesz     = be32(ph + 16);
		uInt p_memsz      = be32(ph + 20);

		if (p_type != 1u)                /* PT_LOAD only */
			continue;
		if (p_memsz == 0)
			continue;

		uChar *dst = (uChar *)(uPtr)p_vaddr;
		const uChar *src = eh + p_offset;

		cprintf(e, "boot-kernel:   PT_LOAD #%d -> 0x%X (%d/%d)\n",
		        (int)i, (unsigned)p_vaddr, (int)p_filesz, (int)p_memsz);

		for (uInt j = 0; j < p_filesz; j++)
			dst[j] = src[j];
		for (uInt j = p_filesz; j < p_memsz; j++)
			dst[j] = 0;
	}

	cprintf(e, "boot-kernel: ELF OK at 0x%X, e_entry=0x%X\n",
	        (unsigned)addr, (unsigned)entry);
	cprintf(e, "             transferring control...\n");

	machine_jump_os(entry, (uInt)(uPtr)&ci_handler);

	/* machine_jump_os does not return. If we somehow get here the
	 * OS image returned via blr or similar -- treat as error. */
	cprintf(e, "boot-kernel: (!) image returned control; halting\n");
	return NO_ERROR;
}

/*
 * `test-boot ( -- )` is the smoke test for the full boot-kernel
 * round trip: copies the built-in test-kernel ELF blob (linked at
 * 0x00800000) from firmware .rodata to that address in DRAM,
 * then calls the same logic as `boot-kernel`. If successful, the
 * test kernel prints "KERNEL OK r5=XXXXXXXX" to UART1 and traps
 * via `twi`, landing in panic_dump with vector 0x700.
 *
 * Expected serial output (abridged):
 *     ok test-boot
 *     test-boot: copying NNN bytes to 0x00800000
 *     boot-kernel: ELF OK at 0x800000, e_entry=0x00800000
 *                  transferring control...
 *     KERNEL OK r5=00XXXXXX
 *     !! PANIC: exception 0x00000700 (Program)
 */
CC(f_test_boot)
{
	Int  size = (Int)(_test_kernel_end - _test_kernel_start);
	uChar *dst = (uChar *)0x00800000u;

	cprintf(e, "test-boot: copying %d bytes to 0x%X\n",
	        (int)size, (unsigned)(uPtr)dst);

	for (Int i = 0; i < size; i++)
		dst[i] = _test_kernel_start[i];

	/* Push the load-addr and tail-invoke boot-kernel. Simpler
	 * than duplicating the parse+jump; also exercises the same
	 * code path a real caller would take. */
	PUSH(e, (Cell)(uPtr)dst);
	return f_boot_kernel(e);
}
