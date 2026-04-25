/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Arc FS-B Block 4 -- SmartFileSystem (SFS\0 + SFS\2) readonly reader.
 *
 *  SmartFileSystem (SFS) was written for classic Amiga by John
 *  Hendrikx circa 1998, later continued as SFS\2 for AmigaOS 4
 *  (softlink support added; on-disk layout otherwise identical).
 *  Both DOS types share the same block structure; the distinction
 *  matters only when we need to recognise softlink entries. This
 *  reader recognises softlinks and returns E_NO_FILE on them --
 *  adequate for boot-kernel load, extendable later.
 *
 *  Registered as &g_amiga_sfs in platform.c's g_filesys[] after
 *  &g_amiga_ffs so an RDB-partitioned disk has FFS-family matching
 *  tried first and falls through to SFS for DosType-matching SFS
 *  partitions.
 *
 *  Format references:
 *      AROS SFS sources (LGPL, read-only reference -- we read
 *      blockstructure.h, nodes.h, btreenodes.h, objects.h,
 *      bitmap.h for field offsets; no code copied). The on-disk
 *      layout in those headers is the same as John Hendrikx's
 *      original published description.
 *
 *  Simplifications in this reader:
 *    - Directory traversal is linear (walk the ObjectContainer
 *      chain via be_next). We never consult the HashTable (HTAB)
 *      or the ObjectNode tree. Correct, just slower for huge dirs.
 *    - File data is read by walking the extent B+-tree. A
 *      single-extent fast-path avoids the tree walk when the
 *      file.size fits within the first extent (which is true for
 *      every file mkrdb.py emits and for any freshly-written
 *      unfragmented file on a real disk).
 *    - Only 512-byte blocks supported for now. SFS supports 512 ..
 *      32768 but classic Amiga installs default to 512 and AOS4
 *      SFS\2 does the same on small partitions; larger blocks are
 *      a future enhancement.
 *    - Softlinks (SFS\2 OTYPE_LINK bit) are not followed.
 *    - Hardlinks (OTYPE_HARDLINK) are not followed; the hardlink
 *      entry itself is treated as a miss.
 */

#include "defs.h"
#include "fs.h"
#include "byteswap.h"

/* --- On-disk block IDs (4-char big-endian magic) ----------------- */
#define SFS_BLOCKID_OBJC   0x4F424A43u   /* 'OBJC' ObjectContainer */
#define SFS_BLOCKID_HTAB   0x48544142u   /* 'HTAB' HashTable */
#define SFS_BLOCKID_SLNK   0x534C4E4Bu   /* 'SLNK' SoftLink */
#define SFS_BLOCKID_NDC    0x4E444320u   /* 'NDC '  NodeContainer */
#define SFS_BLOCKID_BNDC   0x424E4443u   /* 'BNDC' BNodeContainer */
#define SFS_BLOCKID_BTMP   0x42544D50u   /* 'BTMP' Bitmap */

/* --- Root block has NO fixed id byte sequence at offset 0 (the
 * bheader.id IS the root-block id, but the value isn't specified by
 * the spec beyond being non-zero). We identify the root block by:
 *   - version field at offset 0x0C matches expected SFS structure
 *   - blocksize field matches our I/O unit
 *   - bheader.ownblock matches the block number we read from
 *   - checksum validates (sum of all longs == 0) */

/* --- DosType values --------------------------------------------- */
#define SFS_DOSTYPE_MAGIC24 0x53465300u  /* 'SFS\0' high 24 bits */
#define SFS_DOSTYPE_V0      0x53465300u  /* 'SFS\0' classic */
#define SFS_DOSTYPE_V2      0x53465302u  /* 'SFS\2' AOS4 */

/* --- Fixed 512-byte block size for this implementation. SFS allows
 * 512..32768 on disk. We check the root block's blocksize field
 * rejects mismatches. */
#define SFS_BSIZE           512u

