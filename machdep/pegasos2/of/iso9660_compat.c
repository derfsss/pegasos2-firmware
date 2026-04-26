/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  ISO9660 path-normalisation wrapper around upstream's
 *  fs/iso9660.c. ISO9660 stores filenames with a `;version`
 *  suffix; upstream `iso_walk` matches the suffix byte-for-byte
 *  against the dir-entry's stored name, so a user-supplied path
 *  with `;1` only resolves on a CD whose on-disk names also
 *  include `;1`. Modern mkisofs invocations (the AmigaOS 4
 *  installation media included) strip the version suffix from
 *  the on-disk record, so the user-typed `boot cd /amigaboot.of;1`
 *  fails on those CDs even though the file is present.
 *
 *  This wrapper strips a trailing `;NNN` from the requested path
 *  before handing it to the upstream reader. Net effect:
 *
 *    on-disk name  user types          before       after
 *    ------------  -----------------   ------       -----
 *    bare          /amigaboot.of       OK           OK
 *    bare          /amigaboot.of;1     no such file OK
 *    versioned     /amigaboot.of;1     OK           OK
 *    versioned     /amigaboot.of       no such file no such file
 *
 *  i.e. every previously-working case still works and one
 *  previously-failing case (the common one for modern CDs) now
 *  succeeds. The remaining failure case (versioned-on-disk + bare
 *  user input) requires per-call retry with the suffix appended,
 *  which would re-trigger upstream's "ISO-9660 filesystem ..."
 *  banner and "no such file" message; we leave that for users to
 *  work around by typing the `;1` form explicitly.
 *
 *  Registration: machdep/pegasos2/of/platform.c lists
 *  `&g_iso9660_compat` in `g_filesys[]` instead of upstream's
 *  `&g_iso9660_fs` so all FS dispatch paths (including
 *  smart-boot's CD pass and any user-typed `boot cd <path>` at
 *  the ok prompt) flow through the normaliser.
 */

#include "defs.h"
#include "fs.h"

extern Retcode iso9660(Environ *e, Filesys_action what, Instance *disk,
                       Byte *path, uLong loc, uLong start, uByte *buf,
                       uInt size, uByte *retbuf, uLong *val);

static Retcode
iso9660_compat_action(Environ *e, Filesys_action what, Instance *disk,
                      Byte *path, uLong loc, uLong start, uByte *buf,
                      uInt size, uByte *retbuf, uLong *val)
{
	Byte normalized[STR_SIZE];

	if (path != NULL && *path) {
		Byte *semicolon = (Byte *)strchr((char *)path, ';');
		if (semicolon != NULL) {
			Int len = (Int)(semicolon - path);
			if (len > 0 && len < (Int)sizeof normalized) {
				memcpy(normalized, path, (size_t)len);
				normalized[len] = '\0';
				path = normalized;
			}
		}
	}

	return iso9660(e, what, disk, path, loc, start, buf, size,
	               retbuf, val);
}

Filesys g_iso9660_compat =
{
	"iso9660",
	iso9660_compat_action,
};
