/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Arc FS-B, Block 1 -- Amiga Rigid Disk Block (RDB) partition parser.
 *
 *  Sits in g_filesys[] alongside dos_partition (MBR) and file-system
 *  readers (iso9660, dosfat, ext2fs, eventually FFS/SFS/PFS3). When
 *  file_system() iterates filesystems, this parser:
 *
 *    - FS_PROBE: scans the first 16 blocks of the disk for the RDSK
 *      magic. If found, reports itself + yields position. If not
 *      found, returns E_NO_FILESYS and the caller advances to the
 *      next filesystem.
 *
 *    - FS_LIST: walks the PartitionBlock chain (anchored at
 *      rdb_PartitionList) and prints one line per partition:
 *          AMIGA partition 0: DH0 DOSType=DOS\7 (1234MB)
 *      When a specific partition is selected via leading digit in
 *      `path`, recurses into file_system for that slice.
 *
 *    - FS_LOAD: same selection logic, but forwards to file_system
 *      on the chosen partition so a filesystem reader (FFS / SFS /
 *      PFS3 / FAT / ext2, added later) can actually load a file.
 *
 *  RDB format reference:
 *    - Ralph Babel, "The Amiga Guru Book" (1996), ch. 15
 *    - AmigaDOS Technical Reference Manual (Commodore, 1986)
 *    - devices/hardblocks.h in the AOS4 SDK (header copyright
 *      Hyperion + Amiga Inc; we reference the format spec but do
 *      not copy any code from the SDK, only standard-format
 *      constants like the "RDSK" / "PART" magic numbers)
 *
 *  Implementation notes:
 *    - All RDB blocks are big-endian 32-bit-aligned structures,
 *      sum-to-zero checksummed across rdb_SummedLongs longwords.
 *    - Partition start-in-bytes is computed from the DosEnvec:
 *          start = de_LowCyl * de_Surfaces * de_BlocksPerTrack
 *                  * de_SizeBlock (in longwords) * 4 bytes/long
 *      de_SizeBlock is the number of longwords per block (128 for
 *      512-byte blocks; 256 for 1024-byte blocks; etc.)
 *    - We cap the PartitionBlock chain traversal at
 *      MAX_PARTITIONS (32) to avoid cycles in malformed RDBs.
 */

#include "defs.h"
#include "fs.h"

/*
 * Cross-FS partition geometry hint.
 *
 * Set by amiga_rdb before recursing into file_system() on a partition
 * slice; read by amiga_ffs/amiga_sfs/amiga_pfs3 when they need to
 * compute root-block or extent-tree positions that depend on the
 * partition size and FS-block size (e.g. FFS root sits at
 * (lowKey + highKey) / 2 in FS-block units).
 *
 * Cleared on amiga_rdb entry to prevent a stale value from a previous
 * partition bleeding into a direct (non-RDB-recursed) FS_PROBE call.
 * Single-threaded firmware with one outstanding boot makes a global
 * safe.
 */
uLong g_amiga_part_byte_size;
uInt  g_amiga_part_block_size;   /* de_SizeBlock * 4 * de_SectorPerBlock */

/* --- On-disk magic numbers (public Amiga DOS format) ------------ */
#define RDB_MAGIC_RDSK    0x5244534Bu   /* 'RDSK' */
#define RDB_MAGIC_PART    0x50415254u   /* 'PART' */
#define RDB_MAGIC_FSHD    0x46534844u   /* 'FSHD' (unused in M1) */

/* --- Probe / traversal limits ----------------------------------- */
#define RDB_PROBE_BLOCKS  16            /* Amiga RDB must live here */
#define BLOCK_SIZE_DEFAULT 512          /* assumption for RDB scan */
#define MAX_PARTITIONS    32            /* chain-length guard */

/* --- RigidDiskBlock field offsets (from hardblocks.h conventions) */
#define RDB_OFF_ID              0
#define RDB_OFF_SUMMEDLONGS     4
#define RDB_OFF_CHKSUM          8
#define RDB_OFF_HOSTID          12
#define RDB_OFF_BLOCKBYTES      16
#define RDB_OFF_FLAGS           20
#define RDB_OFF_PARTITIONLIST   28      /* rdb_PartitionList: first PART block */

/* --- PartitionBlock field offsets ------------------------------- */
#define PART_OFF_ID             0
#define PART_OFF_SUMMEDLONGS    4
#define PART_OFF_CHKSUM         8
#define PART_OFF_NEXT           16      /* pb_Next: next PartitionBlock */
#define PART_OFF_FLAGS          20
#define PART_OFF_DRIVENAME      36      /* pb_DriveName: BSTR (len byte + 31 chars) */
#define PART_OFF_ENVIRONMENT    128     /* pb_Environment[20]: DosEnvec */

