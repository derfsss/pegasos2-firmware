/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Compat shim force-included before compiling upstream/fs/dosfat.c.
 *
 *  dosfat.c has a single dead declaration "extern struct device
 *  devices[]" that is never actually referenced in any function
 *  body. It's a leftover from an older SF version where the FAT
 *  driver hooked a device table, and was carried forward even
 *  after the hook was removed. GCC rejects an `extern ARRAY OF
 *  incomplete type`, so we just need a complete (but otherwise
 *  unused) `struct device` definition visible when dosfat.c
 *  compiles. The linker drops the unreferenced extern.
 *
 *  No functional effect: no code reads or writes `devices[]`.
 */

#ifndef DOSFAT_COMPAT_H
#define DOSFAT_COMPAT_H

struct device {
	int pegasos2_unused_stub_field;
};

#endif /* DOSFAT_COMPAT_H */
