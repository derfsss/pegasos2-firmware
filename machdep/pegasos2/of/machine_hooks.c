/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  SmartFirmware machdep contract callbacks for the OS-handoff path.
 *
 *    machine_probe_all      -- "probe-all" device-tree walker hook
 *                              (no-op on Pegasos2)
 *    machine_secondary_diag -- extended diagnostics
 *                              (no-op on Pegasos2)
 *    machine_init_load      -- chooses where on-disk file contents
 *                              get buffered before exec_load runs
 *    machine_init_program   -- picks a g_exec_list[] handler that
 *                              recognises the loaded image's magic
 *    machine_go             -- final stage: walks PT_LOADs, sets
 *                              up r1, hands off via machine_jump_os
 *
 *  These all live behind SF's `boot` / `load` / `go` Forth words --
 *  see admin.c try_load + admin.c f_go for the call sequence.
 */

#include "defs.h"
#include "exe.h"

extern uInt g_boot_image_high_end;
extern void machine_jump_os(uInt entry, uInt ci_handler_addr,
                            uInt stack_top);
extern int  ci_handler(void *args);

/* No-op hooks. Pegasos2 has no probe-all-the-things concept and no
 * extended diagnostic mode. */
CC(machine_probe_all)       { (void)e; return NO_ERROR; }
CC(machine_secondary_diag)  { (void)e; return NO_ERROR; }

/*
 * machine_init_load (--): called by SF's admin.c:try_load before
 * invoking the disk's `load` method. Sets e->load to a DRAM
 * address where the file contents will be buffered. Per spec 07
 * Load-address, the canonical default is 0x00400000 (4 MiB mark)
 * -- situated above SF's malloc pool (0x01100000+) and below the
 * x86emu buffer at 0x01000000 -- i.e. clear of every other
 * firmware-claimed region in the 0..16 MiB window.
 *
 * We intentionally don't use g_machine_memory + g_machine_memory_size
 * (the bebox/i386 pattern), since g_machine_memory_size reports the
 * whole DRAM range available to the OS. Adding the size to the base
 * lands at DRAM top, past the last valid byte.
 */
CC(machine_init_load)
{
	e->load = (Byte *)0x00400000u;
	return NO_ERROR;
}

/*
 * machine_init_program (--): called by f_init_program (admin.c)
 * after the image is in e->load / e->loadlen. Probes g_exec_list[]
 * to pick a handler and stashes it in e->loadentry; returns
 * NO_ERROR on match or E_BAD_IMAGE if nothing recognises the magic.
 */
CC(machine_init_program)
{
	return exec_is_exec(e) ? NO_ERROR : E_BAD_IMAGE;
}

/*
 * machine_go (--): the final transfer. Called from admin.c:f_go
 * (or from `boot` which expands to load + go). At entry,
 * e->load/e->loadlen contain the raw file bytes; exec_load() hands
 * them to our handler's load callback which walks PT_LOADs and
 * sets e->entrypoint + g_boot_image_high_end. We then compute r1
 * and call machine_jump_os to perform the spec-07 handoff (BATs,
 * MSR[IR|DR], r3..r7 per boot_kernel.S). Does not return on
 * success.
 *
 * If exec_load returns an error (malformed image) we propagate it
 * back to f_go, which reports it and returns the user to the ok
 * prompt -- same behaviour as test-boot-bad's hardening checks.
 */
CC(machine_go)
{
	if (!exec_is_exec(e))
		return E_BAD_IMAGE;

	Retcode ret = exec_load(e);
	if (ret != NO_ERROR)
		return ret;

	/* Stack pointer handed to the OS. Matches the f_boot_kernel
	 * Forth-path convention: 4 KiB of headroom above the highest
	 * PT_LOAD end, aligned down to 16 (PPC SysV ABI). If +4 KiB
	 * would overflow the address space, pin to high_end (the OS
	 * relocates r1 immediately after entry anyway). */
	uInt stack_top = g_boot_image_high_end + 0x1000u;
	if (stack_top < g_boot_image_high_end)
		stack_top = g_boot_image_high_end;
	stack_top &= ~0xFu;

	cprintf(e, "machine_go: e_entry=0x%X r1=0x%X; transferring...\n",
	        (unsigned)e->entrypoint, (unsigned)stack_top);

	/* Echo /chosen/bootpath + /chosen/bootargs so the next
	 * stage's expectations are visible in the log. The OS reads
	 * these via `getprop /chosen bootargs ...` shortly after
	 * entry. Spec 07 §AOS4 lists `bootdevice=` as the canonical
	 * AOS4 argument that selects which RDB partition to boot
	 * from. */
	{
		Byte *prop_val = NULL;
		Int   prop_len = 0;
		if (prop_get_str(e->chosen->props, "bootpath", CSTR,
				 &prop_val, &prop_len) == NO_ERROR && prop_val)
			cprintf(e, "  /chosen/bootpath = \"%S\"\n",
				prop_val, prop_len);
		prop_val = NULL; prop_len = 0;
		if (prop_get_str(e->chosen->props, "bootargs", CSTR,
				 &prop_val, &prop_len) == NO_ERROR && prop_val)
			cprintf(e, "  /chosen/bootargs = \"%S\"\n",
				prop_val, prop_len);
	}

	machine_jump_os((uInt)e->entrypoint,
	                (uInt)(uPtr)&ci_handler, stack_top);

	/* Unreachable; machine_jump_os does not return. */
	return NO_ERROR;
}
