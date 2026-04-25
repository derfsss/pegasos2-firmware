/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Arc FS-B Block 2 -- Amiga OFS/FFS/FFS2 readonly filesystem reader.
 *
 *  Covers DOS\0..\3 (OFS, FFS, OFS-Intl, FFS-Intl), \6, \7 (the
 *  LNFS long-name variants, of which DOS\7 "FFS2" is the AOS4
 *  priority target). DOS\4/\5 (DirCache) fall back to reading
 *  directory entries the slow way -- the DirCache blocks are an
 *  optimisation, not the only path.
 *
 *  Registered as &g_amiga_ffs in platform.c's g_filesys[] after
 *  &g_amiga_rdb, so the typical call chain is:
 *      file_system(FS_LOAD, ...)
 *        -> amiga_rdb: recognise RDB, walk partitions, pick the one
 *           whose DosType matches "DOS\\<n>"; recurse into
 *           file_system for that partition's byte slice
 *        -> amiga_ffs: read root block, walk hash table from the
 *           path, chase file-header -> extension-block -> data-block
 *           chain, copy file contents to retbuf
 *
 *  Format reference:
 *      "The .ADF (Amiga Disk File) format FAQ" by Laurent Clevy,
 *      v1.14 (April 2017). Freely redistributable per the author's
 *      stated license. Provides complete on-disk field offsets for
 *      every block type.
 *
 *      "DCFS and LNFS Low Level Data Structures" on the AmigaOS
 *      Documentation Wiki (Olaf Barthel, 2015). Covers the DOS\6/\7
 *      long-name and DOS\4/\5 DirCache extensions layered on top
 *      of the base FFS format.
 *
 *  All structures are described at the FIELD-OFFSET level from the
 *  references above; no implementation code from ADFlib / AROS /
 *  any GPL source was read or copied. The same structural
 *  layouts appear in the AOS4 SDK's devices/hardblocks.h but we
 *  don't copy code from there either.
 */

#include "defs.h"
#include "fs.h"

/* Set by amiga_rdb before recursing into file_system on a partition.
 * Lets us compute the FFS root block position exactly instead of
 * probing a hardcoded list of likely offsets, which can read past
 * end-of-disk on partitions whose actual root sits between two
 * probe-table entries. See amiga_rdb.c for the full rationale. */
extern uLong g_amiga_part_byte_size;
extern uInt  g_amiga_part_block_size;

/* --- Block-type identifiers ------------------------------------ */
#define T_HEADER       2        /* root, userdir, file header, link */
#define T_DATA         8        /* OFS data block header */
#define T_LIST         16       /* file extension block */

#define ST_ROOT        1        /* secondary type: root block */
#define ST_USERDIR     2        /* secondary type: user directory */
#define ST_FILE        (-3)     /* secondary type: file header */
#define ST_LINKFILE    (-4)     /* hard link -- followed */
#define ST_LINKDIR     (-5)     /* soft link -- not followed in M-min */

/* --- Block size + field-offset arithmetic ---------------------- */
/* Amiga FFS supports 512, 1024, 2048, 4096-byte FS blocks. Classic
 * floppy + small-disk partitions use 512; AOS4 FFS2 on partitions
 * >= 1 GiB sets de_SectorPerBlock=2 in the RDB envvec, yielding
 * 1024-byte blocks where the trailer fields (sec_type, NaC, etc.)
 * shift by an extra 512 bytes from their classic positions.
 *
 * MAX_BSIZE bounds the static rootbuf; runtime g_ffs.bsize tracks
 * the actual on-disk block size derived from the RDB envvec
 * (de_SizeBlock * 4 * de_SectorPerBlock). Defaults to 512 for the
 * direct (non-RDB-recursed) FS_PROBE entry path. All trailer-field
 * offsets are now expressions in terms of g_ffs.bsize so the same
 * reader serves both 512- and 1024-byte FFS partitions. */
#define MAX_BSIZE      4096
#define BSIZE_DEFAULT  512    /* fallback when no RDB hint available */

/* Header-block tail fields (measured DOWN from g_ffs.bsize) */
#define OFF_SEC_TYPE       (g_ffs.bsize - 4)
#define OFF_EXTENSION      (g_ffs.bsize - 8)
#define OFF_PARENT         (g_ffs.bsize - 12)
#define OFF_NEXT_HASH      (g_ffs.bsize - 16)