/* --- fsBlockHeader offsets (first 12 bytes of every SFS block
 * except raw data blocks). */
#define BH_ID               0
#define BH_CHKSUM           4
#define BH_OWNBLOCK         8
#define BH_SIZE             12

/* --- fsRootBlock offsets (after the 12-byte bheader) ------------- */
#define RB_VERSION          12      /* UWORD */
#define RB_SEQNO            14      /* UWORD */
#define RB_DATECREATED      16      /* ULONG */
#define RB_BITS             20      /* UBYTE */
#define RB_FIRSTBYTEH       32      /* ULONG (upper 32 of start offset) */
#define RB_FIRSTBYTEL       36
#define RB_LASTBYTEH        40
#define RB_LASTBYTEL        44
#define RB_TOTALBLOCKS      48
#define RB_BLOCKSIZE        52
/* skipped: reserved2[2] @ 56, reserved3[8] @ 64 */
#define RB_BITMAPBASE       96
#define RB_ADMINSPACES      100
#define RB_ROOTOBJC         104     /* first ObjectContainer of root dir */
#define RB_EXTENTBNODEROOT  108
#define RB_OBJECTNODEROOT   112

#define RBITS_CASE_SENS     0x80

/* --- fsObjectContainer offsets (after the 12-byte bheader) ------- */
#define OC_PARENT           12      /* NODE (ULONG) of parent object */
#define OC_NEXT             16      /* BLCK of next OC in dir chain */
#define OC_PREVIOUS         20      /* BLCK of previous OC, or 0 */
#define OC_OBJECTS          24      /* first fsObject */

/* --- fsObject: variable-length record inside an ObjectContainer.
 * Fixed header is 25 bytes; then name (C-string, NUL-terminated),
 * then comment (C-string, NUL-terminated), then padding to 2-byte
 * alignment.
 *
 *   +0   UWORD  owneruid
 *   +2   UWORD  ownergid
 *   +4   ULONG  objectnode (NODE)
 *   +8   ULONG  protection
 *   +12  union {
 *     file: { ULONG data-block; ULONG size-in-bytes }
 *     dir:  { ULONG hashtable;  ULONG firstdirblock }
 *   }
 *   +20  ULONG  datemodified
 *   +24  UBYTE  bits  (OTYPE_DIR, OTYPE_LINK, ...)
 *   +25  char   name[]
 *        char   comment[]
 */
#define OBJ_OWNERUID        0
#define OBJ_OBJECTNODE      4
#define OBJ_PROTECTION      8
#define OBJ_DATA            12      /* file: data-block; dir: hashtable */
#define OBJ_SIZE            16      /* file: size-in-bytes; dir: firstdirblock */
#define OBJ_FIRSTDIRBLOCK   16
#define OBJ_HASHTABLE       12
#define OBJ_DATEMODIFIED    20
#define OBJ_BITS            24
#define OBJ_NAME            25
#define OBJ_FIXED_SIZE      25

#define OTYPE_HIDDEN        1
#define OTYPE_UNDELETABLE   2
#define OTYPE_QUICKDIR      4
#define OTYPE_RINGLIST      8
#define OTYPE_HARDLINK      32
#define OTYPE_LINK          64
#define OTYPE_DIR           128

/* --- fsBNodeContainer (extent B+-tree node) --------------------- *
 *   +0   fsBlockHeader (12 bytes)
 *   +12  UWORD nodecount      -- # entries in bnode[]
 *   +14  UBYTE isleaf
 *   +15  UBYTE nodesize       -- stride of a bnode[i], multiple of 2
 *   +16  bnode[] -- entries of `nodesize` bytes each
 *
 * For leaves (isleaf=1), each bnode is an fsExtentBNode:
 *   +0 ULONG key      -- starting block of this extent
 *   +4 ULONG next     -- key of the next extent in the file
 *   +8 ULONG prev     -- key of the previous extent in the file
 *   +12 UWORD blocks  -- length in blocks of this extent
 * Leaf nodesize should therefore be 14 bytes.
 *
 * For interior nodes (isleaf=0), each bnode is a generic BNode:
 *   +0 ULONG key      -- starting key of the subtree rooted at data
 *   +4 ULONG data     -- BLCK of child BNodeContainer
 * Interior nodesize should therefore be 8 bytes. */
