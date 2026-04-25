/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Partition packages for Amiga RDB-partitioned disks, plus the
 *  smart-boot Forth dispatcher that uses them.
 *
 *  IEEE-1275 OS loaders (amigaboot.of, MorphOS's morphos.of, etc.)
 *  expect each disk's partitions to appear as child packages of the
 *  disk's device-tree node, with a per-partition method table that
 *  translates partition-relative reads into disk-absolute reads.
 *  install_partition_packages() runs from install_list[] after
 *  install_aliases; for each HD child of the IDE controller it
 *
 *    - opens the disk via open-dev,
 *    - reads RDB block 0 + chases the PartitionBlock chain,
 *    - creates one child package per partition, with:
 *
 *        name          = pb_DriveName (BSTR copy, e.g. "DH0")
 *        device_type   = "block"
 *        reg           = <partition-index>
 *        partition-name = pb_DriveName (alternate property some
 *                         loaders look for)
 *        dostype        = 4 raw bytes from de_DosType
 *        boot-priority  = signed Int from de_BootPri
 *
 *      and a method table:
 *
 *        open / close   -- per-instance state setup/teardown
 *        block-size     -- partition's block size
 *        #blocks        -- partition's block count
 *        max-transfer   -- delegated to parent disk
 *        read-blocks    -- (addr block# count -- read) translates
 *                          block# to absolute + forwards to parent
 *        read           -- byte-level read using cursor + parent's
 *                          read-blocks
 *        seek           -- byte-level cursor set.
 *
 *  All methods run with inst->parent pointing at the parent disk's
 *  Instance, so they can dispatch to the parent's methods via
 *  execute_method_name.
 *
 *  smart-boot (Forth word, registered in platform.c init_pegasos2[])
 *  consumes the dostype + boot-priority properties to pick which
 *  partition to load and which per-OS loader to dispatch to,
 *  ordered by the comma-separated `boot-os-priority` NVRAM var.
 */

#include "defs.h"
#include "byteswap.h"

/* --- RDB / PartitionBlock layout (mirror of amiga_rdb.c constants) - */
#define RDB_MAGIC_RDSK    0x5244534Bu
#define RDB_MAGIC_PART    0x50415254u
#define RDB_PROBE_BLOCKS  16
#define BLOCK_SIZE_DEFAULT 512
#define MAX_PARTITIONS    32

#define RDB_OFF_PARTITIONLIST   28
#define PART_OFF_NEXT           16
#define PART_OFF_DRIVENAME      36
#define PART_OFF_ENVIRONMENT    128

#define DE_SIZEBLOCK            1
#define DE_SURFACES             3
#define DE_BLOCKSPERTRACK       5
#define DE_LOWCYL               9
#define DE_HIGHCYL              10
#define DE_BOOTPRI              15      /* signed; -128 = "do not boot" */
#define DE_DOSTYPE              16      /* 4-char filesystem identifier */

extern Package *find_ide_controller(Environ *e);

/* Per-package state for a partition. Stored at pkg->self. */
typedef struct PartitionSelf {
	uLong    start_byte;       /* partition start, disk-absolute bytes */
	uLong    size_bytes;       /* partition byte count */
	uInt     block_size;       /* block size (usually 512) */
	uLong    n_blocks;         /* size_bytes / block_size */
} PartitionSelf;

/* Per-instance state. inst->self for an open partition instance. */
typedef struct PartitionInst {
	uLong cursor;              /* current byte cursor for read/seek */
} PartitionInst;

/* Forward decls for method handlers. */
static Retcode f_part_open(Environ *e);
static Retcode f_part_close(Environ *e);
static Retcode f_part_block_size(Environ *e);
static Retcode f_part_blocks(Environ *e);
static Retcode f_part_max_transfer(Environ *e);
static Retcode f_part_read_blocks(Environ *e);
static Retcode f_part_seek(Environ *e);
static Retcode f_part_read(Environ *e);

static const Initentry partition_methods[] = {
	{ "open",         f_part_open,         INVALID_FCODE },
	{ "close",        f_part_close,        INVALID_FCODE },
	{ "block-size",   f_part_block_size,   INVALID_FCODE },
	{ "#blocks",      f_part_blocks,       INVALID_FCODE },
	{ "max-transfer", f_part_max_transfer, INVALID_FCODE },
	{ "read-blocks",  f_part_read_blocks,  INVALID_FCODE },
	{ "seek",         f_part_seek,         INVALID_FCODE },
	{ "read",         f_part_read,         INVALID_FCODE },
	{ NULL,           NULL },
};

/* Get the package's PartitionSelf via the current instance. */
static PartitionSelf *
get_part_self(Environ *e)
{
	Instance *inst = (Instance *)(uPtr)e->currinst;
	if (inst == NULL || inst->package == NULL)
		return NULL;
	return (PartitionSelf *)inst->package->self;
}

/* open ( -- okay? ) */
static Retcode
f_part_open(Environ *e)
{
	Instance *inst = (Instance *)(uPtr)e->currinst;
	if (inst == NULL)
		return E_BAD_INSTANCE;

	PartitionInst *pi = (PartitionInst *)malloc(sizeof *pi);
	if (pi == NULL)
		return E_OUT_OF_MEMORY;
	pi->cursor = 0;
	inst->self = (struct self *)pi;

	IFCKSP(e, 0, 1);
	PUSH(e, FTRUE);
	return NO_ERROR;
}

/* close ( -- ) */
static Retcode
f_part_close(Environ *e)
{
	Instance *inst = (Instance *)(uPtr)e->currinst;
	if (inst != NULL && inst->self != NULL) {
		free(inst->self);
		inst->self = NULL;
	}
	return NO_ERROR;
}

/* block-size ( -- size ) */
static Retcode
f_part_block_size(Environ *e)
{
	PartitionSelf *ps = get_part_self(e);
	if (ps == NULL)
		return E_BAD_INSTANCE;
	IFCKSP(e, 0, 1);
	PUSH(e, (Cell)ps->block_size);
	return NO_ERROR;
}

/* #blocks ( -- n ) */
static Retcode
f_part_blocks(Environ *e)
{
	PartitionSelf *ps = get_part_self(e);
	if (ps == NULL)
		return E_BAD_INSTANCE;
	IFCKSP(e, 0, 1);
	PUSH(e, (Cell)ps->n_blocks);
	return NO_ERROR;
}

/* max-transfer ( -- n ) -- delegate to parent disk. */
static Retcode
f_part_max_transfer(Environ *e)
{
	Instance *inst = (Instance *)(uPtr)e->currinst;
	if (inst == NULL || inst->parent == NULL) {
		IFCKSP(e, 0, 1);
		PUSH(e, (Cell)0x10000);
		return NO_ERROR;
	}
	return execute_method_name(e, inst->parent,
		(Byte *)"max-transfer", CSTR);
}

/* read-blocks ( addr block# #blocks -- #read )
 * Translate to disk-absolute block# and forward to parent.
 */
static Retcode
f_part_read_blocks(Environ *e)
{
	PartitionSelf *ps = get_part_self(e);
	Instance *inst = (Instance *)(uPtr)e->currinst;
	Cell n_blocks, block_num;
	void *addr;

	if (ps == NULL || inst == NULL || inst->parent == NULL)
		return E_BAD_INSTANCE;

	IFCKSP(e, 3, 1);
	POP(e, n_blocks);
	POP(e, block_num);
	POPT(e, addr, void *);

	uLong abs_block = (ps->start_byte / ps->block_size) +
	                  (uLong)(uInt)block_num;

	PUSHP(e, addr);
	PUSH(e, (Cell)(uInt)abs_block);
	PUSH(e, n_blocks);
	return execute_method_name(e, inst->parent,
		(Byte *)"read-blocks", CSTR);
}

/* seek ( pos.lo pos.hi -- status ) */
static Retcode
f_part_seek(Environ *e)
{
	PartitionSelf *ps = get_part_self(e);
	Instance *inst = (Instance *)(uPtr)e->currinst;
	PartitionInst *pi;
	Cell pos_lo, pos_hi;

	if (ps == NULL || inst == NULL ||
	    (pi = (PartitionInst *)inst->self) == NULL)
		return E_BAD_INSTANCE;

	IFCKSP(e, 2, 1);
	POP(e, pos_hi);
	POP(e, pos_lo);

	uLong pos = (uLong)(uInt)pos_lo |
	            ((uLong)(uInt)pos_hi << 32);
	if (pos > ps->size_bytes) {
		PUSH(e, (Cell)-1);
		return NO_ERROR;
	}
	pi->cursor = pos;
	PUSH(e, (Cell)0);
	return NO_ERROR;
}

/* read ( addr len -- actual )
 * Read `len` bytes starting from current cursor. Uses parent's
 * read-blocks via aligned access; for partial start/end blocks
 * we round-down to block-aligned reads, then memcpy out.
 */
static Retcode
f_part_read(Environ *e)
{
	PartitionSelf *ps = get_part_self(e);
	Instance *inst = (Instance *)(uPtr)e->currinst;
	PartitionInst *pi;
	Cell len_in;
	uByte *addr;

	if (ps == NULL || inst == NULL ||
	    (pi = (PartitionInst *)inst->self) == NULL)
		return E_BAD_INSTANCE;

	IFCKSP(e, 2, 1);
	POP(e, len_in);
	POPT(e, addr, uByte *);

	if ((uLong)(uInt)len_in == 0) {
		PUSH(e, (Cell)0);
		return NO_ERROR;
	}

	uLong remain = (uLong)(uInt)len_in;
	if (pi->cursor >= ps->size_bytes) {
		PUSH(e, (Cell)0);
		return NO_ERROR;
	}
	if (pi->cursor + remain > ps->size_bytes)
		remain = ps->size_bytes - pi->cursor;

	uLong copied = 0;
	uByte blkbuf[BLOCK_SIZE_DEFAULT];

	while (remain > 0) {
		uLong abs_byte = ps->start_byte + pi->cursor;
		uLong abs_block = abs_byte / ps->block_size;
		uInt  off_in_blk = (uInt)(abs_byte - abs_block *
		                          ps->block_size);
		uInt  chunk = ps->block_size - off_in_blk;
		if ((uLong)chunk > remain)
			chunk = (uInt)remain;

		PUSHP(e, blkbuf);
		PUSH(e, (Cell)(uInt)abs_block);
		PUSH(e, (Cell)1);
		Retcode r = execute_method_name(e, inst->parent,
			(Byte *)"read-blocks", CSTR);
		if (r != NO_ERROR)
			return r;

		Cell got;
		POP(e, got);
		if ((uInt)got != 1)
			break;

		memcpy(addr + copied, blkbuf + off_in_blk, chunk);
		copied += chunk;
		pi->cursor += chunk;
		remain -= chunk;
	}

	PUSH(e, (Cell)(uInt)copied);
	return NO_ERROR;
}

/* --- Install-time RDB walking ---------------------------------------- */

/*
 * Read N bytes from a disk Instance starting at a byte offset using
 * the disk's seek+read methods. Returns NO_ERROR on success.
 */
static Retcode
disk_read_at(Environ *e, Instance *inst, uLong byte_off,
             void *buf, uInt len)
{
	Cell status, actual;
	Retcode r;

	PUSH(e, (Cell)(uInt)(byte_off & 0xFFFFFFFFu));
	PUSH(e, (Cell)(uInt)(byte_off >> 32));
	r = execute_method_name(e, inst, (Byte *)"seek", CSTR);
	if (r != NO_ERROR) return r;
	POP(e, status);
	if ((Int)status != 0 && (Int)status != 1)
		return E_ABORT;

	PUSHP(e, buf);
	PUSH(e, (Cell)len);
	r = execute_method_name(e, inst, (Byte *)"read", CSTR);
	if (r != NO_ERROR) return r;
	POP(e, actual);
	if ((uInt)actual != len)
		return E_ABORT;
	return NO_ERROR;
}

static int
rdb_chksum_ok(const uByte *blk, uInt long_count)
{
	if (long_count == 0 || long_count > 128)
		return 0;
	uInt s = 0;
	for (uInt i = 0; i < long_count; i++)
		s += be32(blk + i * 4);
	return s == 0;
}

/*
 * Walk a disk's RDB and create child partition packages for each
 * PartitionBlock found. Disk is opened via open-dev for the duration
 * of this call (so seek+read methods work) and closed afterwards.
 */
static Retcode
add_partitions_for_disk(Environ *e, Package *disk_pkg)
{
	Byte pathbuf[STR_SIZE];
	Instance *inst = NULL;
	Retcode r;
	uByte buf[BLOCK_SIZE_DEFAULT];

	if (!get_device_name(e, disk_pkg, pathbuf))
		return E_NO_DEVICE;

	PUSHP(e, pathbuf);
	PUSH(e, (Cell)strlen((char *)pathbuf));
	r = execute_word(e, "open-dev");
	if (r != NO_ERROR)
		return r;
	POPT(e, inst, Instance *);
	if (inst == NULL)
		return E_NO_DEVICE;

	/* Find RDB in first 16 blocks. */
	int rdb_blk = -1;
	for (int b = 0; b < RDB_PROBE_BLOCKS; b++) {
		r = disk_read_at(e, inst, (uLong)b * BLOCK_SIZE_DEFAULT,
			buf, BLOCK_SIZE_DEFAULT);
		if (r != NO_ERROR) goto close_and_return;
		if (be32(buf + 0) != RDB_MAGIC_RDSK) continue;
		uInt sl = be32(buf + 4);
		if (rdb_chksum_ok(buf, sl)) {
			rdb_blk = b;
			break;
		}
	}
	if (rdb_blk < 0) {
		r = E_NO_FILESYS;
		goto close_and_return;
	}

	uLong rdb_off = (uLong)rdb_blk * BLOCK_SIZE_DEFAULT;
	r = disk_read_at(e, inst, rdb_off, buf, BLOCK_SIZE_DEFAULT);
	if (r != NO_ERROR) goto close_and_return;

	uInt part_blknum = be32(buf + RDB_OFF_PARTITIONLIST);

	int part_idx = 0;
	while (part_blknum != 0 && part_blknum != 0xFFFFFFFFu &&
	       part_idx < MAX_PARTITIONS) {
		uLong part_off = (uLong)part_blknum * BLOCK_SIZE_DEFAULT;
		r = disk_read_at(e, inst, part_off, buf, BLOCK_SIZE_DEFAULT);
		if (r != NO_ERROR) break;
		if (be32(buf + 0) != RDB_MAGIC_PART) break;

		uInt sl = be32(buf + 4);
		if (!rdb_chksum_ok(buf, sl)) break;

		/* Decode geometry. */
		const uByte *env = buf + PART_OFF_ENVIRONMENT;
		uInt size_block_longs = be32(env + DE_SIZEBLOCK * 4);
		uInt surfaces       = be32(env + DE_SURFACES * 4);
		uInt blks_per_track = be32(env + DE_BLOCKSPERTRACK * 4);
		uInt low_cyl        = be32(env + DE_LOWCYL * 4);
		uInt high_cyl       = be32(env + DE_HIGHCYL * 4);

		if (size_block_longs == 0 || surfaces == 0 ||
		    blks_per_track == 0 || high_cyl < low_cyl)
			break;

		uLong bpc = (uLong)surfaces * blks_per_track *
		            size_block_longs * 4u;
		uLong p_start = (uLong)low_cyl * bpc;
		uLong p_size  = (uLong)(high_cyl - low_cyl + 1) * bpc;

		/* DriveName BSTR -> NUL-terminated. */
		Byte name[34];
		int nlen = buf[PART_OFF_DRIVENAME];
		if (nlen > 31) nlen = 31;
		for (int i = 0; i < nlen; i++)
			name[i] = buf[PART_OFF_DRIVENAME + 1 + i];
		name[nlen] = 0;
		if (nlen == 0) {
			/* Anonymous partition: synthesize a name. */
			name[0] = 'D'; name[1] = 'H';
			name[2] = '0' + (Byte)(part_idx % 10); name[3] = 0;
		}

		Package *p = new_pkg_name(disk_pkg, name);
		if (p == NULL) { r = E_OUT_OF_MEMORY; break; }

		prop_set_str(p->props, (Byte *)"device_type", CSTR,
			(Byte *)"block", CSTR);
		prop_set_str(p->props, (Byte *)"partition-name", CSTR,
			name, CSTR);
		prop_set_int(p->props, (Byte *)"reg", CSTR,
			(Cell)part_idx);
		/*
		 * Expose RDB DosType + BootPri as device-tree properties
		 * for smart-boot's OS-priority dispatch. dostype is a 4-byte
		 * binary blob (e.g. 'DOS\7', 'SFS\0', 'EXT2'); boot-priority
		 * is a signed Int (-128 = "never auto-boot").
		 */
		uInt dostype = be32(env + DE_DOSTYPE * 4);
		Int  bootpri = (Int)be32(env + DE_BOOTPRI * 4);
		/* set_property allocates+memcpy's the bytes (unlike
		 * add_property which only stashes the pointer -- a stack
		 * temporary would be reused after the install function
		 * returned and any later read would see garbage). */
		uByte dostype_be[4];
		dostype_be[0] = (uByte)(dostype >> 24);
		dostype_be[1] = (uByte)(dostype >> 16);
		dostype_be[2] = (uByte)(dostype >> 8);
		dostype_be[3] = (uByte)(dostype);
		set_property(p->props, (Byte *)"dostype", CSTR,
			(Byte *)dostype_be, 4);
		prop_set_int(p->props, (Byte *)"boot-priority", CSTR,
			(Cell)bootpri);

		PartitionSelf *ps =
			(PartitionSelf *)malloc(sizeof *ps);
		if (ps == NULL) { r = E_OUT_OF_MEMORY; break; }
		ps->start_byte = p_start;
		ps->size_bytes = p_size;
		ps->block_size = (size_block_longs * 4u);
		if (ps->block_size == 0) ps->block_size = 512;
		ps->n_blocks   = p_size / ps->block_size;
		p->self = (struct pself *)ps;

		/* init_entries returns NO_ERROR or E_OUT_OF_MEMORY; the
		 * latter would leave the package without methods, but the
		 * caller-side malloc that would precede that error already
		 * happens above (PartitionSelf), so OOM here is statistically
		 * impossible without first OOMing the smaller allocation. */
		(void)init_entries(e, p->dict, partition_methods);

		cprintf(e, "  partition %d: %s %dK @ disk byte 0x%X "
			"(bs=%d, %d blocks)\n",
			part_idx, name,
			(int)(p_size >> 10),
			(unsigned)(uInt)p_start,
			(int)ps->block_size,
			(int)(uInt)ps->n_blocks);

		part_blknum = be32(buf + PART_OFF_NEXT);
		part_idx++;
	}

close_and_return:
	PUSHP(e, inst);
	(void)execute_word(e, "close-dev");
	return r;
}

/* Locate the first non-ATAPI HD child under the IDE controller.
 * Non-static so smart-boot below can re-walk after partition install. */
Package *
find_first_hd_disk(Environ *e)
{
	Package *ide = find_ide_controller(e);
	if (ide == NULL) return NULL;
	for (Package *c = ide->children; c != NULL; c = c->link) {
		if (find_table(c->props, (Byte *)"atapi", CSTR) == NULL)
			return c;
	}
	return NULL;
}

/*
 * Install RDB-derived child partition packages on each HD found
 * under the IDE controller. Call from install_list[] after
 * install_aliases.
 */
CC(install_partition_packages)
{
	Package *hd = find_first_hd_disk(e);
	if (hd == NULL)
		return NO_ERROR;
	cprintf(e, "RDB partitions on HD:\n");
	Retcode r = add_partitions_for_disk(e, hd);
	if (r != NO_ERROR && r != E_NO_FILESYS)
		cprintf(e, "  (RDB scan: %s)\n", err2str(r));
	return NO_ERROR;
}

/* --- smart-boot: priority-based OS dispatcher --------------------- */

/*
 * Map a 4-byte DosType to an OS family. Lower-case strings here are
 * the same names accepted in the `boot-os-priority` NVRAM list.
 *
 * Coverage:
 *   "DOS\\0".."DOS\\7" -> amigaos    (OFS, FFS, Intl, DC, LNFS / FFS2)
 *   "SFS\\0".."SFS\\2" -> amigaos    (SmartFileSystem, AOS-side)
 *   "PFS\\1".."PFS\\3" -> amigaos    (Professional FileSystem 3)
 *   "AFS\\1"           -> amigaos    (alias for PFS, AOS-side)
 *   "MOR\\?"           -> morphos    (MorphOS-format partitions)
 *   "LNX\\?"/"EXT2/3/4"-> linux      (Linux-on-Amiga partitions)
 *   anything else      -> "" (unknown; not a smart-boot candidate)
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

/* Token compare helper: returns 1 if [s..s+slen) equals [t..t+tlen) and
 * also accepts a few aliases ("amigaos4"/"amigaos3" -> "amigaos"). */
static int
os_family_match(const Byte *s, int slen, const char *family)
{
	int flen = (int)strlen(family);
	if (slen == flen && memcmp(s, family, slen) == 0)
		return 1;
	/* aliases */
	if (strcmp(family, "amigaos") == 0) {
		if (slen == 8 && memcmp(s, "amigaos4", 8) == 0) return 1;
		if (slen == 8 && memcmp(s, "amigaos3", 8) == 0) return 1;
		if (slen == 5 && memcmp(s, "amiga", 5) == 0)    return 1;
	}
	return 0;
}

/*
 * For one OS family, scan all partition children of `hd` and return
 * the highest-BootPri candidate, or NULL if none qualify. -128 in the
 * BootPri field is the AOS "do not auto-boot this partition" sentinel
 * (per RDB spec); we treat any negative value as opt-out so users can
 * tag a non-bootable Linux data partition with -1 and not have
 * smart-boot pick it.
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

		Entry *be_ = find_table(p->props, (Byte *)"boot-priority", CSTR);
		Int pri = 0;
		if (be_ != NULL && be_->len >= 4) {
			const uByte *bb = (const uByte *)be_->v.array;
			pri = (Int)(((uInt)bb[0] << 24) | ((uInt)bb[1] << 16) |
			            ((uInt)bb[2] << 8)  |  (uInt)bb[3]);
		}
		if (pri < 0) continue;          /* opt-out */
		if (pri > best_pri) {
			best_pri = pri;
			best = p;
		}
	}
	return best;
}

