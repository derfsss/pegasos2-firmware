/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  smart-boot Forth word: priority-based OS dispatcher.
 *
 *  smart-boot is the default value of the `boot-command` NVRAM
 *  variable. At auto-boot expiry (or whenever the user types
 *  `smart-boot` at the ok prompt) it walks every RDB partition
 *  exposed by install_partition_packages, classifies each by its
 *  on-disk DosType into one of three OS families (amigaos / linux
 *  / morphos), and dispatches to the per-family loader following
 *  the comma-separated `boot-os-priority` NVRAM list.
 *
 *  Adding a new OS loader:
 *    1. Add a DosType-prefix -> family-name mapping in
 *       classify_dostype() if your OS uses a new DosType.
 *    2. Add the family name + any aliases in os_family_match().
 *    3. Implement loader_<family>() -- it should interp_text
 *       whatever Forth boot sequence is right (or load+jump
 *       directly via boot_kernel.c for a raw ELF), and return
 *       NO_ERROR on a launched loader (successful boots don't
 *       return) or an error code on failure (smart-boot will
 *       continue to the next family in the priority list).
 *    4. Wire it into the if/else chain in f_smart_boot() below.
 *
 *  Reads device-tree properties published by partition_pkg.c:
 *      dostype       -- 4 raw bytes from RDB de_DosType
 *      boot-priority -- signed Int from RDB de_BootPri
 */

#include "defs.h"

extern Package *find_first_hd_disk(Environ *e);
extern Package *find_first_cd(Environ *e);

/* --- DosType classification ------------------------------------ */

/*
 * Map a 4-byte DosType to an OS-family name. The names returned
 * here are the same tokens accepted in the boot-os-priority list.
 *
 * Coverage:
 *   "DOS\\0".."DOS\\7" -> amigaos    (OFS, FFS, Intl, DC, LNFS / FFS2)
 *   "SFS\\0".."SFS\\2" -> amigaos    (SmartFileSystem)
 *   "PFS\\1".."PFS\\3" -> amigaos    (Professional FileSystem 3)
 *   "AFS\\1"           -> amigaos    (alias for PFS, AOS-side)
 *   "MOR\\?"           -> morphos    (MorphOS-format partitions)
 *   "LNX\\?"/"EXT2/3/4"-> linux      (Linux partitions)
 *   anything else      -> ""         (not a smart-boot candidate)
 */
static const char *
classify_dostype(uInt dostype)
{
	uInt high3 = dostype & 0xFFFFFF00u;
	if (high3 == 0x444F5300u) return "amigaos"; /* DOS\* */
	if (high3 == 0x53465300u) return "amigaos"; /* SFS\* */
	if (high3 == 0x50465300u) return "amigaos"; /* PFS\* */
	if (high3 == 0x41465300u) return "amigaos"; /* AFS\* */
	if (high3 == 0x4D4F5200u) return "morphos"; /* MOR\* */
	if (high3 == 0x4C4E5800u) return "linux";   /* LNX\* */
	if (dostype == 0x45585432u ||                /* "EXT2" */
	    dostype == 0x45585433u ||                /* "EXT3" */
	    dostype == 0x45585434u)                  /* "EXT4" */
		return "linux";
	return "";
}

/*
 * Match a priority-list token [s..s+slen) against a canonical
 * family name. Accepts a few user-friendly aliases for amigaos
 * (amigaos4, amigaos3, amiga). Case-sensitive.
 */
static int
os_family_match(const Byte *s, int slen, const char *family)
{
	int flen = (int)strlen(family);
	if (slen == flen && memcmp(s, family, slen) == 0)
		return 1;
	if (strcmp(family, "amigaos") == 0) {
		if (slen == 8 && memcmp(s, "amigaos4", 8) == 0) return 1;
		if (slen == 8 && memcmp(s, "amigaos3", 8) == 0) return 1;
		if (slen == 5 && memcmp(s, "amiga", 5) == 0)    return 1;
	}
	return 0;
}

/* --- Partition selection --------------------------------------- */

/*
 * For one OS family, scan all partition children of `hd` and
 * return the highest-BootPri candidate, or NULL if none qualify.
 *
 * BootPri convention: per the RDB spec, -128 means "do not auto-
 * boot this partition" (the AOS sentinel). We treat any negative
 * value as opt-out, so users can tag a non-bootable Linux data
 * partition with -1 in Media Toolbox and not have smart-boot
 * pick it.
 */
