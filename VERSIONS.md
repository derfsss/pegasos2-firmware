# Upstream version pins

Every file under `upstream/` is a verbatim copy of a specific
snapshot of an external project. This file records the exact
snapshots in use so the clean-room audit can reproduce them.

## upstream/smartfirmware/

- Source: https://github.com/openbios/smartfirmware
- Commit: `06ef397e962b8b1c86a57e8c0515f2d9d0a19c70` (master at time
  of vendoring, 2026-04-21)
- License: CodeGen source license — see `LICENSES/CodeGen-smartfirmware.txt`
- Obtained via: `git clone --depth 1`; `.git` removed after clone
- Local modifications: **none** at time of vendoring. Any future
  changes go under `patches/smartfirmware/` as separate patch
  files, never as edits to `upstream/smartfirmware/` in place.

## upstream/x86emu/

- Source: U-Boot 2015.d as distributed by ACube Systems for the
  Sam460ex board
- URL: https://www.acube-systems.biz/download.php?file=u-boot-2015.d_prod.tar.gz
- SHA-256 of downloaded tarball: (record on next pull)
- Path within tarball: `u-boot-2015.d/drivers/bios_emulator/`
- License: SciTech permissive — see `LICENSES/SciTech-x86emu.txt`
- Obtained via: `tar xzf` then `cp -r drivers/bios_emulator/. upstream/x86emu/`
- Files excluded: `Makefile` (U-Boot-specific, not reusable)
- Local modifications: **none** at time of vendoring. Spec 09
  requires additions — they go under `patches/x86emu/` as
  separate patch files, never as edits to `upstream/x86emu/`
  in place. Required patches (see docs/09-known-bugs.md §Bug 1
  Required behaviour):
    - `0F FE` opcode (PADDB)
    - `0F 01` / `0F 20` / `0F 22` coverage verification
    - INT 10h fallback stubs for AH 0x00..0x1F
    - BDA + IVT initialisation
