#!/usr/bin/env python3
"""
Minimum-viable exFAT raw-volume test-image generator.

Emits a single-file exFAT filesystem image containing "test.elf"
in the root directory. Used to smoke-test Arc FS-C's fs_exfat.c
reader without depending on external tools like mkfs.exfat or a
loop-mount. The output is a whole-disk raw image (no MBR, no RDB);
machdep/pegasos2/of/fs_exfat.c will find the EXFAT signature at
volume offset 0 and recognise it.

Layout (512-byte sectors):
  sector 0          main boot sector
  sectors 1..8      extended boot sectors (only the 0xAA550000
                    signature at sector tail matters for our reader)
  sectors 9..10     OEM parameters / reserved (zero filled)
  sector 11         boot checksum (our reader skips validation)
  sectors 12..23    backup boot region (mirror of 0..11)
  sectors 24..31    FAT (8 sectors = 1024 entries * 4 bytes)
  sectors 32..      cluster heap, cluster size 4 KiB (8 sectors)

Cluster assignment:
  cluster 2         root directory (single cluster, 128 entries)
  clusters 3..N     test.elf data (contiguous, FAT chain)

Root directory entries for test.elf:
  +0x00   File entry       (EntryType 0x85, SecondaryCount 2)
  +0x20   Stream Extension (EntryType 0xC0)
  +0x40   File Name        (EntryType 0xC1) -- "test.elf" fits in one

We intentionally omit the Allocation Bitmap (0x81) and Up-case
Table (0x82) primary entries: Microsoft's spec requires them but
fs_exfat.c skips those entry types on the read path, and exFAT
volumes produced by real formatters include them at positions
before our File entry. A stricter reader or a future write path
would need them.

Usage:
  python3 mkexfat.py OUT.img [SECTORS] [--elf test_kernel.elf]
"""

import struct, sys

SECTOR       = 512
DEFAULT_SECS = 8192             # 4 MiB volume
SPC_SHIFT    = 3                # 2^3 = 8 sectors per cluster
BPS_SHIFT    = 9                # 2^9 = 512 bytes per sector
CLUSTER      = SECTOR << SPC_SHIFT   # 4 KiB
FAT_OFF      = 24
FAT_LEN      = 8                # sectors -- 1024 entries
CLUSHEAP_OFF = 32
ROOT_CLUSTER = 2
DATA_CLUSTER = 3


def boot_sector(volume_sectors, cluster_count):
    """Construct the 512-byte main boot sector."""
    blk = bytearray(SECTOR)
    # JumpBoot
    blk[0:3] = b"\xEB\x76\x90"
    # FileSystemName "EXFAT   " (3 trailing spaces)
    blk[3:11] = b"EXFAT   "
    # MustBeZero (53 bytes at 11..63) stays zero
    # PartitionOffset (u64 LE) at 64
    struct.pack_into("<Q", blk, 64, 0)
    # VolumeLength (u64 LE) at 72
    struct.pack_into("<Q", blk, 72, volume_sectors)
    # FatOffset (u32) at 80
    struct.pack_into("<I", blk, 80, FAT_OFF)
    # FatLength (u32) at 84
    struct.pack_into("<I", blk, 84, FAT_LEN)
    # ClusterHeapOffset (u32) at 88
    struct.pack_into("<I", blk, 88, CLUSHEAP_OFF)
    # ClusterCount (u32) at 92
    struct.pack_into("<I", blk, 92, cluster_count)
    # FirstClusterOfRootDirectory (u32) at 96
    struct.pack_into("<I", blk, 96, ROOT_CLUSTER)
    # VolumeSerialNumber (u32) at 100 -- any 32-bit value
    struct.pack_into("<I", blk, 100, 0x50453203)
    # FileSystemRevision (u16) at 104: high byte major, low byte minor
    # 1.0 is stored as u16 0x0100.
    struct.pack_into("<H", blk, 104, 0x0100)
    # VolumeFlags (u16) at 106 -- ActiveFat=0 (first), clean
    struct.pack_into("<H", blk, 106, 0)
    # BytesPerSectorShift at 108
    blk[108] = BPS_SHIFT
    # SectorsPerClusterShift at 109
    blk[109] = SPC_SHIFT
    # NumberOfFats at 110
    blk[110] = 1
    # DriveSelect at 111 (INT 13h drive -- we're not a PC BIOS)
    blk[111] = 0x80
    # PercentInUse at 112
    blk[112] = 0xFF
    # Boot code: 0xF4 (HLT) filler at 120..509
    for i in range(120, 510):
        blk[i] = 0xF4
    # Boot signature 0xAA55 LE at 510
    struct.pack_into("<H", blk, 510, 0xAA55)
    return bytes(blk)


