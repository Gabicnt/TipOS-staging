# Relatório de Compatibilidade Linux: Xterm e FVWM no TipOS

**Auditoria do workspace:** `/home/gabicnt/TipOS-staging`, 2026-09-30  
**Escopo:** código atual, documentação versionada e `disk.img` presente no workspace.  
**Limite da auditoria:** não foram executados Xorg, Xterm ou FVWM no QEMU durante esta auditoria.
CONTINUE. ITEM 5 + 7. SÓ CÓDIGO. SEM NARRAR.

Item 5: line discipline no slave (ICANON, ECHO, ISIG).
Arquivos: OvsbMk/kernel/drivers/tty.c, tty.h.

Requisitos:
1. tty_pair_write: quando escrevendo no slave, aplicar line discipline antes de entregar:
   - Se ICANON: buffer até newline, backspace (VERASE) apaga último char, Ctrl+D (VEOF) entrega
   - Se ECHO: copia chars de volta pro master (eco)
   - Se ISIG: Ctrl+C (VINTR) marca flag pendente (Fase 3 entrega SIGINT); Ctrl+Z (VSUSP) idem
   - Se !ICANON: modo raw, entrega byte a byte
2. Buffer de linha no slave (ex: 256 bytes) pra modo canônico.
3. tty_pair_write do master pro slave também aplica (Xterm digita, shell recebe com line discipline).
4. Master sempre raw (sem line discipline).
5. read() no slave em modo canônico só retorna quando tem linha completa.

Item 7: teste mínimo.
Arquivos: tests/pty_test.c (novo), Makefile.

Requisitos:
6. Escrever tests/pty_test.c em C que:
   - Abre /dev/ptmx
   - Chama ptsname -> /dev/pts/N
   - Abre /dev/pts/N
   - Escreve "hello\n" no master, lê no slave, verifica conteúdo
   - Escreve "world\n" no slave, lê no master, verifica conteúdo
   - ioctl TIOCGPTN retorna N correto
   - ioctl TCGETS/TCSETS funcionam
   - ioctl TIOCGWINSZ/TIOCSWINSZ funcionam
   - Imprime PASS/FAIL por bloco, retorna 0 se tudo passou
7. Adicionar target no Makefile: build-pty-test (compila com gcc do host, estático).
8. Copiar pra /BIN/PTY_TEST no disk.img.
SÓ O DIFF. SEM EXPLICAÇÃO.
## Leitura Executiva

O TipOS tem uma base de compatibilidade Linux útil para ELF64 estático/PIE, incluindo I/O, VFS, processos, pipes, AF_UNIX simplificado e acesso ao framebuffer. Isso é suficiente para explicar o estado histórico documentado em que uma compilação específica do Xorg inicializou.

O estado atual não é um ambiente X reproduzível: o `disk.img` contém `/BIN/DOOM` e `/APPS/DOOM1.WAD`, mas não Xorg, FVWM, bibliotecas X11, fontes, XKB ou terminfo. Não existe PTY/devpts, entrega POSIX de sinais ou loader dinâmico completo. Xterm depende diretamente de PTY e controle de terminal; FVWM depende de um cliente X executável e de uma conexão cliente-servidor efetivamente testada.

Há divergência entre documentação e checkout: `VERSION` declara `v0.7.5.0`; o histórico contém commits com título “clone(56) + Xorg 100%”, mas não há confirmação aqui de release `v0.7.6.0`. O working tree contém alterações locais não commitadas; os achados descrevem os arquivos presentes no workspace, não apenas `HEAD`.

## 1. Syscalls

