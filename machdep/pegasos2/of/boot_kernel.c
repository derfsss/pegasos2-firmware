/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Spec 07 ELF32 PPC BE loader. Provides two public surfaces:
 *
 *  1. Machdep `Exec_entry` registration. SF's admin.c
 *     `f_load`/`f_go` path reads the file into `e->load`, calls
 *     exec_is_exec() to pick a handler from g_exec_list[], then
 *     exec_load() to place PT_LOADs and set e->entrypoint. Our
 *     machdep platform.c exports `g_exec_list[]` pointing at the
 *     `pegasos2_ppc_elf_exec` struct defined at the bottom of this
 *     file; its is_exec / load callbacks are `elf32_ppc_be_is_exec`
 *     and `elf32_ppc_be_load` below.
 *
 *  2. Forth word `boot-kernel ( load-addr -- )`. Directly calls
 *     the same validation + load logic on a caller-supplied
 *     address and hands off via machine_jump_os. Used by
 *     `test-boot` and `test-boot-bad` to exercise the full spec 07
 *     register handoff without needing the open-dev/disk-label/FS
 *     stack to be working.
 *
 *  Boot-time register state (spec 07 §register state):
 *      r1 = valid small stack in loaded image region
 *      r3 = 0
 *      r4 = 0
 *      r5 = CI handler entry pointer
 *      r6 = 0 (reserved)
 *      r7 = e_entry (some OSes check it)
 *      MSR = supervisor, translation on, interrupts disabled
 *  All of that is assembled by machine_jump_os (boot_kernel.S).
 *
 *  Cache-coherency note: on this firmware SF runs with MSR[IR]=MSR[DR]=0
 *  (real mode) from reset through handoff, so memory writes by the
 *  load loop go straight to DRAM without populating the D-cache. Only
 *  machine_jump_os flips MSR[IR|DR] on after the load and right before
 *  bctr, at which point the I-cache is cold for the freshly-loaded
 *  code -- the first fetch after bctr will miss and pull the correct
 *  bytes straight from DRAM. No explicit dcbf/icbi is required in the
 *  current design. If future code runs with MSR[DR]=1 between the
 *  load and the jump, a dcbf/sync/icbi/sync loop over the loaded
 *  range becomes mandatory here.
 */

#include "defs.h"
#include "exe.h"
#include "byteswap.h"

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
 * Module-global: highest PT_LOAD end-address computed by the most
 * recent successful `elf32_ppc_be_load`. Consumed by machine_go in
 * machdep.c (M6) and by f_boot_kernel (direct path) to derive the
 * spec 07 stack_top. Single-threaded firmware, so no synchronization
 * needed.
 */
uInt g_boot_image_high_end;

/*
 * Read a big-endian 16 or 32-bit field from a byte pointer.
 * The PPC CPU is BE natively; a direct load would work, but
 * open-coding the fetch keeps us alignment-safe and avoids the
 * compiler second-guessing the memory model on an arbitrary
 * user-supplied address.
 */
/* Wraparound-safe 32-bit addition. Returns non-zero on overflow. */
static int
u32_add_ovf(uInt a, uInt b, uInt *out)
{
	*out = a + b;
	return (*out < a) ? 1 : 0;
}

/*
 * Cheap magic-only check: just the leading 7 bytes of e_ident.
 * Callable from both the SF exec_is_exec probe path (which may
 * invoke is_exec on any random buffer) and from the Forth
 * boot-kernel path (where the user passed a supposed load-addr).
 * Returns TRUE only if the buffer looks like an ELFCLASS32/MSB/v1
 * image for EM_PPC with a reasonable e_type.
 */
Bool
elf32_ppc_be_is_exec(Environ *e, uByte *load, uInt loadlen)
{
	(void)e;

	if (load == NULL || loadlen < 32)
		return FFALSE;

	const uChar *eh = (const uChar *)load;

	if (eh[0] != ELF_MAGIC_0 || eh[1] != ELF_MAGIC_1 ||
	    eh[2] != ELF_MAGIC_2 || eh[3] != ELF_MAGIC_3)
		return FFALSE;
	if (eh[4] != ELF_CLASS_32) return FFALSE;
	if (eh[5] != ELF_DATA_MSB) return FFALSE;
	if (eh[6] != ELF_VERSION_1) return FFALSE;

	uInt e_type    = be16(eh + 16);
	uInt e_machine = be16(eh + 18);
	if (e_type != ELF_TYPE_EXEC && e_type != ELF_TYPE_DYN)
		return FFALSE;
	if (e_machine != ELF_MACHINE_PPC)
		return FFALSE;

	return FTRUE;
}