/* --- DosEnvec field offsets (within pb_Environment, longword idx) */
#define DE_SIZEBLOCK            1       /* longs per block */
#define DE_SURFACES             3
#define DE_SECTORPERBLOCK       4
#define DE_BLOCKSPERTRACK       5
#define DE_LOWCYL               9
#define DE_HIGHCYL              10
#define DE_DOSTYPE              16      /* de_DosType: FS identifier */

static uInt
be32(const uByte *p)
{
	return ((uInt)p[0] << 24) | ((uInt)p[1] << 16) |
	       ((uInt)p[2] << 8)  | (uInt)p[3];
}

/*
 * Sum-to-zero checksum over `long_count` big-endian longwords.
 * Valid RDB/PART/FSHD/LSEG blocks have the sum of their longwords
 * (including the stored checksum field) equal to zero mod 2^32.
 * Returns 0 iff checksum valid.
 */
static uInt
rdb_checksum_ok(const uByte *block, uInt long_count)
{
	if (long_count == 0 || long_count > 128)
		return 0;   /* obviously bogus count */

	uInt sum = 0;
	for (uInt i = 0; i < long_count; i++)
		sum += be32(block + i * 4);

	return sum == 0 ? 1 : 0;
}

/*
 * String-compare the AOS DosType field (stored as 4 bytes:
 * 3 ASCII chars + 1 version byte) to a human-readable name we
 * print for FS_LIST. Returns a short label for common types.
 */
static const char *
dostype_label(uInt dostype)
{
	/* "DOS\0" .. "DOS\7" -- OFS/FFS family */
	if ((dostype & 0xFFFFFF00u) == 0x444F5300u) {
		switch (dostype & 0xFFu) {
		case 0: return "DOS\\0 (OFS)";
		case 1: return "DOS\\1 (FFS)";
		case 2: return "DOS\\2 (OFS-Intl)";
		case 3: return "DOS\\3 (FFS-Intl)";
		case 4: return "DOS\\4 (OFS-DC)";
		case 5: return "DOS\\5 (FFS-DC)";
		case 6: return "DOS\\6 (OFS-LN)";
		case 7: return "DOS\\7 (FFS-LN/FFS2)";
		}
	}
	/* "SFS\0" .. "SFS\2" */
	if ((dostype & 0xFFFFFF00u) == 0x53465300u)
		return "SFS-00";
	/* "PFS\1" .. "PFS\3" */
	if ((dostype & 0xFFFFFF00u) == 0x50465300u)
		return "PFS";
	/* "CD01" (CDFS) */
	if (dostype == 0x43443031u) return "CDFS";
	return "<unknown>";
}

/*
 * Print a BSTR (length-prefixed Amiga string) up to `max` chars,
 * trimming whitespace. Used for the drive-name field.
 */
static void
print_bstr(Environ *e, const uByte *p, int max)
{
	int len = p[0];
	if (len > max - 1) len = max - 1;
	for (int i = 0; i < len; i++)
		cprintf(e, "%c", p[1 + i]);
}

/*
 * Scan blocks 0..RDB_PROBE_BLOCKS-1 looking for a RigidDiskBlock
 * with valid RDSK magic AND valid checksum. Returns block-index of
 * the first valid RDB found, or -1 if none. `buf` must be at least
 * BLOCK_SIZE_DEFAULT bytes.
 */
static int
find_rdb(Environ *e, Instance *disk, uLong disk_loc, uByte *buf)
{
	for (int b = 0; b < RDB_PROBE_BLOCKS; b++) {
		Retcode ret = filesys_read_bytes(e, disk,
			disk_loc + (uLong)b * BLOCK_SIZE_DEFAULT,
			BLOCK_SIZE_DEFAULT, buf);
		if (ret != NO_ERROR)
			return -1;

		if (be32(buf + RDB_OFF_ID) != RDB_MAGIC_RDSK)
			continue;

		uInt summed_longs = be32(buf + RDB_OFF_SUMMEDLONGS);
		if (!rdb_checksum_ok(buf, summed_longs))
			continue;

		return b;
	}
	return -1;
}

/*
 * Compute a partition's byte offset on disk and byte size from
 * its pb_Environment fields. Returns 0 on success + fills *start/
 * *size, non-zero on overflow/bad-env.
 */
