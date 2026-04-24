/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Arc FS-B Block 5 -- Professional File System 3 (PFS3) readonly
 *  reader. Covers classic PFS\1 and AOS4-era PFS\2 (plus AFS\1,
 *  which shares the block layout -- MultiUser variants muAF / muPF
 *  are not recognised yet).
 *
 *  Registered as &g_amiga_pfs3 in platform.c's g_filesys[] after
 *  &g_amiga_sfs. RDB-partitioned disks route into this reader when
 *  the partition's DosType starts with 'PFS' or 'AFS' magic.
 *
 *  Format reference:
 *      Michiel Pelt's pfs3aio header files (blocks.h, struct.h),
 *      published under a 4-clause BSD licence at
 *      github.com/tonioni/pfs3aio. Used as an on-disk format spec
 *      (struct field offsets, block IDs, predefined anode numbers,
 *      layout conventions) rather than as a code template. No
 *      implementation code was copied; the advertising clause of
 *      the pfs3aio licence therefore does not propagate to our
 *      source tree.
 *
 *  Format summary:
 *    - Block 0 / block 1: boot blocks (4-byte DosType at offset 0)
 *    - Block 2: root block (no checksum; integrity via `datestamp`)
 *    - Root block lists `idx.small.indexblocks[0..98]` BLCK pointers
 *      for the anode index tree (small-disk variant covers up to
 *      ~5 GiB with 512-byte reserved blocks -- plenty for the boot
 *      slice we care about).
 *    - Anode lookup tree is 3 levels:
 *        rootblock.indexblocks[H] -> indexblock at BLCK B
 *        indexblock.index[M]       -> anodeblock at BLCK C
 *        anodeblock.nodes[L]       -> { clustersize, blocknr, next }
 *    - Predefined anode 5 (ANODE_ROOTDIR) is the root directory.
 *    - Directory blocks ('DB' id) hold a packed array of
 *      variable-length direntries. Each direntry has:
 *          +0  UBYTE  next (total record size in bytes)
 *          +1  BYTE   type (ST_USERDIR=2, ST_FILE=-3, ...)
 *          +2  ULONG  anode (file/dir's anode number)
 *          +6  ULONG  fsize
 *          +17 UBYTE  nlength
 *          +18 UBYTE  name[nlength]
 *          +18+nlength UBYTE comment_length, comment...
 *    - File data is read by walking the anode chain starting at
 *      direntry.anode; each anode gives (blocknr, clustersize,
 *      next), read clustersize 512-byte sectors from blocknr,
 *      recurse into .next until zero.
 *
 *  Simplifications for this reader:
 *    - Only 512-byte reserved blocks AND 512-byte data blocks
 *      (reserved_blksize = 512, disk sector = 512). PFS3 allows
 *      1K/2K/4K reserved and larger data; we probe root.blocksize
 *      style fields and reject mismatches rather than silently
 *      reading wrong offsets.
 *    - Small-disk variant only (rootblock.idx.small path).
 *    - Softlinks (type ST_SOFTLINK=3) and hardlinks
 *      (ST_LINKFILE=-4) are not followed; treated as E_NO_FILE
 *      on FS_LOAD.
 *    - DelDir entries are ignored (they're already logically
 *      deleted; our reader never advertises them).
 */

#include "defs.h"
#include "fs.h"

#define PFS_BSIZE           512u

/* --- DosType values --------------------------------------------- */
#define PFS_DT_PFS1         0x50465301u   /* 'PFS\1' */
#define PFS_DT_PFS2         0x50465302u   /* 'PFS\2' (AOS4 era) */
#define PFS_DT_AFS1         0x41465301u   /* 'AFS\1' (identical layout) */

/* --- Block IDs (big-endian 16-bit, stored at block offset 0) ---- */
#define PFS_ID_DB           0x4442u       /* 'DB' -- directory block */
#define PFS_ID_AB           0x4142u       /* 'AB' -- anode block */
#define PFS_ID_IB           0x4942u       /* 'IB' -- anode index block */
#define PFS_ID_BM           0x424Du       /* 'BM' -- bitmap block */
#define PFS_ID_MI           0x4D49u       /* 'MI' -- bitmap index block */

/* --- rootblock field offsets ------------------------------------ *
 * Packed to 2-byte alignment per pfs3aio's #pragma pack(2). */
#define RB_DISKTYPE         0
#define RB_OPTIONS          4
#define RB_DATESTAMP        8
#define RB_CREATIONDAY      12
#define RB_CREATIONMIN      14
#define RB_CREATIONTICK     16
#define RB_PROTECTION       18
#define RB_DISKNAME         20      /* Pascal string, 32 bytes */
#define RB_LASTRESERVED     52
#define RB_FIRSTRESERVED    56
#define RB_RESERVED_FREE    60
#define RB_RESERVED_BLKSIZE 64      /* UWORD */
#define RB_RBLKCLUSTER      66      /* UWORD */
#define RB_BLOCKSFREE       68
#define RB_ALWAYSFREE       72
#define RB_ROVING_PTR       76
#define RB_DELDIR           80
#define RB_DISKSIZE         84
#define RB_EXTENSION        88
#define RB_NOT_USED         92
/* idx.small union begins at +96:
 *   bitmapindex[5]     +96..+115
 *   indexblocks[99]    +116..+511  (small-disk variant) */
#define RB_IDX_SMALL        96
#define RB_SMALL_BITMAPIDX  96      /* 5 BLCKs = 20 bytes */
#define RB_SMALL_INDEXBLOCKS 116    /* 99 BLCKs = 396 bytes */
#define SMALL_NUM_INDEXBLOCKS 99

/* Option bits we recognise */
#define PFS_MODE_HARDDISK       0x0001
#define PFS_MODE_SPLITTED_ANODES 0x0002
#define PFS_MODE_DIR_EXTENSION  0x0004
#define PFS_MODE_DELDIR         0x0008
#define PFS_MODE_SIZEFIELD      0x0010
#define PFS_MODE_EXTENSION      0x0020
#define PFS_MODE_DATESTAMP      0x0040
#define PFS_MODE_SUPERINDEX     0x0080   /* we require this be clear */
#define PFS_MODE_LARGEFILE      0x0800

/* --- anode / indexblock / dirblock offsets --------------------- */
/* indexblock: +0 UWORD id, +2 UWORD not_used, +4 ULONG datestamp,
 *             +8 ULONG seqnr, +12 LONG index[0] */
#define IB_INDEX_BASE       12

/* anodeblock: +0 UWORD id, +2 UWORD not_used, +4 ULONG datestamp,
 *             +8 ULONG seqnr, +12 ULONG not_used_2, +16 anode[0] */
#define AB_NODES_BASE       16
#define ANODE_SIZE          12      /* clustersize(4) + blocknr(4) + next(4) */
#define ANODE_CLUSTERSIZE   0
#define ANODE_BLOCKNR       4
#define ANODE_NEXT          8

/* dirblock: +0 UWORD id, +2 UWORD not_used, +4 ULONG datestamp,
 *           +8 UWORD not_used_2[2], +12 ULONG anodenr, +16 ULONG parent,
 *           +20 direntry[0] */
#define DB_ENTRIES_BASE     20

/* direntry offsets (all at file-offset-relative from direntry start) */
#define DE_NEXT             0       /* UBYTE, total record size */
#define DE_TYPE             1       /* BYTE */
#define DE_ANODE            2       /* ULONG */
#define DE_FSIZE            6       /* ULONG */
#define DE_CREATIONDAY      10
#define DE_CREATIONMIN      12
#define DE_CREATIONTICK     14
#define DE_PROTECTION       16      /* UBYTE */
#define DE_NLENGTH          17      /* UBYTE */
#define DE_STARTOFNAME      18      /* first byte of name */
#define DE_HEADER_SIZE      18

/* direntry type bytes (ST_* constants from Amiga DOS) */
#define ST_USERDIR          2
#define ST_SOFTLINK         3
#define ST_LINKDIR          4
#define ST_FILE_BYTE        0xFD    /* (BYTE)-3 */
#define ST_LINKFILE_BYTE    0xFC    /* (BYTE)-4 */

/* Predefined anode numbers */
#define ANODE_EOF           0
#define ANODE_ROOTDIR       5
#define ANODE_USERFIRST     6

/* --- Per-volume cached state ------------------------------------ */
static struct {
	Instance *disk;
	uLong     part_loc;
	uInt      dostype;           /* low byte */
	uInt      reserved_blksize;  /* bytes */
	uInt      disksize;          /* blocks */
	uInt      firstreserved;
	uInt      lastreserved;
	uInt      anodes_per_block;
	uInt      entries_per_ib;
	/* Up to SMALL_NUM_INDEXBLOCKS (99) BLCKs addressing indexblocks. */
	uInt      indexblocks[SMALL_NUM_INDEXBLOCKS];
} g_pfs;

/* --- Byte/word/longword big-endian readers --------------------- */
static uInt
be32(const uByte *p)
{
	return ((uInt)p[0] << 24) | ((uInt)p[1] << 16) |
	       ((uInt)p[2] << 8)  | (uInt)p[3];
}

static uInt
be16(const uByte *p)
{
	return ((uInt)p[0] << 8) | (uInt)p[1];
}

/* --- Block I/O helper ------------------------------------------- */
static Retcode
read_block(Environ *e, Instance *disk, uLong part_loc, uInt block_num,
	   uByte *buf)
{
	return filesys_read_bytes(e, disk,
		part_loc + (uLong)block_num * PFS_BSIZE, PFS_BSIZE, buf);
}

/* --- Case-folded byte equality (Intl-style, always enabled for
 * PFS3 which is AmigaDOS-case-insensitive by default) ------------ */
static uByte
pfs_fold(uByte c)
{
	if (c >= 'a' && c <= 'z') return c - 0x20;
	if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
	return c;
}

static int
pfs_name_eq(const uByte *a, uInt alen, const uByte *b, uInt blen)
{
	if (alen != blen) return 0;
	for (uInt i = 0; i < alen; i++)
		if (pfs_fold(a[i]) != pfs_fold(b[i])) return 0;
	return 1;
}

/* --- Open volume: parse rootblock at partition block 2 ---------- */
static Retcode
pfs_open_volume(Environ *e, Instance *disk, uLong part_loc,
		uInt dostype, uByte *buf)
{
	Retcode r = read_block(e, disk, part_loc, 2, buf);
	if (r != NO_ERROR)
		return r;

	uInt dt        = be32(buf + RB_DISKTYPE);
	uInt options   = be32(buf + RB_OPTIONS);
	uInt res_bsize = be16(buf + RB_RESERVED_BLKSIZE);
	uInt disksize  = be32(buf + RB_DISKSIZE);
	uInt firstres  = be32(buf + RB_FIRSTRESERVED);
	uInt lastres   = be32(buf + RB_LASTRESERVED);

	/* The rootblock's disktype must agree with the boot block's. */
	if (dt != dostype)
		return E_NO_FILESYS;

	/* Small-disk layout requires the SuperIndex bit to be clear.
	 * Large-disk / SuperIndex support would read idx.large.bitmapindex
	 * and the rootblock-extension block for the superindex BLCKs,
	 * which this reader doesn't implement. */
	if (options & PFS_MODE_SUPERINDEX)
		return E_NO_FILESYS;

	/* We only support 512-byte reserved blocks. */
	if (res_bsize != PFS_BSIZE)
		return E_NO_FILESYS;

	if (disksize == 0 || firstres == 0 || lastres < firstres)
		return E_NO_FILESYS;

	g_pfs.disk              = disk;
	g_pfs.part_loc          = part_loc;
	g_pfs.dostype           = dostype & 0xFF;
	g_pfs.reserved_blksize  = res_bsize;
	g_pfs.disksize          = disksize;
	g_pfs.firstreserved     = firstres;
	g_pfs.lastreserved      = lastres;
	/* Per-block capacity derived from reserved_blksize. */
	g_pfs.anodes_per_block  = (res_bsize - AB_NODES_BASE) / ANODE_SIZE;
	g_pfs.entries_per_ib    = (res_bsize - IB_INDEX_BASE) / 4;

	/* Cache the small-variant index-block pointers. */
	for (uInt i = 0; i < SMALL_NUM_INDEXBLOCKS; i++)
		g_pfs.indexblocks[i] =
		    be32(buf + RB_SMALL_INDEXBLOCKS + i * 4);

	return NO_ERROR;
}

/* --- Anode lookup: given an anode number, read the containing
 * anodeblock and fill out_cluster/out_blocknr/out_next. Returns
 * E_READ_ERROR on corruption / out-of-range anodes. */
static Retcode
pfs_get_anode(Environ *e, uInt anodenr, uInt *out_cluster,
	      uInt *out_blocknr, uInt *out_next, uByte *scratch)
{
	uInt apb = g_pfs.anodes_per_block;
	uInt epi = g_pfs.entries_per_ib;
	if (apb == 0 || epi == 0)
		return E_READ_ERROR;

	uInt ab_idx   = anodenr / apb;       /* which anodeblock */
	uInt ab_off   = anodenr % apb;       /* which anode within it */
	uInt ib_idx   = ab_idx  / epi;       /* which indexblock */
	uInt ib_sub   = ab_idx  % epi;       /* offset within that indexblock */

	if (ib_idx >= SMALL_NUM_INDEXBLOCKS)
		return E_READ_ERROR;

	uInt ib_blk = g_pfs.indexblocks[ib_idx];
	if (ib_blk == 0)
		return E_READ_ERROR;

	Retcode r = read_block(e, g_pfs.disk, g_pfs.part_loc, ib_blk, scratch);
	if (r != NO_ERROR)
		return r;
	if (be16(scratch) != PFS_ID_IB)
		return E_READ_ERROR;

	uInt ab_blk = be32(scratch + IB_INDEX_BASE + ib_sub * 4);
	if (ab_blk == 0)
		return E_READ_ERROR;

	r = read_block(e, g_pfs.disk, g_pfs.part_loc, ab_blk, scratch);
	if (r != NO_ERROR)
		return r;
	if (be16(scratch) != PFS_ID_AB)
		return E_READ_ERROR;

	const uByte *a = scratch + AB_NODES_BASE + ab_off * ANODE_SIZE;
	*out_cluster = be32(a + ANODE_CLUSTERSIZE);
	*out_blocknr = be32(a + ANODE_BLOCKNR);
	*out_next    = be32(a + ANODE_NEXT);
	return NO_ERROR;
}

/* --- Scan one directory block for a matching name. If found,
 * fills out_type + out_anode + out_size and returns NO_ERROR.
 * Returns E_NO_FILE if scanned to the end of the block without a
 * match; caller should advance along the dir anode chain. */
static Retcode
pfs_scan_dirblock(const uByte *dblk, const uByte *name, uInt namelen,
		  uByte *out_type, uInt *out_anode, uInt *out_size)
{
	if (be16(dblk) != PFS_ID_DB)
		return E_READ_ERROR;

	uInt off = DB_ENTRIES_BASE;
	while (off + DE_HEADER_SIZE < PFS_BSIZE) {
		uByte next_size = dblk[off + DE_NEXT];
		if (next_size == 0)
			break;                 /* end-of-entries sentinel */
		if (next_size < DE_HEADER_SIZE)
			break;                 /* corruption */
		if (off + next_size > PFS_BSIZE)
			break;                 /* ditto */

		const uByte *de = dblk + off;
		uByte nlength = de[DE_NLENGTH];
		if (nlength > 0 && off + DE_STARTOFNAME + nlength <= PFS_BSIZE) {
			const uByte *nm = de + DE_STARTOFNAME;
			if (pfs_name_eq(nm, nlength, name, namelen)) {
				*out_type  = de[DE_TYPE];
				*out_anode = be32(de + DE_ANODE);
				*out_size  = be32(de + DE_FSIZE);
				return NO_ERROR;
			}
		}

		off += next_size;
	}
	return E_NO_FILE;
}

/* --- Scan a directory. anodenr is the directory's anode number.
 * Walks the anode chain: for each anode in the chain, read
 * `clustersize` consecutive reserved blocks starting at `blocknr`
 * (each of which is a dirblock) and pfs_scan_dirblock each one. */
static Retcode
pfs_dir_lookup(Environ *e, uInt dir_anode, const uByte *name, uInt namelen,
	       uByte *out_type, uInt *out_anode, uInt *out_size,
	       uByte *scratch)
{
	uInt anr = dir_anode;
	int  guard = 0;

	while (anr != ANODE_EOF && guard++ < 4096) {
		uInt cluster = 0, blocknr = 0, next_anr = 0;
		Retcode r = pfs_get_anode(e, anr, &cluster, &blocknr,
					  &next_anr, scratch);
		if (r != NO_ERROR)
			return r;
		if (blocknr == 0 || cluster == 0)
			return E_NO_FILE;

		for (uInt i = 0; i < cluster; i++) {
			uByte dblk[PFS_BSIZE];
			r = read_block(e, g_pfs.disk, g_pfs.part_loc,
				       blocknr + i, dblk);
			if (r != NO_ERROR)
				return r;
			r = pfs_scan_dirblock(dblk, name, namelen,
					      out_type, out_anode, out_size);
			if (r == NO_ERROR)
				return NO_ERROR;
			if (r != E_NO_FILE)
				return r;
		}

		anr = next_anr;
	}
	return E_NO_FILE;
}

/* --- Walk '/'-separated path from root. Returns type, anode, size
 * of the last resolved component. */
static Retcode
pfs_walk_path(Environ *e, const Byte *path, uByte *out_type,
	      uInt *out_anode, uInt *out_size, uByte *scratch)
{
	while (*path == '/' || *path == '\\')
		path++;

	if (*path == '\0') {
		*out_type  = ST_USERDIR;
		*out_anode = ANODE_ROOTDIR;
		*out_size  = 0;
		return NO_ERROR;
	}

	uInt cur_anode = ANODE_ROOTDIR;
	const Byte *cur = path;

	while (*cur) {
		const Byte *sep = cur;
		while (*sep && *sep != '/' && *sep != '\\') sep++;
		uInt complen = (uInt)(sep - cur);
		if (complen == 0) { cur = sep + 1; continue; }

		uByte typ = 0;
		uInt  an = 0, sz = 0;
		Retcode r = pfs_dir_lookup(e, cur_anode,
			(const uByte *)cur, complen, &typ, &an, &sz, scratch);
		if (r != NO_ERROR)
			return r;

		if (*sep == '\0') {
			*out_type  = typ;
			*out_anode = an;
			*out_size  = sz;
			return NO_ERROR;
		}

		if (typ != ST_USERDIR)
			return E_NO_FILE;
		cur_anode = an;
		cur = sep + 1;
	}

	*out_type  = ST_USERDIR;
	*out_anode = cur_anode;
	*out_size  = 0;
	return NO_ERROR;
}

/* --- Read file contents by walking the file's anode chain. Copies
 * up to `byte_size` bytes into `out`. */
static Retcode
pfs_read_file(Environ *e, uInt file_anode, uInt byte_size, uByte *out,
	      uLong *out_len, uByte *scratch)
{
	uInt written = 0;
	uInt anr     = file_anode;
	int  guard   = 0;

	while (written < byte_size && anr != ANODE_EOF) {
		if (++guard > 65536)
			return E_READ_ERROR;

		uInt cluster = 0, blocknr = 0, next_anr = 0;
		Retcode r = pfs_get_anode(e, anr, &cluster, &blocknr,
					  &next_anr, scratch);
		if (r != NO_ERROR)
			return r;
		if (cluster == 0)
			break;

		for (uInt i = 0; i < cluster && written < byte_size; i++) {
			uByte dblk[PFS_BSIZE];
			r = read_block(e, g_pfs.disk, g_pfs.part_loc,
				       blocknr + i, dblk);
			if (r != NO_ERROR)
				return r;
			uInt remain = byte_size - written;
			uInt copy   = (remain < PFS_BSIZE) ? remain : PFS_BSIZE;
			memcpy(out + written, dblk, copy);
			written += copy;
		}

		anr = next_anr;
	}

	*out_len = written;
	return NO_ERROR;
}

/* --- List a directory's entries ---------------------------------- */
static void
pfs_list_dir(Environ *e, uInt dir_anode, uByte *scratch)
{
	uInt anr = dir_anode;
	int  guard = 0;

	while (anr != ANODE_EOF && guard++ < 4096) {
		uInt cluster = 0, blocknr = 0, next_anr = 0;
		Retcode r = pfs_get_anode(e, anr, &cluster, &blocknr,
					  &next_anr, scratch);
		if (r != NO_ERROR)
			return;
		if (blocknr == 0 || cluster == 0)
			return;

		for (uInt i = 0; i < cluster; i++) {
			uByte dblk[PFS_BSIZE];
			r = read_block(e, g_pfs.disk, g_pfs.part_loc,
				       blocknr + i, dblk);
			if (r != NO_ERROR) return;
			if (be16(dblk) != PFS_ID_DB) continue;

			uInt off = DB_ENTRIES_BASE;
			while (off + DE_HEADER_SIZE < PFS_BSIZE) {
				uByte nsz = dblk[off + DE_NEXT];
				if (nsz == 0) break;
				if (nsz < DE_HEADER_SIZE) break;
				if (off + nsz > PFS_BSIZE) break;

				const uByte *de = dblk + off;
				uByte typ  = de[DE_TYPE];
				uInt  sz   = be32(de + DE_FSIZE);
				uByte nlen = de[DE_NLENGTH];
				const uByte *nm = de + DE_STARTOFNAME;
				if (off + DE_STARTOFNAME + nlen > PFS_BSIZE)
					break;

				if (typ == ST_USERDIR) {
					cprintf(e, "          [%S]\n",
						nm, (Int)nlen);
				} else if (typ == ST_SOFTLINK ||
					   typ == ST_LINKDIR) {
					cprintf(e, "     link  %S\n",
						nm, (Int)nlen);
				} else if (typ == ST_FILE_BYTE) {
					cprintf(e, "%8d  %S\n",
						(int)sz, nm, (Int)nlen);
				} else {
					cprintf(e, "    type%d  %S\n",
						(int)(signed char)typ,
						nm, (Int)nlen);
				}

				off += nsz;
			}
		}
		anr = next_anr;
	}
}

/* --- Top-level dispatcher --------------------------------------- */
Retcode
amiga_pfs3(Environ *e, Filesys_action what, Instance *disk,
	   Byte *path, uLong loc, uLong start_unused, uByte *buf,
	   uInt size, uByte *retbuf, uLong *val)
{
	(void)start_unused;
	if (size < PFS_BSIZE) return E_BLOCKSIZE;

	/* Identify via boot block. PFS stores the DosType at the
	 * first 4 bytes of block 0. */
	uByte bootbuf[PFS_BSIZE];
	Retcode r = read_block(e, disk, loc, 0, bootbuf);
	if (r != NO_ERROR)
		return E_NO_FILESYS;
	uInt dt = be32(bootbuf);
	if (dt != PFS_DT_PFS1 && dt != PFS_DT_PFS2 && dt != PFS_DT_AFS1)
		return E_NO_FILESYS;

	if (pfs_open_volume(e, disk, loc, dt, buf) != NO_ERROR)
		return E_NO_FILESYS;

	switch (what) {
	case FS_PROBE:
		strcat((char *)retbuf, ",amiga-pfs3");
		*val = loc;
		return R_END;

	case FS_LIST: {
		uByte scratch[PFS_BSIZE];
		uByte typ = 0;
		uInt  an = 0, sz = 0;
		Retcode ret = pfs_walk_path(e, path, &typ, &an, &sz, scratch);
		if (ret != NO_ERROR)
			return E_NO_FILE;

		if (typ == ST_USERDIR) {
			cprintf(e, "PFS%s volume:\n",
				(g_pfs.dostype == 2) ? "\\2" : "\\1");
			pfs_list_dir(e, an, scratch);
		} else {
			cprintf(e, "%8d  %s\n", (int)sz, (char *)path);
		}
		return R_END;
	}

	case FS_LOAD: {
		uByte scratch[PFS_BSIZE];
		uByte typ = 0;
		uInt  an = 0, sz = 0;
		Retcode ret = pfs_walk_path(e, path, &typ, &an, &sz, scratch);
		if (ret != NO_ERROR)
			return E_NO_FILE;

		if (typ == ST_USERDIR || typ == ST_SOFTLINK ||
		    typ == ST_LINKDIR || typ == ST_LINKFILE_BYTE)
			return E_NO_FILE;

		uLong got = 0;
		ret = pfs_read_file(e, an, sz, retbuf, &got, scratch);
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

Filesys g_amiga_pfs3 = {
	"amiga-pfs3",
	amiga_pfs3,
};
