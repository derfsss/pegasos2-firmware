/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Pegasos2 machdep header for the vendored SmartFirmware OF runtime
 *  (upstream/smartfirmware/bin/of). Parallels bebox/machdep.h and the
 *  other per-board ports in the upstream tree: declares the types
 *  SmartFirmware uses, the memory-pool and stack sizes, and the
 *  identity strings that appear in the banner.
 *
 *  This is Commit 1 of a multi-commit OF bring-up. Nothing in this
 *  header is yet called from our firmware; it exists so SmartFirmware's
 *  portable core compiles cleanly against a Pegasos2 machdep worldview.
 *  Subsequent commits add machine_* function definitions, wire malloc
 *  to a DRAM arena, and finally hand off from phase1 to SmartFirmware's
 *  main() to reach an `ok` prompt on UART1.
 */

#ifndef MACHDEP_H
#define MACHDEP_H

/*
 * Endianness: Pegasos2 is a 32-bit big-endian PowerPC. SmartFirmware
 * defaults to big-endian when LITTLE_ENDIAN is undefined, so we simply
 * leave it undefined here (unlike the i386 / amd64 ports).
 *
 * 64-bit cell width: the MPC7447A has 32-bit GPRs, so Cells are 32-bit
 * and SF_64BIT stays undefined.
 */
/* #define LITTLE_ENDIAN */
/* #define SF_64BIT */

/*
 * __LONGLONG enables the Long / uLong / Octlet types in SmartFirmware.
 * GCC on PPC supports `long long` natively, so we turn it on -- the
 * Forth engine's 64-bit math words (funcs64.c) need it.
 */
#define __LONGLONG long long

/*
 * Turn on SmartFirmware's built-in debug allocator. This is a simple
 * static-arena allocator that lives inside stdlib.c; sufficient for
 * bring-up before we swap in lib/aoa or similar in a later commit.
 */
#define DEBUG_MALLOC

/*
 * Memory pool sizing.
 *
 * MALLOC_POOL is the total number of bytes handed to init_malloc().
 * SmartFirmware carves the Forth data memory (MEM_SIZE), xtok table,
 * capture buffer, device-tree node allocations, and Forth stacks out
 * of this pool.
 *
 * Spec 07 §Load-address contract: heap lives "above 0x200000 but
 * below 0x400000" so the default kernel load at 0x400000 has a clear
 * region. We put the pool at 0x00200000 (right where x86emu used to
 * be -- x86emu moved to 0x01000000) and size it at 2 MiB so it ends
 * at 0x003FFFFF. Compliance verified by the `heap-info` Forth word.
 */
#define MALLOC_POOL        (2 * 1024 * 1024)    /* 2 MiB */
#define MEM_SIZE           (256 * 1024)         /* 256 KiB Forth user dict */

#define STR_SIZE           256          /* OF spec minimum */
#define STACK_SIZE         128          /* Forth data-stack depth */
#define RET_STACK_SIZE     64           /* Forth return-stack depth */
#define MAX_ADDR_CELLS     4            /* #address-cells cap, spec min 4 */
#define MAX_ALARMS         16           /* max pending alarms */

/*
 * Identity strings that appear in the OF banner and device-tree root
 * properties. No commas allowed per SmartFirmware convention.
 * FIRMWARE_REV bumps with each release; 0.1 reflects pre-alpha.
 */
#define MANUFACTURER       "bPlan-CodeGen"
#define MACHINE_TYPE       "Pegasos2"
#define FIRMWARE_REV       "0.1-cleanroom"
#define STANDARD_REV       0x00030000   /* IEEE-1275 version 3.0 */

/*
 * Placeholder MAC address. On real HW this would come from the SROM
 * of the on-board Marvell SysKonnect GbE; on QEMU the rtl8139 -netdev
 * derives one. We ship a visible-as-bogus default so it's obvious
 * when machine_init hasn't been patched to probe the real one yet.
 */
