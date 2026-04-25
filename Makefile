# Pegasos II clean-room firmware -- top-level build
#
# This Makefile runs as a plain Linux build. Driver expectation:
#
#   From WSL Ubuntu (default):
#     make                 # build firmware-raw.bin
#     make clean           # remove build/
#     make info            # show toolchain versions
#
#   From Windows (MSYS2 / PowerShell / cmd):
#     wsl.exe -- make -C /mnt/c/msys64/home/rich_/Projects/Pegasos2-bios-impl
#
#   Required packages (Ubuntu 24.04):
#     gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu
#
# Output: build/firmware-raw.bin, exactly 524288 bytes (512 KiB),
# ready to flash or hand to QEMU -bios.

PREFIX  := powerpc-linux-gnu-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
OBJDUMP := $(PREFIX)objdump

BUILD       := build
MACHDEP     := machdep/pegasos2
X86EMU      := upstream/x86emu
SF          := upstream/smartfirmware/bin/of
SF_MACHDEP  := $(MACHDEP)/of

# CONFIG_TARGET selects the runtime environment: `qemu` (default) or
# `hw`. Identical code paths -- everything that differs (M48T59 NVRAM
# presence, W83194 SMBus FSB probe, SM501 framebuffer, etc.) is
# detected at runtime via probing. This flag only branches the
# compile-time NVRAM defaults baked into init_options_from_nvram,
# since on QEMU the M48T59 chip isn't instantiated and defaults
# load fresh every boot, whereas real HW has battery-backed M48T59
# that persists user `setenv` changes across reboots.
#
# Defaults exposed:
#   qemu : auto-boot? = true,  auto-boot-timeout = 3000 ms
#          (boots the install-time boot-command after a 3-sec
#           countdown that any keypress aborts)
#   hw   : auto-boot? = false, auto-boot-timeout = 5000 ms
#          (drops to ok prompt; user enables auto-boot via
#           `setenv auto-boot? true` and the change persists)
#
# Build:
#   make                    # CONFIG_TARGET=qemu (default)
#   make CONFIG_TARGET=hw   # firmware tuned for real Pegasos II
CONFIG_TARGET ?= qemu
ifeq ($(CONFIG_TARGET),qemu)
TARGET_CFLAGS := -DPEGASOS_TARGET_QEMU=1
else ifeq ($(CONFIG_TARGET),hw)
TARGET_CFLAGS := -DPEGASOS_TARGET_HW=1
else
$(error CONFIG_TARGET must be 'qemu' or 'hw' (got '$(CONFIG_TARGET)'))
endif

# Bare-metal flags. No Linux runtime, no built-ins, big-endian
# 32-bit PowerPC targeting the 7447/7450 family.
CFLAGS := \
    -m32 -mbig-endian -mcpu=7450 \
    -msoft-float -mno-altivec \
    -ffreestanding -fno-builtin \
    -fno-pic -fno-stack-protector \
    -fno-asynchronous-unwind-tables \
    -O2 -g -std=gnu11 \
    -Wall -Wextra -Werror \
    $(TARGET_CFLAGS) \
    -I$(MACHDEP)/x86compat \
    -I$(MACHDEP) \
    -I$(X86EMU)/include

ASFLAGS := $(CFLAGS) -Wa,-mregnames

LDFLAGS := \
    -nostdlib -static \
    -Wl,-T,$(MACHDEP)/firmware.ld \
    -Wl,--build-id=none \
    -Wl,-Map=$(BUILD)/firmware.map

# libgcc provides __udivdi3 / __umoddi3 (64-bit unsigned division +
# modulo) referenced from deblock.c's seek method and any other 64-
# bit arithmetic that appears in pulled upstream code. The PowerPC
# soft-float cross-toolchain ships these as part of libgcc.a and they
# are compatible with our BSD licensing via GCC's runtime library
# exception. Link last so our own symbols win where they exist.
LIBS := -lgcc