static Package *
pick_partition_for_family(Package *hd, const char *family)
{
	Package *best = NULL;
	Int      best_pri = -129;

	for (Package *p = hd->children; p != NULL; p = p->link) {
		Entry *de = find_table(p->props, (Byte *)"dostype", CSTR);
		if (de == NULL || de->len < 4) continue;
		const uByte *db = (const uByte *)de->v.array;
		uInt dt = ((uInt)db[0] << 24) | ((uInt)db[1] << 16) |
		          ((uInt)db[2] << 8)  |  (uInt)db[3];

		const char *fam = classify_dostype(dt);
		if (fam[0] == 0 || strcmp(fam, family) != 0) continue;

		Entry *be_ = find_table(p->props,
		                        (Byte *)"boot-priority", CSTR);
		Int pri = 0;
		if (be_ != NULL && be_->len >= 4) {
			const uByte *bb = (const uByte *)be_->v.array;
			pri = (Int)(((uInt)bb[0] << 24) |
			            ((uInt)bb[1] << 16) |
			            ((uInt)bb[2] << 8)  |
			             (uInt)bb[3]);
		}
		if (pri < 0) continue;
		if (pri > best_pri) {
			best_pri = pri;
			best = p;
		}
	}
	return best;
}

/* --- Per-family loaders ---------------------------------------- */

/*
 * Each loader returns NO_ERROR if it successfully transferred
 * control out of the firmware (the function does not actually
 * return in the success case), or an error code on failure
 * (smart-boot then continues to the next family in the priority
 * list).
 */

static Retcode
loader_amigaos(Environ *e, Package *part)
{
	(void)part;
	/* amigaboot.of does its own RDB walk + de_BootPri sort +
	 * selection menu, so we hand off without specifying the
	 * partition: amigaboot picks the highest-priority AOS-family
	 * partition itself, and displays a menu if multiple installs
	 * exist on the same disk. */
	const char *cmd = "boot hd:0 amigaboot.of";
	return interp_text(e, (Byte *)cmd, strlen(cmd));
}

static Retcode
loader_linux(Environ *e, Package *part)
{
	(void)part;
	cprintf(e, "smart-boot: Linux loader not implemented yet "
	        "(future: yaboot-style kernel + initrd from /boot)\n");
	return E_UNSUPPORTED_FILESYS;
}

static Retcode
loader_morphos(Environ *e, Package *part)
{
	(void)part;
	cprintf(e, "smart-boot: MorphOS loader not implemented yet "
	        "(future: load morphos.of / boot.img from disk root)\n");
	return E_UNSUPPORTED_FILESYS;
}

/* --- Per-family CD-media loaders ------------------------------- *
 *
 * Called when the per-family HD walk found no candidate but a CD
 * is in the drive. The CD scan is filename-driven: we ask SF to
 * `boot cd <path>` and let the existing ELF/handoff path do the
 * work. SF returns a non-NO_ERROR retcode if the file isn't on
 * the volume, so we just chain candidate filenames in priority
 * order until one sticks.
 *
 * Filename choices come from the install-media conventions of
 * each OS family:
 *   amigaos: `/amigaboot.of` is the standard AOS4 OpenFirmware
 *            bootstrap. AOS3 doesn't ship CDs; ignore.
 *   linux:   `/boot/vmlinux` (Debian PPC live), then `/vmlinux`
 *            (custom), then `/yaboot` (yaboot-aware media). All
 *            are ELF; SF's ELF path takes them.
 *   morphos: `/boot.img` is the MorphOS install-CD bootstrap.
 *            Untested -- the loader_morphos stub handles HD;
 *            on CD we still try, so a future MorphOS loader can
 *            adopt the same path.
 */

static Retcode try_cd_boot(Environ *e, const char *path)
{
	Byte cmd[64];
	/* bprintf's return value is unreliable (it returns the post-
	 * loop strlen of an already-NUL-terminated walking pointer,
	 * which is 0 -- documented in machdep.c's dprintf comment).
	 * Walk the buffer ourselves for the actual length. */
	bprintf((char *)cmd, "boot cd %s", path);
	int n = (int)strlen((char *)cmd);
	return interp_text(e, cmd, n);
}

static Retcode
loader_amigaos_cd(Environ *e)
{
	cprintf(e, "smart-boot: trying AmigaOS bootstrap from CD\n");
	return try_cd_boot(e, "amigaboot.of");
}

static Retcode
loader_linux_cd(Environ *e)
{
	cprintf(e, "smart-boot: trying Linux bootstrap from CD\n");
	Retcode r;
	if ((r = try_cd_boot(e, "boot/vmlinux"))     == NO_ERROR) return r;
	if ((r = try_cd_boot(e, "vmlinux"))          == NO_ERROR) return r;
	if ((r = try_cd_boot(e, "yaboot"))           == NO_ERROR) return r;
	return E_UNSUPPORTED_FILESYS;
}

static Retcode
loader_morphos_cd(Environ *e)
{
	cprintf(e, "smart-boot: trying MorphOS bootstrap from CD\n");
	return try_cd_boot(e, "boot.img");
}

/* --- Forth entry point ----------------------------------------- */

