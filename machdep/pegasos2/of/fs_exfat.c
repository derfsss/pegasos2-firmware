/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Arc FS-C -- exFAT readonly filesystem reader.
 *
 *  exFAT is Microsoft's successor to FAT32 (original spec 2006,
 *  publicly republished 2019 as Win32-fileio/exfat-specification).
 *  Distinguishing features relative to FAT32:
 *    - Fixed 32-byte directory entries with multi-entry "entry sets"
 *      (primary File + Stream Extension + one or more File Name).
 *    - Unicode (UTF-16LE) file names, up to 255 chars.
 *    - Separate Allocation Bitmap directory entry (not inline in FAT).
 *    - 64-bit file sizes.
 *    - Boot-sector checksum sector at sector 11.
 *
 *  This reader implements enough of the spec to find and load a
 *  named file from the root directory. It skips:
 *    - Boot-sector checksum validation (skipping saves ~1 KiB of
 *      code and 11 block reads on every open; robustness cost is
 *      bounded because we still validate the FileSystemName magic).
 *    - Allocation Bitmap (0x81) and Up-case Table (0x82) entries,
 *      which matter for write integrity and locale-aware case
 *      folding respectively, but not for locating a named file.
 *    - TexFAT (NumberOfFats=2) secondary FAT handling; we use the
 *      First FAT as selected by VolumeFlags bit 0 being 0.
 *    - Unicode beyond the ASCII subset in filename comparison
 *      (matches dosfat.c's LFN comparison behaviour).
 *
 *  Registered as &g_fs_exfat in platform.c's g_filesys[] after the
 *  Amiga families and iso9660 but before dosfat/ext2, so an MBR
 *  partition flagged as exFAT gets this reader before dosfat's
 *  FAT32 fallback tries and fails.
 *
 *  Format reference: Microsoft "exFAT file system specification",
 *  published at learn.microsoft.com/windows/win32/fileio/
 *  exfat-specification (2019). That page is a canonical public
 *  spec; nothing in this file is derived from any GPL reader.
 */

#include "defs.h"
#include "fs.h"

/* --- Boot sector field offsets -------------------------------- */
#define BOOT_JUMP           0       /* 3 bytes: EB 76 90 */
#define BOOT_FSNAME         3       /* 8 bytes: "EXFAT   " */
#define BOOT_MUSTBEZERO     11      /* 53 bytes */
#define BOOT_PARTOFF        64      /* u64 */
#define BOOT_VOLLEN         72      /* u64 -- volume length in sectors */
#define BOOT_FATOFF         80      /* u32 -- first sector of FAT */
#define BOOT_FATLEN         84      /* u32 -- FAT length in sectors */
#define BOOT_CLUSHEAPOFF    88      /* u32 -- first sector of cluster heap */
#define BOOT_CLUSCOUNT      92      /* u32 */
#define BOOT_ROOTCLUSTER    96      /* u32 -- first cluster of root dir */
#define BOOT_VOLSERIAL      100
#define BOOT_FSREV          104     /* u16 -- major/minor */
#define BOOT_VOLFLAGS       106     /* u16 */
#define BOOT_BPS_SHIFT      108     /* u8  -- log2 bytes per sector */
#define BOOT_SPC_SHIFT      109     /* u8  -- log2 sectors per cluster */
#define BOOT_NUMFATS        110     /* u8  -- 1 or 2 */
#define BOOT_DRIVESEL       111
#define BOOT_PERCENTINUSE   112
#define BOOT_RESERVED       113
#define BOOT_BOOTCODE       120
#define BOOT_SIGNATURE      510     /* 0xAA55 */

/* --- Directory entry types ------------------------------------ */
#define ENT_ALLOCATION_BITMAP 0x81
#define ENT_UPCASE_TABLE      0x82
#define ENT_VOLUME_LABEL      0x83
#define ENT_FILE              0x85
#define ENT_STREAM_EXT        0xC0
#define ENT_FILE_NAME         0xC1

/* A zero EntryType byte marks end-of-directory. The TopBit rule:
 *   bit 7 set  => in-use entry
 *   bit 7 clear => unused (< 0x80) -- skip, but not end-of-dir
 *                  except value 0x00 which IS end-of-dir. */
#define ENT_END_OF_DIR    0x00

/* --- File entry field offsets (32 bytes) ---------------------- */
#define FE_ENTRYTYPE        0       /* u8 -- 0x85 */
#define FE_SECONDARYCOUNT   1       /* u8 -- count of secondary entries */
#define FE_SETCHECKSUM      2       /* u16 */
#define FE_GENPRIMFLAGS     4       /* u16 */
#define FE_RESERVED1        6
#define FE_CREATETIME       8
#define FE_LASTMODTIME      12
#define FE_LASTACCTIME      16
#define FE_FILEATTRS        20      /* 2 bytes */
#define FE_RESERVED2        22

/* --- Stream Extension field offsets (32 bytes) ---------------- */
#define SE_ENTRYTYPE        0       /* u8 -- 0xC0 */
#define SE_GENSECFLAGS      1       /* u8 -- bit 0 AllocationPossible, bit 1 NoFatChain */
#define SE_RESERVED1        2
#define SE_NAMELENGTH       3       /* u8 -- chars in name (UTF-16 code units) */
#define SE_NAMEHASH         4       /* u16 */
#define SE_RESERVED2        6
#define SE_VALIDDATALEN     8       /* u64 -- valid data length */
#define SE_RESERVED3        16
#define SE_FIRSTCLUSTER     20      /* u32 */
#define SE_DATALENGTH       24      /* u64 */

#define GSF_ALLOCATION_POSSIBLE 0x01
#define GSF_NO_FAT_CHAIN        0x02

/* --- File Name field offsets (32 bytes) ----------------------- */
#define FN_ENTRYTYPE        0       /* u8 -- 0xC1 */
#define FN_GENSECFLAGS      1
#define FN_NAME             2       /* 30 bytes = 15 UTF-16 code units */
#define FN_CHARS_PER_ENTRY  15

/* --- Per-volume cached state ---------------------------------- */
static struct {
	Instance *disk;
	uLong     part_loc;
	uInt      bytes_per_sector;     /* 512 for our supported config */
	uInt      sectors_per_cluster;
	uInt      bytes_per_cluster;
	uInt      fat_offset;           /* sector index within the partition */
	uInt      cluster_heap_offset;  /* sector index */
	uInt      cluster_count;
	uInt      root_cluster;
} g_ex;

/* --- Little-endian readers (exFAT is LE, unlike everything else
 * we read in this tree) --------------------------------------- */
static uInt
le16(const uByte *p)
{
	return ((uInt)p[1] << 8) | (uInt)p[0];
}

static uInt
le32(const uByte *p)
{
	return ((uInt)p[0]) | ((uInt)p[1] << 8) |
	       ((uInt)p[2] << 16) | ((uInt)p[3] << 24);
}

/* --- Sector-level read helper. `sector` is relative to the
 * partition. `size` must be a multiple of bytes_per_sector. */
static Retcode
read_sector(Environ *e, uInt sector, uByte *buf, uInt size)
{
	return filesys_read_bytes(e, g_ex.disk,
		g_ex.part_loc + (uLong)sector * g_ex.bytes_per_sector,
		size, buf);
}

/* --- Open volume: parse boot sector. Returns NO_ERROR if this is
 * a valid exFAT superblock, else E_NO_FILESYS. */
static Retcode
exfat_open_volume(Environ *e, Instance *disk, uLong part_loc, uByte *buf)
{
	/* Boot sector is always 512 bytes at the start; later we may
	 * discover a different bytes_per_sector. Read 512 first. */
	Retcode r = filesys_read_bytes(e, disk, part_loc, 512, buf);
	if (r != NO_ERROR)
		return r;

	/* Quick magic check -- "EXFAT   " at offset 3. */
	static const char magic[] = "EXFAT   ";
	for (int i = 0; i < 8; i++)
		if (buf[BOOT_FSNAME + i] != (uByte)magic[i])
			return E_NO_FILESYS;

	/* Boot signature 0xAA55 at sector offset 510. */
	if (le16(buf + BOOT_SIGNATURE) != 0xAA55)
		return E_NO_FILESYS;

	uByte bps_shift = buf[BOOT_BPS_SHIFT];
	uByte spc_shift = buf[BOOT_SPC_SHIFT];
	if (bps_shift < 9 || bps_shift > 12)
		return E_NO_FILESYS;
	if (spc_shift > 25)
		return E_NO_FILESYS;

	uInt bps = 1u << bps_shift;
	uInt spc = 1u << spc_shift;

	/* This reader only supports 512-byte logical sectors. Larger
	 * sectors would need a per-call buffer larger than our callers
	 * pass; reject rather than read past the caller's buffer. */
	if (bps != 512)
		return E_NO_FILESYS;

	g_ex.disk                = disk;
	g_ex.part_loc            = part_loc;
	g_ex.bytes_per_sector    = bps;
	g_ex.sectors_per_cluster = spc;
	g_ex.bytes_per_cluster   = bps * spc;
	g_ex.fat_offset          = le32(buf + BOOT_FATOFF);
	g_ex.cluster_heap_offset = le32(buf + BOOT_CLUSHEAPOFF);
	g_ex.cluster_count       = le32(buf + BOOT_CLUSCOUNT);
	g_ex.root_cluster        = le32(buf + BOOT_ROOTCLUSTER);

	if (g_ex.root_cluster < 2 ||
	    g_ex.root_cluster > g_ex.cluster_count + 1)
		return E_NO_FILESYS;
	if (g_ex.bytes_per_cluster == 0 ||
	    g_ex.bytes_per_cluster > 32u * 1024u)
		return E_NO_FILESYS;      /* >32K cluster -- too big for stack */

	return NO_ERROR;
}

/* --- FAT lookup: next cluster after `cluster`. Returns 0xFFFFFFFF
 * on end-of-chain or invalid, `next` cluster otherwise. */
static uInt
exfat_fat_next(Environ *e, uInt cluster, uByte *scratch)
{
	if (cluster < 2 || cluster > g_ex.cluster_count + 1)
		return 0xFFFFFFFFu;

	/* FAT entry is 4 bytes at offset cluster*4 from the FAT start. */
	uInt entry_off  = cluster * 4u;
	uInt sector     = g_ex.fat_offset + entry_off / g_ex.bytes_per_sector;
	uInt sector_off = entry_off % g_ex.bytes_per_sector;

	if (read_sector(e, sector, scratch, g_ex.bytes_per_sector) != NO_ERROR)
		return 0xFFFFFFFFu;

	return le32(scratch + sector_off);
}

/* --- Cluster -> sector conversion. Cluster index 2 maps to
 * cluster_heap_offset. */
static uInt
exfat_cluster_to_sector(uInt cluster)
{
	if (cluster < 2) return 0;
	return g_ex.cluster_heap_offset +
	       (cluster - 2) * g_ex.sectors_per_cluster;
}

/* --- Simple ASCII case-folded equality between a C-string `c_name`
 * and a UTF-16LE buffer `utf16` of `utf16_chars` code units. exFAT
 * file names go up to 255 chars; we cap at 255 in the caller. Non-
 * ASCII code points are compared as-is (matches dosfat.c's LFN
 * behaviour -- acceptable for boot-kernel filenames which are
 * ASCII by convention). */
static int
exfat_name_eq(const uByte *c_name, uInt c_len,
	      const uByte *utf16, uInt utf16_chars)
{
	if (c_len != utf16_chars) return 0;
	for (uInt i = 0; i < c_len; i++) {
		uInt ulo = utf16[i * 2];
		uInt uhi = utf16[i * 2 + 1];
		if (uhi != 0) return 0;      /* not ASCII */
		uInt a = c_name[i];
		uInt b = ulo;
		if (a >= 'a' && a <= 'z') a -= 0x20;
		if (b >= 'a' && b <= 'z') b -= 0x20;
		if (a != b) return 0;
	}
	return 1;
}

/* --- Copy a UTF-16LE name (ASCII subset) to a plain ASCII buffer,
 * NUL-terminating. Used for the FS_LIST pretty-print path. */
static uInt
exfat_name_to_ascii(const uByte *utf16, uInt utf16_chars,
		    char *out, uInt out_max)
{
	uInt n = 0;
	if (out_max == 0) return 0;
	for (uInt i = 0; i < utf16_chars && n + 1 < out_max; i++) {
		uInt lo = utf16[i * 2];
		uInt hi = utf16[i * 2 + 1];
		if (hi != 0) { out[n++] = '?'; continue; }
		out[n++] = (char)lo;
	}
	out[n] = '\0';
	return n;
}

/* --- Walk a directory starting at `dir_cluster`. For each File
 * entry set (0x85 + 0xC0 + 0xC1...), invoke visit() with the
 * parsed fields. visit() returns non-zero to stop the walk. */
struct entry_set {
	const uByte *file_entry;     /* 0x85 primary entry (32 bytes) */
	const uByte *stream_entry;   /* 0xC0 secondary entry */
	uByte        name_utf16[2 * 255];
	uInt         name_chars;     /* UTF-16 code units in name */
};

/* --- Read one 512-byte directory sector at a time, walking the
 * cluster chain. Collects full entry sets (primary File entry
 * plus its secondary entries) and calls visit() on each.
 *
 * visit() receives the fully-parsed entry_set and a cookie. Returns
 * 0 to continue, 1 to stop with success, -1 to stop with error. */
typedef int (*exfat_visit_fn)(const struct entry_set *set, void *cookie);

static Retcode
exfat_walk_dir(Environ *e, uInt dir_cluster, exfat_visit_fn visit,
	       void *cookie, uByte *scratch)
{
	uInt cluster = dir_cluster;
	int  cluster_guard = 0;

	while (cluster >= 2 && cluster <= g_ex.cluster_count + 1 &&
	       cluster_guard++ < 65536) {
		uInt first_sector = exfat_cluster_to_sector(cluster);

		for (uInt s = 0; s < g_ex.sectors_per_cluster; s++) {
			Retcode r = read_sector(e, first_sector + s,
				scratch, g_ex.bytes_per_sector);
			if (r != NO_ERROR) return r;

			for (uInt off = 0; off + 32 <= g_ex.bytes_per_sector;
			     off += 32) {
				const uByte *ent = scratch + off;
				uByte type = ent[0];

				if (type == ENT_END_OF_DIR) {
					/* End of directory -- but we can't
					 * guarantee the rest of the cluster
					 * holds only zeros, so stop the
					 * whole walk. */
					return NO_ERROR;
				}
				if ((type & 0x80) == 0)
					continue;    /* unused entry */
				if (type != ENT_FILE)
					continue;    /* bitmap/upcase/label
					              * or stream-ext we reach
					              * via its preceding File
					              * entry */

				/* Start of a file entry set. Collect primary
				 * + SecondaryCount secondaries.  SecondaryCount
				 * is in byte 1 of the File entry and is at
				 * least 2 (one Stream Extension + one File
				 * Name). We need them CONTIGUOUS in memory;
				 * a set can straddle a sector boundary. For
				 * simplicity we buffer each entry into the
				 * entry_set structure as we encounter them,
				 * reading across sector/cluster boundaries as
				 * needed. */
				uByte secondary_count = ent[FE_SECONDARYCOUNT];
				if (secondary_count < 2)
					continue;
				uInt entries_needed = 1u + secondary_count;

				struct entry_set set;
				set.file_entry = NULL;
				set.stream_entry = NULL;
				set.name_chars = 0;

				/* We'll re-read entries sequentially from
				 * (current cluster, current sector, current
				 * off) to collect the whole set. The primary
				 * entry is in `ent`; save a copy into a local
				 * buffer so we don't lose it when we read
				 * further sectors. */
				static uByte primary_buf[32];
				static uByte stream_buf[32];
				memcpy(primary_buf, ent, 32);
				set.file_entry = primary_buf;

				uInt collected  = 1;
				uInt walk_clu   = cluster;
				uInt walk_sec   = s;
				uInt walk_off   = off + 32;

				int ok = 1;
				while (collected < entries_needed) {
					/* Advance (walk_clu, walk_sec, walk_off)
					 * to the next 32-byte entry. */
					if (walk_off + 32 > g_ex.bytes_per_sector) {
						walk_off = 0;
						walk_sec++;
						if (walk_sec >= g_ex.sectors_per_cluster) {
							walk_sec = 0;
							walk_clu = exfat_fat_next(e,
								walk_clu, scratch);
							if (walk_clu < 2 ||
							    walk_clu > g_ex.cluster_count + 1) {
								ok = 0;
								break;
							}
						}
					}
					/* The outer `scratch` buffer is
					 * mid-use (we hold `ent` inside it).
					 * Switch to a second scratch sector
					 * and come back. We do this by reading
					 * the needed sector into `scratch2`
					 * and treating that as the live page
					 * for the inner loop -- conceptually
					 * we're reading ahead to complete the
					 * entry set. */
					static uByte scratch2[4096];
					if (g_ex.bytes_per_sector > sizeof scratch2) {
						ok = 0;
						break;
					}
					uInt walk_abs_sector =
					    exfat_cluster_to_sector(walk_clu) + walk_sec;
					Retcode r2 = read_sector(e, walk_abs_sector,
						scratch2, g_ex.bytes_per_sector);
					if (r2 != NO_ERROR) { ok = 0; break; }
					const uByte *nxt = scratch2 + walk_off;

					if (nxt[0] == ENT_STREAM_EXT && set.stream_entry == NULL) {
						memcpy(stream_buf, nxt, 32);
						set.stream_entry = stream_buf;
					} else if (nxt[0] == ENT_FILE_NAME) {
						/* Copy up to 15 UTF-16 chars. */
						uInt remaining = (set.stream_entry) ?
						    (uInt)set.stream_entry[SE_NAMELENGTH] -
						        set.name_chars : 0;
						uInt take = FN_CHARS_PER_ENTRY;
						if (take > remaining) take = remaining;
						if (set.name_chars + take > 255)
							take = 255 - set.name_chars;
						memcpy(set.name_utf16 + set.name_chars * 2,
						       nxt + FN_NAME, take * 2);
						set.name_chars += take;
					}
					collected++;
					walk_off += 32;
				}

				if (ok && set.stream_entry != NULL) {
					int v = visit(&set, cookie);
					if (v != 0)
						return (v < 0) ? E_READ_ERROR : NO_ERROR;
				}
			}
		}

		cluster = exfat_fat_next(e, cluster, scratch);
		if (cluster == 0xFFFFFFFFu || cluster < 2)
			break;
	}
	return NO_ERROR;
}

/* --- FS_LIST callback: pretty-print each file --------------- */
struct list_cookie { Environ *e; };

static int
exfat_visit_list(const struct entry_set *set, void *cookie_void)
{
	struct list_cookie *c = (struct list_cookie *)cookie_void;
	char ascii[256];
	uInt n = exfat_name_to_ascii(set->name_utf16, set->name_chars,
		ascii, sizeof ascii);
	(void)n;

	uInt flags = le16(set->file_entry + FE_GENPRIMFLAGS);
	uByte attrs = set->file_entry[FE_FILEATTRS];
	uInt is_dir = (attrs & 0x10) ? 1 : 0;
	(void)flags;

	uInt sz_lo = le32(set->stream_entry + SE_DATALENGTH);
	uInt sz_hi = le32(set->stream_entry + SE_DATALENGTH + 4);
	if (is_dir)
		cprintf(c->e, "          [%s]\n", ascii);
	else if (sz_hi)
		cprintf(c->e, "    >4GiB  %s\n", ascii);
	else
		cprintf(c->e, "%8d  %s\n", (int)sz_lo, ascii);
	return 0;
}

/* --- FS_LOAD walk-path cookie. Matches on exact case-folded
 * ASCII name; records the stream entry's first_cluster + data_len
 * + flags for the caller to read file contents with. */
struct find_cookie {
	const uByte *name;
	uInt         namelen;
	uInt         first_cluster;
	uInt         data_len;
	uInt         flags;
	int          found;
};

static int
exfat_visit_find(const struct entry_set *set, void *cookie_void)
{
	struct find_cookie *c = (struct find_cookie *)cookie_void;

	if (!exfat_name_eq(c->name, c->namelen,
	                   set->name_utf16, set->name_chars))
		return 0;

	c->first_cluster = le32(set->stream_entry + SE_FIRSTCLUSTER);
	c->data_len      = le32(set->stream_entry + SE_DATALENGTH);
	c->flags         = set->stream_entry[SE_GENSECFLAGS];
	/* sz_hi != 0 means file > 4 GiB; we cap at 4 GiB for this
	 * reader (our retbuf is at most a few MiB anyway). */
	uInt sz_hi = le32(set->stream_entry + SE_DATALENGTH + 4);
	if (sz_hi != 0)
		return -1;
	c->found = 1;
	return 1;   /* stop walk on match */
}

/* --- Read file contents starting at first_cluster, copying
 * data_len bytes into `out`. Follows FAT chain unless NoFatChain
 * flag is set (then assume contiguous). */
static Retcode
exfat_read_contents(Environ *e, uInt first_cluster, uInt data_len, int no_fat,
		    uByte *out, uLong *out_len, uByte *scratch)
{
	uInt cluster = first_cluster;
	uInt written = 0;
	int  guard = 0;

	while (written < data_len &&
	       cluster >= 2 && cluster <= g_ex.cluster_count + 1 &&
	       guard++ < 100000) {
		uInt first_sector = exfat_cluster_to_sector(cluster);

		for (uInt s = 0; s < g_ex.sectors_per_cluster &&
		                 written < data_len; s++) {
			Retcode r = read_sector(e, first_sector + s,
				scratch, g_ex.bytes_per_sector);
			if (r != NO_ERROR) return r;
			uInt remain = data_len - written;
			uInt copy   = (remain < g_ex.bytes_per_sector)
			              ? remain : g_ex.bytes_per_sector;
			memcpy(out + written, scratch, copy);
			written += copy;
		}

		if (written >= data_len) break;

		if (no_fat) {
			/* Contiguous allocation -- just advance linearly. */
			cluster++;
		} else {
			cluster = exfat_fat_next(e, cluster, scratch);
			if (cluster == 0xFFFFFFFFu) break;
		}
	}

	*out_len = written;
	return NO_ERROR;
}

/* --- Main Filesys callback ---------------------------------- */
Retcode
fs_exfat(Environ *e, Filesys_action what, Instance *disk,
	 Byte *path, uLong loc, uLong start_unused, uByte *buf,
	 uInt size, uByte *retbuf, uLong *val)
{
	(void)start_unused;
	if (size < 512) return E_BLOCKSIZE;

	if (exfat_open_volume(e, disk, loc, buf) != NO_ERROR)
		return E_NO_FILESYS;

	switch (what) {
	case FS_PROBE:
		strcat((char *)retbuf, ",exfat");
		*val = loc;
		return R_END;

	case FS_LIST: {
		/* We ignore sub-directory walking for now; exFAT root-only
		 * is sufficient for the boot-kernel use case. */
		uByte scratch[512];
		struct list_cookie c = { .e = e };
		cprintf(e, "exFAT volume:\n");
		Retcode ret = exfat_walk_dir(e, g_ex.root_cluster,
			exfat_visit_list, &c, scratch);
		(void)ret;
		return R_END;
	}

	case FS_LOAD: {
		/* Strip leading '/'. Empty path -> not supported (need
		 * a file name). No sub-dir traversal yet. */
		while (*path == '/' || *path == '\\')
			path++;
		if (*path == '\0')
			return E_NO_FILE;

		/* Strip any leading sub-dir components -- for the boot-
		 * kernel use case the file is always at root, and any
		 * hd:0 path chopping handled by the RDB layer upstream
		 * of us (for exFAT-on-MBR disks dospart.c recurses). */
		const Byte *tail = path;
		for (const Byte *p = path; *p; p++)
			if (*p == '/' || *p == '\\') tail = p + 1;

		uInt namelen = 0;
		while (tail[namelen]) namelen++;
		if (namelen == 0) return E_NO_FILE;

		uByte scratch[512];
		struct find_cookie c = {
			.name = (const uByte *)tail,
			.namelen = namelen,
		};
		Retcode ret = exfat_walk_dir(e, g_ex.root_cluster,
			exfat_visit_find, &c, scratch);
		if (ret != NO_ERROR) return ret;
		if (!c.found) return E_NO_FILE;

		uLong got = 0;
		int no_fat = (c.flags & GSF_NO_FAT_CHAIN) ? 1 : 0;
		ret = exfat_read_contents(e, c.first_cluster, c.data_len,
			no_fat, retbuf, &got, scratch);
		if (ret != NO_ERROR) return ret;
		*val = got;
		return R_END;
	}

	default:
		break;
	}

	return E_NO_FILESYS;
}

Filesys g_fs_exfat = {
	"exfat",
	fs_exfat,
};
