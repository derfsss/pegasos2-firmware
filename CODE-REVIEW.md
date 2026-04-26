# Code review — refactoring + organisation findings

Code review of `machdep/pegasos2/` after v0.5. Findings are
prioritised by impact (P1 = should fix before next release, P2 =
worthwhile cleanup, P3 = nice to have). None of these are
correctness bugs; they're all about long-term maintainability.

## Summary

| priority | category | items |
|----------|----------|-------|
| P1 | Code duplication | 1 |
| P1 | Outdated narrative comments | 1 |
| P2 | File organisation (split candidates) | 2 |
| P2 | Long functions (split candidates) | 1 |
| P2 | Dead / vestigial code | 0 (cleaned in last sweep) |
| P3 | Style consistency | 2 |
| P3 | Documentation polish | 1 |

---

## P1 — Should fix before next release

### P1.1 — `be32` is duplicated across six files

The same 5-line big-endian-32 byte reader appears verbatim in:

```
machdep/pegasos2/of/amiga_ffs.c       :158
machdep/pegasos2/of/amiga_pfs3.c      :185
machdep/pegasos2/of/amiga_rdb.c       :107
machdep/pegasos2/of/amiga_sfs.c       :202
machdep/pegasos2/of/boot_kernel.c     :116    (uses uChar* instead of uByte*)
machdep/pegasos2/of/partition_pkg.c   :112
```

**Recommendation**: introduce `machdep/pegasos2/of/byteswap.h` with
`be16`, `be32`, `be64`, `le16`, `le32`, `le64` as `static inline`
functions. Keep them small and trivial; modern GCC will inline
at -O2. Each consumer drops its local copy and adds
`#include "byteswap.h"`.

