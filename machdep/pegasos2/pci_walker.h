/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Recursive PCI enumeration -- the clean-room fix for Bug 2
 *  (docs/09-known-bugs.md). Walks both MV64361 host bridges,
 *  issuing Type-0 cycles on each host's primary bus and Type-1
 *  cycles on all buses beneath PCI-to-PCI bridges.
 *
 *  No IEEE-1275 device-tree nodes are created here -- that is
 *  Phase-2 work. This walker prints the discovered topology to
 *  UART1 so Phase-1 boots can be visually audited, and it programs
 *  bridge bus-number registers so subsequent accesses from this
 *  firmware (or the Forth runtime later) can reach every device.
 */

#ifndef PCI_WALKER_H
#define PCI_WALKER_H

/* Enumerate both host bridges, print the tree on UART1. */
void pci_walk(void);

#endif
