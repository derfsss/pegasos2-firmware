/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 */

#include "vt8231.h"
#include "mv64361.h"
#include "io.h"
#include "pegasos2.h"

/*
 * Minimum chain to make UART1 respond at ISA 0x3F8:
 *
 *   1. Enable I/O + MEM + MASTER decode on the VT8231 ISA-bridge
 *      function by setting PCI_COMMAND bits 0..2.
 *
 *   2. Unlock the SuperIO config window by setting bit 2 of the
 *      VT8231 fn-0 PCI-config register 0x50. Without this, writes
 *      to ISA ports 0x3F0/0x3F1 are silently dropped by the
 *      emulation rather than routed into the SuperIO config window.
 *
 *   3. Set SuperIO index 0xF2 (Function Select) bit 2 to enable
 *      UART1 decode. Reset default of 0xF2 is 0x03, so 0x07 keeps
 *      existing enables and adds UART1.
 *
 *   4. Leave SuperIO index 0xF4 at its reset default 0xFE -- the
 *      VT8231 model interprets that as UART1 I/O base = 0x3F8.
 *
 * See SPEC-QUESTIONS.md Q3 for the spec-clarity follow-up.
 */

void vt8231_enable_uart1(void)
{
	pci_cfg_write32(VT8231_HOST, VT8231_BUS, VT8231_DEV, VT8231_FN_ISA,
			0x04, 0x00000007u);

	pci_cfg_write32(VT8231_HOST, VT8231_BUS, VT8231_DEV, VT8231_FN_ISA,
			0x50, 0x00000004u);

	mmio_write8(SUPERIO_IDX_ADDR, 0xF2);
	mmio_write8(SUPERIO_DAT_ADDR, 0x07);
}
