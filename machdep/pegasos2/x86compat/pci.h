/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *
 *  Shim for U-Boot's <pci.h> when compiling the vendored
 *  upstream/x86emu/ sources outside U-Boot. The core emulator
 *  (ops/ops2/prim_ops/decode/debug/sys) does NOT use any of the
 *  pci_dev_t / pci_read_config_* symbols -- those are only in the
 *  wrapper layers we are replacing with our own
 *  (machdep/pegasos2/x86_bios.c, future). So this file stays empty
 *  and just exists to satisfy the <pci.h> #include.
 *
 *  If wrapper files ever get compiled against this tree, add the
 *  missing pci_dev_t typedef and pci_{read,write}_config_* prototypes
 *  here.
 */

#ifndef COMPAT_PCI_H
#define COMPAT_PCI_H

#endif