/*
 * Full spec-07 compliant load. Validates the image (the 8 hardening
 * checks from Boot 3/N), walks PT_LOAD segments to place bytes in
 * DRAM, sets *entrypoint, and stashes the highest end-address in
 * g_boot_image_high_end for later stack-top computation.
 *
 * Returns NO_ERROR on success, E_BAD_IMAGE on validation failure,
 * with a cprintf diagnostic identifying which check fired.
 */
Retcode
elf32_ppc_be_load(Environ *e, uByte *load, uInt loadlen, uLong *entrypoint)
{
	const uChar *eh = (const uChar *)load;

	if (!elf32_ppc_be_is_exec(e, load, loadlen)) {
		cprintf(e, "elf32: header validation failed\n");
		return E_BAD_IMAGE;
	}

	uInt entry    = be32(eh + 24);
	uInt phoff    = be32(eh + 28);
	uInt phentsz  = be16(eh + 42);
	uInt phnum    = be16(eh + 44);

	if (phoff == 0 || phnum == 0) {
		cprintf(e, "elf32: no program headers\n");
		return E_BAD_IMAGE;
	}
	if (phentsz < MIN_PHENTSZ || phentsz > MAX_PHENTSZ) {
		cprintf(e, "elf32: bad phentsz=%d\n", (int)phentsz);
		return E_BAD_IMAGE;
	}
	if (phnum > MAX_PHNUM) {
		cprintf(e, "elf32: phnum=%d exceeds cap %d\n",
		        (int)phnum, (int)MAX_PHNUM);
		return E_BAD_IMAGE;
	}
	uInt pht_span;
	if (u32_add_ovf(phoff, phnum * phentsz, &pht_span)) {
		cprintf(e, "elf32: phoff+phnum*phentsz overflow\n");
		return E_BAD_IMAGE;
	}

	/* Pass 1 -- validate every PT_LOAD, find high_end, confirm
	 * entry lives inside some PT_LOAD. */
	int  entry_in_phdr = 0;
	uInt high_end      = 0;
	for (uInt i = 0; i < phnum; i++) {
		const uChar *ph   = eh + phoff + i * phentsz;
		uInt p_type       = be32(ph + 0);
		uInt p_offset     = be32(ph + 4);
		uInt p_vaddr      = be32(ph + 8);
		uInt p_filesz     = be32(ph + 16);
		uInt p_memsz      = be32(ph + 20);

		if (p_type != 1u) continue;
		if (p_memsz == 0) continue;

		if (p_filesz > p_memsz) {
			cprintf(e, "elf32: PT_LOAD #%d p_filesz %d > p_memsz %d\n",
			        (int)i, (int)p_filesz, (int)p_memsz);
			return E_BAD_IMAGE;
		}
		uInt src_end;
		if (u32_add_ovf(p_offset, p_filesz, &src_end)) {
			cprintf(e, "elf32: PT_LOAD #%d p_offset+p_filesz overflow\n",
			        (int)i);
			return E_BAD_IMAGE;
		}
		uInt dst_end;
		if (u32_add_ovf(p_vaddr, p_memsz, &dst_end)) {
			cprintf(e, "elf32: PT_LOAD #%d p_vaddr+p_memsz overflow\n",
			        (int)i);
			return E_BAD_IMAGE;
		}
		if (p_vaddr >= FLASH_RANGE_START || dst_end > FLASH_RANGE_START) {
			cprintf(e, "elf32: PT_LOAD #%d p_vaddr=0x%X overlaps flash\n",
			        (int)i, (unsigned)p_vaddr);
			return E_BAD_IMAGE;
		}

		if (entry >= p_vaddr && entry < dst_end)
			entry_in_phdr = 1;
		if (dst_end > high_end)
			high_end = dst_end;
	}
	if (!entry_in_phdr) {
		cprintf(e, "elf32: e_entry 0x%X not in any PT_LOAD\n",
		        (unsigned)entry);
		return E_BAD_IMAGE;
	}

	/* Pass 2 -- place PT_LOAD bytes (memcpy + zero-fill). */
	for (uInt i = 0; i < phnum; i++) {
		const uChar *ph   = eh + phoff + i * phentsz;
		uInt p_type       = be32(ph + 0);
		uInt p_offset     = be32(ph + 4);
		uInt p_vaddr      = be32(ph + 8);
		uInt p_filesz     = be32(ph + 16);
		uInt p_memsz      = be32(ph + 20);

		if (p_type != 1u) continue;
		if (p_memsz == 0) continue;

		uChar *dst = (uChar *)(uPtr)p_vaddr;
		const uChar *src = eh + p_offset;

		cprintf(e, "elf32:   PT_LOAD #%d -> 0x%X (%d/%d)\n",
		        (int)i, (unsigned)p_vaddr, (int)p_filesz, (int)p_memsz);

		for (uInt j = 0; j < p_filesz; j++)
			dst[j] = src[j];
		for (uInt j = p_filesz; j < p_memsz; j++)
			dst[j] = 0;
	}

	*entrypoint = entry;
	g_boot_image_high_end = high_end;
	return NO_ERROR;
}