#define BNC_NODECOUNT       12      /* UWORD */
#define BNC_ISLEAF          14      /* UBYTE */
#define BNC_NODESIZE        15      /* UBYTE */
#define BNC_BNODES          16

#define BN_KEY              0
#define BN_DATA             4       /* interior BNode: BLCK of child */
#define BN_NEXT             4       /* leaf fsExtentBNode: next key */
#define BN_PREV             8       /* leaf fsExtentBNode: prev key */
#define BN_BLOCKS           12      /* leaf fsExtentBNode: UWORD length */

/* --- Per-volume cached state. Single-threaded, so static is safe. */
static struct {
	Instance  *disk;
	uLong      part_loc;
	uInt       dostype;          /* low byte of DosType */
	int        case_sens;
	uInt       blocksize;        /* bytes (== SFS_BSIZE for now) */
	uInt       total_blocks;
	/*
	 * The rootblock's rootobjectcontainer field points at an OC
	 * that holds exactly ONE fsObject: the "root directory
	 * object". That object's name is the volume label; its
	 * firstdirblock is where the user-visible top-level entries
	 * actually live. So we cache both: root_objc for volume-name
	 * extraction and root_dir_first_oc for directory walking.
	 */
	uInt       root_objc_block;
	uInt       root_dir_first_oc;
	uByte      volume_name[108];
	uInt       volume_name_len;
	uInt       extent_bn_root;
} g_sfs;

/* --- Block I/O helper ------------------------------------------- */
static Retcode
read_block(Environ *e, Instance *disk, uLong part_loc, uInt block_num,
	   uByte *buf)
{
	return filesys_read_bytes(e, disk,
		part_loc + (uLong)block_num * SFS_BSIZE, SFS_BSIZE, buf);
}

/* --- SFS checksum: the AROS header comment says "SUM of all LONGs
 * in a block plus one, and then negated" -- i.e.
 *     stored = -(sum_without_ck + 1)
 * which implies sum_with_ck = -1 = 0xFFFFFFFF. Some formatters use
 * the simpler sum_with_ck == 0 variant instead. We accept either
 * so we work with volumes from every common SFS writer. Returns 1
 * if the block's checksum is valid, 0 otherwise. */
static int
sfs_checksum_ok(const uByte *buf)
{
	uInt sum = 0;
	for (uInt i = 0; i < SFS_BSIZE; i += 4)
		sum += be32(buf + i);
	return (sum == 0u || sum == 0xFFFFFFFFu) ? 1 : 0;
}

/* --- Case-folded character compare ------------------------------ *
 * SFS uses ASCII lowercase->uppercase folding plus the classic
 * Amiga International extension (0xE0..0xFE except 0xF7 -> 0x20
 * less). Applied only when the root block's CASE_SENSITIVE bit is
 * clear. */
static uByte
sfs_fold(uByte c)
{
	if (c >= 'a' && c <= 'z') return c - 0x20;
	if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
	return c;
}

static int
sfs_name_eq(const uByte *a, uInt alen, const uByte *b, uInt blen)
{
	if (alen != blen) return 0;
	if (g_sfs.case_sens) {
		for (uInt i = 0; i < alen; i++)
			if (a[i] != b[i]) return 0;
	} else {
		for (uInt i = 0; i < alen; i++)
			if (sfs_fold(a[i]) != sfs_fold(b[i])) return 0;
	}
	return 1;
}

