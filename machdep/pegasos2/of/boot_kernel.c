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
#define ELF_TYPE_DYN  3u
#define ELF_MACHINE_PPC 20u

/* Validation caps. Anything beyond these is either nonsense or a
 * hostile/corrupt image; reject rather than hand garbage to the
 * loader. Standard ELF32 has phentsz == 32 exactly; the cap lets a
 * future image add fields without us coercing them to zero, but
 * keeps the upper bound finite. phnum cap = 32 is generous (Linux
 * PPC kernels ship ~4 PT_LOADs). */
#define MIN_PHENTSZ   32u
#define MAX_PHENTSZ   256u
#define MAX_PHNUM     32u

/* Flash mapping on QEMU (0xFFF00000..0xFFF7FFFF) and on real HW
 * (0xFFF80000..0xFFFFFFFF) both fall inside this range; PT_LOAD
 * into it is rejected to avoid write-amplification on the real flash
 * part and to keep the firmware self-consistent during the handoff
 * window. */
#define FLASH_RANGE_START 0xFFF00000u

extern void machine_jump_os(uInt entry, uInt ci_handler_addr, uInt stack_top);
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

/* Wraparound-safe 32-bit addition. Returns non-zero on overflow. */
static int
u32_add_ovf(uInt a, uInt b, uInt *out)
{
	*out = a + b;
	return (*out < a) ? 1 : 0;
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

	/* e_type and e_machine at offsets 16, 18. Spec 07 §ELF accepts
	 * ET_EXEC (2) and ET_DYN (3) -- the latter covers statically
	 * bound position-independent kernels. */
	uInt e_type    = be16(eh + 16);
	uInt e_machine = be16(eh + 18);
	if (e_type != ELF_TYPE_EXEC && e_type != ELF_TYPE_DYN) {
		cprintf(e, "boot-kernel: not ET_EXEC or ET_DYN (got %d)\n",
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

	uInt phoff    = be32(eh + 28);   /* e_phoff   */
	uInt phentsz  = be16(eh + 42);   /* e_phentsize */
	uInt phnum    = be16(eh + 44);   /* e_phnum   */

	if (phoff == 0 || phnum == 0) {
		cprintf(e, "boot-kernel: no program headers\n");
		return NO_ERROR;
	}
	if (phentsz < MIN_PHENTSZ || phentsz > MAX_PHENTSZ) {
		cprintf(e, "boot-kernel: bad phentsz=%d\n", (int)phentsz);
		return NO_ERROR;
	}
	if (phnum > MAX_PHNUM) {
		cprintf(e, "boot-kernel: phnum=%d exceeds cap %d\n",
		        (int)phnum, (int)MAX_PHNUM);
		return NO_ERROR;
	}
	uInt pht_span;
	if (u32_add_ovf(phoff, phnum * phentsz, &pht_span)) {
		cprintf(e, "boot-kernel: phoff+phnum*phentsz overflow\n");
		return NO_ERROR;
	}

	/* Two-pass load: validate all PT_LOADs before touching memory,
	 * so a bad image can't partially-load. Also track whether e_entry
	 * falls within any PT_LOAD's p_vaddr..p_vaddr+p_memsz range; an
	 * entry point outside every loaded segment would jump to unmapped
	 * or unloaded memory. Track the highest end address seen so we
	 * can hand the OS a stack pointer sitting immediately above the
	 * image (spec 07 §register state: r1 = valid small stack). */
	int  entry_in_phdr = 0;
	uInt high_end      = 0;
	for (uInt i = 0; i < phnum; i++) {
		const uChar *ph   = eh + phoff + i * phentsz;
		uInt p_type       = be32(ph + 0);
		uInt p_offset     = be32(ph + 4);
		uInt p_vaddr      = be32(ph + 8);
		uInt p_filesz     = be32(ph + 16);
		uInt p_memsz      = be32(ph + 20);

		if (p_type != 1u)
			continue;
		if (p_memsz == 0)
			continue;

		if (p_filesz > p_memsz) {
			cprintf(e, "boot-kernel: PT_LOAD #%d p_filesz %d > p_memsz %d\n",
			        (int)i, (int)p_filesz, (int)p_memsz);
			return NO_ERROR;
		}
		uInt src_end;
		if (u32_add_ovf(p_offset, p_filesz, &src_end)) {
			cprintf(e, "boot-kernel: PT_LOAD #%d p_offset+p_filesz overflow\n",
			        (int)i);
			return NO_ERROR;
		}
		uInt dst_end;
		if (u32_add_ovf(p_vaddr, p_memsz, &dst_end)) {
			cprintf(e, "boot-kernel: PT_LOAD #%d p_vaddr+p_memsz overflow\n",
			        (int)i);
			return NO_ERROR;
		}
		if (p_vaddr >= FLASH_RANGE_START || dst_end > FLASH_RANGE_START) {
			cprintf(e, "boot-kernel: PT_LOAD #%d p_vaddr=0x%X overlaps flash\n",
			        (int)i, (unsigned)p_vaddr);
			return NO_ERROR;
		}

		if (entry >= p_vaddr && entry < dst_end)
			entry_in_phdr = 1;
		if (dst_end > high_end)
			high_end = dst_end;
	}
	if (!entry_in_phdr) {
		cprintf(e, "boot-kernel: e_entry 0x%X not in any PT_LOAD\n",
		        (unsigned)entry);
		return NO_ERROR;
	}

	for (uInt i = 0; i < phnum; i++) {
		const uChar *ph   = eh + phoff + i * phentsz;
		uInt p_type       = be32(ph + 0);
		uInt p_offset     = be32(ph + 4);
		uInt p_vaddr      = be32(ph + 8);
		uInt p_filesz     = be32(ph + 16);
		uInt p_memsz      = be32(ph + 20);

		if (p_type != 1u)
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

	/* Stack pointer handed to the OS: 4 KiB of headroom above the
	 * highest PT_LOAD end, aligned down to 16 (PPC SysV ABI
	 * requires 16-byte alignment). Spec 07 calls this "a valid
	 * small stack in the loaded image's memory region"; we
	 * interpret "region" loosely as "just above the image" since
	 * the image itself is typically packed end-to-end and offers
	 * no free words for a stack. OSes replace r1 with their own
	 * stack within the first few instructions of entry. */
	uInt stack_top;
	if (u32_add_ovf(high_end, 0x1000u, &stack_top)) {
		cprintf(e, "boot-kernel: image top 0x%X +4KiB overflows; "
		        "using image top\n", (unsigned)high_end);
		stack_top = high_end;
	}
	stack_top &= ~0xFu;

	cprintf(e, "boot-kernel: ELF OK at 0x%X, e_entry=0x%X, "
	        "r1=0x%X\n",
	        (unsigned)addr, (unsigned)entry, (unsigned)stack_top);
	cprintf(e, "             transferring control...\n");

	machine_jump_os(entry, (uInt)(uPtr)&ci_handler, stack_top);

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

/*
 * `test-boot-bad ( -- )` exercises the hardening checks in
 * boot-kernel. Six malformed ELF headers are built in a scratch
 * buffer one at a time; each is fed to boot-kernel, which should
 * reject and return. Returning is itself the pass signal: if any
 * scenario slipped through validation it would either transfer to
 * an empty/garbage image (firmware exits, observable as loss of the
 * ok prompt) or trap (observable as !! PANIC). Reaching the final
 * "6/6 PASS" cprintf means all six rejections fired.
 */

/* Scratch ELF scratch pad. 256 bytes covers an Ehdr (52) + two
 * Phdrs (64) with slack. Lives in .bss. */
static uChar bb_scratch[256];

/*
 * Fill bb_scratch with a syntactically valid minimal ELF32 PPC BE:
 * one PT_LOAD covering e_entry. Each scenario below mutates exactly
 * one field of this template so rejection diagnostics unambiguously
 * map to the hardening check that fired.
 */
static void
bb_build_valid(void)
{
	for (uInt i = 0; i < sizeof(bb_scratch); i++)
		bb_scratch[i] = 0;

	/* Ehdr at offset 0 */
	bb_scratch[0] = ELF_MAGIC_0;
	bb_scratch[1] = ELF_MAGIC_1;
	bb_scratch[2] = ELF_MAGIC_2;
	bb_scratch[3] = ELF_MAGIC_3;
	bb_scratch[4] = ELF_CLASS_32;
	bb_scratch[5] = ELF_DATA_MSB;
	bb_scratch[6] = ELF_VERSION_1;
	/* e_type = ET_EXEC at offset 16 (u16 BE) */
	bb_scratch[17] = ELF_TYPE_EXEC;
	/* e_machine = EM_PPC at offset 18 (u16 BE) */
	bb_scratch[19] = ELF_MACHINE_PPC;
	/* e_entry = 0x00900080 at offset 24 (u32 BE) */
	bb_scratch[25] = 0x90;
	bb_scratch[27] = 0x80;
	/* e_phoff = 52 at offset 28 (u32 BE) */
	bb_scratch[31] = 52;
	/* e_phentsize = 32 at offset 42 (u16 BE) */
	bb_scratch[43] = 32;
	/* e_phnum = 1 at offset 44 (u16 BE) */
	bb_scratch[45] = 1;

	/* Phdr at offset 52 */
	uChar *p = bb_scratch + 52;
	/* p_type = PT_LOAD (1) */
	p[3] = 1;
	/* p_offset = 100 */
	p[7] = 100;
	/* p_vaddr = 0x00900080 (equal to e_entry) */
	p[9] = 0x90;
	p[11] = 0x80;
	/* p_paddr = 0x00900080 */
	p[13] = 0x90;
	p[15] = 0x80;
	/* p_filesz = 32 */
	p[19] = 32;
	/* p_memsz = 32 */
	p[23] = 32;
}

CC(f_test_boot_bad)
{
	Int total = 0;
	Int passed;

	/* Pre-invoke pass count: each boot-kernel return bumps this. */
	cprintf(e, "test-boot-bad: exercising 6 malformed-ELF scenarios\n");
	passed = 0;

	/* 1. Magic corrupted */
	bb_build_valid();
	bb_scratch[0] = 0x00;
	cprintf(e, "  [1/6] bad-magic\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	/* 2. phentsz too small (standard is 32; 8 is nonsense) */
	bb_build_valid();
	bb_scratch[42] = 0;
	bb_scratch[43] = 8;
	cprintf(e, "  [2/6] bad-phentsz\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	/* 3. phnum above cap */
	bb_build_valid();
	bb_scratch[44] = 0;
	bb_scratch[45] = 100;   /* 100 > MAX_PHNUM (32) */
	cprintf(e, "  [3/6] oversized-phnum\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	/* 4. p_filesz > p_memsz */
	bb_build_valid();
	{
		uChar *p = bb_scratch + 52;
		/* p_filesz = 0x1000, p_memsz = 32 */
		p[16] = 0; p[17] = 0; p[18] = 0x10; p[19] = 0;
		p[20] = 0; p[21] = 0; p[22] = 0;    p[23] = 32;
	}
	cprintf(e, "  [4/6] filesz-gt-memsz\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	/* 5. PT_LOAD target inside flash range */
	bb_build_valid();
	{
		uChar *p = bb_scratch + 52;
		/* p_vaddr = 0xFFF80000 (inside flash) */
		p[8]  = 0xFF; p[9]  = 0xF8; p[10] = 0x00; p[11] = 0x00;
	}
	cprintf(e, "  [5/6] load-to-flash\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	/* 6. e_entry outside every PT_LOAD */
	bb_build_valid();
	/* e_entry = 0xABCD0000; PT_LOAD covers 0x00900080..0x009000A0 */
	bb_scratch[24] = 0xAB;
	bb_scratch[25] = 0xCD;
	bb_scratch[26] = 0x00;
	bb_scratch[27] = 0x00;
	cprintf(e, "  [6/6] entry-outside-phdr\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	cprintf(e, "test-boot-bad: %d/%d PASS\n", (int)passed, (int)total);
	return NO_ERROR;
}