/* File-header tail fields */
#define OFF_COMMENT        (g_ffs.bsize - 184)
#define OFF_NAME           (g_ffs.bsize - 80)
#define OFF_BYTE_SIZE      (g_ffs.bsize - 188)

/* LNFS (DOS\6/\7) Name-and-Comment area.
 *
 * LNFS replaces the classic 80-byte NAME field (at BSIZE-80) and the
 * 79-byte COMMENT field (at BSIZE-184) with a single NaC region that
 * starts at the COMMENT offset and extends forward to BSIZE-80. The
 * first BSTR in the region is the file's name; the second is the
 * comment. Reference: Olaf Barthel's LNFS writeup on the AmigaOS
 * Documentation Wiki. */
#define OFF_LNFS_NAC       (g_ffs.bsize - 184)

/* Hash table occupies [HT_OFFSET .. bsize-200). Entries scale with
 * block size: 72 for BSIZE=512, 200 for BSIZE=1024, etc. */
#define HT_OFFSET          0x18
#define HT_ENTRIES         ((g_ffs.bsize - HT_OFFSET - 200) / 4)

/* File-header data_blocks[] is an array of LBA pointers stored with
 * index 0 (= the file's first data block) at the highest offset
 * DB_HIGH_OFF and growing downward toward HT_OFFSET (0x18). The
 * topmost slot sits at BSIZE-204, per Clevy's ADF FAQ. DB_MAX is
 * the maximum entries that fit between HT_OFFSET and DB_HIGH_OFF. */
#define DB_HIGH_OFF        (g_ffs.bsize - 204)
#define DB_MAX             ((g_ffs.bsize - 228) / 4)

/* --- DosType bit decoding -------------------------------------- */
/* Low byte of DosType, mask 0x07:
 *   bit 0: FFS (vs OFS)
 *   bit 1: International (locale-aware case folding)
 *   bit 2: DirCache (implies bit 1 -- INTL)
 * bit 2 with bit 0 => FFS, bit 2 without bit 0 => OFS, etc.
 *
 * LNFS (DOS\6/\7) is not visible in the DosType bits -- we must
 * look at bit 2 *plus* the top three bytes. DOS\6/\7 disks have
 * 'DOS\6'/'DOS\7' exactly, where bit 2 is set but the LNFS marker
 * lives in a root-block field added by the AOS4 FS (not in the
 * DosType itself). Safer: detect LNFS at root-block read time by
 * the "FileSystemType" field at root_block[0x80] containing 'DOS\6'
 * or 'DOS\7', per Barthel's LNFS writeup.
 */
#define DOSTYPE_FFS_BIT   0x01
#define DOSTYPE_INTL_BIT  0x02
#define DOSTYPE_DC_BIT    0x04   /* DirCache -- DOS\4/\5 only */

/* Root block's LNFS signature field (see Barthel's doc):
 * for DOS\6/\7 root blocks it contains 'DOS\6' (0x44_4F_53_06) or
 * 'DOS\7' (0x44_4F_53_07). Other root blocks leave it zero. */
#define ROOT_OFF_FS_TYPE  0x80

/* --- Per-instance state. Opened at FS_LOAD/FS_LIST time, freed
 * at end. Held in a static because file_system() has no per-call
 * context slot and we're single-threaded. */
static struct {
	Instance  *disk;
	uLong      part_loc;   /* partition byte offset on disk */
	uInt       part_size;  /* partition size in bytes (from RDB) */
	uInt       bsize;      /* FS block size (512/1024/2048) */
	uInt       dostype;    /* low byte, for FFS/OFS/Intl bits */
	int        is_ffs;     /* FFS (no data-block header) vs OFS */
	int        is_intl;    /* international case folding */
	int        is_lnfs;    /* long-name variant (DOS\6/\7) */
	uByte      rootbuf[MAX_BSIZE];
	uInt       root_block; /* block number of root block */
} g_ffs;

/* --- Byte / word / longword big-endian readers ----------------- */
static uInt
be32(const uByte *p)
{
	return ((uInt)p[0] << 24) | ((uInt)p[1] << 16) |
	       ((uInt)p[2] << 8)  | (uInt)p[3];
}

/* --- Block I/O helper ------------------------------------------ */
/* Read `block_num` (relative to the partition, 0-based in
 * `bsize` units) into buf. The bsize parameter avoids any reliance
 * on g_ffs.bsize being valid -- callers thread the right size. */
