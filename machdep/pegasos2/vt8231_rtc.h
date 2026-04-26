/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  VT8231 internal RTC + battery-backed CMOS RAM. The schematic
 *  (references/Pegasos_2b5.pdf, sheet 23 "VT8231 Batterie") shows the
 *  Pegasos II keeps wall-clock time and persistent firmware
 *  configuration in the VT8231 southbridge's own RTC unit, fed by a
 *  CR2032 lithium cell on pin VBAT and a 32.768 kHz crystal between
 *  RTCX1 / RTCX2. There is no separate ST M48T59 NVRAM chip; an
 *  earlier draft of this firmware assumed one based on docs/08-nvram.md
 *  but the schematic-vs-docs audit (SPEC-QUESTIONS.md Q7) found that
 *  device is not on the board.
 *
 *  Programming model:
 *    - Two ISA I/O ports, MC146818-compatible, behind the VT8231:
 *        0x70  W: CMOS index (which byte of the 128-byte RTC bank)
 *        0x71  R/W: data at the selected index
 *    - Indices 0x00..0x0D are the time-keeping and control registers
 *      (BCD seconds/minutes/hours/day/date/month/year, status A..D).
 *    - Indices 0x0E..0x7F are 114 bytes of general-purpose CMOS RAM
 *      backed by the same VBAT cell -- they survive power cycles on
 *      real hardware, and survive QEMU process lifetime under the
 *      mc146818 model (QEMU does not persist them across qemu runs
 *      unless `-rtc base=...` plus an external storage hook is used,
 *      so on QEMU the firmware still reloads compile-time defaults
 *      every boot).
 *
 *  ISA I/O ports 0x70/0x71 sit inside the VT8231 PCI1 I/O window at
 *  CPU physical address 0xFE000000 (PCI1_IO_BASE in pegasos2.h), the
 *  same routing UART1 and the SuperIO already use.
 */

#ifndef VT8231_RTC_H
#define VT8231_RTC_H

#include <stdint.h>

/* CMOS index/data ports inside the VT8231 ISA I/O space. */
#define VT8231_RTC_INDEX_PORT   0x70u
#define VT8231_RTC_DATA_PORT    0x71u

/*
 * Time-keeping registers, MC146818 layout (matches the VT8231 RTC
 * function and QEMU's pegasos2 ds1385-rtc model).
 */
#define VT8231_RTC_SECONDS      0x00u   /* BCD 00..59 */
#define VT8231_RTC_MINUTES      0x02u
#define VT8231_RTC_HOURS        0x04u
#define VT8231_RTC_DAY_OF_WEEK  0x06u
#define VT8231_RTC_DAY          0x07u
#define VT8231_RTC_MONTH        0x08u
#define VT8231_RTC_YEAR         0x09u
#define VT8231_RTC_STATUS_A     0x0Au
#define VT8231_RTC_STATUS_B     0x0Bu   /* bit 7 = SET (halt update) */
#define VT8231_RTC_STATUS_C     0x0Cu
#define VT8231_RTC_STATUS_D     0x0Du

/* Status A bit 7 = "Update In Progress"; reads of time registers
 * during UIP can return a half-rolled-over value. Spin until clear
 * before/after a multi-byte read. */
#define VT8231_RTC_STATUS_A_UIP 0x80u

/* Status B bit 7 = SET. Setting it freezes the user-visible time
 * registers so a multi-byte write is atomic; clear it again after. */
#define VT8231_RTC_STATUS_B_SET 0x80u
#define VT8231_RTC_STATUS_B_DM  0x04u   /* 0 = BCD, 1 = binary */
#define VT8231_RTC_STATUS_B_24  0x02u   /* 0 = 12-hour, 1 = 24-hour */

/*
 * General-purpose CMOS RAM range. Indices 0x0E..0x7F are 114 bytes
 * of battery-backed scratch we can freely use for OF environment
 * variables. (The first 14 bytes are time-keeping + status and are
 * off-limits for SF's NVRAM partitioning.)
 */
#define VT8231_CMOS_RAM_BASE    0x0Eu
#define VT8231_CMOS_RAM_SIZE    (0x80u - VT8231_CMOS_RAM_BASE) /* 114 B */

/* Byte access to any CMOS index (time registers + scratch RAM). */
uint8_t vt8231_rtc_read_byte(unsigned index);
void    vt8231_rtc_write_byte(unsigned index, uint8_t val);

/*
 * Read / write the wall-clock time. Returns 0 on success, -1 if no
 * RTC responds (every register reads back 0xFF, the typical signature
 * of an absent or power-off chip). Years are returned as the full
 * 4-digit value; the chip stores only two BCD digits, with year<70
 * mapped to 20xx and year>=70 to 19xx (firmware/OS convention).
 */
int vt8231_rtc_read(int *year, int *month, int *day,
                    int *hour, int *minute, int *second);
int vt8231_rtc_write(int year, int month, int day,
                     int hour, int minute, int second);

#endif /* VT8231_RTC_H */