#define MAC_ADDRESS        "Pgsos!"     /* 6 bytes == MAC length */

#define MACHDEP_HELP_TITLE "pegasos2"
#define MACHDEP_HELP \
	"Pegasos II PowerPC (clean-room rewrite, work in progress).\n" \
	"See https://github.com/... for status and docs/ for specification.\n"

/*
 * Integer typedefs the SmartFirmware core assumes. Signed and unsigned
 * are documented here (bebox comments reproduced and adapted).
 */
typedef char           Char;      /* >=8-bit signed */
typedef unsigned char  uChar;     /* >=8-bit unsigned */
typedef short          Short;     /* >=16-bit signed */
typedef unsigned short uShort;    /* >=16-bit unsigned */
typedef long           Int;       /* >=32-bit signed */
typedef unsigned long  uInt;      /* >=32-bit unsigned (== size_t role) */

#ifdef __LONGLONG
typedef __LONGLONG Long;                 /* 64-bit signed */
typedef unsigned __LONGLONG uLong;       /* 64-bit unsigned */
#else
typedef long          Long;
typedef unsigned long uLong;
#endif

typedef long          Ptr;        /* must hold any pointer */
typedef unsigned long uPtr;       /* unsigned any-pointer */

typedef int Bool;                 /* 0/1, whatever the fastest CPU int is */

/*
 * Forth-level width aliases. On our 32-bit PPC:
 *    Byte   = 8 bits
 *    Word   = 16 bits
 *    Cell   = 32 bits (and must hold any pointer)
 *    Doublet, Quadlet == Word, Cell
 *    Octlet = 64 bits (iff __LONGLONG)
 */
typedef Char  Byte;
typedef uChar uByte;
typedef Short Word;

#ifdef SF_64BIT
#  ifndef __LONGLONG
#    error SF_64BIT requires __LONGLONG
#  endif
typedef Long  Cell;
typedef uLong uCell;
#else
typedef Int  Cell;
typedef uInt uCell;
#endif

typedef Short  Doublet;
typedef uShort uDoublet;
typedef Int    Quadlet;
typedef uInt   uQuadlet;

#ifdef __LONGLONG
typedef Long  Octlet;
typedef uLong uOctlet;
#endif

/* Alignment requirements. On PowerPC we follow natural alignment. */
#define ALIGNMENT     (sizeof (Cell))
#define QALIGNMENT    (sizeof (Quadlet))
#define DALIGNMENT    (sizeof (Doublet))
#define BALIGNMENT    (sizeof (Byte))

#ifdef __LONGLONG
#  define OALIGNMENT  (sizeof (Octlet))
#endif

/* Bit counts and all-ones masks for each width. */
#define BYTE_SIZE     8
#define BYTE_MASK     0xFFu
#define DOUBLET_SIZE  16
#define DOUBLET_MASK  0xFFFFu
#define QUADLET_SIZE  32
#define QUADLET_MASK  0xFFFFFFFFul

#ifdef __LONGLONG
#  define OCTLET_SIZE 64
#  define OCTLET_MASK ((uOctlet)0xFFFFFFFFFFFFFFFFull)
#endif

/*
 * Default font selection. SmartFirmware's fb.c expects a compile-time
 * font chosen by FONT_FILE and matching metrics. Commit 1 doesn't
 * pull in fb.c, so the macros below are only consulted by code we
 * might add in a later commit (framebuffer support). Using the 8x16
 * courier matches bebox's default and keeps the image small.
 */
#define FONT_FILE     "cour8x16.font"
#define FONT_WIDTH    8
#define FONT_HEIGHT   18   /* 16 + one zero row top + one bottom */
#define FONT_ADVANCE  1
#define FONT_FIRST    ' '
#define FONT_LAST     '~'
#define FONT_COUNT    (FONT_LAST - FONT_FIRST + 1)

#endif /* MACHDEP_H */