Bonus: fixes the typing inconsistency
(`be32(const uByte*)` vs. `be32(const uChar*)` — currently both work
because uByte and uChar are the same type, but it's a smell).

**Effort**: ~30 minutes. Six files lose 6 lines each, gain 1
include line. Net: ~30-line reduction.

### P1.2 — Outdated narrative comments referring to bring-up steps

`grep` for "Commit N" / "Block N/N" found 13 instances:

```
machdep/pegasos2/x86emu_stubs.c    :12   "As of Commit 4 of the OF bring-up..."
machdep/pegasos2/of/boot_kernel.c  :11   "Block 6/N..."
machdep/pegasos2/of/ide_driver.c   :9    "Block 2/N..."
machdep/pegasos2/of/ide_driver.c   :25   "(Block 1/N)..."
machdep/pegasos2/of/ide_driver.c   :391  "Block 2/N gotcha..."
machdep/pegasos2/of/machdep.c      :14   "This is Commit 2 of the multi-commit OF bring-up..."
machdep/pegasos2/of/machdep.c      :18   "Commit 3..."
machdep/pegasos2/of/pci_tree.c     :9    "Block 1/N..."
machdep/pegasos2/of/platform.c     :234  "Commit 6+ replaces..."
machdep/pegasos2/of/platform.c     :769  "Block 6/N..."
machdep/pegasos2/of/machdep.h      :15   "This is Commit 1 of a multi-commit..."
machdep/pegasos2/of/machdep.h      :191  "Commit 1 doesn't..."
```

These are bring-up artefacts — accurate when written, misleading
now that the work they describe is done. A reader of v0.5 source
who hits "this is Commit 1, nothing here is called yet" gets a
contradicting picture from the rest of the codebase.

**Recommendation**: rewrite each as evergreen technical context
(why the file exists, what it provides), the same treatment
applied to amiga_ffs.c / amiga_rdb.c / partition_pkg.c in the
last review pass. Roughly:

| from | to |
|------|----|
| "This is Commit N..." | (delete the line; the file's purpose is in the rest of the comment) |
| "Block N/N -- pegasos2 PCI device-tree installer." | "pegasos2 PCI device-tree installer. Run from install_list[] after install_chosen + install_memory." |
| "Commit 6+ replaces..." | (delete; the placeholder it referred to is gone) |

**Effort**: ~45 minutes across 7 files.

---

## P2 — Worthwhile cleanup

### P2.1 — `partition_pkg.c` mixes two concerns

The file currently contains:

1. **Partition-package methods + RDB walking**
   (lines 1-560, the per-partition device-tree node + its method
   table + `install_partition_packages`).
2. **smart-boot dispatcher**
   (lines 561-761, OS-family classification + per-family loaders +
   the Forth word).

These are loosely related (smart-boot reads properties produced
by partition_pkg) but conceptually separable: the partition
package code is "device-tree integration"; smart-boot is "auto-
boot policy".

**Recommendation**: split into
- `partition_pkg.c` (~560 lines) — device-tree partition packages
  + the install hook.
- `smart_boot.c` (~250 lines) — OS classification, per-family
  loaders, the Forth word.

Both files would `#include` a small new `partition.h` with the
extern declarations they share (`find_first_hd_disk`,
`g_amiga_part_*`).

Trade-off: more files vs. clearer single-responsibility. The split
makes adding a new OS loader (e.g. real Linux/MorphOS) self-
contained; today you'd need to navigate past the partition-package
code to find the dispatch table.

**Effort**: ~1 hour. Mechanical move + Makefile entry + one new
header.

### P2.2 — `platform.c` is a kitchen-sink (927 lines, 5 distinct concerns)

`grep` shows the file holds:

| concern | lines | ~size |
|---------|-------|-------|
| Compile-time NVRAM defaults (`g_nvram[]`) | 100-185 | 86 |
| Default font hook (`machine_font`) | 215-241 | 27 |
| `machine_*` SF callbacks (init_load, init_program, go) | 257-398 | 142 |
| Diagnostic Forth words (`test-ci`, `set-bootargs`, `heap-info`, `test-ci-boot`) | 400-682 | 283 |
| Time-of-day Forth words (`get/set-time-of-day`) | 578-637 | 60 |
| Spec-06 client-services install (`install_pegasos2_ci_services`) | 700-767 | 68 |
| Forth-word + install-list registries | 684-925 | 242 |

**Recommendation** (suggested split, Makefile-driven):

| new file | content |
|----------|---------|
| `platform.c` (smaller) | NVRAM defaults, font, the two registries (`init_list[]`, `install_list[]`) |
| `machine_hooks.c` | machine_init_load / machine_init_program / machine_go |
| `forth_diag.c` | Diagnostic Forth words (test-ci, set-bootargs, heap-info, test-ci-boot) |
| `rtc_word.c` | get-time-of-day / set-time-of-day Forth words + the CI-services-side install |

This trims `platform.c` to ~250 lines — the registry that ties
everything together, easier to read at a glance. Each split file
is ~100-300 lines, focused.

Note: this is a bigger change. It'll need:
- 4 new Makefile entries
- Forward-declarations consolidated into one new `pegasos2_words.h`
  (drops the 8 scattered `extern Retcode f_...` lines from platform.c)
- Per-file copyright headers

**Effort**: ~2 hours. Lots of mechanical work; risk is low because
the build either compiles or doesn't.

### P2.3 — `phase1_c_main` is 298 lines

It's a sequential bring-up function — DRAM test, PCI enum, x86emu
self-test, option-ROM POST, clock calibration, decrementer test,
syscall test — and reads top-down like a startup script.

It's *long*, but it's not *complex*. There's no nested control
flow; each subsystem is initialised, prints a status line, and
moves on. Splitting into helpers would actually reduce
readability for the sequential narrative.

**Recommendation** (mild): leave the main function alone but
consider extracting the **option-ROM POST** block (the one that
loads the bochs-VGA ROM into the x86emu and runs it; ~50 lines)
into a helper `run_option_rom_post()` because that's the part
most likely to be reused or replaced. Otherwise no change.

**Effort**: ~15 minutes if you do the option-ROM extraction; zero
if you decide to leave it.

---

## P3 — Nice to have

### P3.1 — `extern Retcode f_*` declarations live in `platform.c`

Eight forward-declarations of Forth words live near the top of
`platform.c` (lines 552-560):

```c
extern Retcode f_boot_kernel(Environ *e);
extern Retcode f_test_boot(Environ *e);
extern Retcode f_test_boot_bad(Environ *e);
extern Retcode f_ls_pci(Environ *e);
extern Retcode f_test_ide_probe(Environ *e);
extern Retcode f_test_read_block(Environ *e);
extern Retcode f_test_iso_ls(Environ *e);
extern Retcode f_test_aliases(Environ *e);
extern Retcode f_smart_boot(Environ *e);
```

They're spread across the source files of those words but get
"re-declared" here for the registry table. Tolerable today; if
P2.2 (platform.c split) lands, these belong in a new
`pegasos2_words.h` together with declarations for any new Forth
words.

**Effort**: ~10 minutes.

### P3.2 — Naming style is mostly consistent but there are a few one-offs

Most of our code follows:

- types: `PascalCase` (matches SF: `Cell`, `Retcode`, `Package`)
- function names: `snake_case` (matches SF: `prop_set_str`,
  `get_device_name`)
- globals: `g_snake_case` (e.g. `g_e`, `g_machine_memory`)
- static module functions: `snake_case` (no prefix needed)
- per-file Forth-word handlers: `f_snake_case` (e.g. `f_smart_boot`)
- per-file install hooks: `install_snake_case` (e.g.
  `install_partition_packages`)

A few outliers:

- `machdep/pegasos2/of/partition_pkg.c::loader_amigaos`,
  `loader_linux`, `loader_morphos` — module-private but missing
  the `static` keyword would be ambiguous from outside; they
  ARE static. OK as-is.
- `machdep/pegasos2/of/partition_pkg.c::os_family_match`,
  `pick_partition_for_family`, `classify_dostype` — fine.
- The `be32` typing variation across files (uByte vs uChar) —
  fixed by the P1.1 shared header.

**Recommendation**: no change today; revisit if P2 splits land
(naming becomes more visible during a refactor).

### P3.3 — `BOOT.md` and `FEATURES.md` overlap somewhat

Both documents describe the auto-boot model and smart-boot,
each from a different angle (BOOT.md from the firmware-internal
side, FEATURES.md from the user side). The overlap is currently
~80 lines.

**Recommendation**: leave as-is. The two audiences are different
(implementer vs. user) and benefit from each having a complete
self-contained description. If divergence appears during future
edits, consolidate then.

---

## Other observations

### What's already good

- **Per-file copyright + license headers** consistent across all
  our code; license-clause-3 (source-availability) preserved
  from upstream.
- **Static visibility**: every helper that doesn't need to be
  global has `static`. (One spot tightened in last pass:
  `find_first_hd_disk` is intentionally non-static so smart-boot
  can call it across files.)
- **Memory safety**: every `malloc` is paired with a matching
  `free` or transferred ownership to a long-lived structure
  (e.g. `p->self`); no leaks observed.
- **Error paths**: every CI dispatch path either succeeds or
  returns a documented error. smart-boot has explicit fall-back
  to `boot` if every priority entry exhausts.
- **Endianness**: explicit `be32`/etc. helpers used everywhere
  for on-disk and on-wire data. No reliance on host endianness
  ambiguity.
- **Hard-coded magic numbers**: addressed via #defined constants
  (`RDB_MAGIC_RDSK`, `T_HEADER`, `OFF_SEC_TYPE`, etc.) with
  references to the relevant spec.
- **NVRAM defaults are CONFIG_TARGET-aware**: cleanly
  parameterised, no surprise at runtime.
- **Build flags documented in FEATURES.md and `Makefile`
  comments**.

### Build system

The Makefile is ~400 lines and handles:
- Phase 1 + OF objs (PHASE1_OBJS, OF_SUBSET, OF_MACHDEP_OBJS)
- Per-file CFLAGS overrides for unusual sources (dosfat, exe.c,
  fs/ subdir resolution)
- CONFIG_TARGET branching
- Linker script + objcopy to firmware-raw.bin
- Phony targets (info, clean, of-test)

It's verbose but explicit. No major issues. Could split into
sub-makefiles but the gain is small for a project this size.

---

## Suggested order if acting on findings

Strongly recommend:
1. **P1.1** (be32 dedup) — 30 min, contained.
2. **P1.2** (outdated comments) — 45 min, contained.

Optional (do if a structural pass is welcome):
3. **P2.1** (split smart-boot) — 1 hour, low risk.
4. **P2.2** (split platform.c) — 2 hours, larger blast radius;
   would benefit most from a clean dedicated commit.
5. **P3.1** (consolidate Forth-word externs) — naturally falls
   out of P2.2.

Skip:
6. **P2.3** (phase1_c_main split) — readability cost > cleanliness
   gain.
7. **P3.2** (naming) — already consistent.
8. **P3.3** (doc overlap) — different audiences justify it.

---

## Appendix — files reviewed

```
machdep/pegasos2/phase1.c          (350 lines)
machdep/pegasos2/panic.c           (102)
machdep/pegasos2/timer.c            (80)
machdep/pegasos2/uart16550.c        (79)
machdep/pegasos2/extint.c          (186)
machdep/pegasos2/vt8231_rtc.c      (140)
machdep/pegasos2/mv64361.c         (111)
machdep/pegasos2/vt8231.c          (243)
machdep/pegasos2/x86_glue.c        (247)
machdep/pegasos2/x86emu_stubs.c    (small)
machdep/pegasos2/pci_walker.c      (549)
machdep/pegasos2/pegasos2.h         (76)
machdep/pegasos2/io.h               (90)
machdep/pegasos2/pci.h              (84)
machdep/pegasos2/mv64361.h         (122)
machdep/pegasos2/vt8231.h          (159)
machdep/pegasos2/vt8231_rtc.h       (99)

machdep/pegasos2/of/machdep.h      (204)
machdep/pegasos2/of/dosfat_compat.h (small)

machdep/pegasos2/of/machdep.c      (722)
machdep/pegasos2/of/platform.c     (927)  <-- biggest
machdep/pegasos2/of/pci_tree.c     (958)
machdep/pegasos2/of/ide_driver.c   (746)
machdep/pegasos2/of/partition_pkg.c (761)
machdep/pegasos2/of/amiga_rdb.c    (444)
machdep/pegasos2/of/amiga_ffs.c    (703)
machdep/pegasos2/of/amiga_sfs.c    (800)
machdep/pegasos2/of/amiga_pfs3.c   (633)
machdep/pegasos2/of/fs_exfat.c     (631)
machdep/pegasos2/of/boot_kernel.c  (493)
machdep/pegasos2/of/ci_entry.c     (505)

Total: 11,616 lines across 30 files
```
