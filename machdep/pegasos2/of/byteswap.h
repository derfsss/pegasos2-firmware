/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Big-/little-endian byte-swapping helpers shared across the
 *  filesystem readers, partition parsers, and ELF loader. All
 *  functions take a const-uByte* (== unsigned char*) and read
 *  unaligned bytes one at a time, so they're safe on any pointer
 *  alignment and fold to a single rlwimi/rlwinm sequence under
 *  modern GCC at -O2 (no actual function-call cost in release
 *  builds).
 *
 *  The PowerPC ISA has lhbrx/lwbrx for little-endian loads from
 *  big-endian memory; we don't use them here because (a) the
 *  inputs are unaligned byte arrays read out of disk buffers,
 *  not register-aligned addresses, and (b) keeping the helpers
 *  pure C makes them trivially correct under all compiler
 *  optimisation levels.
 */

#ifndef PEGASOS2_BYTESWAP_H
#define PEGASOS2_BYTESWAP_H

#include "defs.h"

static inline uInt
be16(const uByte *p)
{
	return ((uInt)p[0] << 8) | (uInt)p[1];
}

static inline uInt
be32(const uByte *p)
{
	return ((uInt)p[0] << 24) | ((uInt)p[1] << 16) |
	       ((uInt)p[2] << 8)  | (uInt)p[3];
}

static inline uLong
be64(const uByte *p)
{
	return ((uLong)be32(p) << 32) | (uLong)be32(p + 4);
}

static inline uInt
le16(const uByte *p)
{
	return ((uInt)p[1] << 8) | (uInt)p[0];
}

static inline uInt
le32(const uByte *p)
{
	return ((uInt)p[3] << 24) | ((uInt)p[2] << 16) |
	       ((uInt)p[1] << 8)  | (uInt)p[0];
}

static inline uLong
le64(const uByte *p)
{
	return ((uLong)le32(p + 4) << 32) | (uLong)le32(p);
}

#endif /* PEGASOS2_BYTESWAP_H */