O dispatcher traduz números Linux x86-64 para números internos TipOS em [syscall_linux.zig](../OvsbMk/kernel/syscall_linux.zig#L210). Os handlers ficam no `switch` de [syscall.c](../OvsbMk/kernel/syscall.c#L670). O kernel não declara uma função C independente por syscall: `syscall_handler(regs)` lê os argumentos dos registradores e devolve o resultado em `rax`. As assinaturas nesta seção são as assinaturas Linux x86-64 convencionais, salvo quando a limitação é indicada.

### Chamadas pedidas: implementadas ou parciais

| Linux nr | Chamada e assinatura | Estado observado |
|---:|---|---|
| 0 | `read(fd, buf, count) -> ssize_t` | Implementada para console, arquivos, pipes, sockets e alguns devices. |
| 1 | `write(fd, buf, count) -> ssize_t` | Implementada para console, arquivos, pipes, sockets e eventfd. |
| 2 | `open(path, flags, mode) -> fd` | Parcial: flags/mode não são aplicados; fd comum não valida a existência até a leitura. |
| 3 | `close(fd) -> int` | Implementada, com limpeza simplificada de fd. |
| 4, 5, 6 | `stat(path, st*)`, `fstat(fd, st*)`, `lstat(path, st*)` | Parciais: não preenchem a estrutura Linux corretamente para arquivos comuns; `lstat` não segue semântica de symlink. |
| 7 | `poll(fds, nfds, timeout) -> int` | Parcial: timeout ignorado, máximo 16 fds, sem espera bloqueante geral. |
| 8 | `lseek(fd, offset, whence) -> off_t` | Implementada com posição de 32 bits; não é `off_t` de 64 bits completo. |
| 9 | `mmap(addr, len, prot, flags, fd, offset) -> addr` | Anônimo e framebuffer; arquivo comum e semântica compartilhada incompletos. |
| 10, 11 | `mprotect(addr,len,prot)`, `munmap(addr,len)` | Implementadas no vm_map/PTE com limitações de proteção/objetos. |
| 12 | `brk(addr) -> addr` | Implementada: consulta e crescimento mapeado; encolher só altera o break. |
| 13 | `rt_sigaction(sig,act,oldact,size)` | Stub: retorna sucesso, não instala ação. |
| 14, 15 | `rt_sigprocmask(...)`, `rt_sigreturn(...)` | Stubs; não existe entrega de sinais. |
| 16 | `ioctl(fd, request, arg)` | Parcial: alguns FB/VT/tty ioctl; retorno 0 para requisições desconhecidas mascara ausência. |
| 19, 20 | `readv(fd,iov,cnt)`, `writev(fd,iov,cnt)` | Implementadas para arquivos/pipes; `readv` limita iovcnt a 16. |
| 21 | `access(path, mode)` | Stub permissivo: retorna sucesso sem validar o caminho/permissão. |
| 22 | `pipe(pipefd*) -> int` | Implementada com buffer de 4 KiB; capacidade global de 8 pipes. |
| 23 | `select(n, readfds, writefds, exceptfds, timeout)` | Parcial: trata readfds; demais conjuntos e timeout não são implementados adequadamente. |
| 32, 33 | `dup(oldfd)`, `dup2(oldfd,newfd)` | Implementadas com tabela simplificada de 16 fds por processo. |
| 35 | `nanosleep(req, rem)` | Stub: retorna imediatamente. |
| 39 | `getpid() -> pid_t` | Implementada. |
| 41, 42, 43 | `socket(domain,type,protocol)`, `connect(fd,addr,len)`, `accept(fd,addr,len)` | AF_UNIX em memória; sem TCP/IP, bloqueio real ou semântica completa. |
| 44–50, 53 | `sendto`, `recvfrom`, `sendmsg`, `recvmsg`, `shutdown`, `bind`, `listen`, `socketpair` | Parcial: domínio AF_UNIX e buffers internos; `SCM_RIGHTS` não passa fds de verdade. |
| 56 | `clone(flags,stack,ptid,tls,ctid)` | Implementada apenas para threads com `CLONE_THREAD|CLONE_VM`; ver seção 4. |
| 57 | `fork() -> pid_t` | Implementada com cópia eager do espaço de usuário. |
| 59 | `execve(path,argv,envp)` | ELF estático/PIE; rejeita `PT_INTERP`/executável dinâmico. |
| 60 | `exit(status)` | Implementada para encerrar thread/processo. |
| 61 | `wait4(pid,status,options,rusage)` | Espera processo, mas ignora `options` e `rusage`; não implementa `WNOHANG`. |
| 62 | `kill(pid,sig)` | Stub: aceita e ignora. `tgkill` também é ignorado. |
| 63 | `uname(buf*)` | Implementada com valores fixos compatíveis. |
| 72 | `fcntl(fd,cmd,arg)` | Parcial: GET/SET FD/FL básicos. |
| 78 | `getdents64(fd,dirp,count)` | Número traduzido para `readdir` próprio TipOS, não para ABI `getdents64` genérico; backend é FAT32. |
| 83, 84, 87 | `mkdir(path,mode)`, `rmdir(path)`, `unlink(path)` | Implementadas parcialmente via FAT32; `mkdir` tem diretórios virtuais `/tmp`. |
| 89, 90 | `readlink(path,buf,size)`, `chmod(path,mode)` | `readlink` apenas sintetiza poucos caminhos; chmod é no-op. |
| 91–93 | `fchmod`, `chown`, `fchown` | No-op, compatível apenas com FS sem permissões/donos. |
| 96 | `gettimeofday(tv,tz)` | Implementada com `timer_ticks`, baixa precisão. |
| 98–100 | `getrusage`, `sysinfo` (Linux 99), `times` | Implementadas como dados zerados/fixos ou ticks globais. |
| 102,104,107,108 | `getuid`, `getgid`, `geteuid`, `getegid` | Implementadas retornando 0. |
| 110,121 | `getppid()`, `getpgid(pid)` | Implementadas de forma simplificada; sem grupos de processo POSIX reais. |
| 131 | `sigaltstack(new,old)` | Stub. |
| 157 | `prctl(...)` | Stub permissivo. |
| 158 | `arch_prctl(code,arg)` | FS.base set/get suportado; GS set/get são no-op. |
| 186 | `gettid()` | Retorna o PID do PCB/thread atual. |
| 202 | `futex(uaddr,op,val,timeout,uaddr2,val3)` | WAIT faz polling/yield; WAKE não acorda fila. |
| 218 | `set_tid_address(tidptr)` | Retorna TID, sem conjunto completo de semântica Linux. |
| 228,229 | `clock_gettime(clockid,tp)`, `clock_getres(clockid,tp)` | Implementadas com relógio/ticks simplificados. |
| 231 | `exit_group(status)` | Traduzido para número interno 212; encerra processos com PML4 compartilhada. |
| 257,258,262 | `openat`, `mkdirat`, `newfstatat` | Parcial: `dirfd` fora de `AT_FDCWD` não é suportado de forma geral; `stat` depende do backend. |
| 263,264,267 | `unlinkat`, `renameat`, `readlinkat` | Implementados com resolução limitada de caminho/dirfd. |
| 273 | `set_robust_list(head,len)` | Stub de sucesso. |
| 318 | `getrandom(buf,len,flags)` | Implementada com xorshift não criptográfico. |

### Outras chamadas presentes

| Linux nr | Chamada e assinatura | Estado observado |
|---:|---|---|
| 44, 45 | `sendto(fd,buf,len,flags,dst,dstlen)`, `recvfrom(fd,buf,len,flags,src,srclen*)` | AF_UNIX simplificado; flags/endereço pouco respeitados. |
| 46, 47 | `sendmsg(fd,msg,flags)`, `recvmsg(fd,msg,flags)` | Buffer do socket; `SCM_RIGHTS` não transfere descritores reais. |
| 48, 49, 50, 51 | `shutdown(fd,how)`, `bind(fd,addr,len)`, `listen(fd,backlog)`, `getsockname(fd,addr,len*)` | AF_UNIX em memória; shutdown ignora a operação e getsockname é parcial. |
| 53 | `socketpair(domain,type,protocol,fds[2])` | Implementada para AF_UNIX; tipo/flags incompletos. |
| 95 -> 400 | `umask(mask) -> oldmask` | Implementada como máscara global, não por processo. |
| 99 -> 401 | `sysinfo(info*)` | Estrutura preenchida com valores fixos aproximados. |
| 121 | `getpgid(pid) -> pgid` | Devolve PID, sem grupos POSIX reais. |
| 157 | `prctl(option,...)` | Stub de sucesso. |
| 213, 291 | `epoll_create(size)`, `epoll_create1(flags)` | Alocam fd fake. |
| 232, 233, 281 | `epoll_wait`, `epoll_ctl`, `epoll_pwait` | No-op/sucesso; não há mecanismo de eventos epoll. |
| 271 | `ppoll(fds,nfds,timeout,mask,size)` | Verifica readiness semelhante a poll; ignora timeout/máscara. |
| 283, 286, 287 | `timerfd_create`, `timerfd_settime`, `timerfd_gettime` | Estruturas aproximadas; não há temporizador real por fd. |
| 290 | `eventfd2(initval,flags)` | Contador simples em fd; não cobre eventfd Linux 284. |
| 334 | `rseq(rseq,len,flags,sig)` | Stub que retorna sucesso. |
| 400–409 internos | `umask`, `sysinfo`, `chmod`, `fchmod`, `chown`, `fchown`, `statfs`, `fstatfs`, `shutdown`, `writev` | Números TipOS usados para contornar colisões do ABI Linux. |

Syscalls TipOS próprias e suas assinaturas internas: `198 kbhit() -> int`; `200 disp_get_fb(&addr,&width,&height,&pitch) -> int`; `201 disp_flush(backbuffer) -> int`; `202 mouse_read(&dx,&dy,&buttons) -> int`; `203 kb_mod() -> bits`; `205 disp_flush_rect(backbuffer,packed_xy,packed_wh) -> int`; `206 kbd_event(mode) -> event`; `207 readdir(path,entries,max_entries) -> count`; `208 execve(path,argv,envp)`; `209 shell_cmd(cmd,outbuf,outsize) -> count`; `210 spawn(path) -> pid`; `211 spawn_shared(path) -> pid`; `214 fork_real() -> pid`. A lista está em [syscall.h](../OvsbMk/kernel/syscall.h#L15); handlers em [syscall.c](../OvsbMk/kernel/syscall.c#L1826).

### Chamadas pedidas que **NÃO EXISTEM**

- `17 pread64`, `18 pwrite64`, `24 sched_yield` (mapeia para 90 sem handler), `34 pause`, `58 vfork`, `79 getcwd`, `80 chdir`, `85 creat`, `105 setuid`, `111 getpgrp`, `112 setsid`, `302 prlimit64`: **NÃO EXISTEM como handlers funcionais**.
- A tabela Zig contém nomes/constantes para algumas chamadas não atendidas. Ter uma entrada na tabela não prova que haja implementação: sem case, o dispatcher cai no retorno padrão `-1`. Exemplo: `prlimit64(302)` tem mapeamento identity, mas não handler; `sched_yield(24)` é redirecionado para 90, também sem case. [Tabela de mapeamento](../OvsbMk/kernel/syscall_linux.zig#L220), [default do dispatcher C](../OvsbMk/kernel/syscall.c#L3125).
- Casos importantes para Xterm/FVWM: `stat/fstat/lstat`, `getdents64`, `access`, `poll/select`, `ioctl`, `nanosleep`, `futex`, sinais e `openat` podem retornar sucesso sem semântica Linux completa. Não os trate como compatibilidade certificada.

## 2. Pseudo-terminal

- `/dev/ptmx`: **NÃO EXISTE**. Não há alocação de master/slave ou multiplexador de PTY.
- `/dev/pts/N`: **NÃO EXISTE**. Não existe `devpts`.
- `posix_openpt`, `openpty`, `grantpt`, `unlockpt`, `ptsname`: **NÃO EXISTEM** no kernel/libc do workspace.
- `TIOCGPTN` e `TIOCSPTLCK`: **NÃO EXISTEM**.
- Há ioctl parcial para `TIOCGWINSZ(0x5413)`, `TCGETS(0x5401)`, `TCSETS*` e alguns ioctl VT/KD. Winsize é fixo; termios é escrito como estrutura básica, sem estado por fd. Ioctls desconhecidos retornam sucesso. [ioctl](../OvsbMk/kernel/syscall.c#L1469).
- Há `/dev/ttyN` virtual somente para scan codes crus destinados ao Xorg, além de `/dev/fb0` e `/dev/input/mice` interceptados no `open_dev`; isso não implementa terminal POSIX. [Devices virtuais](../OvsbMk/kernel/syscall.c#L234).
- Local recomendado para começar: driver/subsistema `OvsbMk/kernel/drivers/tty.*` (ou `OvsbMk/kernel/tty.*`), integração no VFS e filesystem virtual `devpts`.

## 3. Sinais

- Entrega de `SIGINT`, `SIGTERM`, `SIGWINCH`, `SIGCHLD`, `SIGSEGV` em ring 3: **NÃO EXISTE** como ABI POSIX.
- `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `sigaltstack` aceitam/no-op; `kill/tgkill` são ignorados. O kernel trata exceções/page fault, mas não as converte para `SIGSEGV` Linux. [Handlers](../OvsbMk/kernel/syscall.c#L2550).
- `SIGWINCH` em resize: **NÃO EXISTE**. Não há estado mutável de winsize nem notificação.
- `wait4` chama `proc_waitpid`, mas ignora `options`/`rusage`; cobre espera simples por processo, não espera POSIX completa. [wait4](../OvsbMk/kernel/syscall.c#L2537), [proc_waitpid](../OvsbMk/kernel/process.c#L300).
- Ctrl+C do shell é entrada ETX/controle local, não prova implementação de signal delivery para processos em ring 3.

## 4. Threads e processos

- `clone(56)` existe e cria PCB novo, mas exige `CLONE_THREAD|CLONE_VM`. Compartilha PML4, vm_map e fd table. Considera `CLONE_SETTLS`, `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID` e `CLONE_CHILD_CLEARTID`; não valida/implementa os demais flags completamente. [syscall case](../OvsbMk/kernel/syscall.c#L912), [proc_clone_thread](../OvsbMk/kernel/process.c#L818).
- `FUTEX_WAIT` compara `*uaddr` com `val` e chama `schedule()` em loop; sem timeout, fila ou semântica de sinal. `FUTEX_WAKE` apenas retorna 0. `pthread_join`: **não confirmado por teste**; há caminho de `clear_tid` + polling, não sincronização Linux completa. [futex](../OvsbMk/kernel/syscall.c#L2609), [clear_tid](../OvsbMk/kernel/process.c#L879).
- `exit_group(231)` é traduzido para 212; `gettid(186)` retorna o PID/TID do PCB. Threads que saem limpam seu PCB e `clear_tid`; o kernel stack da thread vaza segundo comentário do código. [thread exit](../OvsbMk/kernel/process.c#L895).
- Scheduler round-robin/preemptivo por PIT em execução user; ring 0 também faz yield/schedule explicitamente. `MAX_PROC=64` slots totais, incluindo idle/shell; não são 64 threads adicionais garantidas. [process.h](../OvsbMk/kernel/process.h#L25), [scheduler](../OvsbMk/kernel/process.c#L360).

## 5. Memória

- Página de 4 KiB para vm_map/brk; ELF usa páginas de 2 MiB. `mprotect`, `munmap` e `brk` têm caminhos reais, mas com cobertura menor que Linux. `munmap` percorre páginas e invalida TLB. [vm_map](../OvsbMk/kernel/vm_map.c#L389), [brk](../OvsbMk/kernel/syscall.c#L2450).
- `MAP_ANON` e `MAP_FIXED` são tratados. Não há mmap comum de arquivo; chamadas não anônimas são convertidas em anônimas, fora o caso framebuffer. `MAP_SHARED/MAP_PRIVATE` não têm semântica completa de objetos/CoW.
- Heap libc TipOS: heap estático de 64 KiB; pedidos a partir de 2048 bytes usam mmap. `HDR_SZ = sizeof(unsigned long)+sizeof(void*)`, 16 bytes em x86-64. [stdlib.c](../src/userland/libc/stdlib.c#L80).
- Não há quota formal por processo. A faixa vm_map chega aproximadamente a `0x7ffff000`; limite efetivo é memória física/allocator e faixa mapeável.

## 6. Arquivos e VFS

- VFS tenta montar ext2 e faz fallback para FAT32. Código ext2 Zig contém cache e mapeamento de blocos diretos, indiretos e double-indirect; FAT32 oferece LFN e leituras posicionais. [mount VFS](../OvsbMk/fs/vfs.c#L34), [ext2 block map](../OvsbMk/fs/ext2_new.zig#L241).
- Ext2 monta **se houver dispositivo/imagem ext2 compatível disponível**. A imagem deste workspace é FAT32; não foi encontrado rootfs ext2 instalado nela.
- I/O posicional usa offsets de 32 bits e não tem limite especial de 32 KiB; o relatório Doom descreve leitura de arquivo grande/WAD e cache de cadeia FAT32. `pread64/pwrite64`: **NÃO EXISTEM**.
- `getdents64(78)` cai no readdir TipOS baseado em FAT32; não há export de enumeração ext2 no VFS para esse caminho. Symlinks POSIX: **NÃO EXISTEM**. `readlink` sintetiza somente `/proc/self/exe`, `/proc/self/fd/N` e um caminho sysfs de framebuffer. [readlink](../OvsbMk/kernel/syscall.c#L288), [readdir](../OvsbMk/kernel/syscall.c#L1946).
- Pipes: buffer de 4 KiB e pool de 8; leituras podem ceder scheduler, mas escrita cheia não segue integralmente semântica Linux. `poll/select` parciais; epoll fake.
- Inventário real de [disk.img](../disk.img): `/BIN/DOOM`, `/APPS/DOOM1.WAD`, diretórios `/USR` e `/LOCAL` vazios. Não há Xorg, FVWM, fontes, XKB, terminfo ou bibliotecas X11 instalados.

## 7. X11 e Xorg

- Documentação declara Xorg 21.1.13, musl static-PIE, instalado historicamente como `/bin/XORG` em rootfs temporário externo. O binário não está no checkout nem no `disk.img`. [Crônica Xorg](XORG-XKB-CA%C3%87ADA.md#L15).
- A crônica descreve `xorg.cfg`/`-config xorg.cfg` e `/tmp/ext2root`; não encontrei essa configuração na imagem atual.
- Framebuffer: `/dev/fb0` é device virtual interceptado por `open_dev`, ioctl FBIO e mmap. Input documentado: `/dev/ttyN` com scan codes PS/2 crus e `/dev/input/mice` com bytes crus. `/dev/input/event*`: **NÃO EXISTE**. [open_dev](../OvsbMk/kernel/syscall.c#L234), [keyboard](../OvsbMk/kernel/drivers/keyboard.zig#L12), [mouse](../OvsbMk/kernel/drivers/mouse.zig#L12).
- `/dev/fb0` não é nó VFS real. AF_UNIX tem implementação em buffers/memória do kernel, com registro de path; não cria socket persistido no FS. TCP/IP para X11: **NÃO EXISTE**. [sockets](../OvsbMk/kernel/syscall.c#L2915).
- `/tmp/.X11-unix/X0` é tratado como caminho virtual pelo kernel; criação/bind não equivale a socket de filesystem Unix Linux. Há código para X clients conectarem ao registry AF_UNIX interno, mas **não há teste de conexão de cliente X real no workspace**.
- `libX11`, `libXaw`, `libXt`, `libXmu`, `libXpm`, `libXext`, `libXrender`: **NÃO EXISTEM no rootfs atual**. `/usr/share/fonts`, XKB e fontes completas: **NÃO EXISTEM na imagem atual**.
- O launcher declara `DISPLAY=:0`, mas passa `envc=0` a `setup_linux_user_stack` nesse caminho; portanto essa variável não é copiada ao `envp` por ele. [launcher](../OvsbMk/kernel/shell.c#L332), [stack env](../OvsbMk/kernel/process.c#L530).
- Verificação histórica: a crônica cita `[XKMDIAG] missing=0x0`, leitura de mouse e renderização, e orienta scripts em `/tmp/tipos` externos. Não há log Xorg nem screenshot Xorg versionado. A imagem `doom-ring3-fbtest.png` é teste Doom/framebuffer, não Xorg. A mesma crônica registra clone/triplo-fault como pendência naquele estado. [estado histórico](XORG-XKB-CA%C3%87ADA.md#L286).

## 8. ABI e ELF loader

- Loader em [elf64.zig](../OvsbMk/kernel/elf64.zig#L1). Suporta ELF64 x86-64 ET_EXEC e ET_DYN/PIE, com base PIE fixa e páginas de 2 MiB.
- ELF estático/PIE é a rota viável documentada. `execve` rejeita ELF com `PT_INTERP`; `spawn` tenta carregar interpretador em base fixa, mas não resolve `DT_NEEDED`/bibliotecas `.so` de forma completa. Loader dinâmico Linux: **NÃO EXISTE de ponta a ponta**. [execve](../OvsbMk/kernel/syscall.c#L1973), [ELF load](../OvsbMk/kernel/elf64.zig#L310).
- `arch_prctl(ARCH_SET_FS/GET_FS)` implementa FS.base; GS é no-op. Clone/context switch têm campos TLS, mas isso não certifica pthread. [arch_prctl](../OvsbMk/kernel/syscall.c#L2420).
- Stack inicial contém `argc/argv/envp/auxv`, alinhamento calculado a 16 bytes e entradas auxiliares Linux. Kernel é compilado sem red-zone; binários locais Doom/userland também usam essa flag, mas ELF arbitrário pode assumir convenções não testadas. [setup stack](../OvsbMk/kernel/process.c#L490), [flags kernel](../OvsbMk/Makefile#L10).
- Compatibilidade de syscall adapta argumento Linux `r10` para o slot C usado como quarto argumento. libc local chama `int 0x80`; há também `syscall_entry.asm`/MSR. [ABI bridge](../OvsbMk/kernel/syscall_linux.zig#L345), [entry](../OvsbMk/kernel/syscall_entry.asm#L7).

## 9. Terminfo e termcap

- `/usr/share/terminfo`, entradas `xterm`, `xterm-256color`, `vt100`, e `/etc/termcap`: **NÃO EXISTEM** na imagem atual nem no projeto como banco instalado.
- O TODO lista terminfo/termcap como trabalho futuro. [TODO](../TODO.md#L683).

## 10. utmp/wtmp

- `/var/run/utmp` e `wtmp`: **NÃO EXISTEM**.
- Registro de login do Xterm: **NÃO IMPLEMENTADO**. É opcional para abrir uma janela, mas necessário para comportamentos/configurações de login que tentem atualizar utmp.

## 11. Bibliotecas no rootfs

- O `disk.img` atual não contém arquivos `.so` ou `.a`; portanto não há versões instaladas de `libX11`, `libXaw`, `libXt`, `libXmu`, `libXpm`, `libXext`, `libXrender`, `libncurses`, `libtinfo`, `libpthread`, `libm` ou `libdl`.
- A libc própria está em [src/userland/libc](../src/userland/libc/stdio.c) e é compilada estaticamente nos programas locais. Não é glibc/musl Linux ABI completa e não substitui runtime de ELF Linux dinâmico.
- Dependências de Xorg citadas na crônica são artefatos de build/rootfs temporário externo; não foram encontrados instalados no disco atual. Versão/cópia de distro: **não aplicável, não instalada**.

## 12. Build e toolchain

- Kernel: GCC host para C, NASM para assembly, Zig para módulos Zig; freestanding, sem libc, red-zone ou SIMD. [OvsbMk Makefile](../OvsbMk/Makefile#L8).
- Userland normal: GCC host + libc própria; pipeline ELF -> flat binary -> Mach-O e instala graphy. Doom possui Makefile separado e gera ELF estático com GCC host. [Makefile raiz](../Makefile#L20), [userland Makefile](../src/userland/Makefile#L10), [Doom Makefile](../src/userland/progs/doom/Makefile#L1).
- ISO: GRUB2 por `grub-mkrescue`; a receita usa `|| true`, então o alvo pode mascarar erro. `disk.img`: `dd` + `mkfs.fat -F 32`, FAT32 de 64 MiB. [ISO](../Makefile#L14), [disk](../Makefile#L27).
- Build unificado Xorg/FVWM: **NÃO EXISTE** no Makefile raiz. Xorg da crônica é artefato preparado fora do repo.

## 13. Busca global de resquícios

- `xterm`: **NÃO ENCONTRADO** em código/docs versionados. `fvwm` aparece em comentários de `DISPLAY`/launcher; não há fonte/config FVWM.
- `Xorg`, `X11`, `Xlib`: aparecem em docs, `syscall.c`, keyboard/mouse Zig, shell/process e changelog. A maioria é código de compatibilidade, comentário, caminho de asset ou alegação histórica. [syscall.c](../OvsbMk/kernel/syscall.c#L234), [crônica](XORG-XKB-CA%C3%87ADA.md#L1).
- `ptmx`, `/dev/pts`, `openpty`, `grantpt`, `unlockpt`, `TIOCGPTN`, `TIOCSPTLCK`: **NÃO ENCONTRADOS como implementação**.
- `termcap`, `terminfo`, `utmp`, `wtmp`, `SIGWINCH`: sem implementação; referências residuais incluem TODOs/documentação. [TODO](../TODO.md#L683).
- `ioctl`: possui handler parcial apesar de docs antigas chamarem de stub. `tty`: é comentário/caminho simplificado `/dev/ttyN`, não driver TTY POSIX.

## 14. O que Doom acrescentou e riscos para X apps

O relatório de estado do port está em [DOOM_PORT_REPORT.md](DOOM_PORT_REPORT.md).

- lseek: libc Doom chama syscall Linux `8`, traduzida para TipOS `204`; não é syscall Linux `199`. Ainda há offset interno de 32 bits.
- libc: pool circular de 16 `FILE`, `feof`, `fseek/ftell`, `fscanf`, `snprintf/vsnprintf`, `vfprintf`, `fflush`, `remove/rename`; formatador documenta largura/precisão inteira. [stdio](../src/userland/libc/stdio.c#L20), [fopen](../src/userland/libc/stdio.c#L160).
- malloc: `HDR_SZ` é 16 bytes em x86-64; heap local de 64 KiB e mmap a partir de 2048 bytes. Isso é da libc TipOS, não dá ABI dinâmica a binários Linux. [stdlib](../src/userland/libc/stdlib.c#L80).
- Stack: alinhamento calculado no setup ELF. O handler de `SYS_exit` ainda tem loop `hlt` como fallback após chamar exit; normalmente o caminho não retorna.
- `mkdir` não retorna sempre zero: apenas `/tmp` e `/tmp/.X11-unix` virtuais são aceitos incondicionalmente; demais caminhos dependem de FAT32. Sob ext2, handler ainda chama FAT32, risco de backend errado. [mkdir](../OvsbMk/kernel/syscall.c#L1137).
- Teclado: buffers crus preservam make/break e prefixo `0xE0`; fila de eventos do Doom converte press/release; `I_GetEvent` processa no máximo 16 eventos por chamada. Buffer pode descartar eventos cheio. [IRQ keyboard](../OvsbMk/kernel/drivers/keyboard.zig#L132), [Doom glue](../src/userland/progs/doom/tipOS_glue.c#L170), [I_GetEvent](../src/userland/progs/doom/doomgeneric/i_input.c#L279).
- PIC atual desmascara IRQ1 e IRQ12. [PIC](../OvsbMk/kernel/pic.c#L25).
- O relatório Doom anexado diz que o processo ainda page-faulta em `D_ProcessEvents` antes de `I_InitGraphics`; portanto menu/frame Doom completo não foi obtido. `nanosleep` do kernel continua no-op. Isso não valida Xterm/FVWM.

## 15. O que falta para rodar Xterm

| Trabalho | Prioridade/esforço | Local recomendado |
|---|---|---|
| PTY master/slave, `/dev/ptmx`, `/dev/pts/N`, `grantpt/unlockpt/ptsname`, buffering/refcount | Alto | Novo `OvsbMk/kernel/drivers/tty.*` + VFS/devpts |
| termios persistente, TIOCGPTN/TIOCSPTLCK/TIOCSCTTY/TIOCGWINSZ e controlling terminal | Alto | Driver TTY + ioctl em `syscall.c` |
| `setsid`, pgrp, foreground process group e entrega de SIGCHLD/SIGWINCH/SIGINT | Alto | `process.c`/novo subsistema signal + TTY |
| Corrigir `stat/fstat`, `getdents64` ext2, open flags, errno e `*at` | Alto | `syscall.c`, `vfs.c`, `ext2_new.zig` |
| `pread64/pwrite64`, file-backed mmap, poll bloqueante/readiness e `sched_yield` | Médio/alto | `syscall.c`, `vm_map.c`, VFS |
| Instalar terminfo, fontes, Xterm e dependências no rootfs de teste | Médio | build/install/rootfs |
| utmp/wtmp | Baixo/opcional no primeiro start | VFS + libc/login support |

## 16. O que falta para FVWM

- **Alto:** instalar um binário FVWM e dependências. Bibliotecas candidatas conforme configuração/build: `libX11`, `libXext`, `libXrender`, `libXpm`, possivelmente `libXt`/`libXmu`; nenhuma está na imagem atual.
- **Alto:** loader dinâmico ELF, relocations e resolução de `DT_NEEDED`/`.so`; alternativa é build estático de FVWM e bibliotecas para o ABI realmente suportado.
- **Alto/médio:** testar cliente X real no AF_UNIX em memória, incluindo pedidos X11/propriedades esperados por WM. Documentação de Xorg inicializado não comprova que FVWM gerencie janelas.
- **Médio:** instalar configuração padrão `.fvwmrc`, fontes/ícones e garantir que `DISPLAY=:0` chegue a `envp`.
- PTY não é requisito central para o WM em si, mas é requisito direto para Xterm e shell iniciado dentro dele.

## 17. Alegações do commit “clone(56) + Xorg 100%”

- Histórico local contém commits `de93732`/`a8a5495` com título “clone(56) + Xorg 100%” e `2cd37a2` relacionado a VFS/ext2/readv. Título de commit não prova teste atual.
- Clone está no código, limitado às threads `CLONE_THREAD|CLONE_VM`; exit individual preserva PML4 compartilhada; exit_group limpa PCBs que compartilham PML4. Futex WAIT é polling e WAKE no-op. `gettid` retorna PID do PCB.
- `fixpath` existe e remapeia prefixo absoluto hardcoded do build Xorg para paths na raiz. [fixpath](../OvsbMk/kernel/syscall.c#L470).
- Ext2 tem código com mount/read/write e indireção dupla. Fontes/regras XKB “completas” são alegação/documentação sobre rootfs externo; **não estão no `disk.img` atual**.
- Xorg “root window visível, zero fatal errors”: **não verificável com os artefatos atuais**. A crônica cita diagnóstico e scripts sob `/tmp/tipos`, que não estão no repo; a imagem atual não contém Xorg/assets.
- Versão: [VERSION](../VERSION#L1) declara `v0.7.5.0`; **não confirma v0.7.6.0**.

## Ordem Recomendada e Riscos

1. Fixar uma imagem rootfs reprodutível e documentar o comando/log de boot Xorg, incluindo binário, config, XKB, fontes e checksum.
2. Criar teste de cliente X mínimo e validar transporte AF_UNIX/protocolo antes de FVWM.
3. Corrigir ABI VFS usado pelos clientes: `stat/fstat`, `getdents64`, `openat`, offsets 64-bit e erros/flags.
4. Implementar TTY/PTY e termios, depois sessões/pgrp e sinais; isso desbloqueia Xterm.
5. Instalar/buildar Xterm e validar seu ciclo master/slave, resize e shell.
6. Para FVWM, adicionar loader dinâmico e bibliotecas X necessárias, ou produzir build estático; só então testar configuração e gerenciamento de janelas.

**Riscos conhecidos:** chamadas stub que retornam sucesso escondem capacidades ausentes; `stat/getdents` não mantêm ABI Linux genérica; AF_UNIX é socket em memória, não socket VFS; ext2 não tem enumeração genérica nesse caminho; `mkdir` ignora backend ext2; `nanosleep` não dorme; sinais não são entregues; o estado Xorg depende de artefatos externos; o working tree da auditoria contém alterações não commitadas.