/*
 * `boot-kernel ( load-addr -- )`
 *
 * Direct Forth-level entry point: validates the ELF at load-addr,
 * places PT_LOADs, then immediately transfers per spec 07 without
 * going through SF's f_load/f_go machinery. Used by test-boot and
 * test-boot-bad to prove the register handoff end-to-end even
 * before the full `boot cd /file.elf` flow is wired (M6 wires that
 * via machine_go / exec_load).
 *
 * Interactive usage:
 *     ok 800000 boot-kernel
 *     [kernel output from 0x800000 onward]
 *
 * The load-addr must match the ELF's PT_LOAD p_vaddr. The caller is
 * responsible for placing the bytes at load-addr beforehand (via
 * test-boot's memcpy from the embedded blob, or via QEMU's -kernel
 * flag).
 */
CC(f_boot_kernel)
{
	uInt addr;

	IFCKSP(e, 1, 0);
	POP(e, addr);

	uLong entry = 0;
	Retcode r = elf32_ppc_be_load(e, (uByte *)(uPtr)addr,
				      0xFFFFFFFFu, &entry);
	if (r != NO_ERROR) {
		/* elf32_ppc_be_load already printed the diagnostic. */
		return NO_ERROR;
	}

	uInt stack_top;
	if (u32_add_ovf(g_boot_image_high_end, 0x1000u, &stack_top)) {
		cprintf(e, "boot-kernel: image top 0x%X +4KiB overflows; "
		        "using image top\n",
		        (unsigned)g_boot_image_high_end);
		stack_top = g_boot_image_high_end;
	}
	stack_top &= ~0xFu;

	cprintf(e, "boot-kernel: ELF OK at 0x%X, e_entry=0x%X, r1=0x%X\n",
	        (unsigned)addr, (unsigned)entry, (unsigned)stack_top);
	cprintf(e, "             transferring control...\n");

	machine_jump_os((uInt)entry, (uInt)(uPtr)&ci_handler, stack_top);

	cprintf(e, "boot-kernel: (!) image returned control; halting\n");
	return NO_ERROR;
}

/*
 * `test-boot ( -- )` copies the firmware-embedded test kernel blob
 * (linked at 0x00800000) to that DRAM address and invokes
 * boot-kernel. Expected serial output: "KERNEL OK r5=..." followed
 * by PANIC 0x700 from the test kernel's deliberate twi.
 */
CC(f_test_boot)
{
	Int  size = (Int)(_test_kernel_end - _test_kernel_start);
	uChar *dst = (uChar *)0x00800000u;

	cprintf(e, "test-boot: copying %d bytes to 0x%X\n",
	        (int)size, (unsigned)(uPtr)dst);

	for (Int i = 0; i < size; i++)
		dst[i] = _test_kernel_start[i];

	PUSH(e, (Cell)(uPtr)dst);
	return f_boot_kernel(e);
}