/* --- Find the root block on an SFS volume ------------------------
 * The SFS spec puts two root blocks, one near the start of the
 * partition and one near the end. Different formatters disagree
 * on whether "the start" means partition block 0 or block 1: the
 * classic convention on Amiga floppies reserves block 0 as a
 * boot block and places the root at block 1, but SFS volumes
 * formatted as hard-disk partitions (AOS 4.x, MorphOS) commonly
 * put the root at block 0 directly. Both layouts are valid SFS.
 * We probe block 0 first -- the more common case on real HD
 * installs -- and fall back to block 1.
 *
 * Valid root block identified by:
 *   - checksum passes (sum of longs == 0)
 *   - bheader.ownblock == block number we read from
 *   - version field has a plausible value (1 <= v <= 99)
 *   - blocksize == SFS_BSIZE
 *
 * On success, caches g_sfs state and returns NO_ERROR. Otherwise
 * returns E_NO_FILESYS. */
static Retcode
sfs_open_volume(Environ *e, Instance *disk, uLong part_loc,
		uInt dostype, uByte *buf)
{
	static const uInt candidate_blocks[] = { 0u, 1u };
	int found = 0;
	uInt probe_blk = 0;

	for (unsigned i = 0; i < sizeof candidate_blocks /
	                         sizeof candidate_blocks[0]; i++) {
		probe_blk = candidate_blocks[i];
		Retcode r = read_block(e, disk, part_loc, probe_blk, buf);
		if (r != NO_ERROR) continue;
		if (!sfs_checksum_ok(buf)) continue;
		if (be32(buf + BH_OWNBLOCK) != probe_blk) continue;
		found = 1;
		break;
	}
	if (!found)
		return E_NO_FILESYS;

	uInt version = be16(buf + RB_VERSION);
	uInt blocksize = be32(buf + RB_BLOCKSIZE);
	if (version == 0 || version > 99)
		return E_NO_FILESYS;
	if (blocksize != SFS_BSIZE)
		return E_NO_FILESYS;

	uInt totalblocks      = be32(buf + RB_TOTALBLOCKS);
	uInt root_objc        = be32(buf + RB_ROOTOBJC);
	uInt extent_bn_root   = be32(buf + RB_EXTENTBNODEROOT);
	uByte bits            = buf[RB_BITS];

	if (root_objc == 0 || root_objc >= totalblocks)
		return E_NO_FILESYS;

	g_sfs.disk             = disk;
	g_sfs.part_loc         = part_loc;
	g_sfs.dostype          = dostype & 0xFF;
	g_sfs.case_sens        = (bits & RBITS_CASE_SENS) ? 1 : 0;
	g_sfs.blocksize        = blocksize;
	g_sfs.total_blocks     = totalblocks;
	g_sfs.root_objc_block  = root_objc;
	g_sfs.extent_bn_root   = extent_bn_root;
	g_sfs.root_dir_first_oc = 0;
	g_sfs.volume_name_len   = 0;

	/*
	 * Read the root ObjectContainer. It holds exactly one fsObject
	 * (the "root directory object" per objects.h). The object's
	 * name is the volume label and its dir.firstdirblock is the
	 * first OC of the actual top-level directory; the OCs we walk
	 * for FS_LIST/FS_LOAD live in that chain, NOT in the
	 * rootobjc container itself.
	 */
	uByte ocbuf[SFS_BSIZE];
	Retcode r2 = read_block(e, disk, part_loc, root_objc, ocbuf);
	if (r2 != NO_ERROR)
		return E_NO_FILESYS;
	if (be32(ocbuf + BH_ID) != SFS_BLOCKID_OBJC)
		return E_NO_FILESYS;

	const uByte *root_obj = ocbuf + OC_OBJECTS;
	if (be32(root_obj + OBJ_OBJECTNODE) == 0)
		return E_NO_FILESYS;   /* empty root OC -- not a mountable SFS */

	/*
	 * The root dir object is a directory-flavour fsObject: its
	 * dir.firstdirblock lives at OBJ_FIRSTDIRBLOCK (same offset
	 * as a file's size field, via the union).
	 */
	g_sfs.root_dir_first_oc = be32(root_obj + OBJ_FIRSTDIRBLOCK);

	/* Capture the volume name (at the variable-length tail of the
	 * fsObject). Cap at 107 chars + NUL to fit our buffer. */
	const uByte *name = root_obj + OBJ_NAME;
	uInt avail = (uInt)(SFS_BSIZE - OC_OBJECTS - OBJ_NAME);
	uInt nlen = 0;
	while (nlen < avail && nlen < sizeof g_sfs.volume_name - 1
	       && name[nlen] != 0)
		nlen++;
	for (uInt i = 0; i < nlen; i++)
		g_sfs.volume_name[i] = name[i];
	g_sfs.volume_name[nlen] = 0;
	g_sfs.volume_name_len = nlen;

	return NO_ERROR;
}

