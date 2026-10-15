# TipOS Doom Port Report

## Status

The DoomGeneric core builds as a static x86-64 ELF at `0x10000000`, and the kernel ELF loader starts it in ring 3. FreeDoom is installed in the test disk image as `/APPS/DOOM1.WAD`. The complete Doom menu has not appeared yet: initialization reaches WAD loading, then repeatedly page-faults at `D_ProcessEvents` (`RIP=0x10002C23`, `CR2=0`). The `I_InitGraphics` callback is not reached in the current run.

The ring 3 framebuffer path has been tested independently and works. `SYS_disp_get_fb` returned an active 32-bit framebuffer with dimensions 1280x800 and pitch 5120 bytes; a ring 3 test wrote four full-width color bands and slept without returning to the shell. The captured result is [doom-ring3-fbtest.png](doom-ring3-fbtest.png).

## Implemented

- Imported the portable DoomGeneric C sources and headers, excluding the host/platform backends and optional audio, joystick, ENDOOM, statdump, and SHA1 modules.
- Added the TipOS framebuffer, timer, keyboard, and window-title glue, plus a Doom entrypoint that selects `/APPS/DOOM1.WAD`.
- Added a standalone static ELF build in `src/userland/progs/doom/Makefile` and a repeatable `fbtest` target.
- Extended the TipOS stdio surface for Doom: pooled `FILE` objects, EOF tracking, `fseek`/`ftell`/`feof`, `fscanf`, `snprintf`/`vsnprintf`, `vfprintf`, `fflush`, `remove`, and `rename`. The formatter handles integer width/precision, including `%i`, `%03d`, and `%.3d`.
- Corrected libc POSIX syscall numbers to use the Linux-compatible dispatcher ABI; `exit` now uses Linux `_exit` 60, and `lseek` resolves through Linux syscall 8 to TipOS 204.
- Made the VFS initialize FAT32 on fallback, added positional FAT32 reads and a per-file cluster-chain cache, and incremented timer ticks for IRQ0 received inside kernel syscalls.
- Added ELF segment metadata and source/destination byte comparison diagnostics in `elf64_load_into_pml4`.

## Initial Repository Reconnaissance

The requested starting files were inspected before edits: `elf64.zig` (443 lines), `syscall.c` (3107), `syscall.h` (155), `stdio.c` (522), `stdio.h` (90), the userland linker script (21), the userland Makefile (83), and the root Makefile (59). The loader accepts static `ET_EXEC` ELF files at `0x10000000`; TipOS syscalls are Linux-translated at the `int 0x80` boundary; the normal userland pipeline packages Mach-O, so Doom received its own static ELF target.

## Root Causes Found

- FAT32 was selected by `vfs_mount`, but `fat32_init` was never called. The BPB and root cluster therefore remained zero, making the shell unable to find files.
- The libc sent TipOS syscall numbers through a dispatcher that first translates Linux syscall numbers. For example, `open(5)` was interpreted as `fstat`, `read(3)` as `close`, and `exit(1)` as `write`.
- FAT32 `vfs_read_at` ignored the requested offset. Doom reread the WAD header instead of its lump directory, producing `PNAMES not found`.
- IRQ0 in ring 0 acknowledged the PIC but did not increment `timer_ticks`, so `nanosleep` inside a syscall could never complete.
- TipOS syscall 200 collides with Linux `tkill`; the dispatcher now recognizes the framebuffer call by its output-pointer arguments. Syscall 198 is likewise disambiguated for the no-argument TipOS `kbhit` call.

## Build And Test

- `make userland`: passed; the existing graphy target still builds.
- `make -C src/userland/progs/doom`: passed; produces a static ELF. Remaining compiler/linker warnings are the existing RWX linker segment and an upstream pointer-to-int cast.
- `make kernel`: passed after the VFS header declaration was corrected.
- `make iso`: passed.
- QEMU booted with FAT32, and `exec doom.elf` loaded the ELF segment with matching copied bytes.
- The complete Doom process loaded FreeDoom and advanced through WAD/game initialization, but currently faults in `D_ProcessEvents` before `I_InitGraphics` and the first rendered Doom frame.
- `make -C src/userland/progs/doom fbtest`: the isolated ring 3 framebuffer test paints all four bands across the 1280x800 output and remains active for inspection.

## Next Blocker

Investigate the repeated user-mode page fault in `D_ProcessEvents`. Its first instruction accesses the RIP-relative `storedemo` BSS variable, while the diagnostic page-fault log reports `CR2=0`; the inspected BSS huge-page entry has the user bit set. Resolve this mismatch before changing the renderer. Once that fault is fixed, rerun Doom through `I_InitGraphics` and confirm the Doom menu and keyboard events in QEMU.