def extended_boot_sector():
    """Extended boot sectors 1..8 each end with the u32 LE signature
    0xAA550000. The body can be zero for our reader."""
    blk = bytearray(SECTOR)
    struct.pack_into("<I", blk, SECTOR - 4, 0xAA550000)
    return bytes(blk)


def file_entry(secondary_count, attrs=0, first_cluster=0, data_len=0):
    """Primary File entry (32 bytes, EntryType 0x85)."""
    blk = bytearray(32)
    blk[0] = 0x85
    blk[1] = secondary_count
    # SetChecksum at 2..3 -- our reader doesn't validate
    # GeneralPrimaryFlags at 4..5: bit 0 AllocationPossible, bit 1 NoFatChain
    # We use 0x0001 (AllocationPossible, follow FAT chain).
    struct.pack_into("<H", blk, 4, 0x0001)
    # FileAttributes at 20..21: 0x0020 = Archive
    struct.pack_into("<H", blk, 20, attrs)
    return bytes(blk)


def stream_ext_entry(name_chars, first_cluster, data_len, no_fat=False):
    """Stream Extension (32 bytes, EntryType 0xC0). NameHash is left
    zero -- our reader doesn't verify it, and Microsoft's formatter
    writes a hash so a stricter reader would fail. This is a known
    test-image limitation; flag via comment if we ever bring up a
    stricter reader."""
    blk = bytearray(32)
    blk[0] = 0xC0
    # GeneralSecondaryFlags: bit 0 AllocationPossible (1 when non-empty),
    # bit 1 NoFatChain
    flags = 0x01
    if no_fat: flags |= 0x02
    blk[1] = flags
    blk[3] = name_chars
    # ValidDataLength at 8..15 (u64 LE)
    struct.pack_into("<Q", blk, 8, data_len)
    # FirstCluster at 20..23 (u32 LE)
    struct.pack_into("<I", blk, 20, first_cluster)
    # DataLength at 24..31 (u64 LE)
    struct.pack_into("<Q", blk, 24, data_len)
    return bytes(blk)


def file_name_entry(name_utf16_chunk):
    """File Name entry (32 bytes, EntryType 0xC1). name_utf16_chunk is
    up to 15 UTF-16LE code units, right-padded with zeros."""
    blk = bytearray(32)
    blk[0] = 0xC1
    # bytes 2..31 hold 15 UTF-16LE code units
    chunk = name_utf16_chunk[:30]
    blk[2:2+len(chunk)] = chunk
    return bytes(blk)


def build_root_dir(file_name, first_cluster, data_len):
    """Return 4 KiB root-dir cluster with File+Stream+FileName entries
    for `file_name`. 128 x 32-byte slots; unused slots stay zero and
    act as end-of-directory markers."""
    entries = bytearray(CLUSTER)
    nm = file_name.encode('utf-16-le')
    name_chars = len(file_name)

    # How many FileName entries do we need? 15 chars per entry.
    fn_count = (name_chars + 14) // 15
    secondary_count = 1 + fn_count   # 1 Stream + N FileName

    off = 0
    entries[off:off+32] = file_entry(secondary_count)
    off += 32
    entries[off:off+32] = stream_ext_entry(
        name_chars, first_cluster, data_len, no_fat=False)
    off += 32
    for i in range(fn_count):
        chunk = nm[i*30:(i+1)*30]
        # pad to 30 bytes
        if len(chunk) < 30:
            chunk = chunk + b"\x00" * (30 - len(chunk))
        entries[off:off+32] = file_name_entry(chunk)
        off += 32

    return bytes(entries)