PHASE1_OBJS := \
    $(BUILD)/reset.o \
    $(BUILD)/exceptions.o \
    $(BUILD)/panic.o \
    $(BUILD)/syscall.o \
    $(BUILD)/timer.o \
    $(BUILD)/phase1.o \
    $(BUILD)/uart16550.o \
    $(BUILD)/mv64361.o \
    $(BUILD)/vt8231.o \
    $(BUILD)/m48t59.o \
    $(BUILD)/extint.o \
    $(BUILD)/pci_walker.o \
    $(BUILD)/x86_glue.o \
    $(BUILD)/x86emu_stubs.o \
    $(BUILD)/x86emu_ops.o \
    $(BUILD)/x86emu_ops2.o \
    $(BUILD)/x86emu_prim_ops.o \
    $(BUILD)/x86emu_decode.o \
    $(BUILD)/x86emu_sys.o \
    $(BUILD)/x86emu_debug.o

# Commit-4 addition: now firmware.bin contains the SmartFirmware OF
# runtime too. phase1_c_main() hands off to SF's main() after its
# existing self-tests. OF_SUBSET and OF_MACHDEP_OBJS are defined
# further down in this file.
OBJS := $(PHASE1_OBJS)

FIRMWARE := $(BUILD)/firmware-raw.bin
ELF      := $(BUILD)/firmware.elf
FLASH_SIZE := 524288

.PHONY: all clean info disasm of-sf-subset of-test
.SUFFIXES:

all: $(FIRMWARE)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(MACHDEP)/%.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: $(MACHDEP)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Vendored x86 emulator core. The sources themselves are copied verbatim
# from U-Boot (SciTech license) and compile under -Wno-* relaxations so
# we don't have to patch their style.
X86EMU_WARNS := \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
    -Wno-sign-compare -Wno-missing-field-initializers \
    -Wno-unused-but-set-variable \
    -D__KERNEL__ -U_FORTIFY_SOURCE
$(BUILD)/x86emu_%.o: $(X86EMU)/x86emu/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(X86EMU_WARNS) -c $< -o $@

# -----------------------------------------------------------------
# Vendored SmartFirmware OF runtime (CodeGen source license; see
# LICENSES/CodeGen-smartfirmware.txt and upstream/smartfirmware/
# COPYRIGHT). This is Commit 1 of the OF bring-up: compile a tiny
# subset of SF's portable core against a freshly-authored Pegasos2
# machdep.h to prove the header setup is sound. Nothing here is
# linked into firmware.bin yet -- `make of-sf-subset` builds only
# the object files and the default `make` ignores them.
#
# Build-option posture:
#   -I $(SF_MACHDEP) first so machdep.h resolves to our Pegasos2
#     version rather than any upstream stub.
#   -I $(SF) so SF's internal stdlib.h / ctype.h / string.h / defs.h
#     are found before any system header of the same name would be.
#   -D_FORTIFY_SOURCE=0 defensively; we're freestanding and have no
#     libc-side __printf_chk / __fortify_fail to satisfy.
# Do NOT define:
#   DEBUG      (enables SF's DPRINTF macro -- ~70 KiB of malloc /
#               add_prop / resolve_path trace output on every boot.
#               Re-enable only when diagnosing a specific OF issue.)
#   STANDALONE (would pull in <stdio.h> and a hosted runtime)
#   MAIN       (would pull in <stdio.h> inside alloc.c)
#   LITTLE_ENDIAN, SF_64BIT (neither applies to 32-bit big-endian PPC)
SF_CFLAGS := \
    -m32 -mbig-endian -mcpu=7450 \
    -msoft-float -mno-altivec \
    -ffreestanding -fno-builtin \
    -fno-pic -fno-stack-protector \
    -fno-asynchronous-unwind-tables \
    -O2 -g -std=gnu11 \
    -Wall \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
    -Wno-sign-compare -Wno-missing-field-initializers \
    -Wno-unused-but-set-variable -Wno-parentheses \
    -Wno-implicit-fallthrough -Wno-char-subscripts \
    -Wno-pointer-sign -Wno-maybe-uninitialized \
    -Wno-shift-count-overflow -Wno-address \
    $(TARGET_CFLAGS) \
    -I$(SF_MACHDEP) \
    -I$(SF) \
    -I$(SF)/exe \
    -U_FORTIFY_SOURCE \
    $(EXTRA_CFLAGS)

