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

BUILD   := build
MACHDEP := machdep/pegasos2

# Bare-metal flags. No Linux runtime, no built-ins, big-endian
# 32-bit PowerPC targeting the 7447/7450 family.
CFLAGS := \
    -m32 -mbig-endian -mcpu=7450 \
    -msoft-float -mno-altivec \
    -ffreestanding -fno-builtin -nostdinc \
    -fno-pic -fno-stack-protector \
    -fno-asynchronous-unwind-tables \
    -Os -g \
    -Wall -Wextra -Werror \
    -Iupstream/x86emu/include

ASFLAGS := $(CFLAGS) -Wa,-mregnames

LDFLAGS := \
    -nostdlib -static \
    -Wl,-T,$(MACHDEP)/firmware.ld \
    -Wl,--build-id=none \
    -Wl,-Map=$(BUILD)/firmware.map

OBJS := $(BUILD)/reset.o

FIRMWARE := $(BUILD)/firmware-raw.bin
ELF      := $(BUILD)/firmware.elf
FLASH_SIZE := 524288

.PHONY: all clean info disasm
.SUFFIXES:

all: $(FIRMWARE)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(MACHDEP)/%.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

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
