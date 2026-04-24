#!/usr/bin/env python3
"""
Minimum-viable Amiga RDB + FFS2 test image generator.

Writes a disk image with:
  - RigidDiskBlock ('RDSK') at disk block 0
  - One PartitionBlock ('PART') at disk block 1, pb_DriveName="DH0",
    DosType=DOS\\7 (FFS2), covering cylinders 2..N-1
  - Inside the partition: a minimal FFS2 filesystem containing one
    file "test.elf" whose contents are the caller-supplied ELF bytes
    (or an all-zero 512-byte placeholder if no --elf is passed).

Layout within the partition (block numbers relative to partition):
    block 0       : boot block carrying the DosType signature
    block 880     : root block (LNFS volume "TEST", one hash entry)
    block 881     : file header for "test.elf"
    block 882..   : raw FFS data blocks (no per-block header)
    block 953..   : extension blocks, one per extra DB_MAX data chunk

Usage:
  python3 mkrdb.py OUT.img [BLOCKS] [--elf test_kernel.elf]
"""

import struct, sys

BLOCK_SIZE   = 512
DEFAULT_BLKS = 6144          # 3 MiB disk -- 4-cylinder partition
SURFACES     = 16
BLKS_PER_TRK = 63
SIZE_BLOCK   = 128           # longs per block (128 * 4 = 512 bytes)
RDB_BLOCK    = 0
PART_BLOCK   = 1

MAGIC_RDSK   = 0x5244534B    # 'RDSK'
MAGIC_PART   = 0x50415254    # 'PART'
DOSTYPE_FFS2 = 0x44_4F_53_07 # 'DOS\7'

# --- Amiga FFS on-disk constants (mirrors machdep/pegasos2/of/amiga_ffs.c) ---
T_HEADER     = 2
T_LIST       = 16
ST_ROOT      = 1
ST_FILE      = -3            # stored as 0xFFFFFFFD
HT_OFFSET    = 0x18
HT_ENTRIES   = (BLOCK_SIZE - HT_OFFSET - 200) // 4   # 72 for 512-byte blocks

OFF_SEC_TYPE  = BLOCK_SIZE - 4
OFF_EXTENSION = BLOCK_SIZE - 8
OFF_PARENT    = BLOCK_SIZE - 12
OFF_NEXT_HASH = BLOCK_SIZE - 16
OFF_NAME      = BLOCK_SIZE - 80
OFF_LNFS_NAC  = BLOCK_SIZE - 92
OFF_BYTE_SIZE = BLOCK_SIZE - 188
DB_HIGH_OFF   = BLOCK_SIZE - 208
DB_MAX        = (BLOCK_SIZE - 228) // 4              # 71 for 512-byte blocks

# Block numbers inside the partition (FFS2 skeleton layout)
ROOT_BLOCK    = 880          # matches probe_mids[0] in amiga_ffs.c
FILE_HDR      = 881
FIRST_DATA    = 882


def pack_u32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def sum_to_zero(data):
    """Return the longword that, when stored in `data`, makes the
    sum of its 32-bit big-endian longwords equal zero mod 2^32."""
    assert len(data) % 4 == 0
    s = 0
    for i in range(0, len(data), 4):
        s = (s + struct.unpack(">I", data[i:i+4])[0]) & 0xFFFFFFFF
    return (-s) & 0xFFFFFFFF


