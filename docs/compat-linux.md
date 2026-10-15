# Modo de Compatibilidade Linux do OvsbMk — Arquitetura

> Documento de referencia para quem mexe no kernel. Se algo aqui divergir do codigo, o codigo vence. Atualize o doc junto com o patch.
> **Referencia: Linux 5.15 LTS (syscall table + ABI x86_64).**

## Indice

- [1. Motivacao e objetivo](#1-motivacao-e-objetivo)
- [2. Arquitetura em camadas](#2-arquitetura-em-camadas)
- [3. Deteccao de binario Linux](#3-deteccao-de-binario-linux)
- [3.1 — execve e loader ELF passo a passo](#31--execve-e-loader-elf-passo-a-passo)
- [3.2 — Entradas AUXV fornecidas](#32--entradas-auxv-fornecidas)
- [4. Fluxo de uma syscall — exemplo `open("/dev/tty", O_RDWR)`](#4-fluxo-de-uma-syscall--exemplo-opendevtty-o_rdwr)
- [5. Traducao ABI](#5-traducao-abi)
- [6. Emulacao vs delegacao](#6-emulacao-vs-delegacao)
- [7. Subsistema TTY e /dev/tty](#7-subsistema-tty-e-devtty)
- [8. Gerenciamento de fds na camada compat](#8-gerenciamento-de-fds-na-camada-compat)
- [9. Catalogo de syscalls obrigatorias](#9-catalogo-de-syscalls-obrigatorias)
- [10. Limitacoes conhecidas e roadmap](#10-limitacoes-conhecidas-e-roadmap)
- [11. Como testar](#11-como-testar)
- [12. Referencias](#12-referencias)

## 1. Motivacao e objetivo

Rodar binarios ELF Linux x86_64 sem recompilar. Alvos iniciais: `dash`/`sh`, `coreutils` (`ls`, `cat`, `true`), `xterm`, `Xorg` + `fvwm2`. Depois: qualquer app userspace que rode em musl/glibc.

Por que nao recompilar tudo para Mach-O? Mach-O e nosso formato legado (`mach_o.c:load`). Recompilar Xorg + xterm + bibliotecas (libX11, libxcb, freetype, fontconfig, dbus) exigiria portar toolchain e todas as dependencias. Reusar ELF Linux nos da acesso direto ao ecossistema Debian/Arch sem forkar cada projeto.

O que ganhamos:
- Executa `BIN/SH`, `BIN/XTERM`, `BIN/XORG` ja compilados para Linux (`disk.img` em FAT32/ext2).
- Reusa `ld-linux-x86-64.so.2` / `ld-musl-x86_64.so.1` via `PT_INTERP` sem reimplementar linker.

O que custa:
- Tradutor de ABI (numeros de syscall diferentes, structs com layout distinto, errno, flags).
- Loader ELF + relocacoes (`elf64.zig`) + AUXV + interpreter.
- Stubs/emulacoes para ~80 syscalls; cada um com semantica propria e testes.

Nao-objetivo: LSB completo, namespaces, cgroups, seccomp, ptrace, ou compat 32-bit.

## 2. Arquitetura em camadas

```
+-----------------------------+
|  app Linux (ELF x86_64)     |  /BIN/SH, /BIN/XTERM, /BIN/XORG
|  libc Linux (musl/glibc)    |  faz syscall via `syscall` / int 0x80
+--------------+--------------+
               | trap
               v
+--------------+--------------+
| syscall_entry.asm           |  OvsbMk/kernel/syscall_entry.asm
|  - salva GPRs em kstack     |  frame de 17 qwords: [0..14]=GPRs inverse
|  - chama syscall_handler_zig|  [15]=RIP, [16]=RFLAGS, [18]=CS, [20]=RSP (irq)
+--------------+--------------+
| syscall_linux.zig           |  OvsbMk/kernel/syscall_linux.zig:359
|  - linux_to_tipos[512]      |  traduz nr Linux -> nr TipOS
|  - r10 -> rcx (arg4)        |  regs[1]=regs[7]
+--------------+--------------+
| syscall.c                   |  OvsbMk/kernel/syscall.c:~1400-5100 switch(num)
|  - dispatch por nr TipOS    |  cada case chama helper nativo
+-------+----------+----------+
        |          |          |
        v          v          v
     VFS        scheduler   TTY/drivers
  OvsbMk/fs/  OvsbMk/kernel/  OvsbMk/kernel/drivers/
  vfs.c/h     process.c/h     tty.c/h, keyboard.zig,
  fat32.c     signal.c/h      mouse.zig, vm_map.c
  ext2*.zig   vm_map.c/h
```

Papeis por arquivo:

- `syscall_entry.asm`: entry point do `int 0x80` e `syscall`. Constroi `regs[0..16]` e chama `syscall_handler_zig`. Nao contem logica de compat.
- `syscall_linux.zig:203`: tabela `linux_to_tipos[512]`. Inicializada como identidade, depois sobrescrita (ex: `map[0]=3` read, `map[59]=208` execve, `map[231]=212` exit_group). O dispatcher `syscall_handler_zig:359` decide se e Linux ou TipOS nativo (`tipos_native` para 198-211) e corrige `regs[1]=regs[7]` para arg4.
- `syscall.c`: handler nativo. `regs[4]=a1 (rdi), regs[3]=a2 (rsi), regs[2]=a3 (rdx), regs[1]=a4 (rcx), regs[5]=a5 (r8), regs[6]=a6 (r9)` apos traducao. Retorno em `regs[0]` (negativo = `-errno`).
- `process.c/h:40`: PCB (`pcb_t`) com `is_user`, `pml4`, `vm_map`, `cwd[256]`, `fds_tab`, `controlling_tty`, `actions[64]`, `blocked/pending`, `pgrp/session`.
- `signal.c/h`: `TIPOS_NSIG=64`, `tipos_sigaction_t`, dispatch de sinais. `signal_setsid:164`, `signal_setpgid:148`.
- `drivers/tty.c/h`: PTY master/slave, `tty_pair_t`, `tty_t` com `termios`, `winsize`, ring buffers. `tty_ioctl:141`.
- `fs/vfs.c/h`: VFS fino sobre FAT32/ext2. `vfs_abs_path`, `vfs_read_at`, `vfs_stat_size_attr`. `VFS_E*` em `vfs.h:12`.
- `elf64.zig`: loader ELF. `is_elf64:142`, `PIE_BASE=0x40000000:90`, `elf64_load_into_pml4`, `elf64_load_at`, `elf64_find_interp:420`.
- `vm_map.c`: `vm_map_find:117`, `vm_map_fixed:141`, `vm_map_wire:262` (huge pages 2MB -> 4KB split).

## 3. Deteccao de binario Linux

O kernel nao tem flag `is_linux_compat` separada. Todo processo userspace e tratado como Linux-compat apos `execve`/`spawn` se o arquivo for ELF valido. Criterios:

1. **Magic** `elf64.zig:142`: `buf[0]==0x7F && buf[1]=='E' && buf[2]=='L' && buf[3]=='F'`. Checagem em `syscall.c:3701` (execve) e `3901` (spawn).
2. **Tipo** `elf64.zig:374,452`: `ehdr.e_type == ET_EXEC (2) || ET_DYN (3)`. `ET_DYN` = PIE (Xorg, xterm). `ET_EXEC` = nao-PIE (alguns sh estaticos).
3. **Interpreter** `elf64.zig:421`: `elf64_find_interp` varre `PT_INTERP (3)`. Se >0, binario e dinamico e precisa `ld.so`. Path tipico: `/lib/ld-linux-x86-64.so.2` ou `/lib/ld-musl-x86_64.so.1`. O loader resolve via `vfs_stat_size` + `vfs_read_file` (`syscall.c:3744`).
4. **OSABI nao e usado para decidir**. `e_ident[EI_OSABI]` e ignorado; qualquer ELF x86_64 valido entra no caminho Linux. Um Mach-O cai no `else` (`syscall.c:3958` `mach_o_load_into_pml4`).

Estado no task: `pcb_t.is_user=1` (`process.h:44`) indica userspace. Nao ha flag booleana `is_linux_compat` por task. Ha um conjunto pequeno e fechado de syscalls nativas TipOS (numeros 198–211), usadas apenas por utilitarios built-in do OvsbMk (ovsh e ferramentas internas). Todo o resto do userspace — qualquer ELF carregado via `execve` com `PT_INTERP` ou `ET_EXEC`/`ET_DYN` — e tratado como Linux-compat. A distincao e feita no numero da syscall, dentro de `syscall_linux.zig:359`: se o numero esta na faixa 198–211 (`tipos_native`), vai direto ao handler nativo; senao, passa pela traducao `linux_to_tipos:203`. Manter uma flag por task seria redundante, porque o proprio numero da syscall ja carrega a informacao de qual ABI o processo esta usando.

- Execve le via VFS, valida ELF, cria `child_pml4 = clone_identity_tables():3707`, marca `U/S` com `pml4_add_user:3724` nos segmentos `PT_LOAD`, carrega com `elf64_load_into_pml4:3731` ou `elf64_load_at:3767` para o interpreter, monta pilha Linux com `setup_linux_user_stack{,_dyn}:3819`.

Exemplo concreto (`/BIN/XTERM`):
```
vfs_read_file("/BIN/XTERM") -> 911192 bytes -> EHDR { ET_DYN, PIE_BASE=0x40000000 }
PT_INTERP -> "/lib/ld-musl-x86_64.so.1" -> vfs_read_file -> elf64_load_at(base=0x60000000)
entry final = interp_entry; AUXV AT_BASE=0x60000000, AT_ENTRY=main_entry
```

### 3.1 — execve e loader ELF passo a passo

`SYS_execve:3619` (`OvsbMk/kernel/syscall.c:3619`) e `SYS_spawn:3920` (fork+exec separado) compartilham o mesmo loader. `SYS_execve` substitui a imagem no proprio `pcb_t`; `spawn` cria `child_pml4` novo e chama `proc_spawn`. Passo a passo do `execve`:

1. **Copia args do userspace** (`syscall.c:3631`): `upath/argv/envp` sao copiados para `kpath/kargv/kenvp` estaticos antes de destruir o espaco antigo (evita use-after-free).
2. **Resolve path** (`syscall.c:3671`): `vfs_abs_path(me->cwd, tmp, absp)` resolve relativo vs `cwd`; se `kpath[0] != '/'`, prefixa `/BIN/` (compat `PATH` minimo).
3. **Valida ELF** (`syscall.c:3701` + `elf64.zig:142`): `vfs_stat_size(kpath)` + `vfs_read_file` + `buf[0..3]==0x7F,'E','L','F'`. Checa `ehdr.e_type==ET_EXEC/ET_DYN (2/3)` e `e_machine==EM_X86_64 (62)` em `elf64.zig:374`.
4. **Cria PML4 novo** (`syscall.c:3707`): `child_pml4 = clone_identity_tables()` (copia identity 0-512MB com `U/S` limpo). Marca `U/S` nos segmentos `PT_LOAD` com `pml4_add_user(child_pml4, va)` (`syscall.c:3724`) para cada `va` de `PT_LOAD` alinhado a 2MB.
5. **Carrega binario principal** (`elf64.zig:531` + `syscall.c:3731`): `elf_base = (ET_DYN ? PIE_BASE : 0)` com `PIE_BASE=0x40000000` (`elf64.zig:90`). `elf64_load_into_pml4(buf, fsize, child_pml4)` mapeia cada `PT_LOAD` com `mmap_user`/`map_2mb_ensure:118` e copia `p_filesz` bytes; BSS (`p_memsz>p_filesz`) fica zero (alocado mas nao copiado). `main_entry = ehdr.e_entry + elf_base`.
6. **Se PT_INTERP presente** (`elf64.zig:421` `elf64_find_interp` + `syscall.c:3744`): le path (ex: `"/lib/ld-musl-x86_64.so.1"` ou `"/lib/ld-linux-x86-64.so.2"`), resolve via VFS, `kmalloc` + `vfs_read_file`, `interp_base = 0x60000000` (`syscall.c:3766`, `PIE_BASE_INTERP`), `elf64_load_at(interp_data, size, child_pml4, interp_base)` -> `interp_entry`. Aplica relocacoes do interpreter (ver passo 7). Se `interp_len<=0`, binario e estatico e pula este passo.
7. **Relocacoes** (`elf64.zig:55` `R_X86_64_RELATIVE=8`, `DT_RELA/DT_RELRSZ/DT_RELR`): `elf64_load_{into,at}` aplica `R_X86_64_RELATIVE` (soma `base` no `r_offset`) e, se `DT_JMPREL/DT_SYMTAB` presentes, `R_X86_64_JUMP_SLOT/GLOB_DAT` delegadas ao `ld.so` quando ha `PT_INTERP`. Para ET_DYN sem interpreter (raro), o kernel aplica apenas `RELATIVE`/`RELRSZ`; PLT fica para runtime (nao suportado sem `ld.so` — documentado como gap).
8. **Stack novo** (`syscall.c:3783`): `ustack = mmap_user(0, 1M, 3, 0)` + `pml4_add_user_range(child_pml4, ustack, ustack+1M)` (so stack tem `U/S`; janela identity 2MB mantem frames de kernel isolados).
9. **Setup AUXV + argv/envp** (`process.c:551` `setup_linux_user_stack` / `process.c:519` `setup_linux_user_stack_dyn`): empilha strings, ponteiros, `argc`, e vetor `auxv` (ver §3.2). Para dinamico: `setup_linux_user_stack_dyn(..., interp_base, main_entry, 1)` com `AT_BASE=interp_base`, `AT_ENTRY=main_entry`; para estatico: `setup_linux_user_stack(..., main_entry)` com `AT_BASE=elf_base`.
10. **Troca CR3 e iretq** (`syscall.c:3814` `mov cr3, child_pml4`; `syscall.c:3850` `regs[15]=entry, regs[16]=0x202, regs[0]=0` + `iretq` via `syscall_entry.asm:119`). Se ha interpreter, `entry = interp_entry` (nao `main_entry`); o `ld.so` resolve `DT_NEEDED` e salta para `AT_ENTRY` no fim.

### 3.2 — Entradas AUXV fornecidas

`setup_linux_user_stack:630` (`OvsbMk/kernel/process.c:630`) empilha AUXV de tras pra frente (`AT_NULL` primeiro, depois pares `value,type` com `*--sp`). Tabela abaixo reflete `process.c:646`:

| Constante | Valor x86_64 | O que é | Fornecido? |
|---|---|---|---|
| AT_NULL | 0 | fim do vetor | OK — `*--sp=0,*--sp=0` (`process.c:646`) |
| AT_PHDR | 3 | endereco dos program headers do binario | OK se `phnum>0` (`process.c:652`), senao omitido |
| AT_PHENT | 4 | tamanho de cada program header | OK se `phnum>0` (`process.c:653`), senao omitido |
| AT_PHNUM | 5 | numero de program headers | OK se `phnum>0` (`process.c:654`), senao omitido |
| AT_PAGESZ | 6 | tamanho de pagina (4096) | OK — `0x1000` (`process.c:657`) |
| AT_BASE | 7 | base do interpreter (ld.so) ou do PIE | OK — `elf_base` (`process.c:658`), dinamico: `interp_base=0x60000000` |
| AT_ENTRY | 9 | entry point do binario principal | OK se `at_entry!=0` (`process.c:648`), senao omitido (musl ld.so usa sem AT_ENTRY) |
| AT_UID / AT_EUID | 11 / 12 | UID real/efetivo | X — nao fornecido (stub: glibc `getuid()` via syscall compensa, mas `ld.so --verify` pode checar) |
| AT_GID / AT_EGID | 13 / 14 | GID real/efetivo | X — idem |
| AT_HWCAP | 16 | CPU features | X — nao fornecido; musl ignora, glibc usa para IFUNC (fallback sem otimizar) |
| AT_CLKTCK | 17 | `sysconf(_SC_CLK_TCK)` | X — nao fornecido |
| AT_SECURE | 23 | 0 (nao setuid) | OK — `0` (`process.c:656`) |
| AT_RANDOM | 25 | 16 bytes aleatorios (canary) | OK — `0xBAADF00D.../0xDEADBEEF...` (`process.c:630`) + `random_addr` (`process.c:659`) |
| AT_EXECFN | 31 | path do binario | OK se `argc>0` (`process.c:661`), aponta para `str_ptrs[0]` |
| AT_SYSINFO_EHDR | 33 | vDSO | X — nao fornecido (sem vDSO; `clock_gettime` via syscall funciona, mas `gettimeofday` via vDSO falha e cai para syscall) |

Gaps marcados `X`/`⚠️`: nenhum quebra `Xorg`/`xterm`/`sh` hoje porque musl faz fallback para syscall (`getuid`/`gettimeofday`). Quebraria app que le `AT_HWCAP` para escolher `memcpy` AVX sem checar `hwcap==0`, ou `AT_SYSINFO_EHDR` assumindo vDSO presente (retornaria `NULL` e daria PF se dereferenciado sem checagem).

## 4. Fluxo de uma syscall — exemplo `open("/dev/tty", O_RDWR)`

```
userspace:  mov rax,2; mov rdi,"/dev/tty"; mov rsi,2; syscall
    │
    ▼
kernel:     syscall_entry.asm — salva GPRs em kstack
    │       (syscall_entry.asm:50, 15 GPRs + RIP/RFLAGS)
    ▼
            syscall_linux.zig: linux_to_tipos[2] = SYS_open (5)
                               regs[1] = regs[7]   (r10 → rcx, arg4)
    │
    ▼
            syscall.c: switch(SYS_open) → case SYS_open:1934
                        ├── compat /dev/tty (controlling_tty? dup : open_dev)
                        ├── open_dev (virt: /dev/fb0 / ttyN / ptmx / pts) :410
                        └── VFS resolve_at + alloc_fd + type=0
    │
    ▼
            ret = fd | -errno;  regs[0] = ret  (syscall.c:5155)
    │
    ▼
kernel:     iretq — restaura RIP/RFLAGS/RSP (syscall_entry.asm:119)
    │
    ▼
userspace:  rax = fd  (ou -errno; errno = -rax em musl)
```

1. **Userspace**: musl faz `mov rax, 2; mov rdi, "/dev/tty"; mov rsi, 2; syscall` (Linux `open=2`). Ou `openat(AT_FDCWD, "/dev/tty", ...)` (257). No TipOS antigo, apps tambem usavam `int 0x80` com mesma ABI.
2. **Trap**: `syscall_entry.asm:27` (via `syscall`) e `idt.asm:98` (`int 0x80`) salvam GPRs no kstack do processo. Layouts distintos — fonte da verdade em `OvsbMk/kernel/syscall_entry.asm:50` e `OvsbMk/kernel/idt.asm:244` (ver comentarios nos `.asm`):

   - **Syscall frame** (`syscall_entry.asm`, 15 GPRs + RIP/RFLAGS — 17 qwords): `regs[0]=rax(num), [1]=rcx, [2]=rdx, [3]=rsi, [4]=rdi, [5]=r8, [6]=r9, [7]=r10, [8]=r11, [9]=rbx, [10]=rbp, [11]=r12, [12]=r13, [13]=r14, [14]=r15, [15]=RIP, [16]=RFLAGS` (RIP/RFLAGS salvos fora do array em `[rsp]`/`[rsp+8]` antes dos pushes; veja `syscall_entry.asm:44`). Sem `CS`/`RSP`/`SS`.
   - **IRQ frame** (`idt.asm:isr_common`, 15 GPRs + int_no/err + iretq — 22 qwords): `regs[0]=r15, [1]=r14, [2]=r13, [3]=r12, [4]=r11, [5]=r10, [6]=r9, [7]=r8, [8]=rbp, [9]=rdi, [10]=rsi, [11]=rdx, [12]=rcx, [13]=rbx, [14]=rax, [15]=int_no, [16]=err_code, [17]=RIP, [18]=CS, [19]=RFLAGS, [20]=RSP, [21]=SS` (ordem de push `rax..r15` em `idt.asm:246`; checagem ring `regs[18] & 3` em `signal.c:188`).
3. **syscall_linux.zig:359** `syscall_handler_zig(regs)`:
   ```zig
   mapped = linux_to_tipos[num];          // 2 -> 5 (SYS_open)
   if (!tipos_native) regs[1] = regs[7];  // r10 -> rcx (arg4)
   syscall_handler(regs);                 // chama C
   ```
   Para `open`, sem arg4, a copia e inofensiva. Para `ppoll`/`pselect6`/`select`, sem ela `regs[1]` contem lixo e o kernel escreve em `0x7118F6AD` (GPF).
4. **syscall.c:1934** `case SYS_open`:
   - `path = fixpath((char*)a1)` — normaliza `/dev/tty`.
   - **Compat /dev/tty** (`syscall.c:1945`): se `path=="/dev/tty"` e `process_current()->controlling_tty != NULL`, aloca fd novo `FD_TTY_SLAVE` apontando para `controlling_tty` e retorna esse fd. Nunca retorna 0/1/2. Se `controlling_tty==NULL`, tenta `open_dev` e cai para `ret=0` (stdin) como fallback.
   - `open_dev(path, flags):410` tenta dispositivos virtuais (`/dev/fb0`->5, `/dev/ttyN`->6, `/dev/input/mice`->7, `/dev/input/eventN`->FD_EVDEV, `/dev/ptmx`->posix_openpt). Se nao casar, retorna -1 e o fluxo continua para VFS.
   - Se `resolve_at(AT_FDCWD, path)` succeeds, abre/cria arquivo real via `alloc_fd():341`, preenche `fds[fd].name`, `type=0`, `pos=0`, `used=1`, retorna fd.
5. **Retorno**: `syscall.c:5113` `regs[0]=ret`. Em erro, `ret = (uint64_t)-VFS_E*` (ex: `-1` = EPERM). Em sucesso, `ret = fd >=0`. O `iretq` restaura RIP/RFLAGS/RSP. Userspace ve `rax = -1` e `errno = -rax`.

**Tabela de dispatch (trecho `syscall_linux.zig:203`):**

| nr Linux | nome Linux | nr TipOS | handler `syscall.c` | obs |
|---|---|---|---|---|
| 0 | read | 3 | `SYS_read` | delega VFS/TTY/socket/pipe |
| 1 | write | 4 | `SYS_write` | idem |
| 2 | open | 5 | `SYS_open:1934` | compat /dev/tty, open_dev, VFS |
| 3 | close | 6 | `SYS_close` | `close_fd` |
| 8 | lseek | 204 | `SYS_lseek:2046` | `fds[fd].pos` |
| 9 | mmap | 197 | `SYS_mmap:2580` | `vm_map` + file-backed via `vfs_read_at` |
| 10 | mprotect | 74 | `SYS_mprotect:2647` | `vm_map_protect` + `pml4_walk_ptes` |
| 11 | munmap | 73 | `SYS_munmap:2649` | `vm_munmap` |
| 13 | rt_sigaction | 134 | `SYS_sigaction` | `signal_sigaction` |
| 16 | ioctl | 54 | `SYS_ioctl:2731` | TIOCGWINSZ, TCGETS, EVIOC*, VT_* |
| 57 | fork | 214 | `SYS_fork_real:3612` | `proc_fork` |
| 59 | execve | 208 | `SYS_execve:3619` | loader ELF + interpreter |
| 79 | getcwd | 79 | `case 79:2690` | `process_current()->cwd` |
| 80 | chdir | 80 | `case 80:2706` | `vfs_abs_path` + `vfs_stat_size_attr` |
| 257 | openat | 257 | `SYS_openat:2176` | mesmo compat /dev/tty de `SYS_open` |

**Conversao de errno**: `vfs.h:12` define `VFS_EPERM=1, ENOENT=2, EIO=5, EBADF=9, ENOMEM=12, EACCES=13, EFAULT=14, EEXIST=17, ENOTDIR=20, EINVAL=22, ENOSPC=28, EROFS=30, ENAMETOOLONG=36, ENOTEMPTY=39`. Kernel retorna `(uint64_t)-VFS_E*`. Musl espera Linux errno (mesmos valores para os basicos). Nao ha tabela de traducao; os codigos coincidem para o subset usado.

## 5. Traducao ABI

Principio: mapear numeros, flags e layouts na borda compat, minimizar copies no kernel.

**Numeros**: `syscall_linux.zig:203` resolve colisoes onde TipOS reutilizou numeros (ex: `map[90]=402` chmod colide com sched_yield 24; `map[47]=92` recvmsg colide com getgid). Apps Linux veem `chmod=90` e caem em `SYS_chmod=402:2590` no C.

**Flags** — tipicamento pass-through porque coincidimos valores por construcao ou normalizamos:

- `O_*` (`syscall.h`): `O_ACCMODE`, `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_DIRECTORY`, `O_CLOEXEC=0x80000`, `O_NONBLOCK=0x800`. `SYS_openat:2180` valida `supported_flags` e rejeita `O_ACCMODE==3` com `-EINVAL`.
- `PROT_*` e `MAP_*`: `mmap:2587` aceita `MAP_ANON=0x20`, `MAP_FIXED=0x10`, `MAP_SHARED=0x01`. `prot` e `flags` vao direto para `vm_map`.
- `SIG*`: `TIPOS_SIGINT=2, SIGKILL=9, SIGCHLD=17, SIGWINCH=28` (`signal.h:16`) — mesmos numeros Linux. `SA_SIGINFO=0x04, SA_NODEFER=0x40000000, SA_RESTORER=0x04000000` (`signal.h:12`).

**Struct layout** — onde diverge, emulamos ou traduzimos campo a campo:

- `stat`: `SYS_stat:2055`, `SYS_fstat:2048` via `stat_path_fill` / `fd_stat_fill`. Preenche `struct stat` Linux (st_mode, st_size, st_ino) a partir de `vfs_stat_size_attr` + `fat32`/`ext2`. Nao expomos `st_dev` real.
- `termios`: `TTY_TCGETS/TCSETS/TCSETSW/TCSETSF` (`syscall.c:2808`). `tty_termios_t` em `drivers/tty.h` tem `c_lflag`, `c_cc[6]` com `ICANON|ECHO|ISIG`. `tty_ioctl:173` copia `target->termios` direto; layout coincide com Linux para o subset usado por `st`/`sh`/`xterm`. Diverge em `c_iflag/c_oflag` nao modelados.
- `sigaction`: `tipos_sigaction_t { handler, flags, restorer, mask }` (`signal.h:27`). `signal_sigaction:39` copia via `process->actions[signo-1]`.
- `pollfd`: `SYS_poll:7` recebe array `struct pollfd { fd, events, revents }` do caller. Copia para kernel, chama `poll_one` por fd, devolve `revents`. `POLLIN/POLLOUT/POLLHUP` sao pass-through.
- `winsize`: `TIOCGWINSZ/TIOCSWINSZ` (`tty.h: TTY_TIOCGWINSZ`). `tty->winsize { ws_row=24, ws_col=80 }` (`tty.c:58`).

Padrao de codigo usado:
```c
// 1. validar ponteiro de usuario
if (!user_buffer_valid(argp, sizeof(struct foo), writable)) return -VFS_EFAULT;
// 2. copiar/convertir
struct foo k; memcpy(&k, argp, sizeof(k)); // ou campo a campo se layout diverge
// 3. operar em estado kernel (fds[], tty, VFS)
// 4. copiar de volta se writable
// 5. ret = 0 ou -errno
```
Exemplo real `SYS_ioctl` TTY (`syscall.c:2808`):
```c
if (req == TTY_TIOCSCTTY) {
    tty_t *ctl = fds[fd].tty;
    if (ctl->flags & TTY_FLAG_MASTER) { /* ptmx -> slave */ }
    if (current) current->controlling_tty = controlling;
    ret = 0; break;
}
ret = tty_ioctl(fds[fd].tty, req, argp);
```

## 6. Emulacao vs delegacao

Delegamos quando a semantica nativa ja e POSIX e o backend existe. Emulamos quando o kernel nao tem o subsistema ou o layout diverge.

**Delegados ao kernel nativo** (chamam VFS/process/vm_map/tty direto):

- `read/write/readv/writev/pread64/pwrite64` — `fds[].type` dispatch: `FD_TTY*` -> `tty_read/write`, `4` socket -> `socks[]`, `3` pipe -> `pipes[]`, `0` arquivo -> `vfs_read_at/write_at`.
- `open/openat/close/dup/dup2/dup3/pipe/pipe2` — `alloc_fd` + `fds[]` + `open_dev`.
- `lseek` — `fds[fd].pos`.
- `stat/fstat/lstat/newfstatat/statx` — `stat_path_fill` via VFS.
- `mmap/mprotect/munmap` — `vm_map_*` + `vm_map_wire` (huge page split).
- `fork/execve` — `proc_fork:3612`, `SYS_execve:3619`.
- `chdir/getcwd` — `pcb_t.cwd` + `vfs_abs_path`.
- `ioctl` TTY/VT/evdev/fb — `tty_ioctl`, `FBIO_*`, `EVIOC*`.

**Emulados na camada compat** (sem backend nativo completo):

- `uname (63:4464)` — struct `utsname` fake: `sysname="Linux", nodename="tipos", release="5.15.0", version="#1", machine="x86_64"`. Apps checam `uname` para decidir path; sem isso, `Xorg` e `dbus` abortam.
- `prctl (157:4341)` — `PR_SET_NAME/PR_SET_DUMPABLE` ignorados, retorna 0. `weston` usa.
- `futex (202->96:4375)` — single-core: `FUTEX_WAIT` compara `*uaddr==val`, `FUTEX_WAKE` acorda. Sem fila real; round-robin ja serializa.
- `epoll_create/epoll_ctl/epoll_wait` (`213/232/233:3325`) — fd fake `FD_EPOLL`, `epolls[]` + `epoll_watch_t`. `ctl` registra, `wait` faz poll linear.
- `getrandom (318:4660)` — `RDRAND` ou `pit_ticks` como fallback.
- `umask (95->400:4591)` — `static cur_umask`.
- `setuid/setgid/chown/chmod` (`410/411/402-405:2590`) — no-op (FAT32 sem dono/permissao), retorna 0. Sem isso `sh` falha em `chown` durante instalacao.
- `arch_prctl (158:4135)` — `ARCH_SET_FS/ARCH_GET_FS` mapeado para `pcb_t.fs_base` (TLS musl). `map_user_4kb` + `wrfsbase`.
- `rseq (334:4368)`, `set_robust_list (273:4357)` — no-op retorna 0; glibc desativa rseq se ENOSYS, mas 0 e mais seguro.
- `timerfd/eventfd/signalfd` (`283/290`) — fd fake com `counter`/`flags`, integrado com `poll`.

Justificativa: delegar reduz duplicacao e mantem VFS/VM como fonte da verdade. Emular evita implementar subsistemas inteiros (scheduler de futex, LSM para prctl, VFS permission para chmod) antes de ter userspace que prove que precisa.

## 7. Subsistema TTY e /dev/tty

Cada `pcb_t` tem `controlling_tty` (`process.h:68`). E a TTY controladora da sessao, herdada no `fork` (`process.c:989` `p->controlling_tty = par->controlling_tty`) e limpa no `setsid` (`signal.c:172` `current->controlling_tty = 0`).

`/dev/tty` nao e um device com major/minor. E um alias para "a TTY controladora deste processo". `open("/dev/tty")` deve **sempre** devolver fd novo `>=3` apontando para o mesmo `tty_t` da controladora, nunca reusar 0/1/2.

Por que nao retornar 0? `sh` faz:
```c
fd = open("/dev/tty", O_RDWR);
tcgetattr(fd, &t);
close(fd);
```
Se `open` devolve 0 (stdin), o `close` fecha stdin. O proximo `tcgetattr(0)` falha com `EBADF`/`EPERM` e o shell morre `0: Operation not permitted` (visto em `xterm -e /BIN/SH` com `pid=0xC` -> `exit code 2` apos fix `getcwd`).

Implementacao (`syscall.c:1945` e `2171` para `SYS_open` e `SYS_openat`):

```c
if (str_equal(path, "/dev/tty")) {
    pcb_t *ct = process_current();
    if (ct && ct->controlling_tty) {
        int d = alloc_fd();
        fds[d].type = FD_TTY_SLAVE;
        fds[d].tty = ct->controlling_tty;
        fds[d].used = 1;
        fds[d].name[0] = '\0'; fds[d].path[0] = '\0';
        return d;
    }
    // fallback: tenta /dev/ttyN se existir, senao stdin
    int devfd = open_dev(path, flags);
    if (devfd != -1) return devfd;
    return 0;
}
```

Ligacao com sessoes:

- `setsid (112:4347)` -> `signal_setsid():164` cria nova sessao, `pgrp = session = pid`, `controlling_tty = NULL`.
- `setpgid (109:4353)` -> `signal_setpgid(pid, pgrp)`. Sem grupos reais, cada processo e grupo de si; a chamada e aceita se pid existe.
- `TIOCSCTTY (0x540E)` (`syscall.c:2834`) — `ioctl(fd, TIOCSCTTY, 0)` torna `fds[fd].tty` (ou slave do ptmx) a `controlling_tty` do processo. `xterm` faz `ioctl(pts_slave, TIOCSCTTY)` apos `fork` antes de `execve(sh)`.
- `TIOCNOTTY` (`syscall.c:2853`) limpa `controlling_tty`.

PTY: `posix_openpt:371` aloca `tty_pair_t` com master+slave. `grantpt:389` é no-op que valida `FD_TTY_MASTER` (`-EBADF` se não for master, senão `0`). `unlockpt:396` valida e chama `tty_pair_unlock`. `ptsname:403` valida e chama `tty_pair_name`. O `EACCES` vem de `open("/dev/pts/N"):427` — `open_dev_impl:432` retorna `-VFS_EACCES` se `pair->flags & TTY_PAIR_FLAG_UNLOCKED == 0`, não de `grantpt`. `xterm` faz `posix_openpt -> grantpt -> unlockpt -> ptsname -> open(pts) -> fork -> dup2(pts, 0/1/2) -> TIOCSCTTY -> execve(SH)`.

## 8. Gerenciamento de fds na camada compat

### Tabela

`syscall.c:41` `fdent_t fds[MAX_FDS]` (ou `fd_tables[MAX_PROC]` quando `fork #72` — `fds_tab` por processo, `process.h:61`). Cada entrada:

```c
typedef struct {
    int used;
    int type;        // 0=arquivo, 3=pipe, 4=socket, 5=fb, 6=ttyN, 7=mice, 8=master, 9=slave, evdev, epoll
    int flags;       // O_* 
    uint64_t pos;    // arquivo
    char name[256];  // path original (para VFS)
    char path[256];  // path resolvido
    struct tty *tty; // se TTY
    int pipe_idx, sock_idx, tty_pair_idx;
} fdent_t;
```

Ciclo:

- `alloc_fd():341` varre `3..MAX_FDS-1` e retorna primeiro `!used`. `0/1/2` sao reservados (stdin/out/err, nunca alocados).
- `install`: preenche `type/TTY/pipe/sock`, `flags`, `name/path`, `used=1`. Retorna fd.
- `close_fd():534` limpa: `used=0`, `type=-1`, `name[0]='\0'`, `path[0]='\0'`, zera `tty/pipe/sock_idx`, `pos=0`.
- `dup2/dup3` copiam `fds[oldfd]` para `fds[newfd]` (com `close` previo se `newfd` usado).

### Estudo de caso: bug `'\\0'` vs `'\0'`

No fix de `/dev/tty` (`syscall.c:1955`), a limpeza foi escrita como:

```c
fds[d].name[0] = '\\0'; fds[d].path[0] = '\\0';
```

Isso e um **multi-character literal**. Em C, `'\\0'` nao e `'\0'`. `'\\0'` sao dois caracteres: `'\'` (0x5C) e `'0'` (0x30) dentro de aspas simples. Em GCC, o valor e calculado como `(0x5C << 8) | 0x30 = 0x5C30`, um `int`. Ao atribuir a um `char` (signed em x86_64), o valor e truncado para os 8 bits menos significativos, resultando sempre em `0x30` (`'0'`). Nao depende de endianness do CPU — a ordem dos bytes afeta o valor do `int`, mas o truncamento pra `char` pega o LSB deterministicamente. O warning e `-Wmultichar` (incluido em `-Wall`), nao `-Wall` diretamente. O byte nao vira zero; vira `0x30`.

Consequencia: `fds[d].name[0]` fica `'0'` (0x30), nao `'\0'` (0x00). `str_equal`/`k_strstr`/`vfs_abs_path` que checam `name[0]=='\0'` para detectar "sem path" falham. O ponto-chave: `alloc_fd():341` varre `fds[]` checando apenas `used` — o bug **nao** esta em `alloc_fd` pular slot. O slot e reaproveitado corretamente (`used=0` em `close_fd():534`). O bug e sobre conteudo residual: `close_fd` deveria zerar `name[0]` e `path[0]` com `'\0'`, mas escreve `'0'` (0x30). A consequencia real: helpers que usam `fds[fd].path[0] ? path : name` (ex: `vfs_read_at` via `fds[fd].path[0] ? fds[fd].path : fds[fd].name`) veem `path[0]='0'` nao-zero e tratam `"0"` como path valido. `vfs_stat_size("0")` falha ou abre arquivo errado, e o `open("/dev/tty")` subsequente pode receber fd 0 em vez de fd novo. O slot e reaproveitado, mas o lixo em `path`/`name` engana os helpers que dependem de `path[0]=='\0'` para detectar slot vazio.

Correcao: `'\0'` (um char, valor 0):

```c
fds[d].name[0] = '\0'; fds[d].path[0] = '\0';
```

Moral: nunca use `CHAR` multi-char para zerar string. `-Wmultichar` avisa. No kernel, wrapper `str_equal` que espera `'\0'` terminal quebra silenciosamente se o terminador nao for zero. Adicione `static_assert('\0'==0)` em `syscall.h` ou use `memset(name,0,sizeof(name))`.

Referencias no codigo: `syscall.c:1955` (open), `syscall.c:2187` (openat), `syscall.c:384-385` (posix_openpt), `syscall.c:445` (pts open) — todos devem usar `'\0'`.

## 9. Catalogo de syscalls obrigatorias

Status: `OK` implementada e testada com `sh`/`xterm`/`Xorg`, `~` parcial (aceita mas sem semantica completa), `X` faltando (retorna `-1` no `default:5075`).

### Processos

| nr L | nr T | nome | status | nota |
|---|---|---|---|---|
| 39 | 20 | getpid | OK | `SYS_getpid` |
| 110 | 110 | getppid | OK | `SYS_getppid` |
| 102/104/107/108 | 24/47/25/48 | getuid/gid/euid/egid | OK | retornam 0 |
| 105/106 | 410/411 | setuid/setgid | OK | no-op |
| 57 | 214 | fork | OK | `SYS_fork_real` via `proc_fork` |
| 59 | 208 | execve | OK | ELF + interpreter + AUXV |
| 60 | 1 | exit | OK | `SYS_exit` |
| 231 | 212 | exit_group | OK | `SYS_exit_group` |
| 61 | 61 | wait4 | OK | `SYS_wait4` |
| 247 | 247 | waitid | ~ | stub — mapeado em `syscall_linux.zig:294` (`map[247]=247`) mas sem handler em `syscall.c` (cai no `default:5120` -> `ret=-1` `ENOSYS`). Nenhum app testado usa `waitid` ainda (todos usam `wait4:61`); stub por falta de uso. `waitid` exige `siginfo_t` + `idtype` `P_PID/P_PIDFD/P_ALL` + flags `WNOWAIT/WSTOPPED` nao modelados |
| 62 | 62 | kill/tgkill/tkill | OK | `SYS_kill:62`, `SYS_tgkill:103`, `SYS_tkill:95` via `signal_send_*` |
| 112 | 112 | setsid | OK | `signal_setsid` |
| 109 | 109 | setpgid | OK | `signal_setpgid` |
| 121 | 121 | getpgid/getpgrp | OK | `SYS_getpgid` |
| 218 | 218 | set_tid_address | OK | `SYS_set_tid_address` |
| 273/274 | 273/274 | set/get_robust_list | OK | `SYS_set_robust_list` |
| 334 | 334 | rseq | OK | no-op |
| 186 | 186 | gettid | OK | `SYS_gettid` |

### Memoria

| nr L | nr T | nome | status | nota |
|---|---|---|---|---|
| 9 | 197 | mmap | OK | `vm_map` + file-backed `vfs_read_at`, 2MB huge split |
| 10 | 74 | mprotect | OK | `vm_map_protect` + `pml4_walk_ptes` |
| 11 | 73 | munmap | OK | `vm_munmap` |
| 12 | 231 | brk | OK | `program_break`/`heap_start` em PCB |
| 25 | 75 | mremap | X | `-1` (musl cai para malloc) |
| 26 | 26 | msync | OK | no-op |
| 28 | 28 | madvise | OK | no-op |
| 158 | 158 | arch_prctl | OK | `ARCH_SET_FS` -> `pcb_t.fs_base` |

### Arquivos

| nr L | nr T | nome | status | nota |
|---|---|---|---|---|
| 2 | 5 | open | OK | compat /dev/tty |
| 257 | 257 | openat | OK | idem |
| 0/1 | 3/4 | read/write | OK | VFS/TTY/socket/pipe |
| 20 | 409 | readv/writev | OK | iovec loop |
| 8 | 204 | lseek | OK | |
| 17/18 | 215/216 | pread64/pwrite64 | OK | |
| 3 | 6 | close | OK | |
| 72 | 72 | fcntl | OK | `F_GETFD/SETFD/GETFL/SETFL` |
| 32/33/292 | 32/91/292 | dup/dup2/dup3 | OK | |
| 22/293 | 22/293 | pipe/pipe2 | OK | `pipes[]` |
| 4/5/6 | 188/189/199 | stat/fstat/lstat | OK | `stat_path_fill` |
| 262 | 262 | newfstatat | OK | |
| 332 | 332 | statx | ~ | parcial |
| 21/269 | 33 | access/faccessat | OK | |
| 90/91/92/93 | 402-405 | chmod/fchmod/chown/fchown | OK | no-op |
| 137/138 | 406/407 | statfs/fstatfs | OK | fake 64MB |
| 78/217 | 207 | getdents/getdents64 | OK | `vfs_readdir` |
| 79 | 79 | getcwd | OK | `case 79:2690` |
| 80 | 80 | chdir | OK | `case 80:2706` |
| 83/84 | 136/137 | mkdir/rmdir | OK | |
| 258 | 258 | mkdirat | ~ | |
| 263 | 263 | unlinkat | OK | |
| 267 | 267 | readlinkat | OK | |
| 88/89/86 | 88/89/86 | symlink/readlink/link | OK | link=copy (FAT32) |
| 221 | 221 | fadvise64 | ~ | no-op |
| 285 | 285 | fallocate | ~ | no-op |

### Sinais

| nr L | nr T | nome | status | nota |
|---|---|---|---|---|
| 13 | 134 | rt_sigaction | OK | `signal_sigaction` |
| 14 | 14 | rt_sigprocmask | OK | `signal_sigprocmask` |
| 15 | 15 | rt_sigreturn | OK | `signal_rt_sigreturn` |
| 127/129/130 | 127/129/130 | rt_sigqueueinfo/timedwait | ~ | stub |
| 131 | 131 | sigaltstack | ~ | |

### IPC / rede

| nr L | nr T | nome | status | nota |
|---|---|---|---|---|
| 41-50,53,288 | 41-53,288 | socket/bind/listen/accept/connect/sendto/recvfrom/sendmsg/recvmsg/shutdown/socketpair/accept4/getsockname | OK | `socks[]` table, unix domain |
| 7 | 7 | poll | OK | |
| 23 | 23 | select | OK | |
| 270/271 | 270/271 | pselect6/ppoll | OK | `regs[1]=regs[7]` fix |
| 213/232/233/291/281 | 213/232/233/291/281 | epoll_* | OK | fake epoll |
| 202 | 96 | futex | ~ | single-core cmp |

### TTY / tempo / misc

| nr L | nr T | nome | status | nota |
|---|---|---|---|---|
| 16 | 54 | ioctl | OK | TIOCGWINSZ, TCGETS/SETS, TIOCSCTTY/NOTTY, VT_*, FBIO_*, EVIOC* |
| 157 | 157 | prctl | OK | no-op |
| 63 | 63 | uname | OK | fake Linux 5.15.0 |
| 96 | 116 | gettimeofday | OK | PIT/RTC |
| 228/229 | 228/229 | clock_gettime/clock_getres | OK | |
| 318 | 318 | getrandom | OK | |
| 97/160/302 | 97/160/302 | getrlimit/setrlimit/prlimit64 | OK | stub |
| 98/100 | 98/100 | getrusage/times | ~ | |
| 99/239 | 401/239 | sysinfo/sysinfo | ~ | |
| 95 | 400 | umask | OK | `cur_umask` |
| 165/166 | 165/166 | mount/umount2 | ~ | |
| 165 | 165 | mount | ~ | |

### Minimo para subir cada alvo

- `true`/`echo`: `execve + exit_group + write` (3 syscalls).
- `ls`: `+ openat + getdents64 + stat + brk/mmap`.
- `sh -c "echo hi"`: `+ fork + wait4 + pipe + dup2 + sigaction + getcwd/chdir + access + ioctl(TCGETS)`.
- `tty`: `+ open("/dev/tty") + ioctl(TIOCGWINSZ) + isatty(fstat)`.
- `xterm`: `+ posix_openpt/ptsname/unlockpt + TIOCSCTTY + setsid/setpgid + poll/select/pselect6 + socket/connect (X11) + mmap(file-backed libs)`.
- `Xorg`: `+ /dev/fb0 mmap + /dev/input/event* + VT ioctls + epoll + getrandom + shm* (via mmap anon)`.

## 10. Limitacoes conhecidas e roadmap

**Nao generico ainda:**

- `uname` fake. Apps que checam `release` para habilitar `io_uring` ou `memfd` veem `5.15.0` (Linux 5.15 LTS — referencia do modo compat, ver §12) e tentam syscalls nao implementadas (retornam `-1`). Alguns abortam se `release < 5.x`. Fix: `uname` deve refletir feature set real ou `ENOSYS` seletivo.
- `epoll` fake linear. `Xorg` com muitos fds (10+) faz `epoll_wait` em loop; nosso `epoll_wait` faz poll linear por `watch`, O(n*m). Nao escala, mas nao quebra ainda.
- `futex` single-core. Sem fila de espera real. `FUTEX_WAIT_PRIVATE` com `val` mismatch retorna 0; apps que dependem de wake ordering podem livelock em SMP futuro.
- `/dev/tty` compat depende de `controlling_tty` herdado via `fork`. Se app faz `open("/dev/tty")` antes de `TIOCSCTTY` (ex: `sh` sem `xterm`), cai no fallback `stdin`. Funciona, mas nao e POSIX estrito (deveria falhar com `ENXIO` se sem controladora).

**Quebra em apps que checam uname/features:**

- `dbus` tenta `socket(AF_UNIX, ...)` em `/run/dbus/system_bus_socket` e espera `SO_PEERCRED`. Nao implementamos `getsockopt(SO_PEERCRED)` — `poll`/`recvmsg` funcionam, mas `dbus` loga `AccessDenied` (`/tmp/tipos.log:13111`) e segue sem bus (Xorg funciona sem dbus, mas `systemd`-aware apps nao).
- `glibc` `getrandom` com `GRND_RANDOM` espera entropia bloqueante; retornamos `pit_ticks` e o caller aceita, mas `systemd` logaria warning.

**Falta para Wayland:**

- `memfd_create (319)` / `shm_open` via `memfd` — Wayland usa `memfd` para buffers. Hoje `shm` e via `mmap anon` + `pipes`; precisa `memfd_create` + `ftruncate` + `mmap` compartilhado entre `fork` (COW).
- `eventfd2 (290)` — ja mapeado `map[290]=290` mas handler `SYS_eventfd2` e stub sem `EFD_NONBLOCK` real.
- `signalfd/signalfd4 (282/283)` — nao mapeado.
- `inotify (253-255)` — `weston` usa para hot-reload de config.
- DRM/KMS (`ioctl` em `/dev/dri/card0`) — Wayland precisa `DRM_IOCTL_MODE_GETRESOURCES`, `CREATE_DUMB`, `MMAP_DUMB`. Hoje so `FBIO_*` para VESA.

**Falta para epoll/io_uring:**

- `epoll_pwait2`, `io_uring_setup (425)`, `io_uring_enter (426)`, `io_uring_register (427)` — nenhum mapeado. `io_uring` exigiria ring buffers compartilhados + `mmap` + `eventfd` + `IORING_OP_*`.
- `splice (275)`, `tee (276)`, `vmsplice (278)` — usados por `socat`/`pipe` otimizados.
- `pidfd_open (434)`, `clone3 (435)` — `clone` atual e `SYS_fork_real` simplificado; falta `CLONE_PIDFD`.

Roadmap sugerido (ordem de impacto):

1. `memfd_create + ftruncate + shm` (desbloqueia Wayland).
2. `signalfd + eventfd` completos (desbloqueia `systemd`/`weston` event loop sem poll-busy).
3. `inotify` minimo (create/modify/delete).
4. `getsockopt(SO_PEERCRED)` / `SO_PASSCRED` (silencia dbus).
5. `splice/tee` (performance de pipe).
6. `io_uring` stub que retorna `ENOSYS` limpo (apps fazem fallback para `epoll`).
7. DRM dumb buffers (KMS minimo).

## 11. Como testar

Ordem crescente de complexidade. Cada binario exercita um layer a mais. Rode no QEMU headless via `bash /tmp/run_auto.sh` (90s + `screendump /tmp/screen.ppm` + greps) ou `make run-curses`.

| # | comando | o que exercita | falha tipica |
|---|---|---|---|
| 1 | `/BIN/TRUE` ou `true` | `execve + exit_group` | `PF@` se `PIE_BASE` errado (`0x40000000` vs `0x100000`) |
| 2 | `echo hi` | `write(1)` + `exit` | `EBADF` se `fds[1]` nao herdado no `fork` |
| 3 | `ls /` | `openat + getdents64 + stat + brk/mmap` | `ENOSYS` se `getdents64` nao mapeado (`map[217]=207`) |
| 4 | `sh -c "echo hi"` | `fork + wait4 + pipe + dup2` | `SH: Operation not permitted` se `/dev/tty` devolve 0 (bug `'\\0'`) |
| 5 | `tty; echo $?` | `open("/dev/tty") + ioctl(TIOCGWINSZ) + isatty` | `not a tty` se `controlling_tty` nulo (sem `TIOCSCTTY`) |
| 6 | `sh -c "ls /dev/pts; exec sh"` | `posix_openpt + ptsname + fork + TIOCSCTTY` | `open("/dev/pts/N"): EACCES` se `TTY_PAIR_FLAG_UNLOCKED` nao setado (sem `unlockpt`), `ptsname: EBADF` se fd nao e `FD_TTY_MASTER`, `TIOCSCTTY: EPERM` se ja tem controladora |
| 7 | `XORG -config /ETC/X11/XORG.CONF` | `socket AF_UNIX + /dev/fb0 mmap + /dev/input/event* + VT ioctls` | `no screens found` se `/dev/fb0` mmap nao faz `pml4_map_phys` |
| 8 | `xterm -e /BIN/SH` | `fork(pty) + execve(SH) + pselect6 + poll` | `failed to map segment from shared object` se `mmap file-backed` nao faz `vfs_read_at` por pagina |
| 9 | `fvwm2` (ou `disp-wm`) | `X11 client: poll + recvmsg + shm + epoll` | `fatal IO error 0` se `close` do filho derruba `socks[peer]` (bug `sock-hold` sem varrer `fd_tables[]`) |

Interpretacao de falhas:

- `ret = -1` com `errno 1 (EPERM)` no log `[sys] n=... ret=0xFFFFFFFF` — syscall caiu no `default:5075` (nao mapeada) ou stub que retorna `-VFS_EPERM`.
- `ret=0xFFFFFFF5 (-11)` — `EAGAIN` em `read`/`poll` nonblock (normal).
- `PF@0x...` / `EXC#13` — mapeamento faltando: `vm_map_wire` nao quebrou huge page, ou `pml4_add_user` nao marcou `U/S`.
- `SLAB-BAD` / `POISON` — double-free ou OOB em `vm_object`/`pcb_table`.
- `Operation not permitted` do SH — `open("/dev/tty")` devolveu fd 0 ou `ioctl(TCGETS)` falhou por `controlling_tty==NULL`.

Script de referencia (`/tmp/run_auto.sh`):

```bash
#!/bin/bash
set -e
cd ~/TipOS-staging
pkill -9 -f qemu-system-x86_64 || true
sleep 2
rm -f /tmp/tipos.log /tmp/screen.ppm
qemu-system-x86_64 -boot order=d -cdrom ~/TipOS-staging/TipOS.iso -m 512M \
  -drive file=~/TipOS-staging/disk.img,format=raw,index=0 \
  -serial file:/tmp/tipos.log -no-reboot -display none \
  -monitor unix:/tmp/qmon.sock,server,nowait &
QPID=$!
sleep 90
echo "screendump /tmp/screen.ppm" | socat -T10 - unix-connect:/tmp/qmon.sock
sleep 2
kill -9 $QPID 2>/dev/null || true
pkill -9 -f qemu-system-x86_64 || true
echo "=== wc ==="; wc -l /tmp/tipos.log
echo "=== PF/EXC/SLAB/accept ==="; grep -cE "PF@|EXC#" /tmp/tipos.log; grep -cE "SLAB-BAD|POISON|DUP" /tmp/tipos.log; grep -c "accept() failed" /tmp/tipos.log
echo "=== execve/ptmx/ovsh ==="; grep -nE "execve\] ok|ptmx|ovsh|XTERM" /tmp/tipos.log | tail -20
echo "=== tail ==="; tail -60 /tmp/tipos.log | tr -d '\r'
```

Para visual, `identify /tmp/screen.ppm` deve ser `1280x800 P6`; tela preta total (`brilho medio 0.02`) = Xorg nao desenhou (provavel `mmap FB` falhou).

## 12. Referencias

> **Referencia do modo compat: Linux 5.15 LTS (syscall table + ABI x86_64).** Toda mencao a `man 2`/`ABI` abaixo assume essa versao, salvo nota.

- **FreeBSD linuxulator**: `sys/compat/linux/linux_syscall.c` + `linux_to_bsd_syscall`. Mesma ideia de tabela de traducao, mas com `linux_sysent` + `linux_syscall` + `linux_emuldata`. Ver `compat/linux/linux_misc.c` para `uname`/`prctl` emulados.
- **CONFIG_IA32_EMULATION** (Linux): `arch/x86/entry/syscalls/syscall_64.tbl` + `arch/x86/ia32/ia32entry.S` + `compat_sys_*`. Mostra como lidar com structs 32-bit em kernel 64-bit (nosso problema de `stat`/`termios` e analogo).
- **ABI x86_64 Linux**: `man 2 syscall` (convencao `rax=num, rdi, rsi, rdx, r10, r8, r9; syscall; ret rax`), `man 2 open`, `man 2 ioctl_tty`, `man 2 termios`, `Documentation/x86/x86_64/abi.txt` (System V AMD64 ABI). Para `struct stat` Linux vs TipOS, compare `stat(2)` + `bits/stat.h` (glibc) vs `syscall.c:2055` `stat_path_fill`.
- **ELF**: `man 5 elf`, `elf(5)` — `ET_EXEC` vs `ET_DYN`, `PT_INTERP`, `DT_RELA/RELRSZ`, `R_X86_64_RELATIVE`. Loader referencia: `elf64.zig:96` + `vm_map.c:262` huge page split.
- **TTY**: `man 4 tty`, `man 4 ptmx`, `man 2 setsid`, `man 2 setpgid`, `man 2 ioctl_tty` (`TIOCSCTTY`, `TIOCNOTTY`, `TIOCGPTN`, `TIOCSPTLCK`), `Documentation/driver-api/tty.rst`.
- **VFS TipOS**: `OvsbMk/fs/vfs.h:42` `vfs_abs_path`, `OvsbMk/fs/fat32.c` `fat32_stat`/`fat32_read_file`, `OvsbMk/fs/ext2_new.zig` para ext2. Dispatch em `syscall.c:1934` (`SYS_open`) e `2176` (`SYS_openat`).
- **Sinais**: `man 7 signal`, `man 2 sigaction`, `signal(7)` `SA_SIGINFO/SA_NODEFER/SA_RESTORER`. No kernel: `signal.h:12` flags, `signal.c:39` sigaction, `signal.c:188` dispatch irq/syscall.

---

*Ultima atualizacao: 2026-10-09 — patch A1–A7 + B1–B3: fix `'\\0'` (0x5C30→0x30, -Wmultichar, sem endianness), §3 is_linux_compat via 198–211, §4 frame regs syscall(17)/IRQ(22) + diagrama ASCII, §8 cadeia `alloc_fd` vs residual, §7/§11 grantpt alinhado (EACCES em `open(/dev/pts/N)`), §9 waitid nota, §10/§12 Linux 5.15 LTS, §3.1 loader ELF 10 passos, §3.2 AUXV 14 entradas, indice com ancoras. Build: `make -C OvsbMk && grub-mkrescue -o TipOS.iso OvsbMk/iso`. Teste: `bash /tmp/run_auto.sh` (90s + screendump /tmp/screen.ppm).*
