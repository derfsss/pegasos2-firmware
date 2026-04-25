/*
 *  Copyright (c) 2026 Pegasos2 clean-room rewrite contributors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted under the terms of the CodeGen source
 *  license reproduced in LICENSES/CodeGen-smartfirmware.txt.
 *
 *  Forward declarations for the pegasos2-specific Forth words that
 *  platform.c registers via init_pegasos2[] / init_pegasos2_ci[].
 *  Each word is implemented in a topic-specific source file:
 *
 *    machine_hooks.c   machine_probe_all, machine_secondary_diag,
 *                      machine_init_load, machine_init_program,
 *                      machine_go (the SF machdep contract surface)
 *
 *    forth_diag.c      test-ci, set-bootargs, heap-info, test-ci-boot
 *                      (firmware-internal diagnostics)
 *
 *    rtc_word.c        get-time-of-day, set-time-of-day, plus the
 *                      install_pegasos2_ci_services hook that adds
 *                      these to /openprom/client-services
 *
 *    boot_kernel.c     boot-kernel, test-boot, test-boot-bad
 *
 *    pci_tree.c        ls-pci
 *    ide_driver.c      test-ide-probe, test-read-block, test-iso-ls,
 *                      test-aliases
 *    smart_boot.c      smart-boot
 *
 *  The init_pegasos2[] table in platform.c references these via a
 *  single header rather than duplicating the prototypes in every
 *  consumer.
 */

#ifndef PEGASOS2_WORDS_H
#define PEGASOS2_WORDS_H

#include "defs.h"

/* SF machdep contract (machine_hooks.c). EC() in defs.h declares
 * these as `extern Retcode <name>(Environ *e)`; we add them here
 * for documentation. */
extern Retcode machine_probe_all(Environ *e);
extern Retcode machine_secondary_diag(Environ *e);
extern Retcode machine_init_load(Environ *e);
extern Retcode machine_init_program(Environ *e);
extern Retcode machine_go(Environ *e);

/* Diagnostic words (forth_diag.c) */
extern Retcode f_test_ci(Environ *e);
extern Retcode f_set_bootargs(Environ *e);
extern Retcode f_heap_info(Environ *e);
extern Retcode f_test_ci_boot(Environ *e);

/* RTC words (rtc_word.c) */
extern Retcode f_get_time_of_day(Environ *e);
extern Retcode f_set_time_of_day(Environ *e);
extern Retcode install_pegasos2_ci_services(Environ *e);

/* Boot-path words (boot_kernel.c) */
extern Retcode f_boot_kernel(Environ *e);
extern Retcode f_test_boot(Environ *e);
extern Retcode f_test_boot_bad(Environ *e);

/* Hardware-probe words */
extern Retcode f_ls_pci(Environ *e);              /* pci_tree.c */
extern Retcode f_test_ide_probe(Environ *e);      /* ide_driver.c */
extern Retcode f_test_read_block(Environ *e);     /* ide_driver.c */
extern Retcode f_test_iso_ls(Environ *e);         /* ide_driver.c */
extern Retcode f_test_aliases(Environ *e);        /* ide_driver.c */

/* OS dispatcher (smart_boot.c) */
extern Retcode f_smart_boot(Environ *e);

#endif /* PEGASOS2_WORDS_H */
