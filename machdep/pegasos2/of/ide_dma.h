/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  VT8231 IDE bus-master DMA driver (M5).
 *
 *  Replaces the per-sector PIO read path that upstream
 *  isa/atadisk.c installs by default. We hook this up by
 *  overriding the `read-blocks` Forth method on every ATA
 *  (non-ATAPI) disk@ child created by probe_ata_disks. The
 *  override calls ide_dma_read_blocks() below, which issues an
 *  ATA READ_DMA command and lets the VT8231's bus-master DMA
 *  engine stream sector data straight into the caller's buffer.
 *
 *  ATAPI (CD/DVD) is left on the upstream PIO path -- ATAPI DMA
 *  uses a different register dance (DMA-aware packet command,
 *  dma-alloc / dma-free callbacks) and isn't on the v0.6 plan.
 *
 *  See machdep/pegasos2/of/ide_dma.c for the register-level
 *  programming model and the limitations of this driver.
 */

#ifndef IDE_DMA_H
#define IDE_DMA_H

#include "defs.h"

/*
 * Wire DMA in. Call from install_ide_driver AFTER probe_ata_disks
 * has populated `ide`'s children. Walks each disk@ child, tries to
 * enable bus-master DMA for it, and overrides its read-blocks
 * method to use the DMA path. Children for which DMA setup fails
 * keep the upstream PIO method -- correct but slow.
 *
 * `ide` is the IDE controller package (parent of all the disk@/cd@
 * children).
 *
 * Returns NO_ERROR on success or partial success (some drives
 * upgraded). A failure return is reserved for catastrophic problems
 * like inability to allocate the PRD table; the caller should treat
 * non-NO_ERROR as "stay on PIO across the board" rather than fatal.
 */
Retcode ide_dma_install(Environ *e, Package *ide);

#endif /* IDE_DMA_H */