static Retcode
read_block_sz(Environ *e, Instance *disk, uLong part_loc, uInt block_num,
	      uInt bsize, uByte *buf)
{
	return filesys_read_bytes(e, disk,
		part_loc + (uLong)block_num * bsize, bsize, buf);
}

static Retcode
read_block(Environ *e, Instance *disk, uLong part_loc, uInt block_num,
	   uByte *buf)
{
	return read_block_sz(e, disk, part_loc, block_num, g_ffs.bsize, buf);
}

/* --- Checksum -------------------------------------------------- */
/* Amiga FFS blocks (root, userdir, file header, extension) have
 * their 32-bit longwords sum to zero mod 2^32, treating the
 * stored checksum at offset 0x14 as part of the sum. Block passes
 * checksum iff sum(buf[0..bsize), 4-byte big-endian) == 0. */
static int
checksum_ok_sz(const uByte *buf, uInt bsize)
{
	uInt sum = 0;
	for (uInt i = 0; i < bsize; i += 4)
		sum += be32(buf + i);
	return sum == 0 ? 1 : 0;
}

static int
checksum_ok(const uByte *buf)
{
	return checksum_ok_sz(buf, g_ffs.bsize);
}

/* --- Name case folding ----------------------------------------- */
/* Normal (non-international): just uppercase A-Z, leave others
 * alone. International: additionally uppercase chars 0xE0..0xFF
 * per the Amiga convention (matches AmigaOS's stricmp I/II). */
static uByte
fold_ch(uByte c, int intl)
{
	if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
	if (intl && c >= 0xE0 && c <= 0xFE && c != 0xF7)
		return c - 0x20;
	return c;
}

/* --- Name hash per Clevy's FAQ ------------------------------
 *     hash = namelen
 *     for each char: hash = (hash*13 + fold(char)) & 0x7FF
 *     return hash mod HT_ENTRIES
 * Same algorithm for OFS / FFS / LNFS; international variant just
 * folds 0xE0..0xFE uppercase as well. */
static uInt
name_hash(const uByte *name, uInt len, int intl)
{
	uInt h = len;
	for (uInt i = 0; i < len; i++)
		h = (h * 13 + fold_ch(name[i], intl)) & 0x7FFu;
	return h % HT_ENTRIES;
}

/* --- Name comparison: case-folding, length-first --------------- */
static int
name_eq(const uByte *a, uInt alen, const uByte *b, uInt blen, int intl)
{
	if (alen != blen) return 0;
	for (uInt i = 0; i < alen; i++) {
		if (fold_ch(a[i], intl) != fold_ch(b[i], intl))
			return 0;
	}
	return 1;
}

/* --- Extract the name from a file-header / userdir block. On
 * DOS\0..\5 the name is a BSTR at offset BSIZE-80 (length byte +
 * up to 30 chars). On LNFS (DOS\6/\7) the name is the FIRST BSTR
 * inside the NaC[112] region at offset BSIZE-92. Returns pointer
 * + length via out params. */
static void
extract_name(const uByte *block, int is_lnfs,
	     const uByte **out_name, uInt *out_len)
{
	const uByte *p;
	if (is_lnfs)
		p = block + OFF_LNFS_NAC;    /* BSIZE-92 */
	else
		p = block + OFF_NAME;        /* BSIZE-80 */
	*out_len  = p[0];
	*out_name = p + 1;
}

/* --- Parse root block. Cache enough state for path-walks. Called
 * exactly once per partition "open" (FS_PROBE or first FS_LOAD).
 *
 * Returns NO_ERROR if the partition contains a recognisable Amiga
 * root block matching the expected DosType bits, else E_NO_FILESYS.
 */