$(BUILD)/of_%.o: $(SF)/%.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -c $< -o $@

# OF_SUBSET: the "minimum viable ok" closure of SmartFirmware's
# portable core.  Grew from 3 files in Commit 1 to the full core in
# Commit 3.  Deferred out (to be added only when their consumer
# arrives):
#   fb.c (framebuffer), deblock.c disklbl.c (block/disk),
#   obptftp.c (TFTP), token.c (Fcode tokenizer dev tool),
#   client.c (IEEE-1275 client interface -- spec 06),
#   sun.c stlb.c (Sun OBP compat), jedec.c flash_*.c gflash.c
#   (flash driver), exe/* (ELF/COFF/aout loaders), fs/*, pci/*,
#   isa/*, scsi/*, usb/*.
OF_SUBSET := \
    $(BUILD)/of_errs.o \
    $(BUILD)/of_stdlib.o \
    $(BUILD)/of_alloc.o \
    $(BUILD)/of_failsafe.o \
    $(BUILD)/of_main.o \
    $(BUILD)/of_table.o \
    $(BUILD)/of_forth.o \
    $(BUILD)/of_exec.o \
    $(BUILD)/of_funcs.o \
    $(BUILD)/of_funcs64.o \
    $(BUILD)/of_cmdio.o \
    $(BUILD)/of_control.o \
    $(BUILD)/of_display.o \
    $(BUILD)/of_admin.o \
    $(BUILD)/of_debug.o \
    $(BUILD)/of_packages.o \
    $(BUILD)/of_other.o \
    $(BUILD)/of_device.o \
    $(BUILD)/of_chosen.o \
    $(BUILD)/of_memory.o \
    $(BUILD)/of_root.o \
    $(BUILD)/of_cpu-ppc.o \
    $(BUILD)/of_nvedit.o \
    $(BUILD)/of_nvram.o \
    $(BUILD)/of_client.o \
    $(BUILD)/of_atadisk.o \
    $(BUILD)/of_deblock.o \
    $(BUILD)/of_disklbl.o \
    $(BUILD)/of_fs.o \
    $(BUILD)/of_iso9660.o \
    $(BUILD)/of_exe.o \
    $(BUILD)/of_dospart.o \
    $(BUILD)/of_dosfat.o \
    $(BUILD)/of_ext2fs.o

# isa/atadisk.c: generic ATA/ATAPI disk driver pulled in Block 2/N.
# Compiled with an extra include path for ../scsi/scsi.h (which its
# source uses for ATAPI CDB structs). The bus-layer glue that
# upstream isa/ata.c provides is replaced by our
# machdep/pegasos2/of/ide_driver.c (native-mode BARs + PCI I/O
# window translation).
$(BUILD)/of_atadisk.o: $(SF)/isa/atadisk.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(SF)/scsi -c $< -o $@

# Block 3/N: deblock.c (variable-size I/O on top of block devices)
# and disklbl.c (disk-label partition handler). disklbl.c needs
# fs/fs.h on the include path because it calls file_system() for
# its load / list-files methods.
$(BUILD)/of_deblock.o: $(SF)/deblock.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -c $< -o $@

$(BUILD)/of_disklbl.o: $(SF)/disklbl.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(SF)/fs -c $< -o $@

# fs/fs.c: filesystem dispatcher (file_system, filesys_read_bytes).
# Lives in fs/ subdir; its own source already finds fs.h via the
# compiler's current-source-file include search. Other callers
# (like disklbl.c) include via -I$(SF)/fs.
$(BUILD)/of_fs.o: $(SF)/fs/fs.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -c $< -o $@

