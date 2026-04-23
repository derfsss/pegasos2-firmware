/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  ST M48T59 TimeKeeper: 8 KiB battery-backed SRAM + RTC, wired into
 *  the VT8231 ISA bus. Spec 08 defines the layout:
 *
 *    0x0000..0x01FF   512 B   firmware private (cold-boot count, ...)
 *    0x0200..0x05FF   1 KiB   system partition (OF env vars)
 *    0x0600..0x17FF   4.5 KiB free partition (OS-reservable)
 *    0x1800..0x1FF7   2 KiB   OS-specific partition
 *    0x1FF8..0x1FFF   8 B     RTC registers (BCD)
 *
 *  Phase 1 of this driver exposes only the byte read/write primitives
 *  and the system-partition window to SmartFirmware. Other partitions
 *  and RTC wiring arrive in later commits (firmware-state, get-time-
 *  of-day, IEEE-1275 §A.5.2 partition-header management).
 */

#ifndef M48T59_H
#define M48T59_H

#include <stdint.h>

/*
 * ISA I/O ports (CPU-visible through the VT8231 PCI1 I/O window at
 * 0xFE000000, same routing we use for UART1 and SuperIO).
 */
#define M48T59_ADDR_LO_PORT   0x74u   /* W: offset low byte */
#define M48T59_ADDR_HI_PORT   0x75u   /* W: offset high byte */
#define M48T59_DATA_PORT      0x77u   /* R/W: data at selected offset */

#define M48T59_TOTAL_SIZE     0x2000u /* 8 KiB SRAM including RTC bytes */

/* Spec 08 §"NVRAM partitioning" partition boundaries. */
#define M48T59_SYSTEM_OFFSET  0x0200u
#define M48T59_SYSTEM_SIZE    0x0400u /* 1 KiB */

/* Byte read/write via the two-register address-indexed protocol. */
uint8_t m48t59_read_byte(unsigned offset);
void    m48t59_write_byte(unsigned offset, uint8_t val);

#endif /* M48T59_H */