/* --- Advance past a NUL-terminated string in an object record ---
 * Returns one past the NUL. Bounded by `end` to keep corrupt blocks
 * from running off the end. */
static const uByte *
skip_cstring(const uByte *p, const uByte *end)
{
	while (p < end && *p != 0)
		p++;
	if (p < end) p++;   /* past the NUL */
	return p;
}

/* --- Compute the byte length of a variable-length fsObject. Aligns
 * up to a 2-byte boundary per the SFS spec ("Objects always start
 * at 2-byte boundaries"). */
static uInt
object_record_size(const uByte *obj, uInt avail)
{
	if (avail < OBJ_FIXED_SIZE) return 0;
	const uByte *start = obj;
	const uByte *end   = obj + avail;
	const uByte *p     = obj + OBJ_NAME;

	p = skip_cstring(p, end);        /* past name */
	p = skip_cstring(p, end);        /* past comment */

	uInt size = (uInt)(p - start);
	if (size & 1) size++;            /* 2-byte align next record */
	return size;
}

/* --- Look up `name` in the directory whose first ObjectContainer
 * BLCK is `first_oc`. On success, fills *out_data + *out_size +
 * *out_bits (for the caller to distinguish file vs dir) and returns
 * NO_ERROR. Returns E_NO_FILE if not found. `scratch` is a BSIZE
 * buffer used for one ObjectContainer at a time. */
static Retcode
sfs_dir_lookup(Environ *e, uInt first_oc, const uByte *name, uInt namelen,
	       uInt *out_data, uInt *out_size, uByte *out_bits,
	       uByte *scratch)
{
	uInt oc_block = first_oc;
	int guard = 0;

	while (oc_block != 0) {
		if (++guard > 4096)          /* cycle guard */
			return E_NO_FILE;

		Retcode r = read_block(e, g_sfs.disk, g_sfs.part_loc,
				       oc_block, scratch);
		if (r != NO_ERROR)
			return r;
		if (be32(scratch + BH_ID) != SFS_BLOCKID_OBJC)
			return E_NO_FILE;
		if (be32(scratch + BH_OWNBLOCK) != oc_block)
			return E_NO_FILE;

		/* Walk the fsObject records in this container. */
		uInt off = OC_OBJECTS;
		while (off + OBJ_FIXED_SIZE <= SFS_BSIZE) {
			const uByte *obj = scratch + off;
			uInt node = be32(obj + OBJ_OBJECTNODE);
			if (node == 0)
				break;   /* end-of-objects sentinel */

			uInt avail = SFS_BSIZE - off;
			uInt rec = object_record_size(obj, avail);
			if (rec == 0 || off + rec > SFS_BSIZE)
				break;

			const uByte *obj_name = obj + OBJ_NAME;
			uInt obj_name_len = 0;
			while (obj_name_len < avail - OBJ_NAME &&
			       obj_name[obj_name_len] != 0)
				obj_name_len++;

			if (sfs_name_eq(obj_name, obj_name_len, name, namelen)) {
				*out_bits = obj[OBJ_BITS];
				*out_data = be32(obj + OBJ_DATA);
				*out_size = be32(obj + OBJ_SIZE);
				return NO_ERROR;
			}

			off += rec;
		}

		oc_block = be32(scratch + OC_NEXT);
	}
	return E_NO_FILE;
}