# Block 4/N: fs/iso9660.c (ISO9660 + Joliet reader). Exports
# Filesys g_iso9660_fs; we add &g_iso9660_fs to g_filesys[] in
# platform.c so disklbl/fs.c's file_system() dispatch routes
# FS_PROBE/FS_LIST/FS_LOAD to it.
$(BUILD)/of_iso9660.o: $(SF)/fs/iso9660.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -c $< -o $@

# Block 6/N: exe/exe.c (exec_is_exec, exec_load, exec_length,
# exec_load_symbols, exec_addr2sym, exec_sym2addr). Dispatcher over
# our machdep-supplied g_exec_list[]. Lives in exe/ subdir; its
# "exe.h" include resolves via source-dir lookup.
$(BUILD)/of_exe.o: $(SF)/exe/exe.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -c $< -o $@

# Arc FS-A: PC filesystems.
#   fs/dospart.c  -- MBR partition table parser (Filesys that
#                    recurses into partitions for downstream FS probing)
#   fs/dosfat.c   -- FAT12/16/32 reader with LFN support
#   fs/ext2fs.c   -- Linux ext2 reader
# All live in fs/ subdir; quoted includes resolve via source-dir
# lookup. No extra -I needed. bsdpart.c (BSD disklabel) is NOT
# pulled -- no BSD target.
$(BUILD)/of_dospart.o: $(SF)/fs/dospart.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -c $< -o $@

$(BUILD)/of_dosfat.o: $(SF)/fs/dosfat.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -include $(SF_MACHDEP)/dosfat_compat.h \
	    -c $< -o $@

$(BUILD)/of_ext2fs.o: $(SF)/fs/ext2fs.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -c $< -o $@

.PHONY: of-sf-subset
of-sf-subset: $(OF_SUBSET)
	@echo "  OF    subset compiled: $(words $(OF_SUBSET)) object(s)"

# Commit-2 addition: the Pegasos2 machdep for SF. Compiled with
# SF_CFLAGS (so SF's include path is primary) PLUS -I $(MACHDEP) so
# our own pegasos2.h / uart16550.h / io.h resolve as siblings.
$(BUILD)/of_machdep.o: $(SF_MACHDEP)/machdep.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(MACHDEP) -c $< -o $@

OF_MACHDEP_OBJS := \
    $(BUILD)/of_machdep.o \
    $(BUILD)/of_platform.o \
    $(BUILD)/of_ci_entry.o \
    $(BUILD)/of_boot_kernel.o \
    $(BUILD)/of_boot_kernel_asm.o \
    $(BUILD)/of_pci_tree.o \
    $(BUILD)/of_ide_driver.o \
    $(BUILD)/of_partition_pkg.o \
    $(BUILD)/of_amiga_rdb.o \
    $(BUILD)/of_amiga_ffs.o \
    $(BUILD)/of_amiga_sfs.o \
    $(BUILD)/of_amiga_pfs3.o \
    $(BUILD)/of_fs_exfat.o

$(BUILD)/of_platform.o: $(SF_MACHDEP)/platform.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(MACHDEP) -I$(SF)/fs -c $< -o $@

$(BUILD)/of_ci_entry.o: $(SF_MACHDEP)/ci_entry.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(MACHDEP) -c $< -o $@

$(BUILD)/of_boot_kernel.o: $(SF_MACHDEP)/boot_kernel.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(MACHDEP) -c $< -o $@

$(BUILD)/of_boot_kernel_asm.o: $(SF_MACHDEP)/boot_kernel.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/of_pci_tree.o: $(SF_MACHDEP)/pci_tree.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(MACHDEP) -c $< -o $@

$(BUILD)/of_ide_driver.o: $(SF_MACHDEP)/ide_driver.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(MACHDEP) -c $< -o $@

$(BUILD)/of_partition_pkg.o: $(SF_MACHDEP)/partition_pkg.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(MACHDEP) -c $< -o $@