/*
 * Forth: smart-boot ( -- )
 *
 * Walk the user's `boot-os-priority` list in order. For each
 * family, find a candidate partition (matching DosType +
 * boot-priority >= 0) and invoke the per-family loader. If the
 * loader returns (only happens on failure), continue to the next
 * family. If the priority list is exhausted with no successful
 * loader, fall back to a plain `boot` so the user still sees SF's
 * default load attempt against boot-device + boot-file.
 */
/*
 * Resolve a comma-separated priority token to a canonical family
 * name, or return NULL if the token is unknown. Caller owns the
 * "skipped" diagnostic.
 */
static const char *
resolve_family_token(const Byte *tok, int len)
{
	if (os_family_match(tok, len, "amigaos")) return "amigaos";
	if (os_family_match(tok, len, "linux"))   return "linux";
	if (os_family_match(tok, len, "morphos")) return "morphos";
	return NULL;
}

CC(f_smart_boot)
{
	/* Pull priority list. get_config returns the bare value bytes
	 * (no length prefix); SF stores it NUL-terminated. */
	Byte *prio = get_config(e, (Byte *)"boot-os-priority", CSTR);
	const char *fallback = "amigaos,morphos,linux";
	if (prio == NULL || *prio == 0)
		prio = (Byte *)fallback;

	Package *hd = find_first_hd_disk(e);
	Package *cd = find_first_cd(e);

	/*
	 * Pass 1: HD partition table. For each family in priority
	 * order, look for an RDB partition with a matching DosType
	 * and a non-negative BootPri. If any HD install matches,
	 * boot it.
	 */
	if (hd != NULL) {
		const Byte *cur = prio;
		while (*cur) {
			while (*cur == ' ' || *cur == '\t') cur++;
			const Byte *tok = cur;
			while (*cur && *cur != ',' && *cur != ' ' && *cur != '\t')
				cur++;
			int tok_len = (int)(cur - tok);
			while (*cur == ' ' || *cur == '\t') cur++;
			if (*cur == ',') cur++;
			if (tok_len == 0) continue;

			const char *family = resolve_family_token(tok, tok_len);
			if (family == NULL) {
				cprintf(e, "smart-boot: unknown family `%S` "
				    "-- skipped\n", tok, (Int)tok_len);
				continue;
			}

			Package *cand = pick_partition_for_family(hd, family);
			if (cand == NULL) continue;

			Byte *pname = NULL; Int pnlen = 0;
			(void)prop_get_str(cand->props,
			                   (Byte *)"partition-name", CSTR,
			                   &pname, &pnlen);
			cprintf(e, "smart-boot: picking %s partition %S\n",
			        family,
			        pname ? pname : (Byte *)"?",
			        pname ? pnlen : 1);

			Retcode r;
			if (strcmp(family, "amigaos") == 0)
				r = loader_amigaos(e, cand);
			else if (strcmp(family, "linux") == 0)
				r = loader_linux(e, cand);
			else
				r = loader_morphos(e, cand);

			if (r == NO_ERROR)
				return NO_ERROR;     /* unreachable on success */
			/* loader failed; try next family */
		}
	}

	/*
	 * Pass 2: CD media. If a CD is in the drive, try each
	 * family's bootstrap filename in priority order. Per-family
	 * loader knows the conventional filenames for that OS's
	 * install/live media; SF returns a non-NO_ERROR retcode if
	 * the file isn't on the volume and we move on.
	 */
	if (cd != NULL) {
		const Byte *cur = prio;
		while (*cur) {
			while (*cur == ' ' || *cur == '\t') cur++;
			const Byte *tok = cur;
			while (*cur && *cur != ',' && *cur != ' ' && *cur != '\t')
				cur++;
			int tok_len = (int)(cur - tok);
			while (*cur == ' ' || *cur == '\t') cur++;
			if (*cur == ',') cur++;
			if (tok_len == 0) continue;

			const char *family = resolve_family_token(tok, tok_len);
			if (family == NULL) continue; /* already warned in pass 1 */

			Retcode r;
			if (strcmp(family, "amigaos") == 0)
				r = loader_amigaos_cd(e);
			else if (strcmp(family, "linux") == 0)
				r = loader_linux_cd(e);
			else
				r = loader_morphos_cd(e);

			if (r == NO_ERROR)
				return NO_ERROR;     /* unreachable on success */
			/* try next family */
		}
	}

	if (hd == NULL && cd == NULL)
		cprintf(e, "smart-boot: no HD or CD; "
		        "falling back to plain boot\n");
	else
		cprintf(e, "smart-boot: no priority match on HD or CD; "
		        "falling back to plain `boot`\n");
	const char *cmd = "boot";
	return interp_text(e, (Byte *)cmd, strlen(cmd));
}