/*
 * `test-boot-bad ( -- )` feeds six deliberately malformed ELF
 * headers into boot-kernel and confirms each is rejected. Reaching
 * the final "6/6 PASS" line is the success criterion: if any
 * scenario slipped through we'd lose the ok prompt or trap.
 */

static uChar bb_scratch[256];

static void
bb_build_valid(void)
{
	for (uInt i = 0; i < sizeof(bb_scratch); i++)
		bb_scratch[i] = 0;

	bb_scratch[0] = ELF_MAGIC_0;
	bb_scratch[1] = ELF_MAGIC_1;
	bb_scratch[2] = ELF_MAGIC_2;
	bb_scratch[3] = ELF_MAGIC_3;
	bb_scratch[4] = ELF_CLASS_32;
	bb_scratch[5] = ELF_DATA_MSB;
	bb_scratch[6] = ELF_VERSION_1;
	bb_scratch[17] = ELF_TYPE_EXEC;
	bb_scratch[19] = ELF_MACHINE_PPC;
	bb_scratch[25] = 0x90;
	bb_scratch[27] = 0x80;
	bb_scratch[31] = 52;
	bb_scratch[43] = 32;
	bb_scratch[45] = 1;

	uChar *p = bb_scratch + 52;
	p[3] = 1;        /* p_type = PT_LOAD */
	p[7] = 100;      /* p_offset */
	p[9] = 0x90; p[11] = 0x80;   /* p_vaddr */
	p[13] = 0x90; p[15] = 0x80;  /* p_paddr */
	p[19] = 32;      /* p_filesz */
	p[23] = 32;      /* p_memsz */
}

CC(f_test_boot_bad)
{
	Int total = 0;
	Int passed;

	cprintf(e, "test-boot-bad: exercising 6 malformed-ELF scenarios\n");
	passed = 0;

	bb_build_valid();
	bb_scratch[0] = 0x00;
	cprintf(e, "  [1/6] bad-magic\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	bb_build_valid();
	bb_scratch[42] = 0;
	bb_scratch[43] = 8;
	cprintf(e, "  [2/6] bad-phentsz\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	bb_build_valid();
	bb_scratch[44] = 0;
	bb_scratch[45] = 100;
	cprintf(e, "  [3/6] oversized-phnum\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	bb_build_valid();
	{
		uChar *p = bb_scratch + 52;
		p[16] = 0; p[17] = 0; p[18] = 0x10; p[19] = 0;
		p[20] = 0; p[21] = 0; p[22] = 0;    p[23] = 32;
	}
	cprintf(e, "  [4/6] filesz-gt-memsz\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	bb_build_valid();
	{
		uChar *p = bb_scratch + 52;
		p[8]  = 0xFF; p[9]  = 0xF8; p[10] = 0x00; p[11] = 0x00;
	}
	cprintf(e, "  [5/6] load-to-flash\n");
	PUSH(e, (Cell)(uPtr)bb_scratch);
	f_boot_kernel(e);
	passed++; total++;

	bb_build_valid();
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

/*
 * Stub length / symbols callbacks. SF's exec_length invokes this
 * only if length is non-NULL; SF's exec_load_symbols invokes
 * symbols only if non-NULL. Returning 0 / NULL is the "unknown /
 * unsupported" signal.
 */
static uInt
elf32_ppc_be_length(Environ *e, uByte *load, uInt loadlen)
{
	(void)e; (void)load;
	/* We don't compute a length from header info; SF callers only
	 * use this for printf-style "image occupies N bytes" UIs which
	 * we don't expose. Return 0 = unknown. */
	return loadlen;
}

/*
 * Exec_entry registration point. platform.c references this via
 * `extern Exec_entry pegasos2_ppc_elf_exec` in its g_exec_list[]
 * table. No per-handler init -- just a struct literal.
 */
Exec_entry pegasos2_ppc_elf_exec = {
	"elf32-ppc-be",
	elf32_ppc_be_is_exec,
	elf32_ppc_be_load,
	elf32_ppc_be_length,
	NULL,                    /* symbols: unsupported */
};
