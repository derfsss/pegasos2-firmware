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
    -I$(MACHDEP)/x86compat \
    -I$(MACHDEP) \
    -I$(X86EMU)/include

ASFLAGS := $(CFLAGS) -Wa,-mregnames

LDFLAGS := \
    -nostdlib -static \
    -Wl,-T,$(MACHDEP)/firmware.ld \
    -Wl,--build-id=none \
    -Wl,-Map=$(BUILD)/firmware.map

OBJS := \
    $(BUILD)/reset.o \
    $(BUILD)/exceptions.o \
    $(BUILD)/panic.o \
    $(BUILD)/syscall.o \
    $(BUILD)/timer.o \
    $(BUILD)/phase1.o \
    $(BUILD)/uart16550.o \
    $(BUILD)/mv64361.o \
    $(BUILD)/vt8231.o \
    $(BUILD)/pci_walker.o \
    $(BUILD)/x86_glue.o \
    $(BUILD)/x86emu_stubs.o \
    $(BUILD)/x86emu_ops.o \
    $(BUILD)/x86emu_ops2.o \
    $(BUILD)/x86emu_prim_ops.o \
    $(BUILD)/x86emu_decode.o \
    $(BUILD)/x86emu_sys.o \
    $(BUILD)/x86emu_debug.o

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
#   -DDEBUG so the DPRINTF() macro expands to real UART prints during
#     bring-up. Will be disabled once the ok prompt is reliable.
#   -D_FORTIFY_SOURCE=0 defensively; we're freestanding and have no
#     libc-side __printf_chk / __fortify_fail to satisfy.
# Do NOT define:
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
    -I$(SF_MACHDEP) \
    -I$(SF) \
    -I$(SF)/exe \
    -DDEBUG \
    -U_FORTIFY_SOURCE

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
    $(BUILD)/of_nvram.o

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
    $(BUILD)/of_platform.o

$(BUILD)/of_platform.o: $(SF_MACHDEP)/platform.c | $(BUILD)
	$(CC) $(SF_CFLAGS) -I$(MACHDEP) -c $< -o $@

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
	$(CC) $(LDFLAGS) $(OBJS) -o $@

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