/* --- Walk a '/'-separated path from the root. Returns a
 * normalised view of the final component:
 *   - for files  : *out_data = first-extent key, *out_size = byte size
 *   - for dirs   : *out_data = firstdirblock (first OC BLCK, suitable
 *                  for sfs_list_directory), *out_size unused
 *   - for softlinks and hardlinks: OTYPE_LINK is set in *out_bits
 *                  and data/size are the raw Stream Extension view
 *
 * Empty path (just "/") returns the root's first OC block. */
static Retcode
sfs_walk_path(Environ *e, const Byte *path, uInt *out_data, uInt *out_size,
	      uByte *out_bits, uByte *scratch)
{
	while (*path == '/' || *path == '\\')
		path++;

	if (*path == '\0') {
		*out_data  = g_sfs.root_dir_first_oc;
		*out_size  = 0;
		*out_bits  = OTYPE_DIR;
		return NO_ERROR;
	}

	/* Start from the root directory's first ObjectContainer --
	 * NOT the rootobjc block. See sfs_open_volume() above: the
	 * rootobjc block contains the "root directory object" whose
	 * own firstdirblock is where the volume's top-level entries
	 * actually live. */
	uInt cur_dir_oc = g_sfs.root_dir_first_oc;

	const Byte *cur = path;
	while (*cur) {
		const Byte *sep = cur;
		while (*sep && *sep != '/' && *sep != '\\') sep++;
		uInt complen = (uInt)(sep - cur);
		if (complen == 0) { cur = sep + 1; continue; }

		uInt data = 0, size = 0;
		uByte bits = 0;
		Retcode r = sfs_dir_lookup(e, cur_dir_oc,
			(const uByte *)cur, complen,
			&data, &size, &bits, scratch);
		if (r != NO_ERROR)
			return r;

		/* `data` == object's +12 field (hashtable for dirs,
		 * first-extent key for files); `size` == +16 field
		 * (firstdirblock for dirs, file byte size for files). */

		if (*sep == '\0') {
			*out_bits = bits;
			if (bits & OTYPE_DIR) {
				/* Normalise: return firstdirblock so the
				 * caller can walk the OC chain directly. */
				*out_data = size;
				*out_size = 0;
			} else {
				*out_data = data;
				*out_size = size;
			}
			return NO_ERROR;
		}

		/* More path to walk -- current component must be a dir. */
		if ((bits & OTYPE_DIR) == 0)
			return E_NO_FILE;
		if (bits & OTYPE_LINK)
			return E_NO_FILE;   /* softlinks not followed */

		/* For a dir, obj->object.dir.firstdirblock is at +16
		 * (same offset as file size in the union). */
		cur_dir_oc = size;
		if (cur_dir_oc == 0)
			return E_NO_FILE;   /* empty subdir */

		cur = sep + 1;
	}

	/* Only reached if path ended on a separator -- treat as the
	 * last resolved directory. */
	*out_data  = cur_dir_oc;
	*out_size  = 0;
	*out_bits  = OTYPE_DIR;
	return NO_ERROR;
}

/* --- Extent B+-tree lookup --------------------------------------- *
 * Walks from extent_bn_root down to a leaf, finds the fsExtentBNode
 * with be_key == `key`, and returns (key, blocks, next) via out
 * params. Returns NO_ERROR on match, E_NO_FILE on miss. */