static Retcode
open_volume(Environ *e, Instance *disk, uLong part_loc, uInt part_size,
	    uByte *buf, uInt dostype)
{
	/* The root block typically lives at the middle of the
	 * partition. For a 512-byte-block partition, the formula is
	 * root = (total_blocks - 1 + reserved + prealloc) / 2 which
	 * rounds to partition_size_blocks / 2. de_Reserved is
	 * usually 2 (RDB standard). Without de_* in scope here we
	 * approximate as part_size_blocks / 2, which matches the
	 * typical FFS layout for RDB partitions and for floppies
	 * (880 on a DD floppy). */
	uInt part_blocks = part_size / g_ffs.bsize;
	uInt root_block  = part_blocks / 2;

	Retcode ret = read_block(e, disk, part_loc, root_block, buf);
	if (ret != NO_ERROR)
		return ret;

	if (be32(buf + 0x00) != (uInt)T_HEADER)
		return E_NO_FILESYS;
	if ((Int)be32(buf + OFF_SEC_TYPE) != ST_ROOT)
		return E_NO_FILESYS;
	if (!checksum_ok(buf))
		return E_NO_FILESYS;

	g_ffs.disk       = disk;
	g_ffs.part_loc   = part_loc;
	g_ffs.part_size  = part_size;
	g_ffs.dostype    = dostype & 0xFF;
	g_ffs.is_ffs     = (g_ffs.dostype & DOSTYPE_FFS_BIT)  ? 1 : 0;
	g_ffs.is_intl    = (g_ffs.dostype & DOSTYPE_INTL_BIT) ? 1 : 0;
	/* DirCache (DOS\4/\5) implies Intl folding even though the
	 * Intl bit itself isn't set on DOS\4's dostype. Barthel's doc
	 * and Commodore's original DC code both make this guarantee --
	 * without it the hash bucket in a DC volume would be computed
	 * with ASCII-only folding on writer and Intl folding on reader
	 * (or vice versa) and name lookups would miss. */
	if (g_ffs.dostype & DOSTYPE_DC_BIT)
		g_ffs.is_intl = 1;
	/* LNFS is detected by the low byte of the partition's DosType
	 * being 6 or 7 (Barthel's root-block signature field is a
	 * secondary hint; the dostype is authoritative for our reader). */
	if (g_ffs.dostype == 6 || g_ffs.dostype == 7) {
		g_ffs.is_lnfs = 1;
		g_ffs.is_intl = 1;   /* LNFS always uses Intl hashing */
	} else {
		g_ffs.is_lnfs = 0;
	}

	g_ffs.root_block = root_block;
	memcpy(g_ffs.rootbuf, buf, g_ffs.bsize);
	return NO_ERROR;
}

/* --- Walk a hash-table chain looking for `name`. `ht_block` is the
 * block containing the hash table (root or userdir). On match,
 * returns the matching file/dir/link header block number, else 0. */
static uInt
lookup_in_hash(Environ *e, const uByte *ht_block,
	       const uByte *name, uInt namelen, uByte *scratch)
{
	uInt bucket = name_hash(name, namelen, g_ffs.is_intl);
	uInt chain = be32(ht_block + HT_OFFSET + bucket * 4);

	while (chain != 0) {
		Retcode r = read_block(e, g_ffs.disk, g_ffs.part_loc,
				       chain, scratch);
		if (r != NO_ERROR)
			return 0;
		if (be32(scratch + 0x00) != (uInt)T_HEADER)
			return 0;

		const uByte *cn; uInt cl;
		extract_name(scratch, g_ffs.is_lnfs, &cn, &cl);
		if (name_eq(cn, cl, name, namelen, g_ffs.is_intl))
			return chain;

		chain = be32(scratch + OFF_NEXT_HASH);
	}
	return 0;
}

/* --- Walk a '/'-separated path starting at the root. `scratch` is
 * a caller-supplied BSIZE buffer used to load intermediate blocks.
 * On success, `scratch` holds the FINAL resolved block (file or
 * dir header), and we return its block number. Returns 0 on
 * not-found / error. */
static uInt
walk_path(Environ *e, const Byte *path, uByte *scratch)
{
	/* Skip leading '/'. */
	while (*path == '/' || *path == '\\')
		path++;

	/* If path is empty, we want the root directory -- copy rootbuf
	 * into scratch and return root_block. */
	if (*path == '\0') {
		memcpy(scratch, g_ffs.rootbuf, g_ffs.bsize);
		return g_ffs.root_block;
	}

	/* Start hash lookup from root's hash table. */
	uByte ht_buf[MAX_BSIZE];
	memcpy(ht_buf, g_ffs.rootbuf, g_ffs.bsize);

	const Byte *cur = path;
	uInt current_block = g_ffs.root_block;

	while (*cur) {
		/* Isolate next path component. */
		const Byte *sep = cur;
		while (*sep && *sep != '/' && *sep != '\\') sep++;
		uInt complen = (uInt)(sep - cur);
		if (complen == 0) { cur = sep + 1; continue; }
		if (complen > 107) return 0;  /* LNFS max */

		uInt next = lookup_in_hash(e, ht_buf,
			(const uByte *)cur, complen, scratch);
		if (next == 0)
			return 0;

		current_block = next;
		/* Step into the child: if it's a directory, copy its
		 * block into ht_buf for the next hop. If it's a file
		 * and there are more path components, fail (no file-
		 * under-file paths). */
		Int sec_type = (Int)be32(scratch + OFF_SEC_TYPE);
		if (*sep == '\0')
			return current_block;   /* found; caller reads */

		if (sec_type != ST_USERDIR && sec_type != ST_ROOT) {
			/* More path to walk but not a dir. */
			return 0;
		}
		memcpy(ht_buf, scratch, g_ffs.bsize);
		cur = sep + 1;
	}

	return current_block;
}