static int
decode_partition_range(const uByte *part_block,
		       uLong *out_start, uLong *out_size)
{
	const uByte *env = part_block + PART_OFF_ENVIRONMENT;

	uInt size_block_longs = be32(env + DE_SIZEBLOCK  * 4);
	uInt surfaces         = be32(env + DE_SURFACES   * 4);
	uInt blks_per_track   = be32(env + DE_BLOCKSPERTRACK * 4);
	uInt low_cyl          = be32(env + DE_LOWCYL     * 4);
	uInt high_cyl         = be32(env + DE_HIGHCYL    * 4);

	if (size_block_longs == 0 || surfaces == 0 || blks_per_track == 0)
		return 1;
	if (high_cyl < low_cyl)
		return 1;

	/* Bytes per cylinder = surfaces * blks/track * size_block * 4 */
	uLong bpc = (uLong)surfaces * blks_per_track *
		    size_block_longs * 4u;

	*out_start = (uLong)low_cyl * bpc;
	*out_size  = (uLong)(high_cyl - low_cyl + 1) * bpc;
	return 0;
}

/*
 * Main Filesys callback. Parses RDB and recurses into file_system
 * for the selected partition, mirroring dospart.c's interaction
 * with file_system.
 *
 * path syntax:
 *   ""            -- no partition specified. For FS_LIST: list all.
 *                    For FS_LOAD: fail (need a partition+file).
 *   "N,..."       -- partition index N (0..chain length-1). Rest of
 *                    path after the comma is the file path.
 *   "N"           -- just the partition index (used for FS_LIST of
 *                    a single partition or FS_LOAD with
 *                    partition-boot-block).
 */