def build_rigid_disk_block(total_blocks):
    """Return 512 bytes of RigidDiskBlock, summed-longs=64."""
    summed_longs = 64
    blk = bytearray(BLOCK_SIZE)
    off = 0
    # rdb_ID, rdb_SummedLongs, rdb_ChkSum, rdb_HostID
    blk[off:off+4]  = pack_u32(MAGIC_RDSK);       off += 4
    blk[off:off+4]  = pack_u32(summed_longs);     off += 4
    chksum_off = off
    blk[off:off+4]  = pack_u32(0); off += 4
    blk[off:off+4]  = pack_u32(7);                off += 4  # HostID
    # rdb_BlockBytes, rdb_Flags
    blk[off:off+4]  = pack_u32(BLOCK_SIZE);       off += 4
    blk[off:off+4]  = pack_u32(0x17);             off += 4  # Flags
    # rdb_Obsolete1 (-1), rdb_PartitionList = PART_BLOCK
    blk[off:off+4]  = pack_u32(0xFFFFFFFF);       off += 4
    blk[off:off+4]  = pack_u32(PART_BLOCK);       off += 4
    # rdb_FileSysHeaderList (-1), rdb_DriveInit (-1), rdb_BootStrapCode (-1)
    for _ in range(3):
        blk[off:off+4] = pack_u32(0xFFFFFFFF);    off += 4
    # rdb_Reserved1[5]
    for _ in range(5):
        blk[off:off+4] = pack_u32(0xFFFFFFFF);    off += 4
    # physical drive geometry
    blk[off:off+4]  = pack_u32(total_blocks // (SURFACES * BLKS_PER_TRK)); off += 4
    blk[off:off+4]  = pack_u32(BLKS_PER_TRK);     off += 4
    blk[off:off+4]  = pack_u32(SURFACES);         off += 4
    # remainder zero-filled is fine
    # Patch checksum over the first summed_longs*4 bytes
    chunk = bytes(blk[:summed_longs * 4])
    blk[chksum_off:chksum_off+4] = pack_u32(sum_to_zero(chunk))
    return bytes(blk)


def build_partition_block(total_blocks):
    """Return 512 bytes of PartitionBlock for DH0 / DOS\\7, covering
    cylinders 2..N-1 where N = total cylinders."""
    summed_longs = 64
    blk = bytearray(BLOCK_SIZE)
    off = 0
    # pb_ID, pb_SummedLongs, pb_ChkSum, pb_HostID
    blk[off:off+4]  = pack_u32(MAGIC_PART);       off += 4
    blk[off:off+4]  = pack_u32(summed_longs);     off += 4
    chksum_off = off
    blk[off:off+4]  = pack_u32(0); off += 4
    blk[off:off+4]  = pack_u32(7);                off += 4  # HostID
    # pb_Next (-1 = end), pb_Flags, pb_Reserved1[2]
    blk[off:off+4]  = pack_u32(0xFFFFFFFF);       off += 4
    blk[off:off+4]  = pack_u32(1);                off += 4  # bootable
    blk[off:off+4]  = pack_u32(0);                off += 4
    blk[off:off+4]  = pack_u32(0);                off += 4
    # pb_DevFlags (0)
    blk[off:off+4]  = pack_u32(0);                off += 4
    # pb_DriveName BSTR "DH0" at offset 36 (1 length + 31 chars).
    blk[36] = 3
    blk[37:40] = b"DH0"
    off = 36 + 32              # skip past the 32-byte DriveName field
    # pb_Reserved2[15]: 60 bytes of zeros
    off += 15 * 4
    # pb_Environment[20] -- DosEnvec
    env = [0] * 20
    env[0]  = 16                                  # de_TableSize
    env[1]  = SIZE_BLOCK                          # de_SizeBlock (longs)
    env[2]  = 0                                   # de_SecOrg
    env[3]  = SURFACES                            # de_Surfaces
    env[4]  = 1                                   # de_SectorPerBlock
    env[5]  = BLKS_PER_TRK                        # de_BlocksPerTrack
    env[6]  = 2                                   # de_Reserved
    env[7]  = 0                                   # de_PreAlloc
    env[8]  = 0                                   # de_Interleave
    total_cyls = total_blocks // (SURFACES * BLKS_PER_TRK)
    env[9]  = 2                                   # de_LowCyl
    env[10] = total_cyls - 1                      # de_HighCyl
    env[11] = 30                                  # de_NumBuffers
    env[12] = 0                                   # de_BufMemType
    env[13] = 0xFFFFFF                            # de_MaxTransfer
    env[14] = 0x7FFFFFFF                          # de_Mask
    env[15] = 0                                   # de_BootPri
    env[16] = DOSTYPE_FFS2                        # de_DosType = DOS\7
    env[17] = 0                                   # de_Baud
    env[18] = 0                                   # de_Control
    env[19] = 1                                   # de_BootBlocks
    for v in env:
        blk[off:off+4] = pack_u32(v); off += 4
    # pb_EReserved[12] zero
    chunk = bytes(blk[:summed_longs * 4])
    blk[chksum_off:chksum_off+4] = pack_u32(sum_to_zero(chunk))
    return bytes(blk)


# --- FFS2 (DOS\7) filesystem builders ---------------------------------------
#
# Case-folding matches fold_ch() in amiga_ffs.c: uppercase ASCII a-z,
# and (for Intl/LNFS) also 0xE0..0xFE except 0xF7. LNFS volumes always
# use Intl folding.

def fold_ch(c):
    if ord('a') <= c <= ord('z'):
        return c - 0x20
    if 0xE0 <= c <= 0xFE and c != 0xF7:
        return c - 0x20
    return c


def name_hash(name_bytes):
    h = len(name_bytes)
    for c in name_bytes:
        h = (h * 13 + fold_ch(c)) & 0x7FF
    return h % HT_ENTRIES


def build_boot_block(dostype):
    """Boot block at partition offset 0 -- amiga_ffs.c reads this to
    recover DosType when the RDB path bypassed that info."""
    blk = bytearray(BLOCK_SIZE)
    struct.pack_into(">I", blk, 0x00, dostype)
    return bytes(blk)


def write_bstr(blk, off, text, maxlen):
    """Write a BSTR (length byte + raw bytes) at `off`. Caller must
    ensure `off + 1 + len(text) <= off + maxlen` fits the reserved
    area."""
    data = text.encode('ascii')
    n = min(len(data), maxlen - 1)
    blk[off] = n
    blk[off + 1 : off + 1 + n] = data[:n]


def build_root_block(volume_name, hash_table):
    """Return 512 bytes of LNFS root block with the given hash table
    (dict bucket -> block number). Volume name is written both at the
    standard BSTR position (OFF_NAME) -- which amiga_ffs.c reads with
    is_lnfs=0 in FS_LIST -- and at the LNFS NaC position for safety."""
    blk = bytearray(BLOCK_SIZE)
    struct.pack_into(">I", blk, 0x00, T_HEADER)        # primary type
    struct.pack_into(">I", blk, 0x0C, HT_ENTRIES)      # ht_size

    for bucket, blknum in hash_table.items():
        assert 0 <= bucket < HT_ENTRIES
        struct.pack_into(">I", blk, HT_OFFSET + bucket * 4, blknum)

    # Volume name at OFF_NAME (what the FS_LIST path reads).
    write_bstr(blk, OFF_NAME, volume_name, 32)
    # Also at OFF_LNFS_NAC with a trailing empty comment BSTR, so an
    # LNFS-aware reader finds the same name.
    write_bstr(blk, OFF_LNFS_NAC, volume_name, 76)
    blk[OFF_LNFS_NAC + 1 + len(volume_name.encode('ascii'))] = 0

    struct.pack_into(">i", blk, OFF_SEC_TYPE, ST_ROOT)

    # Checksum after all other fields are set.
    struct.pack_into(">I", blk, 0x14, sum_to_zero(bytes(blk)))
    return bytes(blk)


def build_file_header(own_key, parent, name, byte_size, data_blocks,
                       next_hash=0, extension=0):
    blk = bytearray(BLOCK_SIZE)
    struct.pack_into(">I", blk, 0x00, T_HEADER)
    struct.pack_into(">I", blk, 0x04, own_key)
    struct.pack_into(">I", blk, 0x08, len(data_blocks))   # high_seq
    if data_blocks:
        struct.pack_into(">I", blk, 0x10, data_blocks[0]) # first_data

    assert len(data_blocks) <= DB_MAX
    for i, db in enumerate(data_blocks):
        struct.pack_into(">I", blk, DB_HIGH_OFF - i * 4, db)

    struct.pack_into(">I", blk, OFF_BYTE_SIZE, byte_size)

    # LNFS name at NaC (the branch amiga_ffs.c takes on DOS\7 file
    # headers). Leave a zero-length comment BSTR after the name.
    nb = name.encode('ascii')
    assert len(nb) <= 74   # 1 len + name + 1 comment-len must fit in 76 bytes
    blk[OFF_LNFS_NAC] = len(nb)
    blk[OFF_LNFS_NAC + 1 : OFF_LNFS_NAC + 1 + len(nb)] = nb
    blk[OFF_LNFS_NAC + 1 + len(nb)] = 0   # empty comment

    struct.pack_into(">I", blk, OFF_EXTENSION, extension)
    struct.pack_into(">I", blk, OFF_PARENT, parent)
    struct.pack_into(">I", blk, OFF_NEXT_HASH, next_hash)
    struct.pack_into(">i", blk, OFF_SEC_TYPE, ST_FILE)

    struct.pack_into(">I", blk, 0x14, sum_to_zero(bytes(blk)))
    return bytes(blk)


def build_extension_block(own_key, parent, data_blocks, next_extension=0):
    blk = bytearray(BLOCK_SIZE)
    struct.pack_into(">I", blk, 0x00, T_LIST)
    struct.pack_into(">I", blk, 0x04, own_key)
    struct.pack_into(">I", blk, 0x08, len(data_blocks))   # high_seq

    assert len(data_blocks) <= DB_MAX
    for i, db in enumerate(data_blocks):
        struct.pack_into(">I", blk, DB_HIGH_OFF - i * 4, db)

    struct.pack_into(">I", blk, OFF_EXTENSION, next_extension)
    struct.pack_into(">I", blk, OFF_PARENT, parent)
    struct.pack_into(">i", blk, OFF_SEC_TYPE, ST_FILE)

    struct.pack_into(">I", blk, 0x14, sum_to_zero(bytes(blk)))
    return bytes(blk)


def build_filesystem(payload, partition_blocks):
    """Return dict {block_idx: 512 bytes} describing a minimal FFS2
    filesystem containing one file 'test.elf'=payload."""
    blocks = {}
    file_name = "test.elf"
    bucket = name_hash(file_name.encode('ascii'))

    # Boot block at partition offset 0 (DosType only).
    blocks[0] = build_boot_block(DOSTYPE_FFS2)

    # Slice payload into BLOCK_SIZE chunks, assigned to data blocks
    # starting at FIRST_DATA.
    num_data = (len(payload) + BLOCK_SIZE - 1) // BLOCK_SIZE
    data_ids = list(range(FIRST_DATA, FIRST_DATA + num_data))

    # Chunk data_ids into groups of DB_MAX for file-header +
    # extension-block coverage.
    chunks = [data_ids[i:i+DB_MAX] for i in range(0, len(data_ids), DB_MAX)] \
             or [[]]
    num_ext = max(0, len(chunks) - 1)
    ext_ids = list(range(FIRST_DATA + num_data,
                         FIRST_DATA + num_data + num_ext))
    last_used_block = FIRST_DATA + num_data + num_ext - 1

    assert last_used_block < partition_blocks, \
        f"FFS layout needs block {last_used_block} > partition size {partition_blocks}"

    # File header covers chunks[0]; extension block k covers chunks[k+1].
    hdr_ext = ext_ids[0] if num_ext > 0 else 0
    blocks[FILE_HDR] = build_file_header(
        own_key=FILE_HDR, parent=ROOT_BLOCK, name=file_name,
        byte_size=len(payload), data_blocks=chunks[0],
        next_hash=0, extension=hdr_ext,
    )

    for k, ext_id in enumerate(ext_ids):
        next_ext = ext_ids[k + 1] if k + 1 < num_ext else 0
        blocks[ext_id] = build_extension_block(
            own_key=ext_id, parent=FILE_HDR,
            data_blocks=chunks[k + 1], next_extension=next_ext,
        )

    # Data blocks: raw bytes (DOS\7 = FFS, no per-block header).
    for i, block_id in enumerate(data_ids):
        start = i * BLOCK_SIZE
        chunk = payload[start:start + BLOCK_SIZE]
        if len(chunk) < BLOCK_SIZE:
            chunk = chunk + b"\x00" * (BLOCK_SIZE - len(chunk))
        blocks[block_id] = chunk

    # Root block last so it can reference the file header.
    blocks[ROOT_BLOCK] = build_root_block("TEST", {bucket: FILE_HDR})

    return blocks


def main():
    args = sys.argv[1:]
    if not args:
        print("usage: mkrdb.py OUT.img [BLOCKS] [--elf path/to/test_kernel.elf]",
              file=sys.stderr)
        sys.exit(1)

    out = args.pop(0)
    elf_path = None
    # Filter --elf out of positional args
    remaining = []
    it = iter(args)
    for a in it:
        if a == "--elf":
            elf_path = next(it)
        else:
            remaining.append(a)
    blocks = int(remaining[0]) if remaining else DEFAULT_BLKS

    if elf_path:
        with open(elf_path, "rb") as fh:
            payload = fh.read()
    else:
        # Smoke-test placeholder: one block of "PEG2\0..." so the
        # RDB parser sees *something* (file header + one data block).
        payload = b"PEG2" + b"\x00" * (BLOCK_SIZE - 4)

    rdb  = build_rigid_disk_block(blocks)
    part = build_partition_block(blocks)
    assert len(rdb)  == BLOCK_SIZE
    assert len(part) == BLOCK_SIZE

    # Partition starts at cylinder 2 -- byte offset in the disk image.
    total_cyls        = blocks // (SURFACES * BLKS_PER_TRK)
    bytes_per_cyl     = SURFACES * BLKS_PER_TRK * BLOCK_SIZE
    part_offset       = 2 * bytes_per_cyl
    partition_blocks  = (total_cyls - 2) * SURFACES * BLKS_PER_TRK

    fs_blocks = build_filesystem(payload, partition_blocks)

    with open(out, "wb") as f:
        # Zero-fill to total size first, then patch in the non-zero blocks.
        f.write(b"\x00" * (blocks * BLOCK_SIZE))
        f.seek(RDB_BLOCK * BLOCK_SIZE);  f.write(rdb)
        f.seek(PART_BLOCK * BLOCK_SIZE); f.write(part)
        for block_idx, data in fs_blocks.items():
            f.seek(part_offset + block_idx * BLOCK_SIZE)
            assert len(data) == BLOCK_SIZE
            f.write(data)

    print(f"wrote {out}: {blocks} blocks = {blocks * BLOCK_SIZE // 1024} KiB")
    print(f"  RDSK @ block 0, PART @ block 1 (DH0, DOS\\7 FFS2)")
    print(f"  partition: cyls 2..{total_cyls-1}, {partition_blocks} blocks "
          f"at disk offset 0x{part_offset:X}")
    if elf_path:
        num_data = (len(payload) + BLOCK_SIZE - 1) // BLOCK_SIZE
        print(f"  FFS2 /test.elf: {len(payload)} bytes in {num_data} data blocks")


if __name__ == "__main__":
    main()
