# Roadmap Auto-Produção do OvsbMk — do shell ao desktop auto-hospedado

> Plano de execução para o OvsbMk auto-produtivo. Arquitetura/ABI em `docs/compat-linux.md` (não repete §2-§8 lá). Ref syscalls: Linux 5.15 LTS x86_64.

## Índice

- [1. Objetivo e definição de auto-produção](#1-objetivo-e-definicao-de-auto-producao)
- [2. Estado atual](#2-estado-atual)
- [3. Bloqueadores críticos](#3-bloqueadores-criticos)
- [4. Fase 7.0 — Auto-produção textual](#4-fase-70--auto-producao-textual-nivel-1)
- [5. Fase 7.5 — Desktop com janelas](#5-fase-75--desktop-com-janelas-nivel-2)
- [6. Fase 8 — Toolchain completa](#6-fase-8--toolchain-completa-nivel-3)
- [7. O que não fazer agora](#7-o-que-nao-fazer-agora)
- [8. Cronograma sugerido](#8-cronograma-sugerido)
- [9. Riscos e mitigações](#9-riscos-e-mitigações)
- [10. Próximos passos concretos](#10-proximos-passos-concretos)

---

## 1. Objetivo e definição de auto-produção

Auto-produção = o sistema compila software não-trivial sem sair dele. Três níveis:

```
[Nível 1] textual ──► [Nível 2] gráfico ──► [Nível 3] auto-hospedagem
  sem X11               X11 + WM + IDE         git clone + make iso + boot
```

### Nível 1 — auto-produção textual (alvo imediato)

No OvsbMk, TTY/PTY puro, sem X11:

- abrir editor (`vim`/`micro`/`busybox vi`), escrever `hello.c`;
- rodar `make`/`tcc`/`cc` e gerar binário ELF;
- executar o binário e ver saída no mesmo terminal;
- shell POSIX (`dash`/`busybox sh`) com pipes, redirecionamento e `for`.

Validação: `vim hello.c` → `tcc hello.c -o hello && ./hello` → `hello` na tela.

### Nível 2 — auto-produção gráfica

Nível 1 + X11:

- `Xorg` sobe com driver `fbdev` em `/dev/fb0`;
- WM (`twm`/`fvwm2`) gerencia janelas;
- `xterm` roda shell e editor;
- IDE simples (`geany` ou `xterm + vim`) já conta como IDE gráfica mínima.

### Nível 3 — auto-hospedagem total

Nível 2 + toolchain completa dentro do SO:

- `git clone` do próprio `TipOS-staging` dentro do OvsbMk;
- `make kernel && make iso` gera `TipOS.iso` nova;
- boot da ISO nova no QEMU aninhado ou reboot.

Fora de escopo agora.

---

## 2. Estado atual

Baseado no §9 de `docs/compat-linux.md` e leitura de `syscall.c`, `syscall_linux.zig`, `vm_map.c`, `drivers/tty.c`.

Legenda: OK=implementado; ~=stub retorna 0; ⚠️=parcial (quebra tcc/make); ❌=ausente/ENOSYS.

| Subsistema | Status | Bloqueia Nível 1? | Evidência |
|---|---|---|---|
| fork/execve/wait4 | OK | não | `syscall_linux.zig:255` `map[57]=214`, `syscall.c:3612` `proc_fork`, `syscall.c:3619` `SYS_execve` com `elf64_load_into_pml4`, `syscall.c:221` `wait4` |
| open/openat/read/write/close | OK | não | `syscall.c:1400` `SYS_read/write`, `syscall.c:1945/2176` `open/openat`, `syscall.c:7` `readv/writev` via `iovec` loop |
| dup/dup2/dup3/pipe/pipe2 | OK | não | `syscall.c:341` `alloc_fd`, `syscall.c:190` `pipes[]`, `syscall.h:1607` |
| lseek/pread/pwrite | OK | não | `syscall.c:204` `SYS_lseek`, `215/216` `pread64/pwrite64` |
| stat/fstat/lstat/newfstatat/statx | OK/~ | não | `syscall.c:2048/2055` `stat_path_fill`, `syscall.c:332` `statx` parcial |
| mmap anon + file-backed read | ⚠️ parcial | **SIM** | `syscall.c:2577` `SYS_mmap` popula com `vfs_read_at` (`syscall.c:2639`) mas força `flags|=MAP_ANON` (`syscall.c:2614`) — perde `MAP_SHARED`, sem write-back; `vm_map.c:15` `vm_alloc_page` |
| mprotect/munmap | OK | não | `syscall.c:2652` `vm_munmap`, `syscall.c:2656` `vm_mprotect`, `vm_map.c:262` `vm_map_wire` faz split 2MB→4KB |
| brk | OK | não | `syscall.c:4173` `SYS_brk`, `process.h:55` `program_break` |
| mremap | ❌ | **SIM** | `syscall.c:2668` `ret=-1` stub; `syscall_linux.zig:234` `map[25]=75`; musl `mallocng` cai em `malloc` mas `tcc` reloca com `mremap` |
| msync/madvise | ~ (no-op) | não | `syscall.c:2660` `madvise ret=0`, `syscall.c:2664` `msync ret=0` — ok até precisar flush |
| AUXV | ⚠️ incompleto | **SIM** | `process.c:646` fornece `AT_NULL/AT_ENTRY/AT_PHDR/PHENT/PHNUM/AT_PAGESZ/AT_BASE/AT_RANDOM/AT_SECURE/AT_EXECFN`; falta `AT_UID/EUID/GID/EGID/AT_CLKTCK/AT_HWCAP/AT_FLAGS` — ver §3.2 de `compat-linux.md` |
| fsync/fdatasync | ❌ | **SIM** | sem `case` em `syscall.c`; sem `map[74]/map[75]` em `syscall_linux.zig:203` — `vim`/`make` chamam `fsync` ao salvar |
| ftruncate/truncate | ❌ | **SIM** | sem `case` em `syscall.c`; Linux 76/77 não mapeados em `syscall_linux.zig` — `tcc -o` e `ld` truncam saída |
| fallocate/fadvise64 | ~ (no-op) | não | `syscall.c:285` `fallocate` não existe, `221` `fadvise64` é `map[221]=221` mas handler ausente → cai no `default:5075` `-ENOSYS`; `tcc` tolera |
| futex | ~ (spin) | talvez | `syscall.c:4383` `SYS_futex` com `while(*uaddr==val) schedule()` sem fila; `syscall_linux.zig:224` `map[202]=96`; bloqueia se `tcc`/`make -j` usar threads |
| epoll/poll/select | OK/~ | talvez | `syscall.c:3325` `epoll_create/ctl/wait` fake linear `epolls[]`, `syscall.c:7` `poll`, `syscall.c:23` `select`; ok para `node` mas O(n·m) |
| socket/unix | OK | não | `syscall.c:41` `socks[]`, `syscall_linux.zig:244` `socket/bind/connect` |
| TTY/PTY | ⚠️ quase | não (N1) | `drivers/tty.c:37` `tty_alloc_role`, `syscall.c:1945` `/dev/tty` compat, `syscall.c:2834` `TIOCSCTTY`; `tty_ioctl:141` cobre `TCGETS/TCSETS/TIOCGWINSZ` |
| /dev/fb0 mmap | ⚠️ esboçado | bloqueia N2 | `syscall.c:2587` `FD_FB` via `pml4_map_phys` funciona mas `FBIOGET_VSCREENINFO` stub em `syscall.c:2732` `SYS_ioctl` |
| /dev/input/event* | ⚠️ esboçado | bloqueia N2 | `syscall.c:2747` `FD_EVDEV` com `EVIOCGID/NAME/BIT` fake; `read` de `input_event` ainda é `mice_read` cru (`syscall.c:3466`) |
| Xorg | ❌ | bloqueia N2 | `BIN/XORG` carrega via `elf64.zig:90` mas `vt`/`fb`/`evdev` incompletos → `no screens found` (§11) |
| arch_prctl FS | OK | não | `syscall.c:4143` `ARCH_SET_FS/GET_FS` → `pcb_t.fs_base` (`process.h:59`), `switch.asm` offset `0xB8` |
| getrandom/uname/prctl | OK | não | `syscall.c:4464` `uname` fake `Linux 6.1-tipos`, `syscall.c:4486` `getrandom` com `AT_RANDOM` fixo, `syscall.c:4342` `prctl` no-op |

---

## 3. Bloqueadores críticos

Ordem de ataque (menor esforço → maior desbloqueio):

### 3.1 mmap file-backed write-back

- **O que falta:** `syscall.c:2613` converte file-backed em anon e copia com `vfs_read_at` (`syscall.c:2639`). Sem `vm_object` com `backing fd + offset` e sem `msync`/`munmap` que flushe páginas sujas via `vfs_write_at`. `MAP_SHARED` vira `MAP_PRIVATE` silencioso
- **Quem precisa:** `tcc` mapeia `.o`/`.a` com `MAP_PRIVATE` (ok read-only) mas `ld` escreve saída via `mmap(MAP_SHARED)`; `vim` com `mmap` de swap; `git` com `mmap` de packfiles.
- **Complexidade:** alta.
- **Atacar:** 1º. Fazer `MAP_PRIVATE` (COW já existe) depois `MAP_SHARED` dirty + `vm_map_wire`→`vfs_write_at` em `msync`/`munmap`.

### 3.2 AUXV incompleto

- **O que falta:** `process.c:646` monta 8 entradas; musl precisa `AT_UID/EUID/GID/EGID` (11-14), glibc checa `AT_CLKTCK` (17)/`AT_HWCAP` (16); ver §3.2 `compat-linux.md`.
- **Quem precisa:** `busybox`/`dash` toleram, mas `tcc` com `glibc` dinâmica e `python3` abortam sem `AT_HWCAP`; `make` lê `AT_CLKTCK` para `times`.
- **Complexidade:** baixa.
- **Atacar:** 2º. Adicionar 5 pares em `process.c:646` (`*--sp = uid; *--sp = 11`).

### 3.3 mremap

- **O que falta:** `syscall.c:2668` retorna `-1` incondicional; `syscall_linux.zig:234` já mapeia `25→75`. Sem `vm_map_find_entry` + `vm_map_remove` + `vm_map_fixed` com `MREMAP_MAYMOVE`.
- **Quem precisa:** `tcc` realoca arena de código com `mremap`; `musl mallocng` usa `mremap` para expandir `mmap` anon — hoje cai no path `malloc` mas fragmenta.
- **Complexidade:** média.
- **Atacar:** 3º. Implementar `mremap` anon primeiro (move entry, `pml4` remap), file-backed depois.

### 3.4 ftruncate / truncate

- **O que falta:** Linux 76/77 sem entrada em `syscall_linux.zig:203` e sem `case` em `syscall.c`. Precisa `vfs_truncate` (FAT32 `fat32_truncate` + ext2 `ext2_truncate`).
- **Quem precisa:** `tcc -o hello` truncates saída antes de escrever; `vim :wq` truncates swap; `busybox dd`.
- **Complexidade:** média (VFS já tem `vfs_stat_size`, falta `truncate`).
- **Atacar:** 4º.

### 3.5 fsync / fdatasync

- **O que falta:** Linux 74/75 sem mapeamento e sem handler. FAT32 só flusha FAT no `close`/`sync`.
- **Quem precisa:** `vim` chama `fsync` após `write` para garantir save; `git commit` chama `fsync` no `HEAD`; `make` com `-j` espera `fsync` do `ar`.
- **Complexidade:** baixa (stub `ret=0` já desbloqueia, flush real depois).
- **Atacar:** 5º. Stub `ret=0` imediato, flush real quando VFS tiver `dirty`.

### 3.6 futex com fila (se threads)

- **O que falta:** `syscall.c:4383` faz spin `while(*uaddr==val) schedule()` sem fila de espera nem `FUTEX_WAKE` que acorda waiter específico. Ver §9 de `compat-linux.md` "futex single-core".
- **Quem precisa:** `tcc -pthread`, `make -j`, `python3` com `GIL`.
- **Complexidade:** média (precisa `wait_queue` por `uaddr`).
- **Atacar:** depois de Nível 1 single-thread.

### 3.7 epoll real e /dev/* para Nível 2

Ver Fase 7.5. Não bloqueia Nível 1.

---

## 4. Fase 7.0 — Auto-produção textual (Nível 1)

```
 busybox ─► dash ─► tcc ─► vim ─► make ─► git/lua/python
   │          │       │      │      │
   └─ valida VFS ─┴─ valida fork/pipe ─┴─ valida mmap ─┴─ valida fsync
```

### 4.1 Syscalls a fechar

| Syscall (Linux→TipOS) | Arquivo:linha | O que fazer | Teste | O que pode quebrar |
|---|---|---|---|---|
| mmap write-back `MAP_SHARED` | `syscall.c:2613` força `MAP_ANON`; `vm_map.c:15` `vm_object_create` | guardar `fd+offset` em `vm_map_entry`, `msync`/`munmap` chamam `vfs_write_at` em páginas dirty | `python3 -c "import mmap; m=mmap.mmap(f.fileno(),4096); m[0]=65; m.flush(); open(f.name).read(1)==b'A'"` | `vm_map_wire` (`vm_map.c:262`) precisa marcar dirty; `fork` COW (`process.c:166` `proc_fork`) deve COW de file-backed também |
| AUXV completo | `process.c:646` `setup_linux_user_stack` | adicionar `AT_UID/AT_EUID/AT_GID/AT_EGID/AT_CLKTCK/AT_HWCAP` | `busybox hexdump -C /proc/self/auxv` não existe — testar `LD_SHOW_AUXV=1 /BIN/SH` ou `python3 -c "import os; print(os.sysconf('SC_CLK_TCK'))"` | `sp` alignment (`process.c:627` `align_pad`) — cada par extra muda `aux_words` |
| mremap (25→75) | `syscall.c:2668` stub | implementar `vm_mremap(addr, old_len, new_len, flags)` com `MAYMOVE` | `tcc -run` que realoca arena; `busybox` `mremap` unit | `vm_map_remove` (`vm_map.c:219`) precisa encurtar/dividir entry corretamente |
| ftruncate (77→?) | ausente em `syscall_linux.zig:203` e `syscall.c` | `map[77]=?` + `case 77: vfs_truncate(fds[fd].path, len)` | `busybox truncate -s 0 /TMP/f && stat -c%s /TMP/f` | FAT32 cluster chain (`fs/fat32.c`) truncar sem leak |
| truncate (76) | ausente | idem via `path` | `busybox truncate -s 1M /TMP/g` | `vfs_abs_path` (`fs/vfs.c`) com `cwd` (`process.h:60`) |
| fsync (74) / fdatasync (75) | ausente | `map[74]=74, map[75]=75` + `case: ret=0` (stub), depois flush `fds[fd].path` | `vim` `:wq` sem `EIO`; `git commit` sem `fsync error` | `fs/fat32.c` FAT flush bloqueante pode travar `schedule()` |
| futex mínimo (202→96) | `syscall.c:4383` spin | adicionar `FUTEX_PRIVATE` flag, `FUTEX_WAIT_BITSET` no-op, fila por `uaddr` se `make -j` travar | `dash -c "make -j2"` com `tcc` | `process.c:509` `schedule()` precisa acordar waiter no `FUTEX_WAKE` |

Prioridade: AUXV (1d) → fsync/ftruncate stub (1d) → mmap SHARED (1 sem) → mremap (3d) → futex fila se `make -j` travar.

### 4.2 Binários a portar (ordem exata)

Fontes: Alpine `apk` static (`https://dl-cdn.alpinelinux.org/alpine/v3.19/main/x86_64/`) ou `https://pkgs.alpinelinux.org`. Se URL incerta, `?` e `apk search`.

| Ordem | Binário | Versão | Link | Por que | O que exercita | Critério de sucesso |
|---|---|---|---|---|---|---|
| 1 | busybox | ? (ex: 1.36.1) | `?` — `apk fetch busybox-static` ou `https://busybox.net/downloads/binaries/` | 1 binário, 300+ applets | `openat/getdents64/stat/brk/mmap/execve` | `busybox --list \| wc -l` ≥ 200 e `busybox sh -c "echo hi"` |
| 2 | dash | ? (0.5.12) | `?` — `apk fetch dash` | shell POSIX mínimo | `fork/pipe/dup2/wait4/sigaction` | `dash -c 'echo hi; exit 42'; echo $?` → 42 |
| 3 | tcc | 0.9.27 | `https://download-mirror.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.gz` + build musl estático | compilador C que cabe em 300KB | `mmap file-backed`, `open/read/write`, `ftruncate` | `tcc hello.c -o hello && ./hello` imprime `hello` |
| 4 | vim | ? (9.0) | `?` — `apk fetch vim` estático ou `https://github.com/vim/vim/releases` | editor real | `termios TCGETS/TCSETS`, `ioctl TIOCGWINSZ`, `poll`, `fsync` | abre, edita, `:wq` salva sem `E484` |
| 5 | make | ? (4.4.1) | `?` — `apk fetch make` | build | `fork/exec` pesado, `stat`, `ftruncate` | `make -f Makefile.2` compila 2 `.c` em 1 bin |
| 6 | toybox | ? (0.8.11) | `https://landley.net/toybox/downloads/toybox-0.8.11.tar.gz` | alternativa ao busybox | idem | `toybox ls` funciona |
| 7 | lua | 5.4.6 | `https://www.lua.org/ftp/lua-5.4.6.tar.gz` + `make linux CC="musl-gcc -static"` | linguagem script | `mmap`, `read`, `poll` | `lua -e 'print("hi")'` → `hi` |
| 8 | python3 | ? (3.11) | `?` — `apk fetch python3` estático (se existir) ou `https://www.python.org/ftp/python/3.11.9/Python-3.11.9.tgz` | linguagem | `mmap`, `poll`, `futex`, `getrandom` | `python3 -c "print(1+1)"` → `2` e REPL |
| 9 | git | ? (2.43) | `?` — `apk fetch git-static` | versionamento | `mmap`, `poll`, `fsync`, `socket` (clone local) | `git init; git add .; git commit -m hi` local |
| 10 | tcc + musl headers | — | `tcc` + `musl-1.2.4.tar.gz` headers em `/usr/include` | toolchain completa | tudo acima | compila projeto multi-arquivo com `#include <stdio.h>` |

Como descobrir versão quando `?`: `apk policy` no Alpine, ou `curl -s https://pkgs.alpinelinux.org/package/edge/main/x86_64/busybox-static | grep Version`.

### 4.3 Bateria de testes incremental

Cada linha é um `execve` novo (ver §11 de `compat-linux.md`). Rodar via `bash /tmp/run_auto.sh` (90s + screendump).

| # | Comando | Valida | Se falhar, olhar |
|---|---|---|---|
| 1 | `busybox true; echo $?` | `execve + exit_group` (`syscall_linux.zig:329` `map[231]=212`) | `PF@` no `tipos.log` — `PIE_BASE` (`elf64.zig:90` `0x40000000`) |
| 2 | `busybox echo hi` | `write(1)` + `fds[1]` herdado no `fork` | `EBADF` — `fds_dup_into_slot` (`syscall.c:158`) |
| 3 | `busybox ls /` | `openat + getdents64 + stat` | `ENOSYS` se `map[217]=207` sem `SYS_readdir` (`syscall.c:3548`) |
| 4 | `busybox cat /ETC/PASSWD` | `open/read` VFS | `vfs_read_at` (`fs/vfs.c`) retorna `-ENOENT` — `cwd` errado (`process.h:60`) |
| 5 | `dash -c 'echo hi'` | `fork + wait4 + execve` | `fork` sem `fds` copy — `process.c:989` `controlling_tty` herdado |
| 6 | `dash -c 'echo hi \| cat'` | `pipe + dup2 + fork` | `pipes[rpos/wpos]` (`syscall.c:190`) sem `wend` → `read` nunca vê EOF |
| 7 | `busybox sh -c 'for i in 1 2 3; do echo $i; done'` | shell loop + `access` | `access` stub (`syscall.c:33`) |
| 8 | `tcc --help` | `mmap` file-backed de `tcc` libs | `failed to map segment from shared object` — `syscall.c:2639` `vfs_read_at` por página |
| 9 | `echo 'int main(){puts("hi");}' > /TMP/hello.c && tcc /TMP/hello.c -o /TMP/hello && /TMP/hello` | toolchain completa | `mmap` sem `PROT_EXEC` — `vm_map_wire` (`vm_map.c:262`) não seta `U/S` + `NX` |
| 10 | `vim /TMP/hello.c -c ':wq'` (via `expect` ou `printf 'ihello\e:wq\n'`) | `termios`, `poll`, `fsync`, `ftruncate` | `fsync` ausente → `EIO` no save; `TCSETS` (`tty_ioctl:173`) sem `c_lflag` |
| 11 | `make -f /TMP/Makefile.2` | `fork/exec` em loop, `stat` mtime | `stat` mtime zero — `fat32_stat` sem `mtime` |
| 12 | `busybox grep -r hello /TMP` | `getdents` recursivo + `mmap` | `getdents64` cookie (`syscall.c:68` `dir_cookie`) |

### 4.4 Critério de aceitação do Nível 1

Checklist objetivo — todos devem passar sem `PF@`, `EXC#`, `SLAB-BAD` em `tipos.log`:

- [ ] `busybox --list` ≥ 200 applets.
- [ ] `vim hello.c` abre/edita/`:wq` salva.
- [ ] `tcc hello.c -o hello` compila (0).
- [ ] `./hello` imprime `hello`.
- [ ] `make` compila `a.c+b.c→app` e `app` roda.
- [ ] `dash -c "for i in 1 2 3; do echo \$i; done"` → `1 2 3`.
- [ ] `busybox sh` interativo sem `Operation not permitted` (`syscall.c:1945`).
- [ ] Nenhum `PF@`/`EXC#`/`SLAB-BAD`/`POISON` (ver §11 `compat-linux.md`).

---

## 5. Fase 7.5 — Desktop com janelas (Nível 2)

### 5.1 Syscalls e drivers a fechar

| Item | Arquivo:linha hoje | O que falta | Teste |
|---|---|---|---|
| `/dev/fb0` mmap usuário | `syscall.c:2587` já faz `pml4_map_phys` mas só para `FD_FB` | validar `FBIOGET_VSCREENINFO` (`syscall.c:2732` `SYS_ioctl`) com `fb_var_screeninfo` real (`g_fb.width/height/pitch` em `lib/gui/vesa.c`) | `Xorg -configure` não dá `no screens found` |
| `ioctl FBIOGET/PUT_VSCREENINFO` | `syscall.c:2732` stub genérico | preencher `struct fb_var_screeninfo` (xres/yres/bits_per_pixel) | `fbset -i` (busybox) mostra 1024x768x32 |
| `/dev/input/event*` evdev | `syscall.c:2747` fake `EVIOCG*` | `read` retornar `struct input_event` (`drivers/mouse.h` + `drivers/keyboard.h` Zig) com `EV_KEY/EV_REL/EV_SYN` | `evtest /dev/input/event0` mostra eventos |
| `ioctl EVIOCG*` | `syscall.c:2758` fake | já ok para `libinput` — faltam `EVIOCS*` e `EVIOCGRAB` | `libinput debug-events` não aborta `PreInit` |
| VT ioctls | `syscall.c:2732` `VT_*` em `tty_ioctl` | `VT_GETMODE/VT_SETMODE/VT_ACTIVATE` para `Xorg` trocar VT | `Xorg` não loga `xf86OpenConsole: VT_WAITACTIVE failed` |
| `mmap` de `/dev/fb0` com offset | `syscall.c:2587` ignora `offset` (`regs[6]`) | aceitar `offset` para double-buffer | `Xorg` fbdev com `ShadowFB` |

### 5.2 Binários a portar

| Binário | Versão | Link | Nota |
|---|---|---|---|
| Xorg | 21.1.9 | `https://www.x.org/releases/individual/xserver/xorg-server-21.1.9.tar.xz` | compilar com `meson -Dudev=false -Ddbus=false -Dhal=false` + driver `xf86-video-fbdev` |
| xterm | 388 | `https://invisible-mirror.net/archives/xterm/xterm-388.tgz` | depende `libX11/libxcb/libXft/fontconfig` — usar Alpine `xterm` estático se existir |
| twm | 1.0.12 | `https://www.x.org/releases/individual/app/twm-1.0.12.tar.gz` | WM mínimo; `fvwm2` é alternativa maior |
| geany | ? | `?` — `apk fetch geany` | exige `GTK3` — se pesado, adiar; `xterm + vim` já é IDE |
| libs | — | Alpine `apk fetch libX11-static fontconfig-static freetype-static` | sem libs estáticas, Xorg não resolve `DT_NEEDED` via `elf64.zig:421` `PT_INTERP` |

### 5.3 Critério de aceitação

- [ ] `Xorg -config /ETC/X11/XORG.CONF` sobe com `fbdev`, log sem `EE no screens found`.
- [ ] `xterm` abre janela (retângulo branco com cursor piscando).
- [ ] `twm` gerencia 2+ `xterm` (mover, focar, fechar).
- [ ] `vim` roda dentro de `xterm` (mesmo teste de 4.4 mas gráfico).
- [ ] Mouse funciona em `xterm` (seleção de texto com arrasto, `EV_REL` via `syscall.c:3466` `mouse_read_delta`).

---

## 6. Fase 8 — Toolchain completa (Nível 3)

Fluxo N3: [N1]→gcc/clang→git clone→make iso→boot ISO (python3/node opcionais).

| Item | O que é | Bloqueador | Critério |
|---|---|---|---|
| gcc ou clang | compilador completo | `mmap SHARED` + `ftruncate` + `fork -j` | `gcc hello.c -o hello` (não só `tcc`) |
| git clone/commit/push local | versionamento | `socket AF_UNIX` já ok, mas `getrandom` + `fsync` + `mmap` de packfiles | `git clone /SRC/tipos /TMP/tipos2` |
| python3 completo | REPL + pip | `futex` com fila + `epoll` real para `asyncio` | `python3 -m http.server` serve arquivo |
| node | JS runtime | `epoll` + `timerfd` (`syscall.c:283` `timerfd_create` stub) + `futex` | `node -e "console.log(1+1)"` → `2` |
| code-server | VS Code web | `node` + `socket` + `epoll` + `fs` | `code-server --bind-addr 127.0.0.1:8080` responde |
| `git clone` do TipOS dentro do SO | auto-hospedagem | todos acima + `disk.img` com espaço (64MB hoje é pouco — ver `fs/fat32.c`) | `git clone https://github.com/.../TipOS-staging` (ou via `virtio` 9p) |
| `make iso` dentro do SO | gera ISO nova | `xorriso`/`grub-mkrescue` portados ou script `make iso` que só precisa `cp` + `mtools` | `ls -lh TipOS.iso` > 10MB |
| Boot na ISO nova | fecha ciclo | QEMU aninhado (`qemu-system-x86_64` dentro do OvsbMk) ou reboot com `kexec` | `qemu -cdrom TipOS.iso -display none -serial stdio` boota até `MkM>` |

Critério de aceitação do Nível 3:

- [ ] `git clone` do repo do OvsbMk dentro do SO (via `socket` ou `9p`).
- [ ] `make kernel` dentro do SO compila `OvsbMk/build/kernel.elf` sem erro.
- [ ] `make iso` gera `TipOS.iso` bootável.
- [ ] Boot da ISO nova no QEMU (aninhado ou host) chega ao shell.

---

## 7. O que não fazer agora

| Item | Por que não agora |
|---|---|
| glibc puro | `musl` estático basta; `glibc` exige `ld-linux`+`DT_NEEDED`/`rpath` — adia. |
| systemd | `prctl`/`getrandom` já enganam `dbus`; `systemd` exige `cgroups`/`kdbus`. |
| dbus | `Xorg` funciona sem `dbus` (`compat-linux.md:501`); só silencia log. |
| Wayland | precisa `memfd_create`+`DRM/KMS` (`compat-linux.md:504`); `X11 fbdev` destrava N2. |
| io_uring completo | stub `ENOSYS` basta, apps caem em `epoll` (`compat-linux.md:514`). |
| ptrace/gdb | `serial_puts`+`tipos.log` já debugam; `ptrace` exige `WSTOPPED`. |
| 32-bit compat | `elf64.zig:374` só `EM_X86_64`; 32-bit exige `compat_sys_*` (`compat-linux.md:584`). |
| Rede/TLS | `AF_UNIX` ok; `AF_INET` exige `virtio-net`+`lwip` — não bloqueia N1. |
| Áudio | sem `ALSA`; não bloqueia build. |
| Impressão | `CUPS`+`avahi` — irrelevante. |
| SMP | `futex` spin (`syscall.c:4383`) ok single-core; SMP exige `APIC`/`IPI`. |

---

## 8. Cronograma sugerido

Foco Fase 7.0 (N1); ajustar conforme `bash /tmp/run_auto.sh` (`PF@`/`SLAB-BAD`).

| Semana | Foco | Entregável |
|---|---|---|
| 1 | `AUXV` completo + `fsync/fdatasync/ftruncate` stubs | `vim :wq` ok; `tcc -o` sem `ENOSYS` |
| 2 | `mmap` file-backed `MAP_PRIVATE` correto + `msync` no-op documentado | `tcc hello.c -o hello` ok; `busybox` 200 applets |
| 3 | `mremap` anon + `MAP_SHARED` dirty tracking | `tcc -run` realoca; `git` packfile |
| 4 | `busybox` completo + `dash` + `make` + bateria §4.3 | Nível 1 completo — §4.4 verde |

Após N1: 2 sem Fase 7.5 (fbdev/evdev/Xorg), 4 sem Fase 8 (gcc/git/iso).

---

## 9. Riscos e mitigações

| Risco | Probabilidade | Impacto | Mitigação |
|---|---|---|---|
| `mmap` write-back complexo | alta | alto | MAP_PRIVATE primeiro (`vm_map.c:52` COW), MAP_SHARED com dirty depois |
| `tcc` não achar `musl` headers | média | alto | instalar `musl-1.2.4` headers em `/usr/include` |
| `vim` depender de `ncurses` | alta | médio | portar `ncurses-static` antes; fallback `busybox vi` |
| `Xorg` exigir `dbus` | média | médio | `meson -Ddbus=false`; stub `SO_PEERCRED` (`compat-linux.md:501`) |
| `disk.img` 64MB pequeno | alta | médio | aumentar para 256MB (`Makefile` `disk.img: mkfs.fat -F 32 -S 512 -s 8` + `mtools mcopy`); ou montar `ext2` maior |
| `futex` livelock com `make -j` | média | médio | fila por `uaddr` (`syscall.c:4383`→`wait_queue`) |
| `mremap` fragmenta `vm_map` | média | baixo | `vm_map_remove` (`vm_map.c:219`) já divide entry; testar `MAYMOVE` |
| Regressão `/dev/tty` | baixa | alto | manter `syscall.c:1945` `open("/dev/tty")` → `alloc_fd` com `'\0'` (`\0` não `'\\0'` — ver `compat-linux.md:350` estudo de caso) |

---

## 10. Próximos passos concretos

Três ações para começar hoje:

1. **Rodar harness e colar log:**
   ```bash
   bash /tmp/run_auto.sh
   # 90s QEMU + screendump /tmp/screen.ppm + greps PF@/SLAB-BAD/accept
   # ver §11 de docs/compat-linux.md para interpretação de falhas
   ```

2. **Baixar busybox estático musl:**
   ```bash
   # Alpine static — descobrir versão exata:
   curl -s https://pkgs.alpinelinux.org/package/edge/main/x86_64/busybox-static | grep -o 'Version[^<]*'
   # baixar:
   wget -O /tmp/busybox https://dl-cdn.alpinelinux.org/alpine/v3.19/main/x86_64/busybox-static-1.36.1-r19.apk
   # ou binário pré-compilado:
   wget -O /tmp/busybox https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
   chmod +x /tmp/busybox
   mcopy -i disk.img /tmp/busybox ::/BIN/BUSYBOX
   ```

3. **Testar `busybox true` dentro do QEMU:**
   ```bash
   # após make iso && make run, no shell MkM> ou via serial:
   /BIN/BUSYBOX true; echo $?
   # esperado: 0, sem PF@ em tipos.log
   # se falhar: grep -n "PF@\|EXC#\|SLAB-BAD" /tmp/tipos.log
   #           grep -n "execve] ok\|mmap->va" /tmp/tipos.log | tail -20
   ```

---

*Refs:* Alpine `https://pkgs.alpinelinux.org`, tcc `https://www.tinycc.org`, musl `https://musl.libc.org`, Xorg `https://www.x.org/wiki/`, syscalls `https://filippo.io/linux-syscall-table/`.