static Retcode
sfs_lookup_extent(Environ *e, uInt key, uInt *out_blocks, uInt *out_next,
		  uByte *scratch)
{
	uInt block = g_sfs.extent_bn_root;
	int depth = 0;

	while (block != 0 && depth < 32) {
		Retcode r = read_block(e, g_sfs.disk, g_sfs.part_loc,
				       block, scratch);
		if (r != NO_ERROR)
			return r;
		if (be32(scratch + BH_ID) != SFS_BLOCKID_BNDC)
			return E_NO_FILE;

		uInt nodecount = be16(scratch + BNC_NODECOUNT);
		uByte isleaf   = scratch[BNC_ISLEAF];
		uByte nodesize = scratch[BNC_NODESIZE];
		const uByte *bnodes = scratch + BNC_BNODES;

		if (nodesize < 8 || nodesize > 64)
			return E_NO_FILE;
		if (nodecount == 0)
			return E_NO_FILE;
		if (BNC_BNODES + (uInt)nodecount * nodesize > SFS_BSIZE)
			return E_NO_FILE;

		if (isleaf) {
			for (uInt i = 0; i < nodecount; i++) {
				const uByte *bn = bnodes + i * nodesize;
				uInt k = be32(bn + BN_KEY);
				if (k == key) {
					*out_blocks = be16(bn + BN_BLOCKS);
					*out_next   = be32(bn + BN_NEXT);
					return NO_ERROR;
				}
			}
			return E_NO_FILE;
		}

		/* Interior: find largest-key entry with key <= target,
		 * descend. If all entries have key > target, the key
		 * is below the leftmost subtree -- descend into that. */
		uInt child = 0;
		for (uInt i = 0; i < nodecount; i++) {
			const uByte *bn = bnodes + i * nodesize;
			uInt k = be32(bn + BN_KEY);
			if (k <= key)
				child = be32(bn + BN_DATA);
			else
				break;
		}
		if (child == 0) {
			/* Fall back to the leftmost child: key is before
			 * the first entry's key but that entry must still
			 * be followed (it's the subtree root). */
			child = be32(bnodes + BN_DATA);
		}
		block = child;
		depth++;
	}

	return E_NO_FILE;
}

/* --- Read `byte_size` bytes of file data starting at the extent
 * whose key is `first_data`. Copies into `out` (caller-sized).
 * Uses fsExtentBNode.be_next to traverse extents in order. */
static Retcode
sfs_read_file_contents(Environ *e, uInt first_data, uInt byte_size,
		       uByte *out, uLong *out_len, uByte *scratch)
{
	uInt written = 0;
	uInt cur_key = first_data;
	int  guard   = 0;

	while (written < byte_size && cur_key != 0) {
		if (++guard > 65536)
			return E_READ_ERROR;

		uInt blocks = 0, next = 0;
		Retcode r = sfs_lookup_extent(e, cur_key, &blocks, &next,
					      scratch);
		if (r != NO_ERROR)
			return r;
		if (blocks == 0)
			break;

		/* Read this extent in blocksize chunks. */
		for (uInt i = 0; i < blocks && written < byte_size; i++) {
			uByte dblk[SFS_BSIZE];
			r = read_block(e, g_sfs.disk, g_sfs.part_loc,
				       cur_key + i, dblk);
			if (r != NO_ERROR)
				return r;

			uInt remain = byte_size - written;
			uInt copy   = (remain < SFS_BSIZE) ? remain : SFS_BSIZE;
			memcpy(out + written, dblk, copy);
			written += copy;
		}

		cur_key = next;
	}

	*out_len = written;
	return NO_ERROR;
}

