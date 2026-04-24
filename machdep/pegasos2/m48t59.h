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

/*
 * RTC register block at the tail of the 8 KiB window (last 8 bytes).
 * Stored in packed BCD; the ST M48T59 datasheet lays them out as:
 *
 *   0x1FF8  Control  W=bit7 lock-write; R=bit6 lock-read; CAL[5..0]
 *   0x1FF9  Seconds  ST=bit7 oscillator stop; SEC=bits 6..0 (BCD 00..59)
 *   0x1FFA  Minutes  bits 6..0 (BCD 00..59)
 *   0x1FFB  Hours    bits 5..0 (BCD 00..23)
 *   0x1FFC  Day-of-week  bits 2..0 (1..7). Bit 6 = FT (freq test).
 *   0x1FFD  Date         bits 5..0 (BCD 01..31)
 *   0x1FFE  Month        bits 4..0 (BCD 01..12)
 *   0x1FFF  Year         bits 7..0 (BCD 00..99)
 *
 * There is no hardware century field; firmware/OS convention treats
 * year<70 as 20xx and year>=70 as 19xx. AmigaOS 4's default is 20xx.
 */
#define M48T59_RTC_CONTROL    0x1FF8u
#define M48T59_RTC_SECONDS    0x1FF9u
#define M48T59_RTC_MINUTES    0x1FFAu
#define M48T59_RTC_HOURS      0x1FFBu
#define M48T59_RTC_DOW        0x1FFCu
#define M48T59_RTC_DATE       0x1FFDu
#define M48T59_RTC_MONTH      0x1FFEu
#define M48T59_RTC_YEAR       0x1FFFu

#define M48T59_CTRL_WRITE_LOCK   0x80u   /* set during a write burst */
#define M48T59_CTRL_READ_LOCK    0x40u   /* set during a read burst */
#define M48T59_SECONDS_STOP      0x80u   /* ST -- oscillator disabled */

/* Byte read/write via the two-register address-indexed protocol. */
uint8_t m48t59_read_byte(unsigned offset);
void    m48t59_write_byte(unsigned offset, uint8_t val);

/*
 * Read / write the full wall-clock time. Returns 0 on success,
 * -1 if no M48T59 responds (every RTC register reads back 0xFF,
 * the typical signature of an absent or power-off chip -- this is
 * what happens on QEMU pegasos2, which wires no M48T59 model at
 * all). Years are returned as the full 4-digit value (e.g. 2026),
 * with the 70+ heuristic above applied to the BCD register.
 */
int m48t59_read_rtc(int *year, int *month, int *day,
                    int *hour, int *minute, int *second);
int m48t59_write_rtc(int year, int month, int day,
                     int hour, int minute, int second);

#endif /* M48T59_H */