# Arc FS-B, Block 1: Amiga Rigid Disk Block partition parser.
# Added to g_filesys[] BEFORE iso9660/dosfat/ext2 so an RDB-
# formatted disk gets partitioned first. When the FFS/SFS/PFS3
# readers land (B2+), they'll be matched on the DosType inside
# each partition's pb_Environment.
$(BUILD)/of_amiga_rdb.o: $(SF_MACHDEP)/amiga_rdb.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(SF)/fs -c $< -o $@

# Arc FS-B, Block 2: Amiga OFS/FFS/Intl/LNFS readonly reader.
# Covers DOS\0..\3 (original FFS + international) and DOS\6/\7
# (long-name + FFS2). Clean-room implementation based on public
# Laurent Clévy adflib FAQ + Olaf Barthel DCFS/LNFS Low Level Data
# Structures page; no GPL code copied. Needs -I$(SF)/fs for fs.h.
$(BUILD)/of_amiga_ffs.o: $(SF_MACHDEP)/amiga_ffs.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(SF)/fs -c $< -o $@

# Arc FS-B, Block 4: SmartFileSystem readonly reader. Covers SFS\0
# (classic Amiga) and SFS\2 (AOS4). On-disk layout identical; SFS\2
# adds softlinks which we recognise-and-skip. Clean-room from the
# AROS LGPL source's blockstructure.h / nodes.h / btreenodes.h /
# objects.h / bitmap.h header files, which describe the on-disk
# format via struct offsets (format spec, not code). Linear dir
# walk + extent B+-tree traversal for file data.
$(BUILD)/of_amiga_sfs.o: $(SF_MACHDEP)/amiga_sfs.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(SF)/fs -c $< -o $@

# Arc FS-B, Block 5: Professional File System 3 (PFS\1 / PFS\2 /
# AFS\1) readonly reader. Clean-room from tonioni/pfs3aio's
# blocks.h + struct.h (BSD 4-clause; used as format spec only, no
# code copy -- so the advertising clause does not propagate).
# Anode-chain based file reading, 3-level indexblock->anodeblock
# lookup, small-disk rootblock variant only.
$(BUILD)/of_amiga_pfs3.o: $(SF_MACHDEP)/amiga_pfs3.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(SF)/fs -c $< -o $@

# Arc FS-C: exFAT readonly reader. Clean-room from Microsoft's
# public 2019 spec at learn.microsoft.com/windows/win32/fileio/
# exfat-specification. Boot-sector parse, FAT chain walk, 32-byte
# directory-entry sets (0x85 File + 0xC0 Stream + 0xC1 FileName).
$(BUILD)/of_fs_exfat.o: $(SF_MACHDEP)/fs_exfat.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(SF)/fs -c $< -o $@

# Append OF to the firmware link target. phase1_c_main() calls SF's
# main() directly; without these objects the link would fail.
OBJS += $(OF_SUBSET) $(OF_MACHDEP_OBJS)

# -----------------------------------------------------------------
# Spec-07 boot-kernel smoke-test image. A standalone PPC ELF linked
# at 0x00800000 that writes "KERNEL OK r5=..." to UART1 and traps.
# Built as a separate ELF, then objcopy'd into a .rodata blob so
# it can be embedded in firmware.bin and copied to DRAM at run time
# by the `test-boot` Forth word.
TK      := test_kernel
TK_ELF  := $(BUILD)/test_kernel.elf
TK_OBJ  := $(BUILD)/test_kernel_blob.o

$(BUILD)/test_kernel.o: $(TK)/kernel.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(TK_ELF): $(BUILD)/test_kernel.o $(TK)/kernel.lds
	$(CC) -nostdlib -static -Wl,-T,$(TK)/kernel.lds -o $@ $<