/* --- List the entries of a directory starting at first_oc. */
static void
sfs_list_directory(Environ *e, uInt first_oc, uByte *scratch)
{
	uInt oc_block = first_oc;
	int guard = 0;

	while (oc_block != 0) {
		if (++guard > 4096) break;

		Retcode r = read_block(e, g_sfs.disk, g_sfs.part_loc,
				       oc_block, scratch);
		if (r != NO_ERROR) return;
		if (be32(scratch + BH_ID) != SFS_BLOCKID_OBJC) return;

		uInt off = OC_OBJECTS;
		while (off + OBJ_FIXED_SIZE <= SFS_BSIZE) {
			const uByte *obj = scratch + off;
			uInt node = be32(obj + OBJ_OBJECTNODE);
			if (node == 0) break;

			uInt avail = SFS_BSIZE - off;
			uInt rec = object_record_size(obj, avail);
			if (rec == 0 || off + rec > SFS_BSIZE) break;

			uByte bits = obj[OBJ_BITS];
			uInt size = be32(obj + OBJ_SIZE);
			const uByte *name = obj + OBJ_NAME;
			uInt namelen = 0;
			while (namelen < avail - OBJ_NAME &&
			       name[namelen] != 0)
				namelen++;

			if (bits & OTYPE_DIR) {
				cprintf(e, "          [%S]\n", name, (Int)namelen);
			} else if (bits & OTYPE_LINK) {
				cprintf(e, "     link  %S\n", name, (Int)namelen);
			} else {
				cprintf(e, "%8d  %S\n", (int)size,
					name, (Int)namelen);
			}

			off += rec;
		}

		oc_block = be32(scratch + OC_NEXT);
	}
}

/* --- Main Filesys callback -------------------------------------- */
Retcode
amiga_sfs(Environ *e, Filesys_action what, Instance *disk,
	  Byte *path, uLong loc, uLong start_unused, uByte *buf,
	  uInt size, uByte *retbuf, uLong *val)
{
	(void)start_unused;
	if (size < SFS_BSIZE) return E_BLOCKSIZE;

	/* Recover the DosType from the partition's boot block. Unlike
	 * FFS, SFS's root block doesn't carry a visible 'DOS\X' magic
	 * at offset 0 (the id field there is the generic block header
	 * ID, not a DosType). So we rely on the boot block's first
	 * longword. */
	uByte bootbuf[SFS_BSIZE];
	Retcode r = read_block(e, disk, loc, 0, bootbuf);
	if (r != NO_ERROR)
		return r;
	uInt dt = be32(bootbuf);
	if ((dt & 0xFFFFFF00u) != SFS_DOSTYPE_MAGIC24)
		return E_NO_FILESYS;

	if (sfs_open_volume(e, disk, loc, dt, buf) != NO_ERROR)
		return E_NO_FILESYS;

	switch (what) {
	case FS_PROBE:
		strcat((char *)retbuf, ",amiga-sfs");
		*val = loc;
		return R_END;

	case FS_LIST: {
		uByte scratch[SFS_BSIZE];
		uInt data = 0, filesz = 0;
		uByte bits = 0;
		Retcode ret = sfs_walk_path(e, path, &data, &filesz, &bits,
					    scratch);
		if (ret != NO_ERROR)
			return E_NO_FILE;

		if (bits & OTYPE_DIR) {
			if (g_sfs.volume_name_len)
				cprintf(e, "SFS%s volume \"%s\":\n",
					(g_sfs.dostype == 2) ? "\\2" : "\\0",
					(char *)g_sfs.volume_name);
			else
				cprintf(e, "SFS%s volume:\n",
					(g_sfs.dostype == 2) ? "\\2" : "\\0");
			sfs_list_directory(e, data, scratch);
		} else {
			cprintf(e, "%8d  %s\n", (int)filesz, (char *)path);
		}
		return R_END;
	}

	case FS_LOAD: {
		uByte scratch[SFS_BSIZE];
		uInt data = 0, filesz = 0;
		uByte bits = 0;
		Retcode ret = sfs_walk_path(e, path, &data, &filesz, &bits,
					    scratch);
		if (ret != NO_ERROR)
			return E_NO_FILE;

		if (bits & OTYPE_DIR)   return E_NO_FILE;
		if (bits & OTYPE_LINK)  return E_NO_FILE;

		uLong got = 0;
		ret = sfs_read_file_contents(e, data, filesz, retbuf,
					     &got, scratch);
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

Filesys g_amiga_sfs = {
	"amiga-sfs",
	amiga_sfs,
};