/* --- Read the data payload of a file given its file-header block
 * (already in `hdr`). Writes up to `max_bytes` into `out`; returns
 * the number actually written via *out_len. The caller provides a
 * BSIZE scratch buffer for extension-block traversal. */
static Retcode
read_file_contents(Environ *e, const uByte *hdr, uByte *out,
		   uLong max_bytes, uLong *out_len, uByte *scratch)
{
	uInt byte_size = be32(hdr + OFF_BYTE_SIZE);
	uLong to_read  = (byte_size < max_bytes) ? byte_size : max_bytes;
	uLong written  = 0;
	const uByte *cur_hdr = hdr;

	/* The data_blocks[] array lives between HT_OFFSET and
	 * DB_HIGH_OFF in a file header or extension block, stored
	 * DOWNWARDS from DB_HIGH_OFF. */
	while (written < to_read) {
		for (uInt i = 0; i < DB_MAX && written < to_read; i++) {
			uInt blkptr = be32(cur_hdr + DB_HIGH_OFF - i * 4);
			if (blkptr == 0)
				goto done;

			uByte dblk[MAX_BSIZE];
			Retcode r = read_block(e, g_ffs.disk,
				g_ffs.part_loc, blkptr, dblk);
			if (r != NO_ERROR)
				return r;

			/* OFS data blocks have a 24-byte header; skip
			 * it and copy the payload. FFS blocks are raw. */
			const uByte *payload;
			uInt payload_len;
			if (g_ffs.is_ffs) {
				payload     = dblk;
				payload_len = g_ffs.bsize;
			} else {
				if (be32(dblk + 0x00) != (uInt)T_DATA)
					return E_READ_ERROR;
				payload     = dblk + 0x18;  /* past header */
				payload_len = be32(dblk + 0x0C); /* Data_size */
				if (payload_len > g_ffs.bsize - 0x18)
					payload_len = g_ffs.bsize - 0x18;
			}

			uLong remaining = to_read - written;
			uLong copy = (payload_len < remaining)
				? payload_len : remaining;
			memcpy(out + written, payload, (size_t)copy);
			written += copy;
		}

		/* More data blocks? Follow the extension chain. */
		uInt ext = be32(cur_hdr + OFF_EXTENSION);
		if (ext == 0 || written >= to_read)
			break;

		Retcode r = read_block(e, g_ffs.disk, g_ffs.part_loc,
				       ext, scratch);
		if (r != NO_ERROR)
			return r;
		if (be32(scratch + 0x00) != (uInt)T_LIST)
			return E_READ_ERROR;
		cur_hdr = scratch;
	}

done:
	*out_len = written;
	return NO_ERROR;
}

/* --- List entries under a directory (root or userdir) block.
 * Walks the hash table, printing each entry's name with a "/"
 * suffix for dirs and size otherwise. */
static void
list_directory(Environ *e, const uByte *dir_block, uByte *scratch)
{
	for (uInt i = 0; i < HT_ENTRIES; i++) {
		uInt chain = be32(dir_block + HT_OFFSET + i * 4);
		while (chain != 0) {
			Retcode r = read_block(e, g_ffs.disk, g_ffs.part_loc,
					       chain, scratch);
			if (r != NO_ERROR) return;

			const uByte *cn; uInt cl;
			extract_name(scratch, g_ffs.is_lnfs, &cn, &cl);

			Int sec = (Int)be32(scratch + OFF_SEC_TYPE);
			if (sec == ST_FILE) {
				uInt bsz = be32(scratch + OFF_BYTE_SIZE);
				cprintf(e, "%8d  %S\n", (int)bsz, cn, (Int)cl);
			} else if (sec == ST_USERDIR) {
				cprintf(e, "          [%S]\n", cn, (Int)cl);
			} else {
				cprintf(e, "     ----  %S\n", cn, (Int)cl);
			}

			chain = be32(scratch + OFF_NEXT_HASH);
		}
	}
}