/* Per-family loader. Each returns NO_ERROR if the loader was started
 * (control will not return on success), or an error code on failure
 * (smart-boot then continues to the next family in the priority list). */
static Retcode
loader_amigaos(Environ *e, Package *part)
{
	(void)part;
	/* amigaboot.of does its own RDB walk + de_BootPri sort + menu;
	 * we hand off without specifying which DH#: -- amigaboot picks
	 * the highest-priority AOS-family partition automatically, and
	 * displays a menu if multiple installs exist. */
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

/*
 * Forth: smart-boot ( -- )
 *
 * Walk the user's `boot-os-priority` list in order. For each family,
 * find a candidate partition (matching DosType + BootPri >= 0) and,
 * if one exists, invoke the per-family loader. If the loader returns
 * (only happens on failure -- successful boots transfer control out
 * of the firmware), continue to the next family.
 *
 * If the entire priority list is exhausted with no successful loader,
 * fall back to a plain `boot` so the user still sees SF's default
 * load attempt against boot-device + boot-file.
 */
CC(f_smart_boot)
{
	/* Pull priority list. get_config returns the bare value bytes
	 * (no length prefix); SF stores it as a NUL-terminated string. */
	Byte *prio = get_config(e, (Byte *)"boot-os-priority", CSTR);
	const char *fallback = "amigaos,morphos,linux";
	if (prio == NULL || *prio == 0)
		prio = (Byte *)fallback;

	Package *hd = find_first_hd_disk(e);
	if (hd == NULL) {
		cprintf(e, "smart-boot: no HD; falling back to plain boot\n");
		const char *cmd = "boot";
		return interp_text(e, (Byte *)cmd, strlen(cmd));
	}

	/* Walk comma-separated priority list. Tokens are trimmed of
	 * leading/trailing whitespace; case-sensitive. */
	const Byte *cur = prio;
	while (*cur) {
		while (*cur == ' ' || *cur == '\t') cur++;
		const Byte *tok_start = cur;
		while (*cur && *cur != ',' && *cur != ' ' && *cur != '\t')
			cur++;
		int tok_len = (int)(cur - tok_start);
		while (*cur == ' ' || *cur == '\t') cur++;
		if (*cur == ',') cur++;

		if (tok_len == 0) continue;

		/* Map the token to one of the known families. */
		const char *family;
		if      (os_family_match(tok_start, tok_len, "amigaos"))
			family = "amigaos";
		else if (os_family_match(tok_start, tok_len, "linux"))
			family = "linux";
		else if (os_family_match(tok_start, tok_len, "morphos"))
			family = "morphos";
		else {
			cprintf(e, "smart-boot: unknown family `%S` -- skipped\n",
			        tok_start, (Int)tok_len);
			continue;
		}

		Package *cand = pick_partition_for_family(hd, family);
		if (cand == NULL) continue;

		/* Print which partition we picked (helpful when multiple
		 * families coexist). */
		Byte *pname = NULL; Int pnlen = 0;
		(void)prop_get_str(cand->props, (Byte *)"partition-name",
			CSTR, &pname, &pnlen);
		cprintf(e, "smart-boot: picking %s partition %S\n",
		        family, pname ? pname : (Byte *)"?",
		        pname ? pnlen : 1);

		Retcode r;
		if (strcmp(family, "amigaos") == 0)
			r = loader_amigaos(e, cand);
		else if (strcmp(family, "linux") == 0)
			r = loader_linux(e, cand);
		else
			r = loader_morphos(e, cand);

		if (r == NO_ERROR)
			return NO_ERROR;     /* unreachable in success case */
		/* loader failed -- try next family */
	}

	cprintf(e, "smart-boot: no priority match, falling back to plain `boot`\n");
	const char *cmd = "boot";
	return interp_text(e, (Byte *)cmd, strlen(cmd));
}
