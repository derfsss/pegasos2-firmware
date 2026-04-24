#!/usr/bin/env python3
"""
Minimum-viable Amiga RDB test image generator.

Writes a disk image with:
  - RigidDiskBlock ('RDSK') at block 0
  - One PartitionBlock ('PART') at block 1, pb_DriveName="DH0",
    DosType=DOS\\7 (FFS2), covering cylinders 2..N

Partition contents are left zero-filled: it's a smoke-test image
for Arc FS-B Block 1 (RDB parser) BEFORE the FFS reader lands in
B2. FS_LIST on this image should print the partition; FS_LOAD
will fail with "no filesystem recognised" until B2 ships.

Usage:
  python3 mkrdb.py OUT.img [BLOCK_COUNT]
"""

import struct, sys

BLOCK_SIZE   = 512
DEFAULT_BLKS = 2048          # 1 MiB disk (tiny)
SURFACES     = 16
BLKS_PER_TRK = 63
SIZE_BLOCK   = 128           # longs per block (128 * 4 = 512 bytes)
RDB_BLOCK    = 0
PART_BLOCK   = 1

MAGIC_RDSK   = 0x5244534B    # 'RDSK'
MAGIC_PART   = 0x50415254    # 'PART'
DOSTYPE_FFS2 = 0x44_4F_53_07 # 'DOS\7'


def pack_u32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def sum_to_zero(data):
    """Compute checksum so the sum of longwords is zero mod 2^32."""
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
    cylinders 2..N where N = total cylinders - 1."""
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
    # pb_DriveName BSTR "DH0" occupies 32 bytes at offset 36
    # (1 length byte + up to 31 chars). Length byte + 3-char payload,
    # rest zero-filled.
    blk[36] = 3
    blk[37:40] = b"DH0"
    off = 36 + 32              # skip past the 32-byte DriveName field
    # pb_Reserved2[15] follows: 60 bytes of zeros
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


def main():
    if len(sys.argv) < 2:
        print("usage: mkrdb.py OUT.img [BLOCKS]", file=sys.stderr)
        sys.exit(1)
    out = sys.argv[1]
    blocks = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_BLKS

    rdb  = build_rigid_disk_block(blocks)
    part = build_partition_block(blocks)
    assert len(rdb)  == BLOCK_SIZE
    assert len(part) == BLOCK_SIZE

    with open(out, "wb") as f:
        f.write(rdb)
        f.write(part)
        f.write(b"\x00" * (BLOCK_SIZE * (blocks - 2)))

    print(f"wrote {out}: {blocks} blocks = {blocks * BLOCK_SIZE // 1024} KiB")
    print(f"  RDSK @ block 0, PART @ block 1 (DH0, DOS\\7 FFS2)")


if __name__ == "__main__":
    main()
