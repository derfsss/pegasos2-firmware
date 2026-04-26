/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  VT8231 internal RTC + 114-byte CMOS RAM driver. See vt8231_rtc.h
 *  for the schematic provenance and the programming model. The chip
 *  is the standard MC146818-compatible RTC unit inside the VT8231
 *  southbridge, accessed via ISA I/O ports 0x70 (index) / 0x71
 *  (data). On the Pegasos II those ports live in the VT8231 PCI1
 *  I/O window at CPU physical 0xFE000000 + port.
 *
 *  Single-threaded firmware, no concurrency guard required: every
 *  caller runs with interrupts masked in phase1, and the OF runtime
 *  serialises NVRAM access through SF's load_nvram/save_config.
 */

#include "pegasos2.h"
#include "io.h"
#include "vt8231_rtc.h"

#define RTC_INDEX_MMIO   (PCI1_IO_BASE + VT8231_RTC_INDEX_PORT)
#define RTC_DATA_MMIO    (PCI1_IO_BASE + VT8231_RTC_DATA_PORT)

uint8_t
vt8231_rtc_read_byte(unsigned index)
{
	mmio_write8(RTC_INDEX_MMIO, (uint8_t)(index & 0x7Fu));
	return mmio_read8(RTC_DATA_MMIO);
}

void
vt8231_rtc_write_byte(unsigned index, uint8_t val)
{
	mmio_write8(RTC_INDEX_MMIO, (uint8_t)(index & 0x7Fu));
	mmio_write8(RTC_DATA_MMIO, val);
}

/* --- BCD <-> binary helpers. The MC146818 stores time fields as
 * packed BCD when Status B bit 2 (DM) is 0, which is the power-on
 * default and what every common BIOS leaves it at. */
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
 * Wait for any in-flight per-second update to finish. The RTC sets
 * Status A bit 7 (UIP) for ~244 us per second while it rolls the
 * time-keeping registers; reading them in that window can return a
 * half-rolled-over value. Bound the spin so a stuck-bit chip can't
 * hang the firmware -- 100 ms is well past any real UIP cycle.
 */
static int
rtc_wait_idle(void)
{
	for (unsigned i = 0; i < 100000u; i++) {
		uint8_t a = vt8231_rtc_read_byte(VT8231_RTC_STATUS_A);
		if (a == 0xFFu)
			return -1;            /* chip absent */
		if ((a & VT8231_RTC_STATUS_A_UIP) == 0u)
			return 0;
		/* ~1 us per loop body on 7447A is plenty; no u_sleep
		 * dependency here -- this code runs before timer
		 * calibration on the early-boot path. */
	}
	return -1;
}

int
vt8231_rtc_read(int *year, int *month, int *day,
                int *hour, int *minute, int *second)
{
	if (rtc_wait_idle() != 0)
		return -1;

	/* Sanity probe: a working chip has Status D bit 7 (VRT, valid
	 * RAM and time) set when the battery is good, and a plausible
	 * BCD month in 01..12. Reject all-ones / all-zeros responses. */
	uint8_t mo_raw = vt8231_rtc_read_byte(VT8231_RTC_MONTH);
	int mo_bin_probe = (int)((mo_raw >> 4) * 10u + (mo_raw & 0x0Fu));
	if (mo_raw == 0xFFu || (mo_bin_probe < 1 || mo_bin_probe > 12))
		return -1;

	uint8_t s   = vt8231_rtc_read_byte(VT8231_RTC_SECONDS) & 0x7Fu;
	uint8_t m   = vt8231_rtc_read_byte(VT8231_RTC_MINUTES) & 0x7Fu;
	uint8_t h   = vt8231_rtc_read_byte(VT8231_RTC_HOURS)   & 0x3Fu;
	uint8_t d   = vt8231_rtc_read_byte(VT8231_RTC_DAY)     & 0x3Fu;
	uint8_t mo  = mo_raw                                    & 0x1Fu;
	uint8_t y   = vt8231_rtc_read_byte(VT8231_RTC_YEAR);

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
vt8231_rtc_write(int year, int month, int day,
                 int hour, int minute, int second)
{
	uint8_t status_b_before = vt8231_rtc_read_byte(VT8231_RTC_STATUS_B);
	if (status_b_before == 0xFFu &&
	    vt8231_rtc_read_byte(VT8231_RTC_YEAR) == 0xFFu)
		return -1;

	/* Set the SET bit so the burst lands atomically. */
	vt8231_rtc_write_byte(VT8231_RTC_STATUS_B,
	                      status_b_before | VT8231_RTC_STATUS_B_SET);

	int y_bin = year % 100;
	vt8231_rtc_write_byte(VT8231_RTC_YEAR,    bin_to_bcd(y_bin));
	vt8231_rtc_write_byte(VT8231_RTC_MONTH,   bin_to_bcd(month));
	vt8231_rtc_write_byte(VT8231_RTC_DAY,     bin_to_bcd(day));
	vt8231_rtc_write_byte(VT8231_RTC_HOURS,   bin_to_bcd(hour));
	vt8231_rtc_write_byte(VT8231_RTC_MINUTES, bin_to_bcd(minute));
	vt8231_rtc_write_byte(VT8231_RTC_SECONDS, bin_to_bcd(second));

	/* Clear SET; the chip resumes counting from the new time. */
	vt8231_rtc_write_byte(VT8231_RTC_STATUS_B,
	                      status_b_before & ~VT8231_RTC_STATUS_B_SET);
	return 0;
}