def build_fat(cluster_count, file_first_cluster, file_clusters):
    """Construct the FAT region (FAT_LEN sectors). Each entry is 4
    bytes LE. Entry 0 = media descriptor, entry 1 = reserved, entry 2
    = root dir (single cluster -> EOC), entries for the file chain
    link cluster -> cluster+1 and the last points to 0xFFFFFFFF."""
    fat_bytes = FAT_LEN * SECTOR
    fat = bytearray(fat_bytes)
    def put(idx, val):
        struct.pack_into("<I", fat, idx * 4, val & 0xFFFFFFFF)
    put(0, 0xFFFFFFF8)
    put(1, 0xFFFFFFFF)
    put(2, 0xFFFFFFFF)       # root directory: single cluster, EOC
    # File chain: cluster k -> k+1, last -> EOC
    for i in range(file_clusters - 1):
        put(file_first_cluster + i, file_first_cluster + i + 1)
    put(file_first_cluster + file_clusters - 1, 0xFFFFFFFF)
    return bytes(fat)


def build_image(out_path, volume_sectors, payload):
    total_bytes = volume_sectors * SECTOR
    cluster_count = (volume_sectors - CLUSHEAP_OFF) >> SPC_SHIFT
    if cluster_count < 3:
        raise ValueError("volume too small for a minimal exFAT test")

    file_clusters = (len(payload) + CLUSTER - 1) // CLUSTER
    if DATA_CLUSTER + file_clusters > cluster_count + 1:
        raise ValueError(
            f"payload needs {file_clusters} clusters but volume has "
            f"only {cluster_count} -- bump SECTORS")

    root_dir = build_root_dir("test.elf", DATA_CLUSTER, len(payload))
    fat = build_fat(cluster_count, DATA_CLUSTER, file_clusters)
    main_boot = boot_sector(volume_sectors, cluster_count)
    ext_boot = extended_boot_sector()

    with open(out_path, "wb") as f:
        f.write(b"\x00" * total_bytes)

        # Main boot region (sectors 0..11)
        f.seek(0);  f.write(main_boot)
        for s in range(1, 9):
            f.seek(s * SECTOR); f.write(ext_boot)
        # sectors 9, 10 stay zero
        # sector 11 (checksum) stays zero -- reader skips

        # Backup boot region (sectors 12..23) -- mirror of 0..11
        f.seek(12 * SECTOR); f.write(main_boot)
        for s in range(13, 21):
            f.seek(s * SECTOR); f.write(ext_boot)

        # FAT
        f.seek(FAT_OFF * SECTOR); f.write(fat)

        # Root directory in cluster 2
        f.seek(CLUSHEAP_OFF * SECTOR); f.write(root_dir)

        # File data starts at cluster DATA_CLUSTER
        data_sector = CLUSHEAP_OFF + (DATA_CLUSTER - 2) * (1 << SPC_SHIFT)
        f.seek(data_sector * SECTOR); f.write(payload)


def main():
    args = sys.argv[1:]
    if not args:
        print("usage: mkexfat.py OUT.img [SECTORS] [--elf ELF]",
              file=sys.stderr)
        sys.exit(1)

    out = args.pop(0)
    elf_path = None
    remaining = []
    it = iter(args)
    for a in it:
        if a == "--elf":
            elf_path = next(it)
        else:
            remaining.append(a)
    volume_sectors = int(remaining[0]) if remaining else DEFAULT_SECS

    if elf_path:
        with open(elf_path, "rb") as fh:
            payload = fh.read()
    else:
        payload = b"PEG2" + b"\x00" * 124   # tiny placeholder

    build_image(out, volume_sectors, payload)

    print(f"wrote {out}: {volume_sectors} sectors = "
          f"{volume_sectors * SECTOR // 1024} KiB")
    print(f"  exFAT root /test.elf = {len(payload)} bytes "
          f"({(len(payload)+CLUSTER-1)//CLUSTER} clusters of {CLUSTER}B)")


if __name__ == "__main__":
    main()
