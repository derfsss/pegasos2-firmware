/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  M48T59 byte read/write over VT8231 ISA I/O. The chip exposes an
 *  address-indexed view of its 8 KiB SRAM via three ISA ports (spec
 *  08 §"M48T59 access"):
 *
 *    out 0x74, offset & 0xFF
 *    out 0x75, (offset >> 8) & 0x1F    -- 13-bit offset total
 *    in  al,  0x77
 *
 *  All three ports sit in the VT8231 PCI1 I/O window at 0xFE000000.
 *  Byte reads/writes on that window are endian-invariant (see io.h
 *  commentary), so the mmio_{read,write}8 helpers are the right
 *  layer.
 *
 *  No concurrency guard: the firmware is single-threaded and every
 *  caller runs with interrupts masked in phase1.
 */

#include "pegasos2.h"
#include "io.h"
#include "m48t59.h"

#define M48T59_ADDR_LO_MMIO   (PCI1_IO_BASE + M48T59_ADDR_LO_PORT)
#define M48T59_ADDR_HI_MMIO   (PCI1_IO_BASE + M48T59_ADDR_HI_PORT)
#define M48T59_DATA_MMIO      (PCI1_IO_BASE + M48T59_DATA_PORT)

static inline void
m48t59_set_addr(unsigned offset)
{
	mmio_write8(M48T59_ADDR_LO_MMIO, (uint8_t)(offset & 0xFFu));
	mmio_write8(M48T59_ADDR_HI_MMIO, (uint8_t)((offset >> 8) & 0x1Fu));
}

uint8_t
m48t59_read_byte(unsigned offset)
{
	m48t59_set_addr(offset);
	return mmio_read8(M48T59_DATA_MMIO);
}

void
m48t59_write_byte(unsigned offset, uint8_t val)
{
	m48t59_set_addr(offset);
	mmio_write8(M48T59_DATA_MMIO, val);
}

/* --- BCD <-> binary helpers. The M48T59 stores all time fields as
 * packed BCD, upper nibble tens, lower nibble units. Mask out any
 * status bits the caller already knows about. */
static inline int
bcd_to_bin(uint8_t bcd)
{
	return (int)((bcd >> 4) * 10u + (bcd & 0x0Fu));
}

static inline uint8_t
bin_to_bcd(int bin)
{
	int b = bin;
	if (b < 0)   b = 0;
	if (b > 99)  b = 99;
	return (uint8_t)(((b / 10) << 4) | (b % 10));
}

/*
 * Full wall-clock read. Seeds the control-register read-lock bit
 * before the burst so the M48T59 presents a consistent snapshot
 * (the chip internally double-buffers the time registers during
 * a locked read), then drains and clears the lock.
 *
 * Year decoding: register holds BCD 00..99. If year_bcd < 70, we
 * report 20xx; else 19xx. Matches AmigaOS 4's M48T59 conventions.
 * Callers that want a different cutoff can post-process.
 *
 * Returns 0 on success, -1 when the chip isn't responding (every
 * RTC register reads 0xFF, which is what QEMU pegasos2 presents
 * since it wires no M48T59 model).
 */
int
m48t59_read_rtc(int *year, int *month, int *day,
                int *hour, int *minute, int *second)
{
	uint8_t ctrl_before = m48t59_read_byte(M48T59_RTC_CONTROL);

	/* Not-present probe. A real chip has non-zero Month (01..12
	 * BCD) and a Year that decodes within 00..99. Two common
	 * failure modes on platforms without an M48T59:
	 *   - every register reads 0xFF (the bus returns all-ones)
	 *   - every register reads 0x00 (silent writes, zero reads)
	 * Both should be treated as absent. We probe with three
	 * registers (year, month, control) and accept only if
	 * either mixed values OR a plausible date shows up. */
	uint8_t y_raw  = m48t59_read_byte(M48T59_RTC_YEAR);
	uint8_t mo_raw = m48t59_read_byte(M48T59_RTC_MONTH);
	if (y_raw == 0xFFu && mo_raw == 0xFFu && ctrl_before == 0xFFu)
		return -1;
	int mo_bin_probe = (int)((mo_raw >> 4) * 10u + (mo_raw & 0x0Fu));
	if (mo_bin_probe < 1 || mo_bin_probe > 12)
		return -1;

	/* Set the R (read) bit in Control to freeze the user-visible
	 * time for the duration of the burst. */
	m48t59_write_byte(M48T59_RTC_CONTROL,
	                  ctrl_before | M48T59_CTRL_READ_LOCK);

	uint8_t s   = m48t59_read_byte(M48T59_RTC_SECONDS) & 0x7Fu;
	uint8_t m   = m48t59_read_byte(M48T59_RTC_MINUTES) & 0x7Fu;
	uint8_t h   = m48t59_read_byte(M48T59_RTC_HOURS)   & 0x3Fu;
	uint8_t d   = m48t59_read_byte(M48T59_RTC_DATE)    & 0x3Fu;
	uint8_t mo  = mo_raw                                & 0x1Fu;
	uint8_t y   = y_raw;

	/* Clear the read lock so the time advances again. */
	m48t59_write_byte(M48T59_RTC_CONTROL,
	                  ctrl_before & ~M48T59_CTRL_READ_LOCK);

	int y_bin = bcd_to_bin(y);
	*year   = (y_bin < 70) ? 2000 + y_bin : 1900 + y_bin;
	*month  = bcd_to_bin(mo);
	*day    = bcd_to_bin(d);
	*hour   = bcd_to_bin(h);
	*minute = bcd_to_bin(m);
	*second = bcd_to_bin(s);
	return 0;
}

int
m48t59_write_rtc(int year, int month, int day,
                 int hour, int minute, int second)
{
	uint8_t ctrl_before = m48t59_read_byte(M48T59_RTC_CONTROL);
	if (ctrl_before == 0xFFu &&
	    m48t59_read_byte(M48T59_RTC_YEAR) == 0xFFu)
		return -1;

	/* Set W (write) bit to halt the user-visible time during
	 * the burst. */
	m48t59_write_byte(M48T59_RTC_CONTROL,
	                  ctrl_before | M48T59_CTRL_WRITE_LOCK);

	int y_bin = year % 100;
	m48t59_write_byte(M48T59_RTC_YEAR,    bin_to_bcd(y_bin));
	m48t59_write_byte(M48T59_RTC_MONTH,   bin_to_bcd(month));
	m48t59_write_byte(M48T59_RTC_DATE,    bin_to_bcd(day));
	m48t59_write_byte(M48T59_RTC_HOURS,   bin_to_bcd(hour));
	m48t59_write_byte(M48T59_RTC_MINUTES, bin_to_bcd(minute));
	m48t59_write_byte(M48T59_RTC_SECONDS, bin_to_bcd(second));

	/* Clear the write lock -- the chip commits the burst and
	 * the oscillator resumes normal counting. */
	m48t59_write_byte(M48T59_RTC_CONTROL,
	                  ctrl_before & ~M48T59_CTRL_WRITE_LOCK);
	return 0;
}
