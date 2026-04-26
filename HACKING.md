# HACKING — extending the firmware

How to add new functionality. Three concrete tutorials covering
the most common contribution shapes:

- [Adding a Forth word](#adding-a-forth-word)
- [Adding a filesystem reader](#adding-a-filesystem-reader)
- [Adding an OS loader](#adding-an-os-loader)

Plus a [filesystem-reader internals walkthrough](#filesystem-reader-internals)
that explains the pattern the existing readers (FFS, SFS, PFS3,
exFAT) all follow.

For the firmware's overall architecture and the boot path itself
see [BOOT.md](BOOT.md). For the layout of every file under
`machdep/pegasos2/` see the project-memory file
`reference_file_layout.md` or just open the directory.

---

## Adding a Forth word

Five-step recipe. The example is a hypothetical `pci-rescan`
word that re-walks the PCI tree at runtime.

### 1. Pick the right file

Topic-specific files live in `machdep/pegasos2/of/`:

| word topic | file |
|------------|------|
| diagnostic / smoke-test | `forth_diag.c` |
| RTC / time-of-day | `rtc_word.c` |
| SF machdep contract (machine_*) | `machine_hooks.c` |
| OS-priority dispatcher | `smart_boot.c` |
| boot / ELF loader | `boot_kernel.c` |
| PCI tree | `pci_tree.c` |
| IDE / disk | `ide_driver.c` |
| RDB partition packages | `partition_pkg.c` |

If your word doesn't fit any of those, add a new file. Don't
add the body to `platform.c` — that file is the registry only.

### 2. Write the body

Forth-word bodies are C functions returning `Retcode` and taking
`Environ *e`. SF wraps them via `CC()`:

```c
#include "defs.h"

CC(f_pci_rescan)
{
    /* Stack effect: ( -- )
     * Re-walk both PCI host bridges and print every device. */
    extern void pci_walk(void);
    pci_walk();
    return NO_ERROR;
}
```

Conventions:

- Function name `f_<word>_<name>` for diagnostic words,
  `f_<name>` for production words.
- `IFCKSP(e, n_pop, n_push)` at the top if the word touches the
  Forth data stack. SF aborts with an under/overflow check
  before your body runs.
- `POP(e, var)`, `POPT(e, var, type)` to consume args; `PUSH(e,
  val)`, `PUSHP(e, ptr)` to leave returns.
- Print user-visible output via `cprintf(e, fmt, ...)`. Don't
  `printf` — there's no hosted libc.

### 3. Forward-declare in `pegasos2_words.h`

```c
/* Hardware-probe words */
extern Retcode f_ls_pci(Environ *e);              /* pci_tree.c */
extern Retcode f_pci_rescan(Environ *e);          /* pci_tree.c */
```

This is what lets `platform.c::init_pegasos2[]` reference your
word without re-declaring the prototype.

### 4. Register in `init_pegasos2[]`

In `machdep/pegasos2/of/platform.c`:

```c
static const Initentry init_pegasos2[] = {
    /* ... existing entries ... */
    { (Byte *)"pci-rescan", f_pci_rescan, INVALID_FCODE,
      F_NONE, T_FUNC HELP(
            "(--)  re-walk PCI tree and print every device") },
    { NULL, NULL, INVALID_FCODE, F_NONE, T_FUNC HELP("") }
};
```

The HELP string is shown when the user types `pci-rescan ?` at
the `ok` prompt. Keep it under one line; mention the stack
effect at the start `(--)`.

### 5. Wire into the Makefile

If your word body lives in a *new* file, add an OBJS entry +
build rule:

```makefile
OF_MACHDEP_OBJS := \
    ... \
    $(BUILD)/of_my_new_file.o

$(BUILD)/of_my_new_file.o: $(SF_MACHDEP)/my_new_file.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(MACHDEP) -c $< -o $@
```

If you put the body in an existing file, no Makefile change is
needed.

### 6. Document

- Add a row to **FEATURES.md**'s "Custom Forth words" table
  with the same stack effect + description.
- Update **README.md** if it's user-facing (most diagnostic
  words aren't).

### 7. Test

```
ok pci-rescan
```

If the word's effect is observable on serial / device tree, also
add a smoke check to your PR description showing the before/after.

---

## Adding a filesystem reader

The firmware reads OS bootloader files (`amigaboot.of`, etc.) via
a small Filesys driver registry. Each driver registers itself in
`g_filesys[]` in `platform.c` and gets called by SF's `file_system()`
on every load attempt.

### Where existing readers live

| reader | DosType | source |
|--------|---------|--------|
| amiga_rdb (partition table only) | `RDSK` | `amiga_rdb.c` |
| amiga_ffs | `DOS\\0..\\7` | `amiga_ffs.c` |
| amiga_sfs | `SFS\\0`, `SFS\\2` | `amiga_sfs.c` |
| amiga_pfs3 | `PFS\\1..\\3`, `AFS\\1` | `amiga_pfs3.c` |
| fs_exfat | exFAT | `fs_exfat.c` |
| iso9660 | CD media | upstream |
| dosfat | FAT12/16/32 | upstream |
| ext2fs | Linux ext2/3/4 | upstream |

### Reader contract

A reader is a single `struct Filesys` with `name` and an
`action(Environ *, Filesys_action, ...)` callback. The action is
called three ways:

- **`FS_PROBE`** — does this filesystem exist on the supplied
  byte range? Return `R_END` + write the FS name into `retbuf`
  if yes, `E_NO_FILESYS` if no.
- **`FS_LIST`** — list files at `path`. The reader prints a
  human-readable listing via `cprintf`.
- **`FS_LOAD`** — read the file at `path` into `retbuf`, set
  `*val` to the byte count.

For each call the reader gets:
- `disk` — the parent device's Instance.
- `path` — caller's filesystem-relative path.
- `loc` — byte offset on the disk where the partition starts.
- `start` — kept for compatibility; usually unused.
- `buf` / `size` — caller-supplied scratch (often only 512
  bytes; allocate your own if you need more — see
  `amiga_ffs.c`'s `g_ffs_iobuf`).
- `retbuf` — for `FS_LOAD`, the destination for file contents.
- `val` — for `FS_LOAD`, where to write the actual byte count.

### Reader skeleton

```c
/* machdep/pegasos2/of/fs_myfs.c */

#include "defs.h"
#include "fs.h"
#include "byteswap.h"

extern uLong g_amiga_part_byte_size;
extern uInt  g_amiga_part_block_size;

#define MYFS_MAGIC 0x4d594653u   /* "MYFS" */

Retcode
fs_myfs(Environ *e, Filesys_action what, Instance *disk,
        Byte *path, uLong loc, uLong start_unused,
        uByte *buf, uInt size, uByte *retbuf, uLong *val)
{
    (void)start_unused;
    if (size < 512) return E_BLOCKSIZE;

    /* Read partition's first block. amiga_rdb publishes the
     * partition byte size + FS-block size if it recursed into
     * us; honour those when present. */
    Retcode r = filesys_read_bytes(e, disk, loc, 512, buf);
    if (r != NO_ERROR) return E_NO_FILESYS;

    if (be32(buf) != MYFS_MAGIC) return E_NO_FILESYS;

    switch (what) {
    case FS_PROBE:
        strcat((char *)retbuf, ",myfs");
        *val = loc;
        return R_END;

    case FS_LIST:
        /* walk the FS structure, cprintf each entry */
        return R_END;

    case FS_LOAD: {
        /* parse path, read file, copy to retbuf, *val = bytes_read */
        return NO_ERROR;
    }

    default:
        return E_NO_FILESYS;
    }
}

Filesys g_fs_myfs = { "myfs", fs_myfs };
```

### Wiring in

1. **Forward-declare** in `platform.c` next to the other
   `extern Filesys g_*;` lines:

   ```c
   extern Filesys g_fs_myfs;          /* machdep/pegasos2/of/fs_myfs.c */
   ```

2. **Add to `g_filesys[]`** in dispatch order. Partition parsers
   first, whole-disk readers after:

   ```c
   Filesys *g_filesys[] = {
       &g_dos_partition,
       &g_amiga_rdb,
       &g_amiga_ffs,
       /* ... */
       &g_fs_myfs,           /* <-- new */
       NULL
   };
   ```

3. **Makefile**: add an OBJS entry + build rule next to the
   other `of_amiga_*.o` rules.

4. **Test** by booting from a disk formatted with your FS and
   trying `boot hd:0 /someFile` at the `ok` prompt.

5. **Update FEATURES.md** § Filesystem support table.

---

## Adding an OS loader

For non-AmigaOS systems (Linux, MorphOS, AOS3 ...) the firmware
provides smart-boot, a priority-ordered dispatcher in
`machdep/pegasos2/of/smart_boot.c`. Adding a loader is a
~30-line edit.

### What smart-boot does

At each ok-prompt boot it:

1. Reads `boot-os-priority` NVRAM (default `amigaos,morphos,linux`).
2. Walks every RDB partition exposed by `install_partition_packages`.
3. Classifies each by its `DosType` into a family (amigaos /
   linux / morphos).
4. For each priority entry, picks the highest-`BootPri`
   matching partition and calls the per-family loader.
5. If the loader returns NO_ERROR, control transferred (don't
   come back). Otherwise smart-boot tries the next family.
6. If everything fails, falls back to plain `boot`.

### Recipe for a new family

Suppose you're adding a NetBSD loader for `BSD\?` partitions.

#### 1. Map the DosType

In `smart_boot.c::classify_dostype()`:

```c
static const char *
classify_dostype(uInt dostype)
{
    uInt high3 = dostype & 0xFFFFFF00u;
    if (high3 == 0x444F5300u) return "amigaos";
    /* ... existing ... */
    if (high3 == 0x42534400u) return "netbsd";   /* NEW: BSD\* */
    return "";
}
```

If your OS uses an exact 4-byte tag rather than a prefix, fall
through to the explicit-match block below the high3 cases.

#### 2. Accept the family token

In `smart_boot.c::os_family_match()`:

```c
if (strcmp(family, "netbsd") == 0) {
    if (slen == 6 && memcmp(s, "netbsd", 6) == 0) return 1;
    /* aliases the user might type */
    if (slen == 3 && memcmp(s, "bsd", 3) == 0)    return 1;
}
```

#### 3. Implement the loader

```c
static Retcode
loader_netbsd(Environ *e, Package *part)
{
    /* Three options for the body:
     *
     * (a) Call interp_text("boot hd:N /netbsd") if your OS
     *     boots straight from a kernel ELF on disk.
     *
     * (b) Open the partition, find the loader file, set up
     *     OS-specific bootargs, machine_jump_os to the entry.
     *
     * (c) Print a "not implemented" notice and return
     *     E_UNSUPPORTED_FILESYS so smart-boot tries the next
     *     family in the priority list.
     */
    (void)part;
    cprintf(e, "smart-boot: NetBSD loader not implemented yet\n");
    return E_UNSUPPORTED_FILESYS;
}
```

#### 4. Wire into the dispatch chain

In `smart_boot.c::f_smart_boot()`'s if/else cascade:

```c
Retcode r;
if (strcmp(family, "amigaos") == 0)
    r = loader_amigaos(e, cand);
else if (strcmp(family, "linux") == 0)
    r = loader_linux(e, cand);
else if (strcmp(family, "netbsd") == 0)            /* NEW */
    r = loader_netbsd(e, cand);
else
    r = loader_morphos(e, cand);
```

#### 5. Update the priority-list parser

Same file:

```c
if      (os_family_match(tok_start, tok_len, "amigaos"))
    family = "amigaos";
else if (os_family_match(tok_start, tok_len, "linux"))
    family = "linux";
else if (os_family_match(tok_start, tok_len, "netbsd"))    /* NEW */
    family = "netbsd";
else if (os_family_match(tok_start, tok_len, "morphos"))
    family = "morphos";
```

#### 6. Document

Update the family table in **FEATURES.md** § smart-boot
dispatcher and the README.md "Booting X manually" section if
applicable.

---

## Filesystem reader internals

Walkthrough of how the existing readers are structured, using
`amiga_ffs.c` as the example. Pattern is the same for SFS,
PFS3, and exFAT.

### The four big blocks

Every reader has the same four-block layout:

1. **Block-size + offset constants** — runtime-`#define`-ed
   field offsets within a block. For dynamic-block-size readers
   like FFS (which can be 512 *or* 1024), these are
   expressions on a `g_xxx.bsize` runtime variable rather than
   compile-time numbers.

2. **Per-FS state singleton** — a `static struct { ... } g_xxx`
   holding everything cached across calls inside one mount:
   the disk Instance, partition byte offset, root block,
   case-folding flags, etc. Single-threaded firmware, single
   outstanding mount, so a global is safe.

3. **On-disk structure helpers** — `read_block`, `checksum_ok`,
   name-hashing, `walk_path`, etc. These are private static.

4. **The `action()` callback** — switches on `FS_PROBE` /
   `FS_LIST` / `FS_LOAD`, sets up state, calls into the
   helpers. Plus a public `Filesys g_xxx_fs = { "xxx", action };`
   at the bottom.

### How a load actually flows

User types `boot hd:0 amigaboot.of`. SF:

1. Resolves `hd:0` via `/aliases/hd` → `/pci@.../disk@0,0`.
2. Calls the disk-label package's `load` method
   (upstream/disklbl.c).
3. disklbl calls `file_system(FS_LOAD, ..., loc=0, ...)`.
4. `file_system` (upstream/fs/fs.c) reads partition byte 0 into
   `buf`, then iterates `g_filesys[]` calling each driver's
   action with `FS_LOAD`.
5. amiga_rdb (registered first among Amiga readers) sees the
   `RDSK` magic, walks the PartitionBlock chain, picks the
   selected partition's byte range, **publishes geometry hints**
   `g_amiga_part_byte_size` + `g_amiga_part_block_size`, and
   recursively calls `file_system(FS_LOAD, ..., loc=part_off,
   ...)`.
6. The recursion's iteration of `g_filesys[]` reaches
   amiga_ffs. amiga_ffs reads the boot block at `loc`, sees
   `DOS\\?` magic, accepts the partition, computes the root
   block from the published geometry hints, and walks the
   path → reads file → fills `retbuf` → `*val = file_size`.

### Geometry hints — `g_amiga_part_byte_size` + `g_amiga_part_block_size`

Set by amiga_rdb just before recursing into `file_system()`,
read by amiga_ffs / amiga_sfs / amiga_pfs3 in their probe paths.
Without them the FS readers can't compute root-block positions
exactly and have to probe a list of likely offsets — which
risks reading past partition end on geometries the probe table
doesn't cover.

If you add a partition parser that recurses into FS readers,
publish these two globals before each `file_system()` call. If
you add an FS reader, prefer the hints over a probe table when
they're set.

### The override-the-passed-buf pattern

SF's disklbl passes `buf` sized to the disk's logical-block
size — usually 512. Some FS readers need bigger blocks (FFS2 is
1024). Reading bigger-than-`size` into the caller's `buf`
overflows the heap and triggers `free: head/tail guard
trashed` later.

amiga_ffs handles this by overriding the parameter on entry:

```c
static uByte g_ffs_iobuf[MAX_BSIZE];
buf = g_ffs_iobuf;
```

For the rest of the reader, `buf` points at our own 4 KiB
buffer. Single-threaded firmware, single outstanding mount —
static is safe.

If you add an FS reader that uses bigger blocks than 512, do
the same.

### Property-set lifetime gotcha

`add_property(props, name, len, ptr, plen)` only **stashes the
pointer**, it doesn't memcpy. If `ptr` is a stack local, the
property reads back garbage as soon as the install function
returns and the stack is reused.

Use `set_property()` instead — it allocates via `prop_alloc` +
memcpy's the bytes. partition_pkg.c learned this the hard way
when its `dostype` blob was stack-local; readers that publish
binary blob properties should default to `set_property`.

### Testing FS readers

Three layers of test, easiest first:

1. **Probe** — does the reader recognise its own DosType and
   reject every other? Boot with a disk that has *only* your
   partition type; smart-boot should pick it up.

2. **List root** — `dev /pci@.../disk@0,0/<DH#>` then `list-files .`
   should print the root-directory entries.

3. **Load** — `boot hd:0 /<some_known_file>` should load it
   into memory. The simplest validation is loading a tiny
   ELF and seeing SF's `boot` parse the magic correctly. A
   richer one is loading an actual amigaboot.of equivalent
   and watching it run.

For diagnosing read failures, the QEMU IDE trace is invaluable:

```bash
qemu-system-ppc -M pegasos2 -m 1024 \
    -bios build/firmware-raw.bin \
    -drive file=test.raw,format=raw,if=none,id=disk \
    -device ide-hd,drive=disk,bus=ide.0 \
    -trace events=ide_trace.cfg \
    --trace file=ide.log \
    -serial mon:stdio
```

with `ide_trace.cfg`:

```
ide_bus_exec_cmd
ide_sector_read
ide_reset
```

`ide.log` shows every ATA command the firmware issues. Diff
against a known-good run (e.g. SFS reader on hd0) to spot where
your reader diverges.