Retcode
amiga_rdb(Environ *e, Filesys_action what, Instance *disk,
	  Byte *path, uLong loc, uLong start_unused, uByte *buf,
	  uInt size, uByte *retbuf, uLong *val)
{
	(void)start_unused;

	if (size < BLOCK_SIZE_DEFAULT)
		return E_BLOCKSIZE;

	/* Reset partition hint on every entry so a non-RDB FS reader
	 * downstream of us never sees a stale value from a previous
	 * partition. The hint is set per partition just before
	 * recursing into file_system, below. */
	g_amiga_part_byte_size  = 0;
	g_amiga_part_block_size = 0;

	int rdb_blknum = find_rdb(e, disk, loc, buf);
	if (rdb_blknum < 0)
		return E_NO_FILESYS;

	uLong rdb_loc = loc + (uLong)rdb_blknum * BLOCK_SIZE_DEFAULT;

	/* Re-read the RDB into buf (find_rdb's last read may have
	 * been a non-matching block). */
	Retcode ret = filesys_read_bytes(e, disk, rdb_loc,
		BLOCK_SIZE_DEFAULT, buf);
	if (ret != NO_ERROR)
		return ret;

	uInt partition_list = be32(buf + RDB_OFF_PARTITIONLIST);

	if (what == FS_PROBE) {
		strcat((char *)retbuf, ",amiga-rdb");
		*val = rdb_loc;
		return R_END;
	}

	/* Partition selection: leading digit in `path`. Matches
	 * dospart.c's convention. */
	int selected_partition = -1;
	if (*path >= '0' && *path <= '9') {
		selected_partition = 0;
		while (*path >= '0' && *path <= '9') {
			selected_partition = selected_partition * 10 +
			                     (*path - '0');
			path++;
		}
		if (*path == ',')
			path++;
	}

	if (partition_list == 0 || partition_list == 0xFFFFFFFFu) {
		if (what == FS_LIST)
			cprintf(e, "AMIGA RDB: no partitions\n");
		return E_NO_FILESYS;
	}

	/* Walk the PartitionBlock chain. Each block lives at
	 * (chain_blkno * BLOCK_SIZE_DEFAULT) within the disk. */
	uInt chain_blkno = partition_list;
	int  partition_idx = 0;

	while (chain_blkno != 0 && chain_blkno != 0xFFFFFFFFu &&
	       partition_idx < MAX_PARTITIONS) {
		uLong part_loc = loc + (uLong)chain_blkno *
				 BLOCK_SIZE_DEFAULT;

		ret = filesys_read_bytes(e, disk, part_loc,
			BLOCK_SIZE_DEFAULT, buf);
		if (ret != NO_ERROR)
			return ret;

		if (be32(buf + PART_OFF_ID) != RDB_MAGIC_PART) {
			cprintf(e, "AMIGA RDB: chain broken at block %d "
				"(no PART magic)\n", (int)chain_blkno);
			return E_NO_FILESYS;
		}

		uLong p_start = 0, p_size = 0;
		if (decode_partition_range(buf, &p_start, &p_size) != 0) {
			cprintf(e, "AMIGA RDB: partition %d has bad env\n",
				partition_idx);
			chain_blkno = be32(buf + PART_OFF_NEXT);
			partition_idx++;
			continue;
		}

		const uByte *env = buf + PART_OFF_ENVIRONMENT;
		uInt dostype = be32(env + DE_DOSTYPE * 4);

		if (what == FS_LIST && selected_partition < 0) {
			/* p_size / p_start are 64-bit uLong; split into 32-bit
			 * halves to avoid any SF-cprintf %lu vs __LONGLONG
			 * varargs mismatch. KB units fit in 32 bits for any
			 * practical partition under 4 TiB. */
			uInt size_kb_lo = (uInt)((p_size >> 10) & 0xFFFFFFFFu);
			uInt size_kb_hi = (uInt)((p_size >> 42) & 0xFFFFFFFFu);
			uInt byte_lo    = (uInt)((loc + p_start) & 0xFFFFFFFFu);
			uInt byte_hi    = (uInt)(((loc + p_start) >> 32) & 0xFFFFFFFFu);
			cprintf(e, "AMIGA partition %d: ", partition_idx);
			print_bstr(e, buf + PART_OFF_DRIVENAME, 32);
			cprintf(e, " DOSType=%s size=%dKB @ 0x%X%08X\n",
				dostype_label(dostype),
				(Int)size_kb_lo,
				(Int)byte_hi, (Int)byte_lo);
			(void)size_kb_hi;
		}

		if (selected_partition == partition_idx) {
			if (what == FS_LIST || what == FS_LOAD) {
				/*
				 * Strip trailing whitespace-delimited args
				 * before handing to the FS reader. The
				 * official AOS4 boot syntax is
				 *   boot hd:0 amigaboot.of bootdevice=DHY
				 * SF's f_disklbl_load concatenates the
				 * filename + bootargs into a single
				 * loadargs string, so by the time we get
				 * here `path` looks like
				 *   "/amigaboot.of bootdevice=DHY".
				 * The FS reader needs the bare path. The
				 * full args still reach /chosen/bootargs
				 * via SF's separate stash, so the OS sees
				 * "bootdevice=DHY" downstream. */
				static Byte path_clean[256];
				int pc_n = 0;
				while (pc_n < (int)sizeof(path_clean) - 1 &&
				       path[pc_n] != 0 &&
				       path[pc_n] != ' ' &&
				       path[pc_n] != '\t')
					pc_n++;
				memcpy(path_clean, path, pc_n);
				path_clean[pc_n] = 0;
				/* Publish geometry hints so the FS reader
				 * (FFS in particular) can compute the
				 * actual root-block position from
				 * partition size instead of guessing.
				 *
				 * AOS FS-block size = de_SizeBlock (longs
				 * per device-block) * 4 * de_SectorPerBlock
				 * (device-blocks per FS-block). Classic
				 * small-disk partitions use 128*4*1=512;
				 * AOS4 FFS2 on >= 1 GiB disks sets
				 * de_SectorPerBlock=2 for 1024-byte FS
				 * blocks, where sec_type and hash-table
				 * sizing shift accordingly. */
				{
				  uInt sz_block_longs = be32(buf +
				    PART_OFF_ENVIRONMENT + DE_SIZEBLOCK * 4);
				  uInt sec_per_blk    = be32(buf +
				    PART_OFF_ENVIRONMENT
				    + DE_SECTORPERBLOCK * 4);
				  if (sz_block_longs == 0) sz_block_longs = 128;
				  if (sec_per_blk    == 0) sec_per_blk    = 1;
				  g_amiga_part_byte_size  = p_size;
				  g_amiga_part_block_size =
				    sz_block_longs * 4u * sec_per_blk;
				}
				/* Recurse into file_system on the partition
				 * slice. Future FFS/SFS/PFS3 readers will
				 * match on the DosType and return success. */
				ret = file_system(e, what, disk, path_clean,
					loc + p_start, p_start,
					buf, BLOCK_SIZE_DEFAULT,
					retbuf, val);
				return (ret == E_NO_FILESYS)
					? E_UNSUPPORTED_FILESYS : ret;
			}
		}

		chain_blkno = be32(buf + PART_OFF_NEXT);
		partition_idx++;
	}

	if (selected_partition >= 0 && selected_partition >= partition_idx) {
		cprintf(e, "AMIGA RDB: no partition %d (found %d)\n",
			selected_partition, partition_idx);
		return E_NO_FILESYS;
	}

	return R_END;
}

/*
 * Filesys entry. g_filesys[] in platform.c adds &g_amiga_rdb
 * before iso9660 / dosfat / ext2fs so an Amiga disk with an RDB
 * gets parsed into partitions before anything tries whole-disk FS
 * detection.
 */
Filesys g_amiga_rdb = {
	"amiga-rdb",
	amiga_rdb,
};