# objcopy --prefix-sections puts our blob in .rodata as an "incbin"-
# style object with symbols _binary_<name>_start/_end/_size. We
# rename them via --redefine-sym for a cleaner API in C. The input
# format "binary" means "treat the file as raw bytes"; the output
# is a linkable ELF section. Target is powerpc:common 32-bit BE to
# match our main build.
$(TK_OBJ): $(TK_ELF)
	$(OBJCOPY) -I binary -O elf32-powerpc -B powerpc:common \
	    --rename-section .data=.rodata.test_kernel,alloc,load,readonly,data,contents \
	    --redefine-sym _binary_$(BUILD)_test_kernel_elf_start=_test_kernel_start \
	    --redefine-sym _binary_$(BUILD)_test_kernel_elf_end=_test_kernel_end \
	    --redefine-sym _binary_$(BUILD)_test_kernel_elf_size=_test_kernel_size \
	    $< $@

OBJS += $(TK_OBJ)

# -----------------------------------------------------------------
# Block 5/N: test-iso target
# -----------------------------------------------------------------
# A minimal ISO9660 image carrying our test kernel as /test.elf.
# Built on demand (`make test-iso`) -- not part of the default
# `make` target since it requires WSL+genisoimage and only matters
# for M5/M6 manual smoke tests. Once built, run as:
#     qemu-system-ppc -M pegasos2 -m 512 \
#         -bios build/firmware-raw.bin \
#         -cdrom build/test.iso \
#         -serial mon:stdio -display none
# Then at the ok prompt issue `boot cd /test.elf` (M6 wires the
# load path) or `test-iso-ls` (M4 lists the root directory).

TEST_ISO   := $(BUILD)/test.iso
ISO_STAGE  := $(BUILD)/iso_stage

$(TEST_ISO): $(TK_ELF)
	@mkdir -p $(ISO_STAGE)
	@cp $< $(ISO_STAGE)/test.elf
	@genisoimage -quiet -o $@ -V 'PEGASOS2-TEST' -J -R -allow-lowercase $(ISO_STAGE)/
	@echo "  ISO   $@ ($$(wc -c < $@) bytes)"

.PHONY: test-iso
test-iso: $(TEST_ISO)

# Commit-2 acceptance target: partial-link the SF subset with
# machdep.o and report remaining undefined symbols. Those are the
# "not yet brought in" SF files; Commit 3 expands the subset to
# satisfy them. Uses `ld -r` which permits undefined symbols; we
# then `nm -u` the product to enumerate them.
.PHONY: of-test
of-test: $(OF_SUBSET) $(OF_MACHDEP_OBJS)
	@echo "  LD -r  $(BUILD)/of-test.o"
	@$(PREFIX)ld -r -o $(BUILD)/of-test.o $(OF_SUBSET) $(OF_MACHDEP_OBJS)
	@echo "  --- Defined symbols ($(words $(OF_SUBSET) $(OF_MACHDEP_OBJS)) objects) ---"
	@$(PREFIX)nm -g --defined-only $(BUILD)/of-test.o | head -5
	@echo "  ..."
	@echo "  --- Undefined symbols (inventory for Commit 3) ---"
	@$(PREFIX)nm -u $(BUILD)/of-test.o | sort -u

$(ELF): $(OBJS) $(MACHDEP)/firmware.ld | $(BUILD)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(FIRMWARE): $(ELF)
	$(OBJCOPY) -O binary --pad-to 0xFFF80000 --gap-fill 0xff $< $@
	@sz=$$(stat -c%s $@); \
	if [ "$$sz" -eq "$(FLASH_SIZE)" ]; then \
	    echo "  BIN   $@ ($$sz bytes = 512 KiB)"; \
	else \
	    echo "  ERROR $@ is $$sz bytes, expected $(FLASH_SIZE)"; \
	    exit 1; \
	fi

disasm: $(ELF)
	$(OBJDUMP) -d $(ELF) | head -30

info:
	@echo "CC       = $(CC)"
	@$(CC) --version | head -1
	@echo "Target   = $$($(CC) -dumpmachine)"
	@echo "CFLAGS   = $(CFLAGS)"
	@echo "LDFLAGS  = $(LDFLAGS)"

clean:
	rm -rf $(BUILD)