/* --- Main Filesys callback ------------------------------------- */
Retcode
amiga_ffs(Environ *e, Filesys_action what, Instance *disk,
	  Byte *path, uLong loc, uLong start_unused, uByte *buf,
	  uInt size, uByte *retbuf, uLong *val)
{
	(void)start_unused;
	if (size < BSIZE_DEFAULT) return E_BLOCKSIZE;

	/* Adopt the FS block size from the RDB hint when we have it;
	 * fall back to the legacy 512-byte default for whole-disk
	 * FS_PROBE entry. Must be set BEFORE any read_block call as
	 * read_block reads g_ffs.bsize bytes per FS block. */
	if (g_amiga_part_block_size >= BSIZE_DEFAULT &&
	    g_amiga_part_block_size <= MAX_BSIZE)
		g_ffs.bsize = g_amiga_part_block_size;
	else
		g_ffs.bsize = BSIZE_DEFAULT;

	/* The caller-supplied scratch is sized to the disk's logical
	 * block (512 bytes for a 512-byte-sector ATA disk). Reading a
	 * 1024-byte FFS2 block into it would overflow the heap allocation
	 * behind disklbl. Use our own MAX_BSIZE-sized buffer for every
	 * read in this reader instead. Single-threaded; static is safe. */
	static uByte g_ffs_iobuf[MAX_BSIZE];
	buf = g_ffs_iobuf;

	/* Quick reject via boot-block DosType. The DOS\N marker
	 * lives in the first 4 bytes of partition byte 0 regardless
	 * of FS block size, so a fixed 512-byte read is sufficient
	 * to test it; we read with the actual bsize so I/O alignment
	 * matches device expectations. */
	uByte bootbuf[MAX_BSIZE];
	uInt dt = 0;
	Retcode r0 = read_block(e, disk, loc, 0, bootbuf);
	if (r0 != NO_ERROR)
		return E_NO_FILESYS;
	uInt boot_id = be32(bootbuf + 0x00);
	if ((boot_id & 0xFFFFFF00u) != 0x444F5300u)
		return E_NO_FILESYS;
	dt = boot_id & 0xFFu;
	if (dt > 7)
		return E_NO_FILESYS;

	/* Compute FFS root-block position. AOS partitions follow
	 *     root = (lowKey + highKey) / 2
	 * where lowKey is the first non-reserved block (de_Reserved,
	 * conventionally 2) and highKey is the last block of the
	 * partition. When amiga_rdb publishes partition byte size +
	 * block size in g_amiga_part_* we have enough to pin root
	 * exactly. Otherwise (whole-disk FS_PROBE entry, no RDB)
	 * we fall back to a small probe table of likely positions.
	 *
	 * The probe table is bounded by part_blocks_hint when we have
	 * one, so a probe entry past partition end never issues an
	 * out-of-range disk read (which on some controllers fails the
	 * entire probe pass with a misleading "device not responding"). */
	int found = 0;
	uInt picked_root = 0;
	uLong part_blocks_hint = 0;

	if (g_amiga_part_byte_size && g_amiga_part_block_size) {
		part_blocks_hint = g_amiga_part_byte_size /
			g_amiga_part_block_size;
		uInt low_key  = 2;
		uInt high_key = (uInt)(part_blocks_hint - 1);
		uInt root_calc = (low_key + high_key) / 2u;

		Retcode r = read_block(e, disk, loc, root_calc, buf);
		if (r == NO_ERROR &&
		    be32(buf + 0x00) == (uInt)T_HEADER &&
		    (Int)be32(buf + OFF_SEC_TYPE) == ST_ROOT &&
		    checksum_ok(buf)) {
			picked_root = root_calc;
			found = 1;
		}
	}

	if (!found) {
		/* Fallback probe table for the whole-disk FS_PROBE case
		 * where we don't have RDB-derived geometry. Skip entries
		 * that would exceed the partition (when we have a hint)
		 * to stay in-bounds. */
		static const uInt probe_mids[] = {
			880,            /* DD floppy standard */
			1760,           /* HD floppy */
			4096,           /* small HD partition ~4 MiB */
			16384,          /* 16 MiB */
			65536,          /* 64 MiB */
			262144,         /* 256 MiB */
			1048576,        /* 1 GiB */
			2097152,        /* 2 GiB */
			0
		};
		for (uInt i = 0; probe_mids[i] != 0; i++) {
			if (part_blocks_hint &&
			    (uLong)probe_mids[i] >= part_blocks_hint)
				break;   /* would read past partition end */
			Retcode r = read_block(e, disk, loc, probe_mids[i], buf);
			if (r != NO_ERROR) break;    /* past disk end */
			if (be32(buf + 0x00) != (uInt)T_HEADER) continue;
			if ((Int)be32(buf + OFF_SEC_TYPE) != ST_ROOT) continue;
			if (!checksum_ok(buf)) continue;
			picked_root = probe_mids[i];
			found = 1;
			break;
		}
	}
	if (!found)
		return E_NO_FILESYS;

	/* Prefer the RDB-published partition size; fall back to the
	 * picked-root-doubled estimate when we don't have the hint. */
	uInt part_size_guess;
	if (g_amiga_part_byte_size &&
	    g_amiga_part_byte_size <= 0xFFFFFFFFul)
		part_size_guess = (uInt)g_amiga_part_byte_size;
	else
		part_size_guess = picked_root * 2 * g_ffs.bsize;
	if (open_volume(e, disk, loc, part_size_guess, buf, dt) != NO_ERROR) {
		/* open_volume re-reads at the rootblock formula position,
		 * which may differ from picked_root; fall back to using
		 * the probed block directly. */
		g_ffs.disk       = disk;
		g_ffs.part_loc   = loc;
		g_ffs.part_size  = part_size_guess;
		g_ffs.dostype    = dt;
		g_ffs.is_ffs     = (dt & DOSTYPE_FFS_BIT)  ? 1 : 0;
		g_ffs.is_intl    = (dt & DOSTYPE_INTL_BIT) ? 1 : 0;
		if (dt & DOSTYPE_DC_BIT) g_ffs.is_intl = 1;
		g_ffs.is_lnfs    = (dt == 6 || dt == 7)    ? 1 : 0;
		if (g_ffs.is_lnfs) g_ffs.is_intl = 1;
		g_ffs.root_block = picked_root;
		memcpy(g_ffs.rootbuf, buf, g_ffs.bsize);
	}

	switch (what) {
	case FS_PROBE:
		strcat((char *)retbuf, ",amiga-ffs");
		*val = loc;
		return R_END;

	case FS_LIST: {
		uByte scratch[MAX_BSIZE];
		/* Walk to the requested directory. Empty path =
		 * root dir. */
		uInt blk = walk_path(e, path, scratch);
		if (blk == 0)
			return E_NO_FILE;
		/* If path was a file, list just its line. If a dir,
		 * enumerate its entries. */
		Int sec = (Int)be32(scratch + OFF_SEC_TYPE);
		if (sec == ST_ROOT || sec == ST_USERDIR) {
			cprintf(e, "Amiga FFS volume ");
			const uByte *vn; uInt vl;
			extract_name(g_ffs.rootbuf, 0, &vn, &vl);
			cprintf(e, "\"%S\":\n", vn, (Int)vl);
			list_directory(e, scratch, buf);
		} else if (sec == ST_FILE) {
			uInt bsz = be32(scratch + OFF_BYTE_SIZE);
			cprintf(e, "%8d  %s\n", (int)bsz, (char *)path);
		}
		return R_END;
	}

	case FS_LOAD: {
		uByte scratch[MAX_BSIZE];
		uInt blk = walk_path(e, path, scratch);
		if (blk == 0)
			return E_NO_FILE;
		Int sec = (Int)be32(scratch + OFF_SEC_TYPE);
		if (sec != ST_FILE)
			return E_NO_FILE;
		uLong got = 0;
		Retcode ret = read_file_contents(e, scratch, retbuf,
			0xFFFFFFFFu, &got, buf);
		if (ret != NO_ERROR)
			return ret;
		*val = got;
		return R_END;
	}

	default:
		break;
	}

	return E_NO_FILESYS;
}

Filesys g_amiga_ffs = {
	"amiga-ffs",
	amiga_ffs,
};
