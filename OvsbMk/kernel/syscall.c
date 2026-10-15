/* moe moe kyun <3 */
/* moe moe kyun <3 */
/* ♥ kernel stuff ~ aqui é onde o bicho pega de verdade! kyun~
 * arquivo: syscall.c ~ funcoes anotadas: 6
 */
/* ~*~ syscall.c ~ "O kernel faz tudo pra vc~ hihi!" ~*~
 * Aqui é onde a magica acontece: as syscalls que o userspace chama via int 0x80.
 * Cada caso é uma chamada de sistema diferente, tipo um menu de restaurante
 * onde o usuario pede "me da um mmap" e o kernel responde "sim senhor!".
 * 
 * Mudei varias coisas aqui durante a sessão de debugging:
 * - SYS_disp_get_fb: tava com a2 e a4 trocados (width vs pitch), arrumei~
 * - SYS_disp_get_fb: adicionei guard pra g_fb.width==0 (nao divida por zero!)
 * - SYS_mouse_read: implementei de verdade (tava so retornando 0)
 * - SYS_spawn: cria processo filho com PML4 propria (isolamento!)
 * - SYS_execve: carrega binario via mach_o_load direto
 * - Debug: print [SC:...] pra cada syscall (polui o log mas ajuda)
 * 
 * Se quebrar, chora pro /dev/null~ kyun! <3
 *~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*/

#include "syscall.h"
#include <stdbool.h>
#include "rtc.h"
#include "../fs/vfs.h"
#include "process.h"
#include "signal.h"
#include "memory.h"
#include "drivers/tty.h"
#include "console.h"
#include "serial.h"
#include "pit.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/virtio_gpu.h"
#include "../fs/fat32.h"
#include "../kernel/utils.h"
#include "../kernel/mach_o.h"
#include "../kernel/vm_map.h"
#include "../lib/gui/vesa.h"

/* ~~ FASE8.1-clock: epoch base do RTC ~~
 * clock_gettime/gettimeofday devolviam timer_ticks puro (uptime) e o
 * relógio do desktop nascia em 00:00:00. Agora: na 1ª chamada lê o
 * CMOS (rtc_read), converte p/ epoch (days-from-civil) e DESCONTA o
 * uptime decorrido — tv_sec = epoch + ticks/100 fica exato p/ sempre.
 * RTC inválido (fora de 2020-2100) = fallback p/ uptime (comportamento
 * antigo). QEMU: -rtc base=localtime p/ hora local (UTC = -4h aqui). */
static uint64_t g_boot_epoch = 0;
static int boot_epoch_done = 0;
static uint64_t rtc_to_epoch(void) {
    rtc_time_t t;
    rtc_read(&t);
    if (t.yr < 2020 || t.yr > 2100 || t.mo < 1 || t.mo > 12 ||
        t.dy < 1 || t.dy > 31 || t.h > 23 || t.m > 59 || t.s > 60)
        return 0;
    int64_t y = (int64_t)t.yr;
    int64_t m = (int64_t)t.mo;
    int64_t d = (int64_t)t.dy;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (uint64_t)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (uint64_t)(d - 1);
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint64_t days = (uint64_t)era * 146097 + doe - 719468;
    return days * 86400ULL + (uint64_t)t.h * 3600ULL + (uint64_t)t.m * 60ULL + (uint64_t)t.s;
}
static void boot_epoch_init(uint64_t ticks_now) {
    uint64_t e = rtc_to_epoch();
    uint64_t now = ticks_now / 100;
    g_boot_epoch = (e > now) ? (e - now) : 0;
    boot_epoch_done = 1;
}

extern void execute(const char *cmd);
extern uint64_t current_rsp0;

/* ~~ raw input pro Xorg (Zig) ~~
 * Scan codes crus do teclado e bytes crus do mouse~ */
extern int  kbd_raw_avail(void);
extern unsigned char kbd_raw_read(void);
extern int  mice_avail(void);
extern int  mice_avail_count(void);
extern unsigned char mice_peek(int);
extern unsigned char mice_read(void);

#define MAX_FDS 64   /* fase 7: 16 nao bastava — accept do Xorg falhava nofd */
/* ~~ fd table POR PROCESSO (fork real #72) ~~
 * Antes era global: close() do filho fechava fd do pai e o
 * pipe do Popen virava bagunça~ Agora cada processo tem a sua;
 * o macro `fds` mantém os usos originais intactos~ kyun~ */
typedef struct {
    int used;
    int type;          /* 0=arquivo, 1=eventfd, 2=timerfd, 3=pipe, 4=socket,
                          5=/dev/fb0, 6=/dev/ttyN, 7=/dev/input/mice */
    int flags;         /* O_NONBLOCK etc (fcntl) */
    int pipe_idx;      /* pipe: indice na tabela de pipes */
    int sock_idx;      /* socket: indice na tabela de sockets */
    int memfd_idx;     /* memfd: indice em g_memfds */
    char name[256];
    char path[VFS_MAX_PATH];
    uint64_t pos;
    uint64_t dir_cookie;
    uint32_t stat_size;
    uint64_t stat_ino;
    uint8_t stat_attr;
    uint8_t stat_valid;
    int wend;          /* pipe: 1 = ponta de ESCRITA (pro EOF do read) */
    uint64_t counter;  /* eventfd: contador; timerfd: intervalo (ms) */
    uint64_t tfd_expiry_ms;   /* FASE5: timerfd: deadline absoluta (ms);
                                 0 = desarmado */
    uint64_t tfd_interval_ms; /* FASE5: timerfd: período (0 = one-shot) */
    tty_t *tty;
    int tty_pair_idx;
} fdent_t;
typedef struct { fdent_t e[MAX_FDS]; } fdtable_t;
/* ~~ POOL ESTÁTICO: as tabelas NÃO podem morrer no heap!~~
 * User stacks são kmalloc'd no MESMO heap — overflow delas
 * corrompia nossas tabelas com bytes de código (vimos type=
 * 0x548B4808 rssrsrs). BSS é imune~ 1 tabela por slot PCB~ */
static fdtable_t fd_tables[MAX_PROC];
static fdtable_t boot_fds;                 /* kernel/shell antes do lazy alloc */
static fdtable_t *fdtab = &boot_fds;
#define fds fdtab->e

/* ~ liga a tabela do processo atual (início de toda syscall)~
 * Aloca lazily na primeira vez que o processo faz syscall~ */

/* ~~ mini-strstr pro freestanding (sem string.h) ~~ */
static const char *k_strstr(const char *h, const char *n) {
    if (!n[0]) return h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return h;
    }
    return 0;
}

static int user_buffer_valid(const void *buffer, size_t size, int writable) {
    if (!buffer || !size) return 0;
    uint64_t start = (uint64_t)(uintptr_t)buffer;
    if (start >= 0x800000000000ULL || size - 1 > UINT64_MAX - start)
        return 0;
    uint64_t end = start + size - 1;
    if (end >= 0x800000000000ULL) return 0;

    pcb_t *current = process_current();
    if (!current || !current->pml4) return 0;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)current->pml4;
    uint64_t page = start & ~0xFFFULL;
    uint64_t last_page = end & ~0xFFFULL;
    for (;;) {
        uint64_t pml4e = pml4[(page >> 39) & 0x1FF];
        if ((pml4e & 0x5) != 0x5 || (writable && !(pml4e & 0x2))) return 0;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & ~0xFFFULL);
        uint64_t pdpte = pdpt[(page >> 30) & 0x1FF];
        if ((pdpte & 0x5) != 0x5 || (writable && !(pdpte & 0x2))) return 0;
        if (pdpte & 0x80) {
            page += 1ULL << 30;
        } else {
            uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & ~0xFFFULL);
            uint64_t pde = pd[(page >> 21) & 0x1FF];
            if ((pde & 0x5) != 0x5 || (writable && !(pde & 0x2))) return 0;
            if (pde & 0x80) {
                page += 1ULL << 21;
            } else {
                uint64_t *pt = (uint64_t *)(uintptr_t)(pde & ~0xFFFULL);
                uint64_t pte = pt[(page >> 12) & 0x1FF];
                if ((pte & 0x5) != 0x5 || (writable && !(pte & 0x2))) return 0;
                page += 0x1000;
            }
        }
        if (page > last_page) break;
    }
    return 1;
}

void fds_bind_current(void) {
    pcb_t *me = process_current();
    if (!me) return;
    if (!me->fds_tab) {
        int myslot = (int)(me - pcb_table);
        if (myslot < 0 || myslot >= MAX_PROC) return;
        me->fds_tab = &fd_tables[myslot];
        fdent_t *ee = ((fdtable_t *)me->fds_tab)->e;
        for (int i = 0; i < MAX_FDS; i++) { ee[i].used = 0; ee[i].type = 0; }
        ee[0].used = ee[1].used = ee[2].used = 1;   /* stdin/out/err~ */
        ee[0].type = ee[1].type = ee[2].type = 6;   /* tty console~ */
    }
    fdtab = (fdtable_t *)me->fds_tab;
}

/* pro fork: copia a tabela atual pro slot DO FILHO (pool!)~
 * NÃO mexe no fdtab global — quem continua rodando é o PAI! */
void fds_dup_into_slot(int child_slot) {
    if (child_slot < 0 || child_slot >= MAX_PROC) return;
    unsigned char *d = (unsigned char *)&fd_tables[child_slot];
    unsigned char *s = (unsigned char *)fdtab;
    for (int i = 0; i < (int)sizeof(fdtable_t); i++) d[i] = s[i];
    /* filho herda stdin/out/err + pipes abertos~ */
}
void *fds_tab_addr(int slot) {
    if (slot < 0 || slot >= MAX_PROC) return 0;
    return &fd_tables[slot];
}
/* ~~ fecha todos os fds >= 3 da tabela de um processo (no exit)~~
 * Sem isso o pipe_writers_alive via escritor morto pra sempre~ */
void fds_close_exec(void *tab) {
    if (!tab) return;
    fdtable_t *t = (fdtable_t *)tab;
    for (int i = 3; i < MAX_FDS; i++) t->e[i].used = 0;
}

/* ~~ device fds pro Xorg ~~
 * type 5 = /dev/fb0 (framebuffer: ioctls FBIO, mmap)
 * type 6 = /dev/ttyN (console: ioctls VT e KD, read = scan codes crus)
 * type 7 = /dev/input/mice (read = bytes crus do PS/2)
 * type 11 = /dev/input/eventN (evdev: ioctls EVIOC, read = input_event) */
#define FD_FB    5
#define FD_TTY   6
#define FD_MICE  7
#define FD_TTY_MASTER 8
#define FD_TTY_SLAVE  9
#define FD_EPOLL      10
#define FD_EVDEV      11
#define FD_MEMFD      12

/* ~~ pipes (issue #52) ~~ um buffer por pipe, read/write pos separados */
#define MAX_PIPES 8
static struct {
    int used;
    uint8_t buf[4096];
    uint32_t rpos, wpos;
} pipes[MAX_PIPES];

/* ~~ sockets AF_UNIX (issue #49) ~~
 * Cada socket tem um buffer próprio (como um pipe) e aponta pro peer.
 * write() grava no buffer do peer; read() lê do próprio buffer.
 * socketpair cria dois fds já conectados (peer <-> peer).
 * listener tem fila de pending: clientes que conectaram e esperam accept~ */
#define MAX_SOCKS 16
#define SOCK_QUEUE 8
/* ~~ buffer por conexao: o Xorg manda replies de >5KB num write so
 * (fontes/propriedades). Com 4KB o write ficava curto + EAGAIN e o
 * servidor derrubava o cliente (fvwm morria com code=1 no meio da
 * inicializacao). 16KB absorve o reply tipico inteiro. ~~ */
#define SOCK_BUF 16384
static struct {
    int used;
    char path[256];      /* path do bind (ex: /tmp/test.sock) */
    int listening;       /* chamou listen() */
    int peer;            /* índice do socket parceiro, -1 se sem */
    int nonblock;        /* O_NONBLOCK herdado do fd */
    int pending[SOCK_QUEUE]; /* fila de conexões aguardando accept */
    int npending;
    /* ~~ 1 = este socket ja tem fd em alguma tabela. O lado do
     * servidor e alocado NO CONNECT (slot proprio), ainda sem fd,
     * ate o accept entregar o fd. ~~ */
    int has_fd;
    uint8_t buf[SOCK_BUF]; /* buffer de leitura (o peer escreve aqui) */
    uint32_t rpos, wpos;
    /* FASE5: fila de fds SCM_RIGHTS pendentes (era 1 slot só: dois
     * create_pool seguidos (foot abre ~5 pools SHM no boot) faziam o
     * 2º sobrescrever o 1º — o 1º pool ganhava fd errado e o 2º
     * nenhum ("file descriptor expected, object"). */
#define SOCK_ANC_N 8
    int anc_n;
    int anc_mi[SOCK_ANC_N]; /* memfd_idx por slot */
} socks[MAX_SOCKS];

/* ~~ memfd (wayland shm, 319) ~ arquivo anonimo via mmap MAP_SHARED ~~
 * Compartilhado via SCM_RIGHTS (1 fd/msg). Paginas em vm_object global
 * compartilhado (refcnt). /proc/self/fd/N eh conveniencia de debug. */
#define MAX_MEMFD 128 /* FASE6-poolchurn: era 16 — foot guarda ~15 pools
 * mapeados (cache próprio) + keymap; 17º pool abortava (ENOMEM).
 * Com GC no create, working set recicla; folga p/ sessões longas. */
#define MFD_CLOEXEC       0x0001
#define MFD_ALLOW_SEALING 0x0002
typedef struct { int used; char name[64]; struct vm_object *obj; size_t size; } memfd_entry_t;
static memfd_entry_t g_memfds[MAX_MEMFD];
static int is_proc_self_fd(const char *path) { if(!path) return 0; const char *want="/proc/self/fd/"; for(int i=0;i<14;i++){ if(!path[i]) return 0; if(path[i]!=want[i]) return 0; } return 1; }


#define MAX_EPOLL 8
typedef struct __attribute__((packed)) {
    uint32_t events;
    uint64_t data;
} linux_epoll_event_t;
typedef struct {
    int used;
    int target_fd;
    uint32_t events;
    uint64_t data;
} epoll_watch_t;
static struct {
    int used;
    epoll_watch_t watches[MAX_FDS];
} epolls[MAX_EPOLL];

/* ~~ O_NONBLOCK (fcntl) ~~ */
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define MSG_CMSG_CLOEXEC 0x40000000
#define O_NONBLOCK 0x800
#define O_CLOEXEC  0x80000
#define O_CREAT     0x40
#define O_EXCL      0x80
#define O_NOCTTY    0x100
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_ACCMODE   0x3
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EACCESS          0x200
#define AT_REMOVEDIR        0x200
#define AT_NO_AUTOMOUNT     0x800
#define AT_EMPTY_PATH       0x1000
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#define LINUX_ELOOP         40
#define LINUX_EXDEV         18
#define LINUX_EOPNOTSUPP    95

struct timeval { uint64_t tv_sec; uint64_t tv_usec; };
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} linux_stat_timespec_t;

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    linux_stat_timespec_t st_atim;
    linux_stat_timespec_t st_mtim;
    linux_stat_timespec_t st_ctim;
    int64_t __reserved[3];
};

_Static_assert(sizeof(struct stat) == 144, "Linux x86-64 stat size");
_Static_assert(__builtin_offsetof(struct stat, st_dev) == 0, "stat.st_dev");
_Static_assert(__builtin_offsetof(struct stat, st_ino) == 8, "stat.st_ino");
_Static_assert(__builtin_offsetof(struct stat, st_nlink) == 16, "stat.st_nlink");
_Static_assert(__builtin_offsetof(struct stat, st_mode) == 24, "stat.st_mode");
_Static_assert(__builtin_offsetof(struct stat, st_uid) == 28, "stat.st_uid");
_Static_assert(__builtin_offsetof(struct stat, st_gid) == 32, "stat.st_gid");
_Static_assert(__builtin_offsetof(struct stat, __pad0) == 36, "stat.__pad0");
_Static_assert(__builtin_offsetof(struct stat, st_rdev) == 40, "stat.st_rdev");
_Static_assert(__builtin_offsetof(struct stat, st_size) == 48, "stat.st_size");
_Static_assert(__builtin_offsetof(struct stat, st_blksize) == 56, "stat.st_blksize");
_Static_assert(__builtin_offsetof(struct stat, st_blocks) == 64, "stat.st_blocks");
_Static_assert(__builtin_offsetof(struct stat, st_atim) == 72, "stat.st_atim");
_Static_assert(__builtin_offsetof(struct stat, st_mtim) == 88, "stat.st_mtim");
_Static_assert(__builtin_offsetof(struct stat, st_ctim) == 104, "stat.st_ctim");
_Static_assert(__builtin_offsetof(struct stat, __reserved) == 120, "stat.__reserved");

/* ~~ ELF64 headers (pro Linux ABI) ~~ */
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;
typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;
#define PT_LOAD 1

extern void *elf64_load_into_pml4(const uint8_t *data, uint32_t len, uint64_t pml4);
extern uint64_t elf64_pie_base(void);

/* ~ cuidado que essa aqui morde ~ */
void syscall_init(void) {
    for (int i = 0; i < MAX_FDS; i++) fds[i].used = 0;
    for (int i = 0; i < MAX_PIPES; i++) pipes[i].used = 0;
    for (int i = 0; i < MAX_SOCKS; i++) {
        socks[i].used = 0; socks[i].peer = -1;
        socks[i].npending = 0; socks[i].has_fd = 0; socks[i].anc_n = 0;
    }
}

/* ~ essa demorou pra debugar, respeita ~ */
void fds_cleanup(void) {
    for (int i = 3; i < MAX_FDS; i++) fds[i].used = 0;
}

/* ~ cuidado que essa aqui morde ~ */
static int alloc_fd(void) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!fds[i].used) return i;
    }
    return -1;
}

/* ~~ open_dev ~ "Abre um device file" ~~
 * /dev/fb0, /dev/ttyN e /dev/input/mice pro Xorg.
 * Retorna o fd (ja com O_NONBLOCK setado, que e como o Xorg abre)
 * ou -1 se o path nao for um device conhecido. */
static int str_equal(const char *a, const char *b);
static int pts_path_index(const char *path) {
    static const char prefix[] = "/dev/pts/";
    for (int i = 0; prefix[i]; i++) {
        if (!path[i]) return -1;
        if (path[i] != prefix[i]) return -1;
    }
    const char *digit = path + sizeof(prefix) - 1;
    if (!*digit) return -2;
    uint32_t value = 0;
    while (*digit >= '0' && *digit <= '9') {
        uint32_t next = value * 10 + (uint32_t)(*digit - '0');
        if (next < value || next >= MAX_TTY_PAIRS) return -2;
        value = next;
        digit++;
    }
    return *digit ? -2 : (int)value;
}

int posix_openpt(int flags) {
    tty_t *master = tty_pair_alloc();
    if (!master) return -VFS_ENOSPC;
    int fd = alloc_fd();
    if (fd < 0) {
        tty_pair_free(master);
        return -VFS_ENOSPC;
    }
    fds[fd].type = FD_TTY_MASTER;
    fds[fd].tty = master;
    fds[fd].tty_pair_idx = tty_pair_index(master);
    fds[fd].flags = flags;
    fds[fd].used = 1;
    fds[fd].name[0] = '\0';
    fds[fd].path[0] = '\0';
    return fd;
}

int grantpt(int fd) {
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].used ||
        fds[fd].type != FD_TTY_MASTER)
        return -VFS_EBADF;
    return 0;
}

int unlockpt(int fd) {
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].used ||
        fds[fd].type != FD_TTY_MASTER)
        return -VFS_EBADF;
    return tty_pair_unlock(fds[fd].tty);
}

int ptsname(int fd, char *buffer, size_t size) {
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].used ||
        fds[fd].type != FD_TTY_MASTER)
        return -VFS_EBADF;
    return tty_pair_name(fds[fd].tty, buffer, size);
}

static int open_dev_impl(const char *path, int flags) {
    if (!path || !path[0]) return -1;
    if (k_strstr(path, "mice") || k_strstr(path, "event") || k_strstr(path, "fb0")) {
        serial_puts("[open-dev] '"); serial_puts(path); serial_puts("'\r\n");
        /* ~~ debug: trace de syscall do Xorg — DESLIGADO (fase 7).
         * Ele logava TODA syscall do Xorg no serial e deixava o boot
         * 5x mais lento (~55s ate o accept). Ligar so pra caçar bug. ~~ */
        extern int g_tipos_trace_pid;
        extern int process_current_pid(void);
        (void)g_tipos_trace_pid; (void)process_current_pid;
        if (0 && k_strstr(path, "mice"))
            g_tipos_trace_pid = process_current_pid();
    }
    if (str_equal(path, "/dev/ptmx")) {
        return posix_openpt(flags);
    }

    int pts_index = pts_path_index(path);
    if (pts_index != -1) {
        if (pts_index < 0) return -VFS_ENOENT;
        tty_pair_t *pair = tty_pair_get((uint32_t)pts_index);
        if (!pair || !pair->slave) return -VFS_ENOENT;
        if (!(pair->flags & TTY_PAIR_FLAG_UNLOCKED)) return -VFS_EACCES;
        int pts_fd = alloc_fd();
        if (pts_fd < 0) return -VFS_ENOSPC;
        fds[pts_fd].type = FD_TTY_SLAVE;
        fds[pts_fd].tty = pair->slave;
        fds[pts_fd].tty_pair_idx = pts_index;
        fds[pts_fd].flags = flags;
        fds[pts_fd].used = 1;
        int name_pos = 0;
        while (path[name_pos] && name_pos < (int)sizeof(fds[pts_fd].name) - 1) {
            fds[pts_fd].name[name_pos] = path[name_pos];
            name_pos++;
        }
        fds[pts_fd].name[name_pos] = '\0';
        fds[pts_fd].path[0] = '\0';
        return pts_fd;
    }

    int type = -1;
    if (str_equal(path, "/dev/fb0")) {
        type = FD_FB;
        /* early console off: Xorg abre fb0 antes do mmap */
        { extern int g_console_fbdev_enabled; if (g_console_fbdev_enabled) { g_console_fbdev_enabled = 0; } }
    } else if (str_equal(path, "/dev/input/mice")) {
        type = FD_MICE;
    } else if (str_equal(path, "/dev/input/event0") ||
               str_equal(path, "/dev/input/event1")) {
        /* ~~ evdev pro libinput do Xorg ~~
         * event0 = mouse, event1 = teclado (checado pelo nome no read/ioctl) */
        type = FD_EVDEV;
    } else if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' &&
               path[4] == '/' && path[5] == 't' && path[6] == 't' && path[7] == 'y' &&
               path[8] >= '0' && path[8] <= '9' && path[9] == '\0') {
        type = FD_TTY;
    }
    if (type < 0) return -1;
    int fd = alloc_fd();
    if (fd < 0) return -1;
    int i = 0;
    while (path[i] && i < 255) { fds[fd].name[i] = path[i]; i++; }
    fds[fd].name[i] = '\0';
    if (k_strstr(fds[fd].name, "def.xkm")) {
        extern int g_km_window;
        g_km_window = 25;
    }
    fds[fd].type = type;
    fds[fd].flags = O_NONBLOCK;
    fds[fd].tty = NULL;
    fds[fd].tty_pair_idx = -1;
    fds[fd].pos = 0;
    fds[fd].used = 1;
    return fd;
}

static int open_dev(const char *path, int flags) {
    return open_dev_impl(path, flags);
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
static void close_fd(int fd) {
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used) {
    int tty_pair_idx = (fds[fd].type == FD_TTY_MASTER ||
                fds[fd].type == FD_TTY_SLAVE)
                   ? fds[fd].tty_pair_idx : -1;
        /* ~~ pipe NÃO morre no close de uma ponta só! O EOF do read
         * usa pipe_writers_alive() que varre as tabelas dos processos~
         * (matar aqui fez o pai ler -1 com o filho vivo! rssrsrs) ~~ */
        if (fds[fd].type == 4) {
            int s = fds[fd].sock_idx;
            /* ~~ fecha ESTE fd primeiro e só mata o socket global se
             * NENHUM outro fd de NENHUMA tabela apontar pra ele ~~~
             * fork copia a fd table (fds_dup_into_slot): o filho herda
             * fd 3 com o MESMO sock_idx. O xterm -e sh faz o filho
             * close() dos fds herdados logo após o fork — sem esta
             * guarda, o close do FILHO derrubava a conexão viva
             * Xterm↔Xorg: Xorg via EOF, derrubava o client, nClients=0
             * → DE_RESET → restart completo do server generation (2º
             * init do Xorg) e o Xterm morria com "fatal IO error 0".
             * Mesmo padrão do still_open do tty_pair logo abaixo~ */
            fds[fd].used = 0;
            if (s >= 0 && s < MAX_SOCKS && socks[s].used) {
                int still_open = 0;
                for (int table = -1; table < MAX_PROC && !still_open; table++) {
                    fdtable_t *candidate = table < 0 ? &boot_fds : &fd_tables[table];
                    for (int entry = 3; entry < MAX_FDS; entry++) {
                        fdent_t *item = &candidate->e[entry];
                        if (item->used && item->type == 4 && item->sock_idx == s) {
                            still_open = 1;
                            break;
                        }
                    }
                }
                if (still_open) {
                    serial_puts("[sock-hold] s="); serial_puthex((uint32_t)s); serial_puts("\r\n");
                } else {
                serial_puts("[sock-kill] s="); serial_puthex((uint32_t)s); serial_puts("\r\n");
                int p = socks[s].peer;
                socks[s].used = 0;
                socks[s].has_fd = 0;
                /* ~~ cliente morreu antes do accept: o slot do lado do
                 * servidor (alocado no connect, ainda sem fd) ficaria
                 * preso na fila do listener pra sempre. ~~ */
                if (p >= 0 && p < MAX_SOCKS && socks[p].used &&
                    !socks[p].has_fd && !socks[p].listening) {
                    for (int L = 0; L < MAX_SOCKS; L++) {
                        if (!socks[L].used || !socks[L].listening) continue;
                        for (int i = 0; i < socks[L].npending; i++) {
                            if (socks[L].pending[i] == p) {
                                for (int j = i + 1; j < socks[L].npending; j++)
                                    socks[L].pending[j - 1] = socks[L].pending[j];
                                socks[L].npending--;
                                break;
                            }
                        }
                    }
                    socks[p].used = 0;
                    socks[p].peer = -1;
                }
                }
            }
        }
        if (fds[fd].type == FD_MEMFD) { /* memfd fd fecha so descritor, objeto global permanece */ }
        fds[fd].used = 0;
        if (tty_pair_idx >= 0 && tty_pair_idx < MAX_TTY_PAIRS) {
            int still_open = 0;
            for (int table = -1; table < MAX_PROC && !still_open; table++) {
                fdtable_t *candidate = table < 0 ? &boot_fds : &fd_tables[table];
                for (int entry = 0; entry < MAX_FDS; entry++) {
                    fdent_t *item = &candidate->e[entry];
                    if (item->used && item->tty_pair_idx == tty_pair_idx &&
                        (item->type == FD_TTY_MASTER || item->type == FD_TTY_SLAVE)) {
                        still_open = 1;
                        break;
                    }
                }
            }
            if (!still_open) tty_pair_free_index((uint32_t)tty_pair_idx);
        }
    }
}

/* ~~ do_readlink ~ resolve symlinks de /proc que o Xorg precisa ~~
 * /proc/self/exe → nome do processo; /proc/self/fd/N → nome do fd.
 * O driver fbdev usa isso pra detectar se /dev/fb0 e PCI ou nao~
 * Devolve bytes escritos (sem NUL, igual readlink real) ou -1. */
static int do_readlink(const char *path, char *buf, size_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return -1;
    const char *target = NULL;
    if (str_equal(path, "/proc/self/exe")) {
        pcb_t *cur = process_current();
        if (cur) target = cur->name;
    } else if (strncmp(path, "/proc/self/fd/", 14) == 0) {
        int fd = 0;
        for (int i = 14; path[i] >= '0' && path[i] <= '9'; i++)
            fd = fd * 10 + (path[i] - '0');
        if (fd >= 0 && fd < MAX_FDS && fds[fd].used)
            target = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
    } else if (strncmp(path, "/sys/class/graphics/fb", 22) == 0) {
        /* ~~ sysfs do fbdev ~ o driver do Xorg le /sys/class/graphics/fbN
         * pra ver se e device PCI/DRM (strstr(buf,"devices/pci")). Aqui
         * devolvemos "/dev/fbN" — sem "devices/pci" — pra ele tratar como
         * fbdev puro e seguir pro FBIOGET_FSCREENINFO~ */
        static char tmp[32];
        tmp[0] = '/'; tmp[1] = 'd'; tmp[2] = 'e'; tmp[3] = 'v'; tmp[4] = '/';
        tmp[5] = 'f'; tmp[6] = 'b';
        int n = 7, i = 22;
        while (path[i] >= '0' && path[i] <= '9' && n < 31) tmp[n++] = path[i++];
        tmp[n] = '\0';
        target = tmp;
    }
    if (!target) return -1;
    int i = 0;
    while (target[i] && i < (int)bufsiz - 1) { buf[i] = target[i]; i++; }
    buf[i] = '\0';
    return i;
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
static int str_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

#define MAX_VIRTUAL_TMP 32
#define VIRTUAL_TMP_BUFSZ 256
/* ~~ arquivo virtual em /tmp ~ existe so na RAM, tem buffer de conteudo ~~
 * O Xorg escreve o PID em /tmp/.tX0-lock e depois relê; sem buffer o
 * write falhava ("Could not write pid")~ kyun~ */
typedef struct {
    char path[VFS_MAX_PATH];
    char buf[VIRTUAL_TMP_BUFSZ];
    int len;
} virtual_tmp_file_t;
static virtual_tmp_file_t g_virtual_tmp[MAX_VIRTUAL_TMP];
static int g_virtual_tmp_count;

static int virtual_tmp_index(const char *path) {
    if (!path) return -1;
    for (int i = 0; i < g_virtual_tmp_count; i++) {
        if (str_equal(g_virtual_tmp[i].path, path)) return i;
    }
    return -1;
}

static int virtual_tmp_exists(const char *path) {
    if (!path) return 0;
    if (str_equal(path, "/tmp") || str_equal(path, "/tmp/.X11-unix")) return 1;
    return virtual_tmp_index(path) >= 0;
}

/* ~~ cria (ou devolve) a entrada virtual~ devolve o indice ou -1 ~~ */
static int virtual_tmp_mark(const char *path) {
    if (!path) return -1;
    int existing = virtual_tmp_index(path);
    if (existing >= 0) return existing;
    if (g_virtual_tmp_count >= MAX_VIRTUAL_TMP) return -1;
    int len = 0;
    while (path[len] && len < VFS_MAX_PATH - 1) len++;
    if (!len) return -1;
    int slot = g_virtual_tmp_count;
    for (int i = 0; i < len; i++) g_virtual_tmp[slot].path[i] = path[i];
    g_virtual_tmp[slot].path[len] = '\0';
    g_virtual_tmp[slot].len = 0;
    g_virtual_tmp_count++;
    return slot;
}

/* ~~ apaga a entrada virtual (unlinkat) ~ compacta o array~ ~~ */
static void virtual_tmp_remove(const char *path) {
    int idx = virtual_tmp_index(path);
    if (idx < 0) return;
    for (int i = idx; i < g_virtual_tmp_count - 1; i++)
        g_virtual_tmp[i] = g_virtual_tmp[i + 1];
    g_virtual_tmp_count--;
}

/* ~~ escreve no buffer de um arquivo virtual (0=append, offset real)~~ */
static int virtual_tmp_write(int slot, const char *buf, int count, uint64_t offset) {
    if (slot < 0 || slot >= g_virtual_tmp_count) return -1;
    virtual_tmp_file_t *v = &g_virtual_tmp[slot];
    if (offset > (uint64_t)VIRTUAL_TMP_BUFSZ) return -1;
    int pos = (int)offset;
    int n = 0;
    while (n < count && pos < VIRTUAL_TMP_BUFSZ) {
        v->buf[pos++] = buf[n++];
    }
    if (pos > v->len) v->len = pos;
    return n;
}

/* ~~ le do buffer de um arquivo virtual ~~ */
static int virtual_tmp_read(int slot, char *buf, int count, uint64_t offset) {
    if (slot < 0 || slot >= g_virtual_tmp_count) return -1;
    virtual_tmp_file_t *v = &g_virtual_tmp[slot];
    if (offset >= (uint64_t)v->len) return 0;
    int pos = (int)offset;
    int n = 0;
    while (n < count && pos < v->len) {
        buf[n++] = v->buf[pos++];
    }
    return n;
}

static int is_virtual_tmp_dir(const char *path) {
    return str_equal(path, "/tmp") || str_equal(path, "/tmp/.X11-unix");
}

static int is_virtual_tmp_file(const char *path) {
    if (!path) return 0;
    if (str_equal(path, "/tmp/.X11-unix") || str_equal(path, "/tmp")) return 0;
    if (virtual_tmp_exists(path)) return 1;
    /* tudo em /tmp que comeca com '.' (lock do Xorg .tX0-lock, sockets
     * .X11-unix/...) e virtual: o FAT32 do disco nao persiste esses arquivos */
    if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' &&
        path[4] == '/' && path[5] == '.')
        return 1;
    return 0;
}

static int is_virtual_tmp_path(const char *path) {
    return is_virtual_tmp_dir(path) || is_virtual_tmp_file(path);
}

int g_use_ext2 = 0;
extern int ext2_init(void);
extern int ext2new_stat(const char *path, unsigned int *size, bool *is_dir);
extern int ext2_read_file(const char *path, unsigned char *buffer, unsigned int size);
extern int ext2_stat(const char *path, unsigned int *size);
extern int ext2_create_file(const char *path);
extern int ext2_write_at(const char *path, unsigned char *buffer, unsigned int size, uint64_t offset);
extern int elf64_find_interp(unsigned char *data, unsigned int len, char *out, unsigned int out_max);
extern void *elf64_load_at(unsigned char *data, unsigned int len, uint64_t pml4, uint64_t force_base);

int fs_read_file(const char *name, uint8_t *buf, uint32_t count) {
    return vfs_read_file(name, buf, count);
}
int vfs_stat_size(const char *name, uint32_t *size, uint8_t *attr) {
    return vfs_stat_size_attr(name, size, attr);
}

#define LINUX_S_IFMT  0170000
#define LINUX_S_IFIFO 0010000
#define LINUX_S_IFCHR 0020000
#define LINUX_S_IFDIR 0040000
#define LINUX_S_IFREG 0100000
#define LINUX_S_IFSOCK 0140000

static uint64_t stat_inode_for_path(const char *path) {
    uint64_t hash = 1469598103934665603ULL;
    while (*path) {
        hash ^= (uint8_t)*path++;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

static uint32_t stat_mode_from_attr(uint8_t attr) {
    if (attr & 0x10) return LINUX_S_IFDIR | 0755;
    return LINUX_S_IFREG | 0644;
}

static void fill_linux_stat(struct stat *st, uint64_t size, uint32_t mode,
                            uint64_t ino, uint64_t rdev) {
    uint8_t *bytes = (uint8_t *)st;
    for (uint32_t index = 0; index < sizeof(*st); index++) bytes[index] = 0;
    st->st_dev = 1;
    st->st_ino = ino ? ino : 1;
    st->st_nlink = (mode & LINUX_S_IFMT) == LINUX_S_IFDIR ? 2 : 1;
    st->st_mode = mode;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = rdev;
    st->st_size = (int64_t)size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)(size / 512 + (size % 512 != 0));
}

static int stat_path_fill(const char *path, struct stat *st, int follow_symlink) {
    if (!path || !st) return -VFS_EFAULT;
    (void)follow_symlink;

    pcb_t *current = process_current();
    char absolute[VFS_MAX_PATH];
    int rc = vfs_abs_path(current ? current->cwd : "/", path,
                          absolute, sizeof(absolute));
    if (rc < 0) return rc;

    if (str_equal(absolute, "/")) {
        fill_linux_stat(st, 0, LINUX_S_IFDIR | 0755,
                        stat_inode_for_path(absolute), 0);
        return 0;
    }
    if (is_virtual_tmp_dir(absolute)) {
        int is_dir = is_virtual_tmp_dir(absolute);
        fill_linux_stat(st, is_dir ? 4096 : 0,
                        is_dir ? (LINUX_S_IFDIR | 0755) : (LINUX_S_IFREG | 0644),
                        stat_inode_for_path(absolute), 0);
        return 0;
    }
    if (is_proc_self_fd(absolute)) {
        // /proc/self/fd/N nao eh regular file p/ stat: deixa cair no VFS (ENOENT) a menos que O_CREAT
        // stat retorna ENOENT para nao-virtuais; open com /proc/self/fd/N sera tratado antes
        return -VFS_ENOENT;
    }
    if (is_virtual_tmp_file(absolute)) {
        if (!virtual_tmp_exists(absolute)) return -VFS_ENOENT;
        fill_linux_stat(st, 0, LINUX_S_IFREG | 0644,
                        stat_inode_for_path(absolute), 0);
        return 0;
    }

    uint32_t size = 0;
    uint8_t attr = 0;
    rc = vfs_stat_size_attr(absolute, &size, &attr);
    if (rc < 0) return rc;
    fill_linux_stat(st, size, stat_mode_from_attr(attr),
                    stat_inode_for_path(absolute), 0);
    return 0;
}

static int lstat_path_fill(const char *path, struct stat *st) {
    return stat_path_fill(path, st, 0);
}

static void fd_capture_stat(int fd, const char *path) {
    if (fd < 0 || fd >= MAX_FDS || !path) return;
    fds[fd].stat_valid = 0;
    fds[fd].stat_size = 0;
    fds[fd].stat_attr = 0;
    fds[fd].stat_ino = 0;

    pcb_t *current = process_current();
    char absolute[VFS_MAX_PATH];
    if (vfs_abs_path(current ? current->cwd : "/", path,
                     absolute, sizeof(absolute)) < 0)
        return;
    int path_index = 0;
    while (absolute[path_index] && path_index < (int)sizeof(fds[fd].path) - 1) {
        fds[fd].path[path_index] = absolute[path_index];
        path_index++;
    }
    fds[fd].path[path_index] = '\0';
    if (absolute[path_index]) return;
    if (str_equal(absolute, "/")) {
        fds[fd].stat_size = 0;
        fds[fd].stat_attr = 0x10;
        fds[fd].stat_ino = stat_inode_for_path(absolute);
        fds[fd].stat_valid = 1;
        return;
    }
    if (is_virtual_tmp_dir(absolute)) {
        int is_dir = is_virtual_tmp_dir(absolute);
        fds[fd].stat_size = is_dir ? 4096 : 0;
        fds[fd].stat_attr = is_dir ? 0x10 : 0x20;
    } else if (is_virtual_tmp_file(absolute)) {
        if (!virtual_tmp_exists(absolute)) return;
        fds[fd].stat_size = 0;
        fds[fd].stat_attr = 0x20;
    } else if (vfs_stat_size_attr(absolute, &fds[fd].stat_size,
                                  &fds[fd].stat_attr) < 0) {
        return;
    }
    fds[fd].stat_ino = stat_inode_for_path(absolute);
    fds[fd].stat_valid = 1;
}

static int fd_stat_fill(int fd, struct stat *st) {
    if (!st) return -VFS_EFAULT;
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].used) return -VFS_EBADF;

    uint32_t mode;
    uint64_t rdev = 0;
    switch (fds[fd].type) {
        case 0:
            if (!fds[fd].stat_valid) return -VFS_ENOENT;
            fill_linux_stat(st, fds[fd].stat_size,
                            stat_mode_from_attr(fds[fd].stat_attr),
                            fds[fd].stat_ino, 0);
            return 0;
        case 1:
        case 2:
            mode = LINUX_S_IFCHR | 0600;
            rdev = ((uint64_t)(uint32_t)fds[fd].type << 32) | (uint32_t)fd;
            fill_linux_stat(st, 0, mode, (uint64_t)fd + 1, rdev);
            return 0;
        case 3:
            fill_linux_stat(st, 0, LINUX_S_IFIFO | 0600,
                            (uint64_t)fds[fd].pipe_idx + 1, 0);
            return 0;
        case 4:
            fill_linux_stat(st, 0, LINUX_S_IFSOCK | 0600,
                            (uint64_t)fds[fd].sock_idx + 1, 0);
            return 0;
        case FD_FB:
        case FD_TTY:
        case FD_MICE:
        case FD_TTY_MASTER:
        case FD_MEMFD: { int mi=fds[fd].memfd_idx; size_t sz=(mi>=0&&mi<MAX_MEMFD&&g_memfds[mi].used)?g_memfds[mi].size:0; fill_linux_stat(st,(uint32_t)sz,LINUX_S_IFREG|0600,(uint64_t)fd+1000,0); return 0; }
        case FD_TTY_SLAVE:
            mode = LINUX_S_IFCHR | 0666;
            rdev = ((uint64_t)(uint32_t)fds[fd].type << 32) | (uint32_t)fd;
            fill_linux_stat(st, 0, mode, (uint64_t)fd + 1, rdev);
            return 0;
        default:
            return -VFS_EBADF;
    }
}

static int seek_offset(uint64_t current, int64_t delta, uint64_t *result) {
    if (!result) return -VFS_EFAULT;
    if (delta >= 0) {
        uint64_t amount = (uint64_t)delta;
        if (UINT64_MAX - current < amount) return -VFS_EINVAL;
        *result = current + amount;
        return 0;
    }
    uint64_t amount = (uint64_t)(-(delta + 1)) + 1;
    if (current < amount) return -VFS_EINVAL;
    *result = current - amount;
    return 0;
}

/* ~~ do_write_fd ~ escreve num fd (eventfd/pipe/socket/arquivo/console) ~~
 * Devolve bytes escritos (ou -1 em erro). Compartilhado entre
 * SYS_write e SYS_writev pra nao duplicar a logica~ */
static int sock_push(int s, const uint8_t *buf, int count);
static int sock_write_all(int peer, const uint8_t *buf, int count, int nonblock);
static int fat32_write_open_file(int fd, const uint8_t *buf, int count);
static int do_write_fd(int fd, const char *buf, int count) {
    if (fd >= 3 && fd < MAX_FDS) {
        serial_puts("[wr-dbg] fd="); serial_puthex((uint32_t)fd);
        serial_puts(" type="); serial_puthex((uint32_t)fds[fd].type);
        serial_puts(" used="); serial_puthex((uint32_t)fds[fd].used);
        serial_puts(" name='"); serial_puts(fds[fd].name);
        serial_puts("'\r\n");
    }
    if (fd >= 0 && fd < MAX_FDS && fds[fd].used &&
        (fds[fd].type == FD_TTY_MASTER || fds[fd].type == FD_TTY_SLAVE)) {
        int written;
        do {
            written = tty_pair_write(fds[fd].tty, buf, (size_t)count);
            if (written != -11 || (fds[fd].flags & O_NONBLOCK)) break;
            schedule();
        } while (1);
        return written;
    }
    /* ~~ pipe em qualquer fd — DEPOIS de dup2(pw, 1) o stdout É o
     * pipe (Popen do Xorg depende disso!), console só se não for~~ */
    if (fd >= 0 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 3) {
        int p = fds[fd].pipe_idx;
        if (p < 0 || p >= MAX_PIPES || !pipes[p].used) return -1;
        if (pipes[p].wpos - pipes[p].rpos >= 4096) {
            if (fds[fd].flags & O_NONBLOCK) return -11;
            return -1;
        }
        int n = 0;
        while (n < count && pipes[p].wpos - pipes[p].rpos < 4096) {
            pipes[p].buf[pipes[p].wpos & 4095] = (uint8_t)buf[n++];
            pipes[p].wpos++;
        }
        return n;
    }
    if (fd>=0 && fd<MAX_FDS && fds[fd].used && fds[fd].type==FD_MEMFD) {
        int mi=fds[fd].memfd_idx; if(mi<0||mi>=MAX_MEMFD||!g_memfds[mi].used) return -1;
        memfd_entry_t *me=&g_memfds[mi]; uint64_t off=fds[fd].pos;
        if(off>(uint64_t)me->size) return -1;
        /* FASE6-keymap: write ESTENDE (semântica de arquivo). Antes
         * truncava em me->size (4KB do create): o keymap XKB de 64KB
         * do foot nunca cabia → foot sem keymap → key_press_release
         * descartava TODA tecla em silêncio (sem pty, sem erro). */
        if(off+(uint64_t)count>(uint64_t)me->size) {
            size_t need_pages = (size_t)((off+(uint64_t)count+0xFFF)>>12);
            if(need_pages>(size_t)me->obj->page_count) {
                size_t old_pages=(size_t)me->obj->page_count;
                uint64_t *np=(uint64_t*)kcalloc(need_pages,sizeof(uint64_t));
                if(np) {
                    for(size_t i=0;i<old_pages;i++) np[i]=me->obj->pages[i];
                    kfree(me->obj->pages);
                    me->obj->pages=np; me->obj->page_count=(int)need_pages;
                    me->obj->size=need_pages<<12;
                } else {
                    /* sem memória p/ estender: comportamento antigo */
                    count=(int)(me->size-off);
                }
            }
            if((uint64_t)(off+(uint64_t)count)>me->size) me->size=off+count;
        }
        if(count<=0) return 0;
        for(int i=0;i<count;i++){ size_t pg=(size_t)((off+(uint64_t)i)>>12); size_t po=(size_t)((off+(uint64_t)i)&0xFFF); if(pg>=(size_t)me->obj->page_count) return -1; uint64_t pa=me->obj->pages[pg]; if(!pa){ pa=(uint64_t)(uintptr_t)mmap_user(0,4096,3,0x1000); if(!pa) return -1; me->obj->pages[pg]=pa; { extern void objref_claim(uint64_t p); objref_claim(pa); } } ((uint8_t*)(uintptr_t)pa)[po]=(uint8_t)buf[i]; }
        fds[fd].pos+=count; return count;
    }
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == FD_MICE) {
        /* ~~ /dev/input/mice write ~ comandos PS/2 (o driver mouse do
         * Xorg manda 0xFF=reset, 0xF4=enable etc). Por enquanto aceita
         * e descarta — o device ja vem habilitado do init ~~ */
        return count;
    }
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 1) {
        /* eventfd write: soma u64 no contador */
        if (count >= 8) {
            uint64_t val = 0;
            for (int i = 0; i < 8; i++) val |= (uint64_t)(uint8_t)buf[i] << (8 * i);
            fds[fd].counter += val;
        }
        return count;
    }
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 3) {
        /* pipe write: copia pro buffer compartilhado */
        int p = fds[fd].pipe_idx;
        if (p < 0 || p >= MAX_PIPES || !pipes[p].used) return -1;
        if (pipes[p].wpos - pipes[p].rpos >= 4096) {
            if (fds[fd].flags & O_NONBLOCK) return -11;
            return -1;
        }
        int n = 0;
        while (n < count && pipes[p].wpos - pipes[p].rpos < 4096) {
            pipes[p].buf[pipes[p].wpos & 4095] = (uint8_t)buf[n++];
            pipes[p].wpos++;
        }
        return n;
    }
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 4) {
        /* socket write: empurra pro buffer do peer */
        int s = fds[fd].sock_idx;
        int peer = (s >= 0 && s < MAX_SOCKS) ? socks[s].peer : -1;
        serial_puts("[sock-wr] pid=");
        serial_puthex((uint32_t)process_current_pid());
        serial_puts(" fd="); serial_puthex((uint32_t)fd);
        serial_puts(" s="); serial_puthex((uint32_t)s);
        serial_puts(" peer="); serial_puthex((uint32_t)peer);
        serial_puts(" n="); serial_puthex((uint32_t)count);
        serial_puts("\r\n");
        /* ~~ write bloqueante: espera o peer ler (socket Unix de
         * verdade). Sem isso, o Xorg levava -1 quando o buffer do
         * cliente encheu (5KB de reply) e derrubava a conexao. ~~ */
        return sock_write_all(peer, (const uint8_t *)buf, count,
                              fds[fd].flags & O_NONBLOCK);
    }
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used) {
        if (g_use_ext2) {
            /* ~~ VFS: create-if-missing + write_at com offset do stdio~~ */
            const char *file_path = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
            unsigned int cursz = 0;
            if (vfs_stat_size_attr(file_path, &cursz, NULL) != 0 &&
                vfs_create_file(file_path) != 0) return -1;
            if (fds[fd].flags & O_APPEND) fds[fd].pos = cursz;
            if (fds[fd].pos == 0) fds[fd].pos = cursz;
            if (vfs_write_at(file_path, (const uint8_t *)buf, count, fds[fd].pos) < 0)
                return -1;
            fds[fd].pos += count;
            return count;
        }
        int r = fat32_write_open_file(fd, (const uint8_t *)buf, count);
        {
            static char wseen[6][96]; static int wn=0;
            int dup=0;
            for (int q=0;q<wn;q++) if (str_equal(wseen[q],fds[fd].name)){dup=1;break;}
            if (!dup && wn<6){
                int q2=0; while (fds[fd].name[q2] && q2<95){wseen[wn][q2]=fds[fd].name[q2];q2++;}
                wseen[wn][q2]='\0'; wn++;
                serial_puts("[wr] "); serial_puts(fds[fd].name);
                serial_puts(" n="); serial_puthex((uint32_t)count);
                serial_puts(" r="); serial_puthex((uint32_t)r);
                serial_puts("\r\n");
            }
        }
        return (r >= 0) ? count : -1;
    }
    if (fd == 1 || fd == 2) {
        for (int i = 0; i < count; i++) console_putchar(buf[i]);
        return count;
    }
    return -1;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
/* ~~ resolve_at_path ~ pra syscalls *at ~~
 * Resolve um path a partir de um dirfd (ou AT_FDCWD = cwd atual).
 * O FAT32 nao entende paths com '/' — entao separa o diretorio
 * do nome, como o spawn faz, e deixa o cwd no dir certo.
 * Retorna 0 e grava o nome base em out (ate maxlen) ou -1.
 * O caller DEVE restaurar o cwd depois (fat32_set_cwd(saved)). */
/* ~~ fixpath ~ remap do prefixo de build do Xorg pra raiz do disco ~~
 * O Xorg foi compilado com prefixo ABSOLUTO do host: paths tipo
 * "/home/vinberkuko/tipos-musl/prefix/share/X11/xkb" chegam crus.
 * No FAT32 a gente serve isso da raiz: "/share/X11/xkb"~ */
#define XPREFIX "/home/vinberkuko/tipos-musl/prefix"
static const char *fixpath(const char *path, char *buf, int buflen) {
    if (!path) return path;

    /* ~~ ${prefix} literal do build system do Xorg ~~ */
    if (path[0] == '$' && path[1] == '{') {

        const char *rest = path + 9; /* pula "${prefix}" */
        if (path[2] == 'p' && path[3] == 'r' && path[4] == 'e') {
            int j = 0;
            if (*rest != '/') buf[j++] = '/';
            while (*rest && j < buflen - 1) buf[j++] = *rest++;
            buf[j] = '\0';
            path = buf;
            return path;
        }
    }

    int pl = sizeof(XPREFIX) - 1;
    int i = 0;
    while (XPREFIX[i] && path[i] == XPREFIX[i]) i++;
    if (i != pl) return path;
    if (path[i] != '\0' && path[i] != '/') return path;
    if (path[i] == '/') i++;
    int j = 0;
    buf[j++] = '/';
    while (path[i] && j < buflen - 1) buf[j++] = path[i++];
    buf[j] = '\0';
    return buf;
}

static int resolve_at(int dirfd, const char *path, int flags,
                      char *out, int outlen);

static int resolve_at_path(int dirfd, const char *path, int flags,
                           char *out, int outlen) {
    return resolve_at(dirfd, path, flags, out, outlen);
}

static int resolve_at(int dirfd, const char *path, int flags,
                      char *out, int outlen) {
    if (!out) return -VFS_EFAULT;
    if (!path || !path[0]) {
        if (!(flags & AT_EMPTY_PATH)) return -VFS_ENOENT;
        if (dirfd == AT_FDCWD) {
            pcb_t *current = process_current();
            return vfs_abs_path("/", current ? current->cwd : "/", out, outlen);
        }
        if (dirfd < 0 || dirfd >= MAX_FDS || !fds[dirfd].used)
            return -VFS_EBADF;
        if (!fds[dirfd].path[0]) return -VFS_ENOENT;
        return vfs_abs_path("/", fds[dirfd].path, out, outlen);
    }

    char fixed[VFS_MAX_PATH];
    path = fixpath(path, fixed, sizeof(fixed));
    if (!path || path[0] == '\0') return -VFS_ENOENT;
    if (path[0] == '/') return vfs_abs_path("/", path, out, outlen);

    const char *base;
    pcb_t *current = process_current();
    if (dirfd == AT_FDCWD) {
        base = current ? current->cwd : "/";
    } else {
        if (dirfd < 0 || dirfd >= MAX_FDS || !fds[dirfd].used)
            return -VFS_EBADF;
        if (fds[dirfd].type != 0 || !fds[dirfd].stat_valid ||
            !(fds[dirfd].stat_attr & 0x10))
            return -VFS_ENOTDIR;
        if (!fds[dirfd].path[0] && !fds[dirfd].name[0]) return -VFS_ENOTDIR;
        base = fds[dirfd].path[0] ? fds[dirfd].path : fds[dirfd].name;
    }
    return vfs_abs_path(base, path, out, outlen);
}

static int fat32_parent_leaf(const char *absolute, char *leaf, int leaf_size,
                             uint32_t *saved_cwd) {
    if (!absolute || !leaf || !saved_cwd) return -VFS_EFAULT;
    char resolved[VFS_MAX_PATH];
    int rc = resolve_at_path(AT_FDCWD, absolute, 0,
                             resolved, sizeof(resolved));
    if (rc < 0) return rc;
    *saved_cwd = fat32_get_cwd();
    int slash = -1;
    for (int index = 0; resolved[index]; index++)
        if (resolved[index] == '/') slash = index;
    const char *name = resolved + slash + 1;
    if (!name[0]) {
        fat32_set_cwd(*saved_cwd);
        return -VFS_EISDIR;
    }
    char parent[VFS_MAX_PATH];
    if (slash <= 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        for (int index = 0; index < slash; index++) parent[index] = resolved[index];
        parent[slash] = '\0';
    }
    if (fat32_change_dir(parent) < 0) {
        fat32_set_cwd(*saved_cwd);
        return -VFS_ENOENT;
    }
    int index = 0;
    while (name[index] && index < leaf_size - 1) { leaf[index] = name[index]; index++; }
    if (name[index]) {
        fat32_set_cwd(*saved_cwd);
        return -VFS_ENAMETOOLONG;
    }
    leaf[index] = '\0';
    return 0;
}

static int fat32_error_to_linux(int result) {
    if (result >= 0) return result;
    switch (result) {
        case FAT_ERR_NOTFOUND: return -VFS_ENOENT;
        case FAT_ERR_EXISTS: return -VFS_EEXIST;
        case FAT_ERR_IO: return -VFS_EIO;
        case FAT_ERR_NOSPACE: return -VFS_ENOSPC;
        case FAT_ERR_NOTDIR: return -VFS_ENOTDIR;
        case FAT_ERR_NOTEMPTY: return -VFS_ENOTEMPTY;
        default: return -VFS_EIO;
    }
}

static int path_stat_metadata(const char *absolute, uint32_t *size, uint8_t *attr) {
    if (!absolute) return -VFS_EFAULT;
    if (str_equal(absolute, "/")) {
        if (size) *size = 0;
        if (attr) *attr = 0x10;
        return 0;
    }
    if (is_virtual_tmp_dir(absolute)) {
        int is_dir = is_virtual_tmp_dir(absolute);
        if (size) *size = is_dir ? 4096 : 0;
        if (attr) *attr = is_dir ? 0x10 : 0x20;
        return 0;
    }
    if (is_proc_self_fd(absolute)) return -VFS_ENOENT;
    if (is_virtual_tmp_file(absolute)) {
        int vi = virtual_tmp_index(absolute);
        if (vi < 0) return -VFS_ENOENT;
        if (size) *size = (uint32_t)g_virtual_tmp[vi].len;
        if (attr) *attr = 0x20;
        return 0;
    }
    return vfs_stat_size_attr(absolute, size, attr);
}

static int create_file_at_path(const char *absolute) {
    if (g_use_ext2) return vfs_create_file(absolute);
    if (is_virtual_tmp_file(absolute)) {
        int slot = virtual_tmp_mark(absolute);
        if (slot >= 0) { g_virtual_tmp[slot].len = 0; }
        return 0;
    }
    char leaf[256];
    uint32_t saved_cwd;
    int rc = fat32_parent_leaf(absolute, leaf, sizeof(leaf), &saved_cwd);
    if (rc < 0) return rc;
    rc = fat32_error_to_linux(fat32_create_file(leaf));
    fat32_set_cwd(saved_cwd);
    return rc;
}

static int truncate_file_at_path(const char *absolute) {
    if (g_use_ext2) return -LINUX_EOPNOTSUPP;
    if (is_virtual_tmp_file(absolute)) {
        int slot = virtual_tmp_mark(absolute);
        if (slot >= 0) { g_virtual_tmp[slot].len = 0; }
        return 0;
    }
    char leaf[256];
    uint32_t saved_cwd;
    int rc = fat32_parent_leaf(absolute, leaf, sizeof(leaf), &saved_cwd);
    if (rc < 0) return rc;
    rc = fat32_error_to_linux(fat32_write_file(leaf, (const uint8_t *)"", 0));
    fat32_set_cwd(saved_cwd);
    return rc;
}

static int fat32_write_open_file(int fd, const uint8_t *buf, int count) {
    const char *absolute = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
    /* ~~ arquivo virtual em /tmp ~ escreve no buffer RAM ~~ */
    if (is_virtual_tmp_file(absolute)) {
        int slot = virtual_tmp_mark(absolute);
        if (slot < 0) return -VFS_ENOMEM;
        uint64_t off = (fds[fd].flags & O_APPEND)
                           ? (uint64_t)g_virtual_tmp[slot].len
                           : (uint64_t)fds[fd].pos;
        int w = virtual_tmp_write(slot, (const char *)buf, count, off);
        if (w < 0) return -VFS_EIO;
        fds[fd].pos = off + (uint64_t)w;
        return w;
    }
    uint32_t old_size = 0;
    uint8_t attr = 0;
    int stat_rc = path_stat_metadata(absolute, &old_size, &attr);
    if (stat_rc == 0 && (attr & 0x10)) return -VFS_EISDIR;
    if (stat_rc < 0 && stat_rc != -VFS_ENOENT) return stat_rc;

    uint8_t *write_buf = (uint8_t *)buf;
    uint32_t write_size = (uint32_t)count;
    uint8_t *combined = NULL;
    if (fds[fd].flags & O_APPEND) {
        if (old_size > 0xFFFFFFFFU - write_size) return -27;
        write_size += old_size;
        combined = (uint8_t *)kmalloc(write_size ? write_size : 1);
        if (!combined) return -VFS_ENOMEM;
        if (old_size > 0) {
            int read_size = vfs_read_at(absolute, combined, old_size, 0);
            if (read_size < 0 || (uint32_t)read_size != old_size) {
                kfree(combined);
                return read_size < 0 ? read_size : -VFS_EIO;
            }
        }
        for (int index = 0; index < count; index++)
            combined[old_size + (uint32_t)index] = buf[index];
        write_buf = combined;
    }

    char leaf[256];
    uint32_t saved_cwd;
    int rc = fat32_parent_leaf(absolute, leaf, sizeof(leaf), &saved_cwd);
    if (rc == 0) {
        int written = fat32_write_file(leaf, write_buf, write_size);
        if (written < 0 && stat_rc == -VFS_ENOENT && fat32_create_file(leaf) == 0)
            written = fat32_write_file(leaf, write_buf, write_size);
        rc = written < 0 ? -VFS_EIO : count;
        fat32_set_cwd(saved_cwd);
    }
    if (combined) kfree(combined);
    if (rc >= 0) {
        fds[fd].pos = (fds[fd].flags & O_APPEND) ? write_size : fds[fd].pos + count;
        fd_capture_stat(fd, absolute);
    }
    return rc;
}

/* ~~ socket helpers (issue #49) ~~ kyun~ */
/* ~~ ainda tem escritor vivo pro pipe p? (EOF do read!)~~
 * Varre as fd tables de TODOS os processos vivos~ */
static int pipe_writers_alive(int p) {
    extern pcb_t pcb_table[64];
    for (int i = 0; i < MAX_PROC; i++) {
        if (pcb_table[i].state == PROC_EMPTY) continue;
        fdtable_t *t = (fdtable_t *)pcb_table[i].fds_tab;
        if (!t) continue;
        for (int j = 0; j < MAX_FDS; j++)
            if (t->e[j].used && t->e[j].type == 3 &&
                t->e[j].pipe_idx == p && t->e[j].wend)
                return 1;
    }
    return 0;
}

static int sock_alloc(void) {
    for (int i = 0; i < MAX_SOCKS; i++) if (!socks[i].used) return i;
    return -1;
}

/* ~~ FASE6-poolchurn: GC de slots memfd (ver memfd_create) ~~ */
static int memfd_has_fdref(int mi) {
    extern pcb_t pcb_table[64];
    for (int i = 0; i < MAX_PROC; i++) {
        if (pcb_table[i].state == PROC_EMPTY) continue;
        fdtable_t *t = (fdtable_t *)pcb_table[i].fds_tab;
        if (!t) continue;
        for (int j = 0; j < MAX_FDS; j++)
            if (t->e[j].used && t->e[j].type == FD_MEMFD &&
                t->e[j].memfd_idx == mi)
                return 1;
    }
    return 0;
}
static int memfd_has_ancref(int mi) {
    for (int s = 0; s < MAX_SOCKS; s++) {
        if (!socks[s].used) continue;
        for (int q = 0; q < socks[s].anc_n; q++)
            if (socks[s].anc_mi[q] == mi) return 1;
    }
    return 0;
}
void memfd_gc(void) {
    for (int i = 0; i < MAX_MEMFD; i++) {
        if (!g_memfds[i].used) continue;
        /* só o dono-slot: sem alias vivo (cada alias +1) */
        if (g_memfds[i].obj && g_memfds[i].obj->refcnt != 1) continue;
        if (memfd_has_fdref(i)) continue;
        if (memfd_has_ancref(i)) continue;
        extern void serial_puts(const char *s);
        extern void serial_puthex(uint32_t v);
        serial_puts("[memfd-gc] slot="); serial_puthex((uint32_t)i);
        serial_puts("\r\n");
        if (g_memfds[i].obj) vm_object_destroy(g_memfds[i].obj);
        g_memfds[i].used = 0;
        g_memfds[i].obj = 0;
        g_memfds[i].size = 0;
    }
}

static int sock_by_path(const char *path) {
    if (!path) return -1;
    for (int i = 0; i < MAX_SOCKS; i++)
        if (socks[i].used && socks[i].listening && str_equal(socks[i].path, path))
            return i;
    return -1;
}

/* devolve o endereço de destino (sun_path) do sockaddr_un */
static const char *sock_path_from_addr(uint64_t addr, uint64_t len) {
    if (!addr || len < 2) return NULL;
    /* sockaddr_un: sa_family (2 bytes) + sun_path (cstring).
     * ~~ Xlib abstract socket (Linux): sun_path[0]='\0' seguido do
     * nome abstrato \0/tmp/.X11-unix/X0. Pula o NUL inicial pro
     * nome casar com o path do listener (Xorg bindou o path real)~~ */
    const char *sp = (const char *)(uintptr_t)addr + 2;
    if (len >= 3 && sp[0] == '\0' && sp[1] != '\0') return sp + 1;
    return sp;
}

/* escreve count bytes no buffer do socket s (chamado pelo peer).
 * Devolve quantos bytes couberam (0 = buffer cheio) ou -1 (s invalido). */
static int sock_push(int s, const uint8_t *buf, int count) {
    if (s < 0 || s >= MAX_SOCKS || !socks[s].used) return -1;
    int n = 0;
    while (n < count && socks[s].wpos - socks[s].rpos < SOCK_BUF) {
        socks[s].buf[socks[s].wpos & (SOCK_BUF - 1)] = buf[n++];
        socks[s].wpos++;
    }
    return n;
}

/* ~~ write completo no peer ~ bloqueia se o buffer encher e o fd for
 * bloqueante (semantica do Unix socket). Devolve count, -11 (EAGAIN
 * com O_NONBLOCK) ou -1 (peer morto). ~~ */
static int sock_write_all(int peer, const uint8_t *buf, int count, int nonblock) {
    int n = 0;
    while (n < count) {
        if (peer < 0 || peer >= MAX_SOCKS || !socks[peer].used)
            return n ? n : -1;
        int r = sock_push(peer, buf + n, count - n);
        if (r < 0) return n ? n : r;
        if (r == 0) {
            if (nonblock) return n ? n : -11;
            schedule();      /* peer nao leu ainda ~ cede a CPU */
            continue;
        }
        n += r;
    }
    return n;
}

/* lê count bytes do buffer do socket s */
static int sock_pop(int s, uint8_t *buf, int count) {
    if (s < 0 || s >= MAX_SOCKS || !socks[s].used) return -1;
    if (socks[s].rpos == socks[s].wpos) {
        if (socks[s].nonblock) return -11; /* -EAGAIN */
        return 0;
    }
    int n = 0;
    while (n < count && socks[s].rpos != socks[s].wpos) {
        buf[n++] = socks[s].buf[socks[s].rpos & (SOCK_BUF - 1)];
        socks[s].rpos++;
    }
    return n;
}

/* 1 se o fd tem dados pra ler (socket com buffer cheio ou listener com pending) */
static int fd_readable(int fd) {
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used &&
        (fds[fd].type == FD_TTY_MASTER || fds[fd].type == FD_TTY_SLAVE))
        return tty_pair_readable(fds[fd].tty);
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == FD_TTY)
        return kbd_raw_avail();
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == FD_MICE)
        return mice_avail();
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == FD_EVDEV) {
        int is_mouse = (k_strstr(fds[fd].name, "event0") != 0);
        return is_mouse ? (mice_avail_count() >= 3) : kbd_raw_avail();
    }
    if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 4) {
        int s = fds[fd].sock_idx;
        if (s >= 0 && s < MAX_SOCKS)
            return (socks[s].rpos != socks[s].wpos || socks[s].npending > 0 ||
                    socks[s].peer < 0 || socks[s].peer >= MAX_SOCKS ||
                    !socks[socks[s].peer].used);   /* EOF = legivel */
    }
    return 0;
}

static short fd_readiness(int fd, short events) {
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].used) return POLLNVAL;
    int type = fds[fd].type;
    short ready = 0;
    if (type == 0) {
        if (fds[fd].stat_attr & 0x10) ready |= POLLIN;
        else ready |= POLLIN | POLLOUT;
    } else if (type == FD_TTY_MASTER || type == FD_TTY_SLAVE) {
        if (tty_pair_readable(fds[fd].tty)) ready |= POLLIN;
        if (tty_pair_writable(fds[fd].tty)) ready |= POLLOUT;
    } else if (type == FD_TTY) {
        if (kbd_raw_avail()) ready |= POLLIN;
        ready |= POLLOUT;
    } else if (type == FD_MICE) {
        /* ~~ /dev/input/mice so tem direcao de leitura. Se a gente
         * reporta POLLOUT sempre, o loop de eventos do Xorg nunca
         * bloqueia (select retorna na hora com o fd "gravel"), o
         * driver le -EAGAIN e volta pro select -> busy-loop que impede
         * o servidor de chegar em "Server ready". ~~ */
        if (mice_avail()) ready |= POLLIN;
    } else if (type == 1) {
        if (fds[fd].counter > 0) ready |= POLLIN;
        ready |= POLLOUT;
    } else if (type == 2) {
        /* FASE5: timerfd legível SÓ se expirado (antes era sempre —
         * o foot via POLLIN fantasma no timer desarmado e abortava
         * em input_repeat). */
        extern volatile uint64_t timer_ticks;
        uint64_t now_ms = timer_ticks * 10ULL;
        if (fds[fd].tfd_expiry_ms && now_ms >= fds[fd].tfd_expiry_ms)
            ready |= POLLIN;
        ready |= POLLOUT;
    } else if (type == 3) {
        int p = fds[fd].pipe_idx;
        if (p < 0 || p >= MAX_PIPES || !pipes[p].used) return POLLERR;
        if (pipes[p].rpos != pipes[p].wpos) ready |= POLLIN;
        if (pipes[p].wpos - pipes[p].rpos < 4096) ready |= POLLOUT;
        if (!(fds[fd].wend) && !pipe_writers_alive(p)) ready |= POLLHUP;
    } else if (type == 4) {
        int s = fds[fd].sock_idx;
        if (s < 0 || s >= MAX_SOCKS || !socks[s].used) return POLLERR | POLLHUP;
        /* ~~ socket com listen() nunca sinaliza POLLHUP/POLLOUT ~~
         * o peer do listener e -1; sem este guard ele reportava
         * POLLHUP o tempo todo, o Xorg via "conexao nova" pra sempre,
         * chamava accept() e bloqueava — nunca lia os 12 bytes ja
         * entregues ao cliente aceito. Sintoma: XOpenDisplay travado
         * no fvwm2 e no xterm (fase 7). ~~ */
        if (socks[s].listening) {
            if (socks[s].npending > 0) ready |= POLLIN;
            return (short)(ready & (events | POLLERR | POLLNVAL));
        }
        if (socks[s].rpos != socks[s].wpos || socks[s].npending > 0) ready |= POLLIN;
        {
            int p = socks[s].peer;
            if (p >= 0 && p < MAX_SOCKS && socks[p].used) {
                if (socks[p].wpos - socks[p].rpos < SOCK_BUF) ready |= POLLOUT;
            } else {
                /* ~~ peer morreu (conexao fechada): sem POLLHUP aqui o
                 * cliente ficava preso no poll pra sempre. ~~ */
                ready |= POLLHUP;
            }
        }
    } else if (type == FD_EVDEV) {
        /* ~~ /dev/input/eventN (libinput) ~ so leitura, requer pacote PS/2 completo ~~
         * Sem este branch o evdev caia no catch-all e virava POLLERR
         * (ou nunca POLLIN), entao o select() do Xorg nunca acordava
         * quando chegava scancode/pacote PS2. Reporta POLLIN so quando
         * tem evento, igual ao FD_MICE. */
        int is_mouse = (k_strstr(fds[fd].name, "event0") != 0);
        if (is_mouse ? (mice_avail_count() >= 3) : kbd_raw_avail()) ready |= POLLIN;
    } else if (type >= FD_FB && type <= FD_MICE) {
        ready |= POLLOUT;
    } else {
        return POLLERR;
    }
    return (short)(ready & (events | POLLERR | POLLHUP | POLLNVAL));
}

static uint64_t ticks_after_milliseconds(uint64_t milliseconds) {
    uint64_t ticks = (milliseconds + 9ULL) / 10ULL;
    return ticks > UINT64_MAX - timer_ticks ? UINT64_MAX : timer_ticks + ticks;
}

/* ~~ syscall_trace ~~
 * Rastreia syscalls Linux unicos (bitmap). Imprime so a primeira
 * ocorrencia de cada numero no serial — debug do Xorg no QEMU.
 * Chamado do dispatcher Zig antes da traducao. */
int g_trace_window = 0;
volatile int g_xorg_fb_mapped = 0;
void syscall_trace(uint64_t num, uint64_t rip, const char *path) {
    if (g_trace_window > 0) {
        g_trace_window--;
        extern int process_current_pid(void);
        serial_puts("[T");
        serial_puthex((uint32_t)process_current_pid());
        serial_puts("]");
        serial_puthex((uint32_t)num);
        serial_puts(" ");
    }
    (void)num;
    (void)rip;
    (void)path;
}

volatile int g_in_syscall = 0;
int g_rd9_log = 0;   /* 1 enquanto o handler C de syscall roda */
int g_tipos_trace_pid = -1;  /* ~~ pid a tracejar (0=off) ~ debug temporario ~~ */

int g_km_window = 0;
void syscall_handler(uint64_t *regs) {
    fds_bind_current();   /* ~~ fd table do processo atual (fork #72) ~~ */
    g_in_syscall = 1;
    pcb_t *syscall_process = process_current();
    if (syscall_process && syscall_process->is_user)
        syscall_process->user_rsp = current_rsp0;
    uint64_t num  = regs[0];
    uint64_t a1 = regs[4];
    uint64_t a2 = regs[3];
    uint64_t a3 = regs[2];
    uint64_t a4 = regs[1];
    uint64_t ret = -1;

    /* ~~ trace temporario: loga toda syscall do Xorg (pid>=3, user) ~~
     * pra achar onde ele trava depois do wr-dbg no mice. */
    {
        extern int g_tipos_trace_pid;
        pcb_t *tp = process_current();
        if (g_tipos_trace_pid > 0 && tp && tp->pid == g_tipos_trace_pid) {
            serial_puts("[sys] n="); serial_puthex((uint32_t)num);
            serial_puts(" a1="); serial_puthex((uint32_t)a1);
            serial_puts("\r\n");
        }
    }

    switch (num) {
    case SYS_exit:
        {
            pcb_t *me_e = process_current();
            if (me_e && me_e->th_parent_pid >= 0) {
                proc_thread_exit((int)a1);   /* só a thread morre~ */
                for (;;) __asm__ volatile("hlt");
            }
            process_exit_current((int)a1);   /* processo de verdade */
        }
        for (;;) __asm__ volatile("hlt");
        break;

    case SYS_write: {
        int fd = (int)a1;
        const char *buf = (const char *)a2;
        int count = (int)a3;
        ret = do_write_fd(fd, buf, count);
        break;
    }

    case SYS_writev: {
        /* ~~ writev ~ o musl usa pra escrever no log (2 iovecs: buffer
         * velho + novo). Antes o writev caia no identity mapping do Zig
         * e virava getpid (20=20)! O __stdio_write do musl recebia o PID
         * como "bytes escritos" e ficava num loop infinito~ rssrsrs */
        int fd = (int)a1;
        const struct iovec *iov = (const struct iovec *)a2;
        int iovcnt = (int)a3;
        if (!iov || iovcnt <= 0) { ret = 0; break; }
        uint64_t total = 0;
        int err = 0;
        for (int i = 0; i < iovcnt; i++) {
            int r = do_write_fd(fd, (const char *)iov[i].iov_base, (int)iov[i].iov_len);
            if (r < 0) { err = 1; break; }
            total += (uint64_t)r;
        }
        ret = (err && total == 0) ? (uint64_t)-1 : total;
        break;
    }

    case SYS_read: {
        int fd = (int)a1;
        char *buf = (char *)a2;
        int count = (int)a3;
        /* ~~ Linux readv (19) ~ o __stdio_read do musl SEMPRE usa
         * readv com 2 iovecs — sem handler TODO fread retornava
         * EOF fantasma (o keymap morria na primeira leitura!)~~ */
        if (fd >= 0 && fd < MAX_FDS && fds[fd].used &&
            (fds[fd].type == FD_TTY_MASTER || fds[fd].type == FD_TTY_SLAVE)) {
            /* FASE6-stdin: vale p/ fd<3 (dup2 slave->0/1/2 do foot).
             * Antes só fd>=3 caía aqui; fd 0 caía no ramo console. */
            int n;
            do {
                n = tty_pair_read(fds[fd].tty, buf, (size_t)count);
                if (n != -11 || (fds[fd].flags & O_NONBLOCK)) break;
                schedule();
            } while (1);
            ret = (uint64_t)n;
            break;
        }
        if (fd>=0 && fd<MAX_FDS && fds[fd].used && fds[fd].type==FD_MEMFD) {
            int mi=fds[fd].memfd_idx; if(mi<0||mi>=MAX_MEMFD||!g_memfds[mi].used){ret=(uint64_t)-9; break;}
            memfd_entry_t *me2=&g_memfds[mi]; uint64_t off=fds[fd].pos; if(off>=me2->size){ret=0; break;}
            int toread=count; if(off+(uint64_t)toread>me2->size) toread=(int)(me2->size-off);
            for(int i=0;i<toread;i++){ size_t pg=(size_t)((off+(uint64_t)i)>>12); size_t po=(size_t)((off+(uint64_t)i)&0xFFF); if(pg>=(size_t)me2->obj->page_count){ret=(uint64_t)-5; break;}
                uint64_t pa=me2->obj->pages[pg]; if(!pa) buf[i]=0; else buf[i]=(char)((uint8_t*)(uintptr_t)pa)[po]; }
            if(toread>0){ fds[fd].pos+=toread; ret=toread; } break;
        }
        /* ~~ pipe read em qualquer fd (dup2(rd,0) etc)~~ */
        if (fd >= 0 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 3) {
            int p = fds[fd].pipe_idx;
            if (p < 0 || p >= MAX_PIPES || !pipes[p].used) { ret = -1; break; }
            if (fds[fd].flags & O_NONBLOCK) {
                if (pipes[p].rpos == pipes[p].wpos) { ret = (uint64_t)-11; break; }
            } else {
                /* ~~ blocking cooperativo: cede a CPU até ter dado ou
                 * todos os escritores fecharem (EOF = 0)~~ */
                for (;;) {
                    if (pipes[p].rpos != pipes[p].wpos) break;
                    if (!pipe_writers_alive(p)) break;
                    schedule();
                }
            }
            int n = 0;
            while (n < count && pipes[p].rpos != pipes[p].wpos) {
                buf[n++] = (char)pipes[p].buf[pipes[p].rpos & 4095];
                pipes[p].rpos++;
            }
            ret = n;
            break;
        }
        if (fd == 0 && buf && count > 0 &&
            fds[0].type != FD_TTY_MASTER && fds[0].type != FD_TTY_SLAVE) {
            /* FASE6-stdin: fd 0 redirecionado (dup2 pty/slave) NÃO é
             * console! Sem este guarda, o shell do foot lia o TECLADO
             * do console em vez do pty — e o spin abaixo (sem schedule)
             * congelava IF=0 p/ sempre (timer morto, mouse morto,
             * serial mudo, tela cinza). Cai no dispatch por tipo. */
            /* ~~ O_NONBLOCK no stdin ~ weston configura nonblock e
             * espera EAGAIN em vez de travar a thread~ */
            if ((fds[0].flags & O_NONBLOCK) && !keyboard_avail()) {
                ret = (uint64_t)-11; /* -EAGAIN */
                break;
            }
            int i;
            for (i = 0; i < count; i++) {
                while (!keyboard_avail()) {
                    /* FASE6-stalin: cede (sti/hlt acordam no timer);
                     * puro delay travava IF=0 p/ sempre. */
                    schedule();
                    for (volatile int j = 0; j < 100; j++);
                }
                buf[i] = keyboard_read();
                if (buf[i] == '\n') { i++; break; }
                if (buf[i] == 3) break;
            }
            ret = i;
            break;
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == FD_TTY) {
            /* ~~ /dev/ttyN read ~ scan codes crus pro driver kbd do Xorg ~~
             * O Xorg abre O_NONBLOCK e espera via select(); se nao tem
             * dado e nao eh nonblock, espera (spin com pause~) */
            int n = 0;
            while (n < count && kbd_raw_avail()) buf[n++] = (char)kbd_raw_read();
            if (n > 0) { ret = n; break; }
            if (fds[fd].flags & O_NONBLOCK) { ret = (uint64_t)-11; break; } /* -EAGAIN */
            while (!kbd_raw_avail()) schedule();
            buf[0] = (char)kbd_raw_read();
            ret = 1;
            break;
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == FD_MICE) {
            /* ~~ /dev/input/mice read ~ bytes crus do PS/2 ~~
             * O driver mouse do Xorg decodifica o protocolo sozinho~ */
            serial_puts("[mice-read] fd=0x"); serial_puthex((uint32_t)fd);
            serial_puts(" avail="); serial_puthex(mice_avail());
            serial_puts("\r\n");
            int n = 0;
            while (n < count && mice_avail()) buf[n++] = (char)mice_read();
            if (n > 0) { ret = n; break; }
            ret = (uint64_t)-11; /* -EAGAIN */
            break;
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == FD_FB) {
            /* ~~ /dev/fb0 read ~ o Xorg nao le, so mmap+ioctl~~ */
            ret = -1;
            break;
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == FD_EVDEV) {
            if (count < 24) { ret = (uint64_t)-22; break; }
            int is_mouse = (k_strstr(fds[fd].name, "event0") != 0);
            // Helpers inline: emite um input_event (24B LE) no buf
            // Layout: 0..7 tv_sec, 8..15 tv_usec, 16..17 type, 18..19 code, 20..23 value
            #define EV_EMIT(t,c,v) do { \
                uint8_t *e = (uint8_t *)buf + produced; \
                for (int _i=0;_i<16;_i++) e[_i]=0; \
                e[16]=(uint8_t)((t)&0xFF); e[17]=(uint8_t)(((t)>>8)&0xFF); \
                e[18]=(uint8_t)((c)&0xFF); e[19]=(uint8_t)(((c)>>8)&0xFF); \
                e[20]=(uint8_t)((v)&0xFF); e[21]=(uint8_t)(((v)>>8)&0xFF); \
                e[22]=(uint8_t)(((v)>>16)&0xFF); e[23]=(uint8_t)(((v)>>24)&0xFF); \
                produced+=24; \
            } while(0)
            int produced = 0;
            if (is_mouse) {
                // Consome pacotes de 3 bytes alinhados (usa mice_peek para nao dessincronizar).
                // Se sobrar 1 ou 2 bytes (pacote incompleto) nao produz ainda.
                extern int mice_avail_count(void);
                extern unsigned char mice_peek(int idx);
                static int ev_prev_btn = 0;
                int avail = mice_avail_count();
                /* FASE6-evdev: pacote SÓ é consumido se TODOS os seus
                 * eventos couberem (até 6: dx+dy+3 btn+SYN). Antes o
                 * loop garantia 1 evento e emitia até 6 → estouro do
                 * buffer do leitor (stack do compositor) → corrupção
                 * do dispatch e crash aleatório sob flood. */
                while (avail >= 3) {
                    unsigned char b0 = mice_peek(0);
                    unsigned char b1 = mice_peek(1);
                    unsigned char b2 = mice_peek(2);
                    if ((b0 & 0x08) == 0) { // dessincronizado: descarta 1 byte
                        (void)mice_read(); avail--;
                        continue;
                    }
                    int dx = (int)(int8_t)b1;
                    int dy = (int)(int8_t)b2;
                    int btn = b0 & 0x07;
                    int need = 1; // SYN sempre
                    if (dx != 0) need++;
                    if (dy != 0) need++;
                    if ((btn & 1) != (ev_prev_btn & 1)) need++;
                    if ((btn & 2) != (ev_prev_btn & 2)) need++;
                    if ((btn & 4) != (ev_prev_btn & 4)) need++;
                    if (produced + 24 * need > count) break; // fica p/ próxima read
                    // Consome os 3
                    (void)mice_read(); (void)mice_read(); (void)mice_read();
                    avail -= 3;
                    // X movement
                    if (dx != 0) EV_EMIT(2, 0, dx);      // EV_REL / REL_X
                    if (dy != 0) EV_EMIT(2, 1, -dy);     // REL_Y (inverte p/ screen)
                    // Botoes (BTN_LEFT=0x110, BTN_RIGHT=0x111, BTN_MIDDLE=0x112)
                    if ((btn & 1) != (ev_prev_btn & 1)) EV_EMIT(1, 0x110, (btn & 1) ? 1 : 0);
                    if ((btn & 2) != (ev_prev_btn & 2)) EV_EMIT(1, 0x111, (btn & 2) ? 1 : 0);
                    if ((btn & 4) != (ev_prev_btn & 4)) EV_EMIT(1, 0x112, (btn & 4) ? 1 : 0);
                    ev_prev_btn = btn;
                    EV_EMIT(0, 0, 0); // EV_SYN / SYN_REPORT
                }
            } else {
                // Teclado: consome scan codes crus e emite EV_KEY com press(1)/release(0).
                // Trata prefixo 0xE0 (extended) mapeando para keycodes Linux 96..127.
                /* FASE6-evdev: pior caso = 2 eventos (key+SYN = 48B).
                 * Mesmo motivo do mouse acima: sem bound exato, flood
                 * de teclas estourava o buffer do leitor. */
                while (produced + 48 <= count && kbd_raw_avail()) {
                    unsigned char sc = kbd_raw_read();
                    if (sc == 0xE0 && kbd_raw_avail()) {
                        unsigned char sc2 = kbd_raw_read();
                        // E0 0x48..0x50 = setas, E0 0x1C/0x1D/0x38 etc
                        // Mapeia para code = 0x60 + sc2 (evita colisao 0..0x7F)
                        int released = (sc2 & 0x80) ? 1 : 0;
                        int code = (sc2 & 0x7F) | 0x60;
                        EV_EMIT(1, code, released ? 0 : 1);
                        if (produced + 24 <= count) EV_EMIT(0, 0, 0);
                        // segundo evento SYN ja conta nos 24 extras se couber; se nao, deixa para proxima read
                        if (produced + 24 > count) break;
                        continue;
                    }
                    if (sc == 0xE0) break; // incompleto — espera proximo byte na proxima read
                    int released = (sc & 0x80) ? 1 : 0;
                    int code = sc & 0x7F;
                    if (code == 0) continue;
                    EV_EMIT(1, code, released ? 0 : 1);
                    if (produced + 24 <= count) EV_EMIT(0, 0, 0);
                    if (produced + 24 > count) break;
                }
            }
            #undef EV_EMIT
            if (produced > 0) { ret = produced; break; }
            if (fds[fd].flags & O_NONBLOCK) { ret = (uint64_t)-11; break; }
            while (!(is_mouse ? (mice_avail_count() >= 3) : kbd_raw_avail())) schedule();
            // Ao acordar, tenta novamente no proximo loop do syscall (evita duplicar logica)
            // Em vez de produzir 1 evento cru, volta a produzir pacote completo
            if (is_mouse) {
                // precisa de 3 bytes; se ainda nao tem 3, devolve EAGAIN e o poll acorda de novo
                extern int mice_avail_count(void);
                if (mice_avail_count() < 3) { ret = (uint64_t)-11; break; }
                // Forca uma iteracao produzindo ao menos SYN
                unsigned char b0 = mice_peek(0), b1 = mice_peek(1), b2 = mice_peek(2);
                if ((b0 & 0x08) == 0) { (void)mice_read(); ret = (uint64_t)-11; break; }
                (void)mice_read(); (void)mice_read(); (void)mice_read();
                uint8_t *e = (uint8_t *)buf;
                for (int _i=0;_i<24;_i++) e[_i]=0;
                int dx=(int)(int8_t)b1, dy=(int)(int8_t)b2;
                // emite 1 evento representativo mesmo se buffer pequeno
                if (dx!=0) { e[16]=2; e[18]=0; e[20]=(uint8_t)dx; ret=24; break; }
                if (dy!=0) { e[16]=2; e[18]=1; int v=-dy; e[20]=v&0xFF; e[21]=(v>>8)&0xFF; e[22]=(v>>16)&0xFF; e[23]=(v>>24)&0xFF; ret=24; break; }
                e[16]=0; e[18]=0; ret=24; break;
            } else {
                unsigned char sc = kbd_raw_read();
                uint8_t *e = (uint8_t *)buf;
                for (int _i=0;_i<24;_i++) e[_i]=0;
                if (sc == 0xE0 && kbd_raw_avail()) {
                    unsigned char sc2 = kbd_raw_read();
                    int released = (sc2 & 0x80) ? 1 : 0;
                    int code = (sc2 & 0x7F) | 0x60;
                    e[16]=1; e[18]=code & 0xFF; e[19]=(code>>8)&0xFF; e[20]= released?0:1;
                    ret=24; break;
                }
                int released = (sc & 0x80) ? 1 : 0;
                int code = sc & 0x7F;
                e[16]=1; e[18]=code & 0xFF; e[19]=(code>>8)&0xFF; e[20]= released?0:1;
                ret=24; break;
            }
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 1) {
            /* eventfd read: devolve contador e zera */
            if (count >= 8) {
                uint64_t v = fds[fd].counter;
                fds[fd].counter = 0;
                for (int i = 0; i < 8; i++) buf[i] = (char)(v >> (8 * i));
            }
            ret = count;
            break;
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 2) {
            /* FASE5: timerfd com semântica real. Desarmado ou ainda não
             * expirado: EAGAIN (nonblock) ou bloqueia até expirar.
             * Antes retornava "1 expiração" sempre — o foot lia o timer
             * de repeat desarmado e abortava sem foco de teclado. */
            extern volatile uint64_t timer_ticks;
            if (count >= 8) {
                for (;;) {
                    uint64_t now_ms = timer_ticks * 10ULL;
                    if (fds[fd].tfd_expiry_ms && now_ms >= fds[fd].tfd_expiry_ms) {
                        uint64_t n = 1;
                        if (fds[fd].tfd_interval_ms) {
                            n += (now_ms - fds[fd].tfd_expiry_ms) / fds[fd].tfd_interval_ms;
                            fds[fd].tfd_expiry_ms += n * fds[fd].tfd_interval_ms;
                        } else {
                            fds[fd].tfd_expiry_ms = 0;   /* one-shot */
                        }
                        for (int i = 0; i < 8; i++) buf[i] = (char)(n >> (8 * i));
                        ret = 8;
                        break;
                    }
                    if (fds[fd].flags & O_NONBLOCK) { ret = (uint64_t)-11; break; }
                    schedule();
                }
            } else {
                ret = (uint64_t)-VFS_EINVAL;
            }
            break;
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 4) {
            /* socket read: lê do próprio buffer */
            int s = fds[fd].sock_idx;
            if (s < 0 || s >= MAX_SOCKS || !socks[s].used) {
                ret = (uint64_t)-VFS_EBADF;
                break;
            }
            int peer_ok = socks[s].peer >= 0 && socks[s].peer < MAX_SOCKS &&
                          socks[socks[s].peer].used;
            if (socks[s].rpos == socks[s].wpos && peer_ok &&
                !(fds[fd].flags & O_NONBLOCK)) {
                serial_puts("[sock-block] pid=");
                serial_puthex((uint32_t)process_current_pid());
                serial_puts(" fd="); serial_puthex((uint32_t)fd);
                serial_puts(" sock="); serial_puthex((uint32_t)s);
                serial_puts(" peer="); serial_puthex((uint32_t)socks[s].peer);
                serial_puts("\r\n");
            }
            while (socks[s].rpos == socks[s].wpos && peer_ok &&
                   !(fds[fd].flags & O_NONBLOCK))
                schedule();
            if (socks[s].rpos == socks[s].wpos &&
                (fds[fd].flags & O_NONBLOCK)) {
                ret = (uint64_t)-11;
                break;
            }
            ret = sock_pop(s, (uint8_t *)buf, count);
            break;
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used) {
            const char *file_path = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
            /* ~~ arquivo virtual em /tmp ~ lê do buffer RAM ~~ */
            if (is_virtual_tmp_file(file_path)) {
                int vi = virtual_tmp_index(file_path);
                if (vi < 0) { ret = (uint64_t)-VFS_ENOENT; break; }
                int r = virtual_tmp_read(vi, buf, count, (uint64_t)fds[fd].pos);
                if (r < 0) { ret = -1; break; }
                fds[fd].pos += (uint32_t)r;
                ret = (uint64_t)r;
                break;
            }
            /* ~~ VFS: leitura com offset REAL — sem buffer estático~~
             * Suporta arquivos de qualquer tamanho (rules/base 49KB ✓)~~ */
            int r = vfs_read_at(file_path, (unsigned char *)buf, count, fds[fd].pos);
            if (r < 0) { ret = -1; break; }
            fds[fd].pos += r;
            ret = r;
            break;
        }
        break;
    }

    /* ~~ Linux clone (56) ~ threads do musl! ~~
     * ATENÇÃO args: clone é identity-mapped, então o Zig NÃO move
     * r10->rcx. Assinatura Linux: clone(flags, stack, ptid, tls,
     * ctid) = rdi, rsi, rdx, r10, r8. No frame: regs[4],regs[3],
     * regs[2],regs[7],regs[5]. LER DIRETO DOS REGS!~~ */
    case 56: {
        uint64_t flags_ = regs[4];
        uint64_t newsp_ = regs[3];
        uint64_t ptid_  = regs[2];
        uint64_t tls_   = regs[7];
        uint64_t ctid_  = regs[5];
        int cpid;
        if ((flags_ & 0x00010000ULL) && (flags_ & 0x00000100ULL)) {
            cpid = proc_clone_thread(flags_, newsp_, ptid_, ctid_, tls_, regs);
        } else {
            cpid = proc_fork(regs);
            if (cpid >= 0) schedule();
        }
        ret = (uint64_t)cpid;
        break;
    }

    /* ~~ Linux clone3 (435) ~ clone com struct (glibc >= 2.34 tenta PRIMEIRO!) ~~
     * Args: struct clone_args *uargs (rdi=a1), size_t size (rsi=a2).
     * Layout (uapi/sched.h, 8 bytes cada):
     *   +0 flags, +8 pidfd, +16 child_tid, +24 parent_tid, +32 exit_signal,
     *   +40 stack, +48 stack_size, +56 tls, +64 set_tid, +72 set_tid_size,
     *   +80 cgroup.
     * Sem isso, pthread_create falha com EPERM (default ret=-1) e a
     * glibc nem tenta o clone(56) — visto na Fase 5/PASSO 2. */
    case 435: {
        uint64_t *uargs = (uint64_t *)(uintptr_t)a1;
        uint64_t usize = a2;
        if (!uargs || usize < 8) { ret = (uint64_t)-22; break; } /* -EINVAL */
        uint64_t c3_flags = uargs[0];
        uint64_t c3_pidfd = (usize >= 16) ? uargs[1] : 0;
        uint64_t c3_ctid  = (usize >= 24) ? uargs[2] : 0;
        uint64_t c3_ptid  = (usize >= 32) ? uargs[3] : 0;
        uint64_t c3_stack = (usize >= 48) ? uargs[5] : 0;
        uint64_t c3_stksz = (usize >= 56) ? uargs[6] : 0;
        uint64_t c3_tls   = (usize >= 64) ? uargs[7] : 0;
        /* stack do filho = topo (Linux: stack+stack_size quando size>0) */
        uint64_t c3_newsp = c3_stack ? (c3_stksz ? c3_stack + c3_stksz : c3_stack) : 0;
        {
            extern void serial_puts(const char *s);
            extern void serial_puthex(uint32_t v);
            serial_puts("[clone3] flags="); serial_puthex((uint32_t)c3_flags);
            serial_puts(" stksz="); serial_puthex((uint32_t)c3_stksz);
            serial_puts(" pidfd="); serial_puthex((uint32_t)c3_pidfd);
            serial_puts("\r\n");
        }
        /* CLONE_PIDFD sem suporte: ignora (join é por futex+clear_tid) */
        (void)c3_pidfd;
        int c3pid;
        if ((c3_flags & 0x00010000ULL) && (c3_flags & 0x00000100ULL)) {
            c3pid = proc_clone_thread(c3_flags, c3_newsp, c3_ptid, c3_ctid, c3_tls, regs);
        } else {
            c3pid = proc_fork(regs);
            if (c3pid >= 0) schedule();
        }
        ret = (uint64_t)c3pid;
        break;
    }

    /* ~~ READV (19): iovecs sobre os mesmos backends do SYS_read ~~ */
    case 19: {
        int fd = (int)a1;
        struct { void *base; unsigned long len; } *iov = (void *)a2;
        int iovcnt = (int)a3;
        if (!iov || iovcnt <= 0 || iovcnt > 16) { ret = -1; break; }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 3) {
            /* pipe: consome sequencialmente pelos iovecs */
            int p = fds[fd].pipe_idx;
            if (p < 0 || p >= MAX_PIPES || !pipes[p].used) { ret = -1; break; }
            uint64_t tot = 0;
            for (int i = 0; i < iovcnt; i++) {
                if (!iov[i].base || !iov[i].len) continue;
                int n = 0;
                while ((uint32_t)n < iov[i].len && pipes[p].rpos != pipes[p].wpos) {
                    ((char *)iov[i].base)[n++] = (char)pipes[p].buf[pipes[p].rpos & 4095];
                    pipes[p].rpos++;
                }
                tot += n;
                if ((uint32_t)n < iov[i].len) break;
            }
            ret = tot;
            break;
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 4) {
            int socket_index = fds[fd].sock_idx;
            if (socket_index < 0 || socket_index >= MAX_SOCKS ||
                !socks[socket_index].used) {
                ret = (uint64_t)-VFS_EBADF;
                break;
            }
            uint64_t total = 0;
            for (int index = 0; index < iovcnt; index++) {
                if (!iov[index].base || !iov[index].len) continue;
                while (socks[socket_index].rpos == socks[socket_index].wpos &&
                       socks[socket_index].peer >= 0 &&
                       !(fds[fd].flags & O_NONBLOCK))
                    schedule();
                if (socks[socket_index].rpos == socks[socket_index].wpos) {
                    if (total == 0 && (fds[fd].flags & O_NONBLOCK))
                        ret = (uint64_t)-11;
                    else
                        ret = total;
                    break;
                }
                int received = sock_pop(socket_index,
                    (uint8_t *)iov[index].base, (int)iov[index].len);
                if (received < 0) {
                    ret = total ? total : (uint64_t)received;
                    break;
                }
                total += (uint32_t)received;
                if ((uint32_t)received < iov[index].len) {
                    ret = total;
                    break;
                }
            }
            if (total > 0) ret = total;
            break;
        }
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used) {
            /* arquivo via VFS: lê iovec a iovec avançando o offset */
            uint64_t tot = 0;
            int hardfail = 0;
            for (int i = 0; i < iovcnt; i++) {
                if (!iov[i].base || !iov[i].len) continue;
                const char *file_path = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
                int r = vfs_read_at(file_path,
                                    (unsigned char *)iov[i].base,
                                    (uint32_t)iov[i].len, fds[fd].pos);
                if (r < 0) { hardfail = 1; break; }
                fds[fd].pos += r;
                tot += r;
                if ((uint32_t)r < iov[i].len) break;   /* short read */
            }
            ret = hardfail ? (uint64_t)-1 : tot;
            break;
        }
        ret = -1;
        break;
    }

    case SYS_open: {
        const char *path = (const char *)a1;
        char fpbuf[512];
        int flags = (int)a2;
        path = fixpath(path, fpbuf, sizeof(fpbuf));
        /* ~~ /dev/tty compat genérico ~~
         * Qualquer app (sh, xterm, bash, python) pode abrir /dev/tty.
         * Retornar fd 0 (stdin) é FATAL: o shell fecha stdin e morre com
         * EPERM/EPERM no tcgetattr. Se o processo já tem um controlling_tty
         * (herdado do fork do xterm, que faz dup do pts slave para 0/1/2),
         * dupamos ele; senão, devolve novo fd apontando pro mesmo tty. */
        if (str_equal(path, "/dev/tty")) {
            pcb_t *ct = process_current();
            if (ct && ct->controlling_tty) {
                int d = alloc_fd();
                if (d < 0) { ret = (uint64_t)-VFS_ENOSPC; break; }
                fds[d].type = FD_TTY_SLAVE;
                fds[d].tty = ct->controlling_tty;
                fds[d].tty_pair_idx = -1;
                fds[d].flags = flags;
                fds[d].used = 1;
                fds[d].name[0] = '\0'; fds[d].path[0] = '\0';
                ret = d; break;
            }
            /* sem controlling_tty: tenta /dev/ttyN se existir, senão stdin */
            {
                int devfd = open_dev(path, flags);
                if (devfd != -1) { ret = (uint64_t)devfd; break; }
            }
            ret = 0; break;
        }
        if (str_equal(path, "/dev/stdin")) { ret = 0; break; }
        if (str_equal(path, "/dev/stdout")) { ret = 1; break; }
        if (str_equal(path, "/dev/stderr")) { ret = 2; break; }
        {
            int devfd = open_dev(path, flags);
            if (devfd != -1) { ret = (uint64_t)devfd; break; }
        }
        if (is_proc_self_fd(path)) {
            int n=0; for(int i=14; path[i]>='0'&&path[i]<='9'; i++) n=n*10+(path[i]-'0');
            // resolve target fd's memfd/file: dup it
            if(n>=0 && n<MAX_FDS && fds[n].used) {
                if(fds[n].type==FD_MEMFD) {
                    int fd2=alloc_fd(); if(fd2<0){ret=(uint64_t)-12; break;}
                    fds[fd2]=fds[n]; fds[fd2].flags=flags; fds[fd2].pos=0; fds[fd2].used=1;
                    ret=fd2; break;
                }
                // generic: dup regular file fd
                int fd2=alloc_fd(); if(fd2<0){ret=(uint64_t)-12; break;}
                fds[fd2]=fds[n]; fds[fd2].flags=flags; fds[fd2].used=1;
                ret=fd2; break;
            }
            ret=(uint64_t)-2; break;
        }
        char absolute[VFS_MAX_PATH];
        int rc = resolve_at(AT_FDCWD, path, 0, absolute, sizeof(absolute));
        if (rc >= 0) {
            uint32_t size = 0;
            uint8_t attr = 0;
            rc = path_stat_metadata(absolute, &size, &attr);
            int exists = rc == 0;
            if (!exists && rc != -VFS_ENOENT) { ret = (uint64_t)rc; break; }
            if (exists && (flags & O_CREAT) && (flags & O_EXCL)) {
                if (!is_virtual_tmp_file(absolute)) {
                    ret = (uint64_t)-VFS_EEXIST; break;
                }
            }
            if (!exists && (flags & O_CREAT)) {
                rc = create_file_at_path(absolute);
                if (rc < 0) { ret = (uint64_t)rc; break; }
            }
            if ((flags & O_TRUNC) && exists) {
                rc = truncate_file_at_path(absolute);
                if (rc < 0) { ret = (uint64_t)rc; break; }
            }
        }
        int fd = alloc_fd();
        if (fd < 0) { ret = -1; break; }
        int i = 0;
        while (path[i] && i < 255) { fds[fd].name[i] = path[i]; i++; }
        fds[fd].name[i] = '\0';
        fds[fd].type = 0;
        fds[fd].pos = 0;
        fds[fd].dir_cookie = 0;
        fds[fd].flags = flags;
        fds[fd].used = 1;
        fd_capture_stat(fd, path);
        ret = fd;
        break;
    }

    case SYS_close:
        close_fd((int)a1);
        ret = 0;
        break;

    case SYS_access: {
        const char *path = (const char *)a1;
        int mode = (int)a2;
        if (mode & ~7) { ret = (uint64_t)-VFS_EINVAL; break; }
        char absolute[VFS_MAX_PATH];
        int rc = resolve_at(AT_FDCWD, path, 0, absolute, sizeof(absolute));
        if (rc < 0) { ret = (uint64_t)rc; break; }
        if (str_equal(absolute, "/dev/tty") ||
            str_equal(absolute, "/dev/stdin") ||
            str_equal(absolute, "/dev/stdout") ||
            str_equal(absolute, "/dev/stderr") ||
            str_equal(absolute, "/dev/fb0") ||
            str_equal(absolute, "/dev/input/mice") ||
            (strncmp(absolute, "/dev/tty", 8) == 0 &&
             absolute[8] >= '0' && absolute[8] <= '9' && !absolute[9])) {
            ret = (mode & 1) ? (uint64_t)-VFS_EACCES : 0;
            break;
        }
        uint32_t size = 0;
        uint8_t attr = 0;
        rc = path_stat_metadata(absolute, &size, &attr);
        if (rc < 0) { ret = (uint64_t)rc; break; }
        uint32_t permissions = (attr & 0x10) ? 0755 : 0755;
        if ((mode & 1) && !(permissions & 0111))
            ret = (uint64_t)-VFS_EACCES;
        else if ((mode & 2) && !(permissions & 0200))
            ret = (uint64_t)-VFS_EACCES;
        else if ((mode & 4) && !(permissions & 0400))
            ret = (uint64_t)-VFS_EACCES;
        else
            ret = 0;
        break;
    }

    case SYS_fstat: {
        struct stat *st = (struct stat *)a2;
        ret = (uint64_t)fd_stat_fill((int)a1, st);
        break;
    }

    case SYS_stat: {
        const char *path = (const char *)a1;
        struct stat *st = (struct stat *)a2;
        char fpbuf[256];
        if (path) path = fixpath(path, fpbuf, sizeof(fpbuf));
        ret = (uint64_t)stat_path_fill(path, st, 1);
        break;
    }

    case SYS_lstat: {
        const char *path = (const char *)a1;
        struct stat *st = (struct stat *)a2;
        char fpbuf[256];
        if (path) path = fixpath(path, fpbuf, sizeof(fpbuf));
        ret = (uint64_t)stat_path_fill(path, st, 0);
        break;
    }

    case SYS_lseek: {
        int fd = (int)a1;
        int64_t off = (int64_t)a2;
        int whence = (int)a3;
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used) {
            ret = (uint64_t)-VFS_EBADF;
            break;
        }
        uint64_t base = 0;
        uint64_t next = 0;
        switch (whence) {
            case 0: base = 0; break;
            case 1: base = fds[fd].pos; break;
            case 2: {
                if(fd>=0&&fd<MAX_FDS&&fds[fd].used&&fds[fd].type==FD_MEMFD){ int mm=fds[fd].memfd_idx; base=(mm>=0&&mm<MAX_MEMFD&&g_memfds[mm].used)?g_memfds[mm].size:0; break; }
                uint32_t size = 0;
                uint8_t attr;
                const char *file_path = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
                int rc = vfs_stat_size(file_path, &size, &attr);
                if (rc < 0) { ret = (uint64_t)rc; break; }
                base = size;
                break;
            }
            default: ret = (uint64_t)-VFS_EINVAL; break;
        }
        if (whence < 0 || whence > 2) { ret = (uint64_t)-VFS_EINVAL; break; }
        int rc = seek_offset(base, off, &next);
        if (rc < 0) { ret = (uint64_t)rc; break; }
        fds[fd].pos = next;
        ret = next;
        break;
    }

    case SYS_pread64: {
        int fd = (int)a1;
        const char *file_path;
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used || fds[fd].type != 0) {
            ret = (uint64_t)-VFS_EBADF;
            break;
        }
        file_path = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
        int r = vfs_read_at(file_path, (uint8_t *)a2, (uint32_t)a3, (uint64_t)a4);
        ret = r < 0 ? (uint64_t)r : (uint64_t)r;
        break;
    }

    case SYS_pwrite64: {
        int fd = (int)a1;
        const char *file_path;
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used || fds[fd].type != 0) {
            ret = (uint64_t)-VFS_EBADF;
            break;
        }
        file_path = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
        int r = vfs_write_at(file_path, (const uint8_t *)a2,
                             (uint32_t)a3, (uint64_t)a4);
        ret = r < 0 ? (uint64_t)r : (uint64_t)r;
        break;
    }

    case SYS_unlink:
        if (is_virtual_tmp_path((const char *)a1)) { ret = 0; break; }
        if (fat32_delete_file((const char *)a1) == 0) ret = 0;
        break;

    case SYS_mkdir2:
        /* ~~ /tmp virtual ~ o musl usa o mkdir antigo (83→136), e o
         * Xorg cria o diretorio do socket unix nele~ */
        if (str_equal((const char *)a1, "/tmp/.X11-unix") ||
            str_equal((const char *)a1, "/tmp")) { ret = 0; break; }
        if (fat32_mkdir((const char *)a1) == 0) ret = 0;
        break;

    case SYS_rmdir2:
        if (fat32_rmdir((const char *)a1) == 0) ret = 0;
        break;

    /* ~~ *at syscalls (issue #53) ~~
     * Dirfd relativo: suportamos AT_FDCWD (o cwd atual do processo).
     * Como o FAT32 nao tem fd de diretorio de verdade, dirfd != AT_FDCWD
     * cai no helper que retorna -1. O padrão é igual ao spawn: salva
     * cwd, resolve o path, opera, restaura. kyun~ */
    case SYS_openat: {
        int dirfd = (int)a1;
        int flags = (int)a3;
        const int supported_flags = O_ACCMODE | O_NONBLOCK | O_CLOEXEC |
            O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND |
            O_DIRECTORY | O_NOFOLLOW;
        if (flags & ~supported_flags) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        if ((flags & O_ACCMODE) == O_ACCMODE) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        char absolute[VFS_MAX_PATH];
        int rc = resolve_at_path(dirfd, (const char *)a2, 0,
                            absolute, sizeof(absolute));
        if (rc < 0) { ret = (uint64_t)rc; break; }
        /* ~~ /dev/tty compat genérico (openat) — mesmo do SYS_open ~~ */
        if (str_equal(absolute, "/dev/tty")) {
            pcb_t *ct = process_current();
            if (ct && ct->controlling_tty) {
                int d = alloc_fd();
                if (d < 0) { ret = (uint64_t)-VFS_ENOSPC; break; }
                fds[d].type = FD_TTY_SLAVE;
                fds[d].tty = ct->controlling_tty;
                fds[d].tty_pair_idx = -1;
                fds[d].flags = flags;
                fds[d].used = 1;
                fds[d].name[0] = '\0'; fds[d].path[0] = '\0';
                ret = d; break;
            }
            {
                int devfd = open_dev(absolute, flags);
                if (devfd != -1) { ret = (uint64_t)devfd; break; }
            }
            ret = 0; break;
        }
        if (str_equal(absolute, "/dev/stdin")) {
            ret = 0;
            break;
        }
        if (str_equal(absolute, "/dev/stdout")) { ret = 1; break; }
        if (str_equal(absolute, "/dev/stderr")) { ret = 2; break; }
        if (is_proc_self_fd(absolute)) {
            int n=0; for(int i=14; absolute[i]>='0' && absolute[i]<='9'; i++) n=n*10+(absolute[i]-'0');
            if(n>=0 && n<MAX_FDS && fds[n].used) {
                if(fds[n].type==FD_MEMFD) {
                    int fd2=alloc_fd(); if(fd2<0){ret=(uint64_t)-12; break;}
                    fds[fd2]=fds[n]; fds[fd2].flags=flags; fds[fd2].pos=0; fds[fd2].used=1;
                    ret=fd2; break;
                }
                int fd2=alloc_fd(); if(fd2<0){ret=(uint64_t)-12; break;}
                fds[fd2]=fds[n]; fds[fd2].flags=flags; fds[fd2].used=1;
                ret=fd2; break;
            }
            ret=(uint64_t)-2; break;
        }
        {
            int devfd = open_dev(absolute, flags);
            if (devfd != -1) {
                if (devfd < 0) {
                    ret = (uint64_t)devfd;
                } else if (flags & O_DIRECTORY) {
                    close_fd(devfd);
                    ret = (uint64_t)-VFS_ENOTDIR;
                } else {
                    fds[devfd].flags = flags;
                    ret = devfd;
                }
                break;
            }
        }
        uint32_t size = 0;
        uint8_t attr = 0;
        rc = path_stat_metadata(absolute, &size, &attr);
        int exists = rc == 0;
        if (!exists && rc != -VFS_ENOENT) { ret = (uint64_t)rc; break; }
        if (exists && (flags & O_CREAT) && (flags & O_EXCL)) {
            if (!is_virtual_tmp_file(absolute)) {
                ret = (uint64_t)-VFS_EEXIST;
                break;
            }
        }
        if (!exists) {
            if (!(flags & O_CREAT)) { ret = (uint64_t)-VFS_ENOENT; break; }
            rc = create_file_at_path(absolute);
            if (rc < 0) { ret = (uint64_t)rc; break; }
            size = 0;
            attr = 0x20;
        }
        if ((flags & O_DIRECTORY) && !(attr & 0x10)) {
            ret = (uint64_t)-VFS_ENOTDIR;
            break;
        }
        if ((flags & O_TRUNC) && (attr & 0x10)) {
            ret = (uint64_t)-VFS_EISDIR;
            break;
        }
        if ((flags & O_TRUNC) && size != 0) {
            rc = truncate_file_at_path(absolute);
            if (rc < 0) { ret = (uint64_t)rc; break; }
            size = 0;
        }
        int fd = alloc_fd();
        if (fd < 0) { ret = -1; break; }
        const char *fd_name = absolute;
        char leaf[256];
        uint32_t saved_cwd;
        if (!g_use_ext2 && !(attr & 0x10) &&
            fat32_parent_leaf(absolute, leaf, sizeof(leaf), &saved_cwd) == 0) {
            fat32_set_cwd(saved_cwd);
            fd_name = leaf;
        }
        int i = 0;
        while (fd_name[i] && i < (int)sizeof(fds[fd].name) - 1) {
            fds[fd].name[i] = fd_name[i];
            i++;
        }
        fds[fd].name[i] = '\0';
        fds[fd].type = 0;
        fds[fd].flags = flags;
        fds[fd].pos = (flags & O_APPEND) ? size : 0;
        fds[fd].dir_cookie = 0;
        fds[fd].used = 1;
        fd_capture_stat(fd, absolute);
        ret = fd;
        break;
    }

    case SYS_mkdirat: {
        int dirfd = (int)a1;
        char absolute[VFS_MAX_PATH];
        int rc = resolve_at_path(dirfd, (const char *)a2, 0,
                            absolute, sizeof(absolute));
        if (rc < 0) { ret = (uint64_t)rc; break; }
        if (str_equal(absolute, "/tmp/.X11-unix") || str_equal(absolute, "/tmp")) {
            ret = 0; break;
        }
        uint32_t existing_size = 0;
        uint8_t existing_attr = 0;
        rc = path_stat_metadata(absolute, &existing_size, &existing_attr);
        if (rc == 0) { ret = (uint64_t)-VFS_EEXIST; break; }
        if (rc != -VFS_ENOENT) { ret = (uint64_t)rc; break; }
        if (g_use_ext2) {
            ret = (uint64_t)vfs_mkdir(absolute);
            break;
        }
        char leaf[256];
        uint32_t saved_cwd;
        rc = fat32_parent_leaf(absolute, leaf, sizeof(leaf), &saved_cwd);
        if (rc < 0) { ret = (uint64_t)rc; break; }
        rc = fat32_error_to_linux(fat32_mkdir(leaf));
        fat32_set_cwd(saved_cwd);
        ret = (uint64_t)rc;
        break;
    }

    case SYS_newfstatat: {
        int dirfd = (int)a1;
        struct stat *st = (struct stat *)a3;
        int flags = (int)a4;
        if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_NO_AUTOMOUNT |
                      AT_EMPTY_PATH)) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        const char *path = (const char *)a2;
        if (path && !path[0] && (flags & AT_EMPTY_PATH) && dirfd != AT_FDCWD) {
            ret = (uint64_t)fd_stat_fill(dirfd, st);
            break;
        }
        char absolute[VFS_MAX_PATH];
        int rc = resolve_at_path(dirfd, path, flags, absolute, sizeof(absolute));
        if (rc < 0) { ret = (uint64_t)rc; break; }
        ret = (uint64_t)((flags & AT_SYMLINK_NOFOLLOW)
            ? lstat_path_fill(absolute, st)
            : stat_path_fill(absolute, st, 1));
        break;
    }

    case SYS_unlinkat: {
        int dirfd = (int)a1;
        const char *path = (const char *)a2;
        int flags = (int)a3;
        if (flags & ~(AT_REMOVEDIR | AT_EACCESS | AT_NO_AUTOMOUNT)) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        char absolute[VFS_MAX_PATH];
        int rc = resolve_at_path(dirfd, path, 0, absolute, sizeof(absolute));
        if (rc < 0) { ret = (uint64_t)rc; break; }
        if (is_virtual_tmp_file(absolute)) {
            virtual_tmp_remove(absolute);
            ret = 0;
            break;
        }
        uint32_t size = 0;
        uint8_t attr = 0;
        rc = path_stat_metadata(absolute, &size, &attr);
        if (rc < 0) { ret = (uint64_t)rc; break; }
        if ((attr & 0x10) && !(flags & AT_REMOVEDIR)) {
            ret = (uint64_t)-VFS_EISDIR;
            break;
        }
        if (!(attr & 0x10) && (flags & AT_REMOVEDIR)) {
            ret = (uint64_t)-VFS_ENOTDIR;
            break;
        }
        if (g_use_ext2) {
            ret = (uint64_t)(flags & AT_REMOVEDIR
                ? vfs_rmdir(absolute)
                : vfs_unlink(absolute));
            break;
        }
        char leaf[256];
        uint32_t saved_cwd;
        rc = fat32_parent_leaf(absolute, leaf, sizeof(leaf), &saved_cwd);
        if (rc < 0) { ret = (uint64_t)rc; break; }
        rc = flags & AT_REMOVEDIR
            ? fat32_error_to_linux(fat32_rmdir(leaf))
            : fat32_error_to_linux(fat32_delete_file(leaf));
        fat32_set_cwd(saved_cwd);
        ret = (uint64_t)rc;
        break;
    }

    case SYS_renameat:
    case 82: { /* FASE5: SYS_rename (82, 2 args) — glibc estático chama
                * ELE, não renameat! Mesma lógica, dirfds = AT_FDCWD. */
        int rd1, rd2;
        const char *rp1, *rp2;
        if (num == 82) {
            rd1 = AT_FDCWD; rp1 = (const char *)a1;
            rd2 = AT_FDCWD; rp2 = (const char *)a2;
        } else {
            rd1 = (int)a1; rp1 = (const char *)a2;
            rd2 = (int)a3; rp2 = (const char *)a4;
        }
        char old_absolute[VFS_MAX_PATH], new_absolute[VFS_MAX_PATH];
        int rc = resolve_at_path(rd1, rp1, 0,
                            old_absolute, sizeof(old_absolute));
        if (rc < 0) { ret = (uint64_t)rc; break; }
        rc = resolve_at_path(rd2, rp2, 0,
                        new_absolute, sizeof(new_absolute));
        if (rc < 0) { ret = (uint64_t)rc; break; }
        uint32_t destination_size = 0;
        uint8_t destination_attr = 0;
        rc = path_stat_metadata(new_absolute, &destination_size, &destination_attr);
        if (rc == 0) {
            ret = (uint64_t)-VFS_EEXIST;
            break;
        }
        /* FASE5: stat de destino inexistente com nome LFN longo volta
         * EPERM (walk falha) em vez de ENOENT — trata qualquer erro
         * como "ausente" e tenta mesmo assim (o create abaixo barra
         * se já existir de verdade). */
        if (g_use_ext2) {
            ret = (uint64_t)-VFS_EROFS;
            break;
        }
        uint32_t saved_cwd = fat32_get_cwd();
        char old_leaf[256], new_leaf[256];
        uint32_t old_parent, new_parent;
        rc = fat32_parent_leaf(old_absolute, old_leaf, sizeof(old_leaf), &old_parent);
        if (rc < 0) { ret = (uint64_t)rc; break; }
        old_parent = fat32_get_cwd();
        fat32_set_cwd(saved_cwd);
        rc = fat32_parent_leaf(new_absolute, new_leaf, sizeof(new_leaf), &new_parent);
        if (rc < 0) { ret = (uint64_t)rc; break; }
        new_parent = fat32_get_cwd();
        fat32_set_cwd(saved_cwd);
        if (old_parent != new_parent) {
            ret = (uint64_t)-LINUX_EXDEV;
            break;
        }
        fat32_set_cwd(old_parent);
        int rn = fat32_error_to_linux(fat32_rename(old_leaf, new_leaf));
        fat32_set_cwd(saved_cwd);
        if (rn == 0) { ret = 0; break; }
        /* FASE5: fat32_rename não lida com nomes LFN longos (TMP do
         * fontconfig tem 45+ chars) e devolve EPERM. Fallback copia +
         * apaga (mesmo ritual do SYS_link). Só FAT real; virtual e
         * ext2 mantêm o erro original. Destino já foi checado (EEXIST
         * acima), então criar aqui não sobrescreve nada. */
        if (g_use_ext2 || is_virtual_tmp_file(old_absolute) ||
            is_virtual_tmp_file(new_absolute)) { ret = (uint64_t)rn; break; }
        {
            uint32_t src_size = 0; uint8_t src_attr = 0;
            int sr = path_stat_metadata(old_absolute, &src_size, &src_attr);
            if (sr < 0) { ret = (uint64_t)sr; break; }
            char nleaf[256];
            uint32_t sv = fat32_get_cwd();
            int cr = fat32_parent_leaf(new_absolute, nleaf, sizeof(nleaf), &sv);
            if (cr < 0) { ret = (uint64_t)cr; break; }
            cr = fat32_error_to_linux(fat32_create_file(nleaf));
            fat32_set_cwd(sv);
            if (cr < 0) { ret = (uint64_t)cr; break; }
            static uint8_t rn_buf[4096];
            uint32_t off = 0;
            int err = 0;
            while (off < src_size && !err) {
                uint32_t want = src_size - off;
                if (want > sizeof(rn_buf)) want = sizeof(rn_buf);
                int got = vfs_read_at(old_absolute, rn_buf, want, off);
                if (got <= 0) { err = got < 0 ? got : -VFS_EIO; break; }
                int put = vfs_write_at(new_absolute, rn_buf, (uint32_t)got, off);
                if (put < 0) { err = put; break; }
                off += (uint32_t)got;
            }
            if (!err) {
                fat32_set_cwd(old_parent);
                err = fat32_error_to_linux(fat32_delete_file(old_leaf));
                fat32_set_cwd(saved_cwd);
            }
            ret = (uint64_t)(err ? err : 0);
        }
        break;
    }

    case SYS_link: {
        /* ~~ link(86) ~ o Xorg tranca o display com /tmp/.X0-lock ~~
         * FAT32 não tem hard link: fazemos uma CÓPIA byte a byte do
         * origem pro destino (o lock do Xorg é 11 bytes, e depois ele
         * dá unlink no temp — semântica idêntica)~ */
        char old_absolute[VFS_MAX_PATH], new_absolute[VFS_MAX_PATH];
        int rc = resolve_at_path(AT_FDCWD, (const char *)a1, 0,
                            old_absolute, sizeof(old_absolute));
        if (rc < 0) { ret = (uint64_t)rc; break; }
        rc = resolve_at_path(AT_FDCWD, (const char *)a2, 0,
                        new_absolute, sizeof(new_absolute));
        if (rc < 0) { ret = (uint64_t)rc; break; }
        int old_virt = is_virtual_tmp_file(old_absolute);
        int new_virt = is_virtual_tmp_file(new_absolute);
        uint32_t old_size = 0, probe_size = 0;
        uint8_t old_attr = 0, probe_attr = 0;
        if (old_virt) {
            int vi = virtual_tmp_index(old_absolute);
            if (vi < 0) { ret = (uint64_t)-VFS_ENOENT; break; }
            old_size = (uint32_t)g_virtual_tmp[vi].len;
        } else {
            rc = path_stat_metadata(old_absolute, &old_size, &old_attr);
            if (rc < 0) { ret = (uint64_t)rc; break; }
        }
        if (new_virt) {
            int ni = virtual_tmp_index(new_absolute);
            if (ni >= 0 && g_virtual_tmp[ni].len > 0) {
                ret = (uint64_t)-VFS_EEXIST;
                break;
            }
        } else {
            rc = path_stat_metadata(new_absolute, &probe_size, &probe_attr);
            if (rc == 0) { ret = (uint64_t)-VFS_EEXIST; break; }
            if (rc != -VFS_ENOENT) { ret = (uint64_t)rc; break; }
        }
        /* ~~ cria o destino: virtual tem mark, ext2 tem create absoluto,
         * FAT precisa do pai ~~ */
        if (new_virt) {
            if (virtual_tmp_mark(new_absolute) < 0) {
                ret = (uint64_t)-VFS_ENOMEM;
                break;
            }
        } else if (g_use_ext2) {
            rc = vfs_create_file(new_absolute);
            if (rc < 0) { ret = (uint64_t)rc; break; }
        } else {
            char leaf[256];
            uint32_t saved_cwd = fat32_get_cwd();
            rc = fat32_parent_leaf(new_absolute, leaf, sizeof(leaf), &saved_cwd);
            if (rc < 0) { ret = (uint64_t)rc; break; }
            rc = fat32_error_to_linux(fat32_create_file(leaf));
            fat32_set_cwd(saved_cwd);
            if (rc < 0) { ret = (uint64_t)rc; break; }
        }
        /* ~~ copia o conteudo em blocos de 4KB (origem pode ser virtual) ~~ */
        {
            static uint8_t link_buf[4096];
            uint32_t off = 0;
            int err = 0;
            while (off < old_size && !err) {
                uint32_t want = old_size - off;
                if (want > sizeof(link_buf)) want = sizeof(link_buf);
                int got;
                if (old_virt) {
                    got = virtual_tmp_read(virtual_tmp_index(old_absolute),
                                           (char *)link_buf, (int)want, off);
                } else {
                    got = vfs_read_at(old_absolute, link_buf, want, off);
                }
                if (got <= 0) { err = got < 0 ? got : -VFS_EIO; break; }
                int put;
                if (new_virt) {
                    put = virtual_tmp_write(virtual_tmp_index(new_absolute),
                                            (const char *)link_buf, got, off);
                } else {
                    put = vfs_write_at(new_absolute, link_buf, (uint32_t)got, off);
                }
                if (put < 0) { err = put; break; }
                off += (uint32_t)got;
            }
            if (err) {
                if (new_virt) {
                    virtual_tmp_remove(new_absolute);
                } else if (!g_use_ext2) {
                    char leaf[256];
                    uint32_t saved_cwd = fat32_get_cwd();
                    if (fat32_parent_leaf(new_absolute, leaf, sizeof(leaf),
                                          &saved_cwd) == 0)
                        fat32_delete_file(leaf);
                    fat32_set_cwd(saved_cwd);
                } else {
                    vfs_unlink(new_absolute);
                }
                ret = (uint64_t)err;
                break;
            }
        }
        ret = 0;
        break;
    }

    case SYS_readlinkat: {
        char absolute[VFS_MAX_PATH];
        int rc = resolve_at_path((int)a1, (const char *)a2, 0,
                            absolute, sizeof(absolute));
        char *buf = (char *)a3;
        size_t bufsiz = (size_t)a4;
        if (rc < 0) { ret = (uint64_t)rc; break; }
        if (bufsiz && !buf) { ret = (uint64_t)-VFS_EFAULT; break; }
        char target[VFS_MAX_PATH];
        int length = do_readlink(absolute, target, sizeof(target));
        if (length < 0) {
            uint32_t size = 0;
            uint8_t attr = 0;
            rc = path_stat_metadata(absolute, &size, &attr);
            ret = (uint64_t)(rc < 0 ? rc : -VFS_EINVAL);
            break;
        }
        size_t copied = (size_t)length < bufsiz ? (size_t)length : bufsiz;
        for (size_t index = 0; index < copied; index++) buf[index] = target[index];
        ret = copied;
        break;
    }

    case SYS_readlink: {
        /* ~~ readlink (89) ~ musl usa o syscall antigo (nao o *at!) ~~
         * O driver fbdev do Xorg faz readlink("/proc/self/fd/N") pra
         * ver se /dev/fb0 e um device PCI — se der ENOSYS ele desiste
         * e o servidor morre com "no screens found"~ rssrsrs */
        const char *path = (const char *)a1;
        char *buf = (char *)a2;
        size_t bufsiz = (size_t)a3;
        ret = do_readlink(path, buf, bufsiz);
        break;
    }

    case SYS_chmod:
    case SYS_fchmod:
        ret = 0;  /* no-op realista — FAT32 sem permissões */
        break;

    case SYS_chown:
    case SYS_fchown:
    case SYS_setuid:
    case SYS_setgid:
        ret = 0;  /* no-op realista — FAT32 sem dono */
        break;

    case SYS_statfs:
    case SYS_fstatfs: {
        /* struct statfs { f_type, f_bsize, f_blocks, f_bfree, ... } */
        uint64_t *sf = (uint64_t *)a2;
        if (!sf) { ret = -1; break; }
        uint32_t total = 131072;   /* 64MB / 512B = 131072 setores */
        uint32_t bsize = 512;
        uint32_t blocks = total / 1;
        sf[0] = 0x4d44;            /* f_type: MSDOS_SUPER_MAGIC */
        sf[1] = bsize;             /* f_bsize */
        sf[2] = blocks;            /* f_blocks */
        sf[3] = blocks - 8192;     /* f_bfree (com uma folguinha~) */
        sf[4] = blocks - 8192;     /* f_bavail */
        sf[5] = 0;                 /* f_files */
        sf[6] = 0;                 /* f_ffree */
        ret = 0;
        break;
    }

    case SYS_mmap: {
        uint64_t hint = a1;
        uint64_t len = a2;
        int prot = (int)a3;
        int flags = (int)a4;
        int mfd = (int)regs[5]; /* Linux mmap: 5o arg = fd (r8) */
        /* ~~ /dev/fb0: mapeia o framebuffer no processo ~~
         * O driver fbdev do Xorg faz mmap(..., MAP_SHARED, fd, 0).
         * Mapeamos o LFB identity (mesma ideia do vesa_init), com U/S
         * pra userland acessar. Retorna o proprio endereco do LFB. */
        if (!(flags & 0x20) && mfd >= 3 && mfd < MAX_FDS && fds[mfd].used && fds[mfd].type == FD_FB) {
            extern framebuffer_t g_fb;
            pcb_t *cur = process_current();
            if (!cur) { ret = (uint64_t)-1; break; }
            uint64_t fb_size = (uint64_t)g_fb.pitch * g_fb.height;
            fb_size = (fb_size + 0x1FFFFF) & ~0x1FFFFFULL;
            int r = pml4_map_phys(cur->pml4, g_fb.addr, g_fb.addr, fb_size, 1);
            if (r < 0) { ret = (uint64_t)-1; break; }
            serial_puts("mmap: fb mapped pid=");
            serial_puthex((uint32_t)cur->pid);
            serial_puts(" va=");
            serial_puthex((uint32_t)g_fb.addr);
            serial_puts("\r\n");
            g_xorg_fb_mapped = 1;
            /* Xorg assume fb: desliga console e cursor HW VGA */
            {
                extern int g_console_fbdev_enabled;
                extern void vesa_fill_screen(unsigned int color);
                if (g_console_fbdev_enabled) {
                    g_console_fbdev_enabled = 0;
                    serial_puts("[FB] console off for Xorg\r\n");
                }
                /* desabilita cursor HW VGA (CRTC 0x0A bit5) */
                __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0x0A), "Nd"((uint16_t)0x3D4));
                __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint16_t)0x3D5));
                serial_puts("[FB] VGA HW cursor off\r\n");
            }
            ret = g_fb.addr;
            break;
        }
        // memfd mmap MAP_SHARED shared object
        if (!(flags & 0x20) && mfd >= 3 && mfd < MAX_FDS && fds[mfd].used && fds[mfd].type == FD_MEMFD) {
            int mi = fds[mfd].memfd_idx;
            if (mi<0||mi>=MAX_MEMFD||!g_memfds[mi].used){ ret=(uint64_t)-1; break; }
            memfd_entry_t *me=&g_memfds[mi];
            uint64_t map_off2 = regs[6];
            if (map_off2 + len > me->size) { ret=(uint64_t)-1; break; }
            uint64_t addr2 = hint;
            /* FASE5-shm: passa o CANÔNICO (me->obj) — o insert apelida
             * (SHARED: mesma identidade, escritas visíveis) ou tira
             * snapshot (PRIVATE). Snapshot-temp aqui QUEBRAVA o shm:
             * cada lado apelidava sua cópia e lia zeros (tela preta).
             * Sem destroy no erro: nada foi apropriado. */
            int is_shared = (flags & 1) ? 1 : 0; // MAP_SHARED=1
            int mmflags = (is_shared ? 1 : 2) | (flags & 0x10);
            // hint handling: if hint!=0 try fixed else find
            int rr2;
            if (hint && (flags & 0x10)) {
                rr2 = vm_map_fixed((void*)process_current()->vm_map, me->obj, map_off2, addr2, len, prot, mmflags);
            } else {
                rr2 = vm_map_find((void*)process_current()->vm_map, me->obj, map_off2, &addr2, len, prot, mmflags);
            }
            if (rr2 < 0) { ret=(uint64_t)-1; break; }
            // wire pages (shared pages already allocated or demand)
            {
                pcb_t *cur2 = process_current();
                vm_map_wire((void*)cur2->vm_map, addr2, len, cur2->pml4);
            }
            ret = addr2;
            break;
        }
        uint64_t map_off = regs[6];   /* Linux mmap: 6o arg = offset (r9) */
        serial_puts("mmap: len="); serial_puthex((uint32_t)len);
        serial_puts(" prot="); serial_puthex((uint32_t)prot);
        serial_puts(" flags="); serial_puthex((uint32_t)flags);
        serial_puts(" hint="); serial_puthex((uint32_t)hint);
        serial_puts(" fd="); serial_puthex((uint32_t)(uint32_t)mfd);
        serial_puts(" off="); serial_puthex((uint32_t)map_off);
        serial_puts("\r\n");
        uint64_t addr = hint;
        int was_file = !(flags & 0x20);
        int map_fd = mfd;
        if (len < 0x1000) len = 0x1000;
        /* Fix fase7.5 bug1 (syscall.c:2580): Linux ANON=0x20 -> TipOS ANON=0x1000.
         * File-backed mantem paginas via demand-paging (vm_map_covers + PF handler);
         * forcar MAP_ANON aqui quebra o fill compartilhado e regride a tela preta. */
        if (flags & 0x20) {
            flags = (flags & ~0x20) | MAP_ANON;
        } else {
            flags = flags & ~0x20;
        }
        int r = vm_mmap(&addr, len, prot, flags);
        if (r < 0) {
            serial_puts("mmap: falhou! hint=");
            serial_puthex((uint32_t)hint);
            serial_puts(" n=");
            { extern int vm_map_count_entries(void *); serial_puthex((uint32_t)vm_map_count_entries((void *)(process_current() ? process_current()->vm_map : 0))); }
            serial_puts("\r\n");
            ret = (uint64_t)-1;
        } else {
            serial_puts("mmap->va="); serial_puthex((uint32_t)addr);
            {
                extern int vm_map_count_entries(void *);
                serial_puts(" n=");
                serial_puthex((uint32_t)vm_map_count_entries((void *)(process_current() ? process_current()->vm_map : 0)));
            }
            serial_puts("\r\n");
            /* ~~ mmap de ARQUIVO: popula as páginas com o conteúdo ~~
             * O ld.so do glibc mapeia libc.so.6 via mmap(fd); sem
             * preencher, as páginas ficam zeradas e o loader morre com
             * "failed to map segment from shared object"~ 
             * Fix iter1: trata offset nao page-aligned. Ver syscall.c:2577. */
            if (was_file && map_fd >= 3 && map_fd < MAX_FDS &&
                fds[map_fd].used && fds[map_fd].type == 0) {
                const char *fpath = fds[map_fd].path[0] ? fds[map_fd].path
                                                        : fds[map_fd].name;
                // diagnostico file-backed
                serial_puts("mmap-file: fd="); serial_puthex((uint32_t)map_fd);
                serial_puts(" off="); serial_puthex((uint32_t)map_off);
                serial_puts(" fpath='"); serial_puts(fpath); serial_puts("'\r\n");
                static uint8_t mmap_fillbuf[4096];
                uint64_t end = addr + ((len + 0xFFF) & ~0xFFFULL);
                uint64_t page_off = map_off & 0xFFFULL;
                uint64_t aligned_off = map_off & ~0xFFFULL;
                int first = 1;
                int any_copied = 0;
                for (uint64_t va = addr; va < end; va += 4096) {
                    uint64_t file_off = aligned_off + (va - addr);
                    int n = vfs_read_at(fpath, mmap_fillbuf, 4096, file_off);
                    if (n <= 0) { if (first) { serial_puts("mmap-file: primeira pagina falhou\r\n"); } break; }
                    if (n > 4096) n = 4096;
                    uint8_t *dst = (uint8_t *)va;
                    if (first && page_off) {
                        if ((uint64_t)n > page_off) {
                            int clen = n - (int)page_off;
                            for (int b = 0; b < clen; b++) dst[b] = mmap_fillbuf[page_off + b];
                            any_copied = 1;
                        }
                        first = 0;
                        page_off = 0;
                        continue;
                    }
                    first = 0;
                    for (int b = 0; b < n; b++) dst[b] = mmap_fillbuf[b];
                    any_copied = 1;
                }
                if (!any_copied && len <= 0x2000) {
                    serial_puts("mmap-file: nenhum byte copiado\r\n");
                }
            }
            ret = addr;
        }
        break;
    }

    case SYS_munmap:
        ret = vm_munmap(a1, a2);
        break;

    case SYS_mprotect:
        ret = vm_mprotect(a1, a2, (int)a3);
        break;

    case SYS_madvise:
        ret = 0;  /* no-op documentado — paginas sao wired de uma vez */
        break;

    case SYS_msync:
        ret = 0;  /* no-op — sem cache de paginas */
        break;

    case SYS_mremap: {
        uint64_t old_addr = a1;
        uint64_t old_len = a2;
        uint64_t new_len = a3;
        uint64_t flags = a4;
        uint64_t new_addr_hint = regs[5];
        const uint64_t MREMAP_MAYMOVE = 1;
        const uint64_t MREMAP_FIXED   = 2;
        if (!old_addr || !old_len || !new_len) { ret = (uint64_t)-VFS_EINVAL; break; }
        old_len = (old_len + 0xFFF) & ~0xFFFULL;
        new_len = (new_len + 0xFFF) & ~0xFFFULL;
        pcb_t *mcur = process_current();
        if (!mcur || !mcur->vm_map) { ret = (uint64_t)-VFS_EINVAL; break; }
        vm_map_t *mmap = (vm_map_t *)mcur->vm_map;
        vm_map_entry_t *e = vm_map_find_entry(mmap, old_addr);
        if (!e || e->start != old_addr || e->end != old_addr + old_len) { ret = (uint64_t)-VFS_EINVAL; break; }
        if ((e->flags & 1) && !(e->flags & 0x1000)) { ret = (uint64_t)-VFS_ENOMEM; break; }
        int prot = e->prot;
        int mflags = e->flags;
        uint64_t off = e->offset;
        vm_object_t *obj = e->object ? vm_object_share(e->object) : NULL;
        if (new_len == old_len) { ret = old_addr; if (obj) vm_object_destroy(obj); break; }
        if (new_len < old_len) {
            uint64_t tail = old_addr + new_len;
            uint64_t tail_end = old_addr + old_len;
            /* free tail pages in vm_object (com guarda: só no último dono) */
            if (e->object) {
                /* FASE5-shm: release MUTA o array — des-apelida antes
                 * p/ não zerar páginas de irmãs MAP_SHARED. Se o unalias
                 * falhar (OOM), pula o release (vaza o tail, sem panic). */
                extern struct vm_object *vm_object_unalias(struct vm_object *o);
                {
                    struct vm_object *uo = vm_object_unalias(e->object);
                    if (uo) e->object = uo;
                }
                extern int vm_object_release_page(struct vm_object *o, size_t p);
                if (e->object) for (uint64_t va = tail; va < tail_end; va += 0x1000) {
                    size_t pg = (va - e->start + off) >> 12;
                    if (pg < (size_t)e->object->page_count && e->object->pages[pg]) {
                        vm_object_release_page(e->object, pg);
                    }
                }
            }
            e->end = old_addr + new_len;
            /* clear PTEs for tail: walk PTs inline */
            {
                uint64_t pml4 = mcur->pml4;
                for (uint64_t va = tail; va < tail_end; va += 0x1000) {
                    uint64_t *p4 = (uint64_t*)(uintptr_t)pml4;
                    uint64_t p4e = p4[(va >> 39) & 0x1FF];
                    if (!(p4e & 1)) continue;
                    uint64_t *pdpt = (uint64_t*)(uintptr_t)(p4e & ~0xFFFULL);
                    uint64_t pdpte = pdpt[(va >> 30) & 0x1FF];
                    if (!(pdpte & 1) || (pdpte & 0x80)) continue;
                    uint64_t *pd = (uint64_t*)(uintptr_t)(pdpte & ~0xFFFULL);
                    uint64_t pde = pd[(va >> 21) & 0x1FF];
                    if (!(pde & 1) || (pde & 0x80)) continue;
                    uint64_t *pt = (uint64_t*)(uintptr_t)(pde & ~0xFFFULL);
                    pt[(va >> 12) & 0x1FF] = 0;
                    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
                }
            }
            if (obj) vm_object_destroy(obj);
            ret = old_addr;
            break;
        }
        if (!(flags & MREMAP_MAYMOVE)) { if (obj) vm_object_destroy(obj); ret = (uint64_t)-VFS_ENOMEM; break; }
        uint64_t after = old_addr + old_len;
        uint64_t need = new_len - old_len;
        int can_inplace = 1;
        for (vm_map_entry_t *it = mmap->head; it; it = it->next) {
            if (it->start < after + need && it->end > after) { can_inplace = 0; break; }
        }
        if (can_inplace) {
            e->end = old_addr + new_len;
            if (e->object) {
                size_t need_pages = new_len >> 12;
                if (need_pages > (size_t)e->object->page_count) {
                    size_t old_pages = e->object->page_count;
                    uint64_t *np = (uint64_t *)kcalloc(need_pages, sizeof(uint64_t));
                    if (np) {
                        for (size_t i = 0; i < old_pages; i++) np[i] = e->object->pages[i];
                        kfree(e->object->pages);
                        e->object->pages = np;
                        e->object->page_count = (int)need_pages;
                        e->object->size = need_pages << 12;
                    }
                }
            }
            vm_map_wire(mmap, after, need, mcur->pml4);
            if (obj) vm_object_destroy(obj);
            ret = old_addr;
            break;
        }
        /* MAYMOVE: new range */
        {
            uint64_t hint = (flags & MREMAP_FIXED) ? new_addr_hint : 0;
            uint64_t new_addr = hint;
            int rr;
            if (flags & MREMAP_FIXED) {
                if (!hint || (hint & 0xFFF)) { if (obj) vm_object_destroy(obj); ret = (uint64_t)-VFS_EINVAL; break; }
                rr = vm_map_fixed(mmap, obj, off, hint, new_len, prot, mflags);
                new_addr = hint;
            } else {
                rr = vm_map_find(mmap, obj, off, &new_addr, new_len, prot, mflags);
            }
            if (rr < 0) { if (obj) vm_object_destroy(obj); ret = (uint64_t)-VFS_ENOMEM; break; }
            if (obj) vm_object_destroy(obj);
            vm_map_entry_t *ne = vm_map_find_entry(mmap, new_addr);
            vm_map_wire(mmap, new_addr, new_len, mcur->pml4);
            for (size_t i = 0; i < old_len; i++) ((uint8_t*)(uintptr_t)new_addr)[i] = ((uint8_t*)(uintptr_t)old_addr)[i];
            vm_map_entry_t *oe = vm_map_find_entry(mmap, old_addr);
            if (oe && oe != ne) vm_munmap(old_addr, old_len);
            ret = new_addr;
            break;
        }
    }

    case SYS_kbhit:
        ret = keyboard_avail();
        break;

    case SYS_kbd_event: {
        extern int keyboard_event_avail(void);
        extern unsigned char keyboard_event_read(void);
        extern void keyboard_event_clear(void);
        if (a1 == 1) {
            keyboard_event_clear();
            ret = 0;
        } else {
            ret = keyboard_event_avail() ? keyboard_event_read() : 256;
        }
        break;
    }

    case SYS_getpid:
        ret = process_current_pid();
        break;

    case SYS_fsync:
    case SYS_fdatasync: {
        int fd = (int)a1;
        if (fd < 0 || fd >= MAX_FDS || !fds[fd].used) { ret = (uint64_t)-VFS_EBADF; break; }
        if (fds[fd].type != 0) { ret = 0; break; } /* tty/pipe/socket: no-op */
        const char *p = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
        ret = (uint64_t)vfs_fsync(p);
        if ((int64_t)ret < 0) ret = (uint64_t)-VFS_EIO;
        else ret = 0;
        break;
    }

    case SYS_truncate: {
        const char *path = (const char *)a1;
        uint64_t len = a2;
        if (!path || len > 0xFFFFFFFFULL) { ret = (uint64_t)-VFS_EINVAL; break; }
        char absp[VFS_MAX_PATH];
        pcb_t *tc = process_current();
        const char *cwd = tc ? tc->cwd : "/";
        int rr = vfs_abs_path(cwd, path, absp, sizeof(absp));
        if (rr < 0) { ret = (uint64_t)rr; break; }
        ret = (uint64_t)vfs_truncate(absp, (uint32_t)len);
        if ((int64_t)ret < 0 && ret != (uint64_t)-VFS_EROFS) ret = (uint64_t)-VFS_EIO;
        else if (ret == (uint64_t)-VFS_EROFS) ret = (uint64_t)-VFS_EIO;
        else ret = 0;
        break;
    }

    case SYS_ftruncate: {
        int fd = (int)a1;
        uint64_t len = a2;
        if (fd < 0 || fd >= MAX_FDS || !fds[fd].used) { ret = (uint64_t)-VFS_EBADF; break; }
        if (len > 0xFFFFFFFFULL) { ret = (uint64_t)-VFS_EINVAL; break; }
        if (fds[fd].type == FD_MEMFD) {
            int mi=fds[fd].memfd_idx; if(mi<0||mi>=MAX_MEMFD||!g_memfds[mi].used){ret=(uint64_t)-9; break;}
            memfd_entry_t *me=&g_memfds[mi]; size_t nlen=(size_t)len; nlen=(nlen+0xFFF)&~0xFFFULL; if(nlen==0) nlen=0x1000;
            if(nlen != me->size) {
                struct vm_object *nobj = vm_object_create(nlen);
                if(!nobj){ret=(uint64_t)-12; break;}
                size_t copy = me->size < nlen ? me->size : nlen;
                for(size_t i=0;i<copy;i++){ size_t pg=i>>12, po=i&0xFFF; if(pg>=(size_t)me->obj->page_count) break; uint64_t pa=me->obj->pages[pg]; if(!pa) continue; size_t npg=i>>12; uint64_t npa=nobj->pages[npg]; if(!npa){ npa=(uint64_t)(uintptr_t)mmap_user(0,4096,3,0x1000); if(!npa){vm_object_destroy(nobj); ret=(uint64_t)-12; break; } nobj->pages[npg]=npa; { extern void objref_claim(uint64_t p); objref_claim(npa); } } ((uint8_t*)(uintptr_t)npa)[po]=((uint8_t*)(uintptr_t)pa)[po]; }
                if(ret==(uint64_t)-12) { vm_object_destroy(nobj); break; }
                vm_object_destroy(me->obj); me->obj=nobj; me->size=nlen;
                fds[fd].stat_size=(uint32_t)nlen;
            }
            ret=0; break;
        }
        if (fds[fd].type != 0) { ret = (uint64_t)-VFS_EINVAL; break; }
        const char *p = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
        char absp[VFS_MAX_PATH];
        pcb_t *fc = process_current();
        const char *cwd2 = fc ? fc->cwd : "/";
        /* p já é absoluto (vfs_abs_path na open), mas normaliza por segurança */
        int rr = vfs_abs_path("/", p, absp, sizeof(absp));
        if (rr < 0) { /* fallback: usa p direto */ 
            int k = 0; while (p[k] && k < VFS_MAX_PATH-1) { absp[k]=p[k]; k++; } absp[k]=0;
        } else {
            (void)cwd2;
        }
        int tr = vfs_truncate(absp, (uint32_t)len);
        if (tr == -VFS_EROFS) tr = -VFS_EIO;
        if (tr < 0) { ret = (uint64_t)tr; break; }
        fds[fd].stat_valid = 0;
        ret = 0;
        break;
    }

    case 79: { /* getcwd (Linux 79) */
        char *buf = (char *)a1;
        size_t size = (size_t)a2;
        if (!buf) { ret = (uint64_t)-VFS_EFAULT; break; }
        if (size == 0) { ret = (uint64_t)-VFS_EINVAL; break; }
        if (size > 4096) size = 4096;
        if (!user_buffer_valid(buf, size, 1)) { ret = (uint64_t)-VFS_EFAULT; break; }
        pcb_t *cur2 = process_current();
        const char *cwd2 = (cur2 && cur2->cwd[0]) ? cur2->cwd : "/";
        size_t len2 = 0; while (cwd2[len2]) len2++;
        if (len2 + 1 > size) { ret = (uint64_t)-34; break; } /* -ERANGE */
        for (size_t i = 0; i <= len2; i++) buf[i] = cwd2[i];
        ret = (uint64_t)(len2 + 1);
        break;
    }

    case 80: { /* chdir (Linux 80) */
        const char *path = (const char *)a1;
        if (!path || !path[0]) { ret = (uint64_t)-VFS_EFAULT; break; }
        char absolute[VFS_MAX_PATH];
        int rr = vfs_abs_path(process_current() ? process_current()->cwd : "/", path, absolute, sizeof(absolute));
        if (rr < 0) { ret = (uint64_t)rr; break; }
        uint32_t sz=0; uint8_t at=0;
        int st = vfs_stat_size_attr(absolute, &sz, &at);
        if (st < 0) { ret = (uint64_t)st; break; }
        if (!(at & 0x10)) { ret = (uint64_t)-VFS_ENOTDIR; break; }
        pcb_t *cc = process_current();
        if (!cc) { ret = (uint64_t)-VFS_EINVAL; break; }
        int k=0; while (absolute[k] && k < 255) { cc->cwd[k]=absolute[k]; k++; } cc->cwd[k]=0;
        if (cc->cwd[0]==0) { cc->cwd[0]='/'; cc->cwd[1]=0; }
        ret = 0; break;
    }
    case SYS_getuid:
    case SYS_geteuid:
    case SYS_getgid:
    case SYS_getegid:
        ret = 0;
        break;

    case SYS_ioctl: {
        /* ~~ ioctl ~ "O terminal tem quantas linhas?" ~~
         * Linux ioctl codes que o st precisa:
         * TIOCGWINSZ (0x5413) → devolve struct winsize { ws_row, ws_col, ws_xpixel, ws_ypixel }
         * TCGETS (0x5401) → devolve struct termios (modo cooked básico)
         * TCSETSW (0x5403) / TCSANOW (0x5402) → só aceita e ignora (modo raw, confia~)
         * Se não conhece o código, retorna 0 (o programa tenta de novo~) */
        int fd = (int)a1;
        unsigned long req = (unsigned long)a2;
        void *argp = (void *)a3;
        serial_puts("[ioctl] fd=");
        serial_puthex((uint32_t)fd);
        serial_puts(" request=");
        serial_puthex((uint32_t)req);
        serial_puts("\r\n");
        if (fd >= 0 && fd < MAX_FDS && fds[fd].used && fds[fd].type == FD_EVDEV) {
            /* ~~ ioctls EVIOC pro libinput ~~
             * O libinput abre /dev/input/eventN e consulta identidade e
             * capabilities antes de aceitar o device. Sem estes ioctls o
             * PreInit retorna 2 e o Xorg aborta o input. Codigos (Linux):
             *   EVIOCGID   = 0x80084502  (struct input_id)
             *   EVIOCGNAME = 0x80084506  (len 8)
             *   EVIOCGPHYS = 0x80084507  (len 8)
             *   EVIOCGBIT  = 0x80084520+ev (ev=0..3)
             *   EVIOCGABS  = 0x80184540  (struct input_absinfo) */
            int is_mouse = (k_strstr(fds[fd].name, "event0") != 0);
            if (req == 0x80084502) { /* EVIOCGID */
                uint16_t *id = (uint16_t *)argp;
                if (id) { id[0]=1; id[1]=1; id[2]=1; id[3]=1; } /* bus,vendor,product,version */
                ret = 0; break;
            }
            if (req == 0x80084506) { /* EVIOCGNAME */
                char *nm = (char *)argp;
                if (nm) {
                    const char *s = is_mouse ? "TipOS Mouse" : "TipOS Keyboard";
                    int k = 0; while (s[k]) { nm[k] = s[k]; k++; }
                    nm[k] = 0;
                }
                ret = is_mouse ? 12 : 15; break;
            }
            if (req == 0x80084507) { /* EVIOCGPHYS */
                char *p = (char *)argp;
                if (p) { const char *s = "tipos/input0"; int k=0; while(s[k]){p[k]=s[k];k++;} p[k]=0; }
                ret = 13; break;
            }
            if (req == 0x80084520) { /* EVIOCGBIT(0) ~ EV types */
                uint8_t *b = (uint8_t *)argp;
                if (b) { b[0] = is_mouse ? 0x07 : 0x03; } /* mouse: EV_SYN|EV_KEY|EV_REL, kbd: EV_SYN|EV_KEY */
                ret = 1; break;
            }
            if (req == 0x80084521) { /* EVIOCGBIT(1) ~ EV_KEY codes */
                uint8_t *b = (uint8_t *)argp;
                if (b) { for (int i=0;i<96;i++) b[i]=0xFF; } /* aceita keycodes */
                ret = 96; break;
            }
            if (req == 0x80084522) { /* EVIOCGBIT(2) ~ EV_REL codes */
                uint8_t *b = (uint8_t *)argp;
                if (b) { b[0] = is_mouse ? 0x0F : 0x00; } /* REL_X/Y/WHEEL */
                ret = 1; break;
            }
            if (req == 0x80084523) { /* EVIOCGBIT(3) ~ EV_ABS codes */
                uint8_t *b = (uint8_t *)argp;
                if (b) b[0] = 0;
                ret = 1; break;
            }
            if (req == 0x80184540) { /* EVIOCGABS(0) ~ struct input_absinfo */
                int32_t *a = (int32_t *)argp;
                if (a) { a[0]=0; a[1]=0; a[2]=0; a[3]=0; a[4]=0; a[5]=0; }
                ret = 0; break;
            }
            /* default: aceita e ignora, como o resto do ioctl~ */
            ret = 0; break;
        }
        if (fd >= 0 && fd < MAX_FDS && fds[fd].used &&
            (fds[fd].type == FD_TTY_MASTER || fds[fd].type == FD_TTY_SLAVE)) {
            size_t arg_size = 0;
            int arg_writable = 0;
            if (req == TTY_TCGETS) {
                arg_size = sizeof(tty_termios_t);
                arg_writable = 1;
            } else if (req == TTY_TCSETS || req == TTY_TCSETSW ||
                       req == TTY_TCSETSF) {
                arg_size = sizeof(tty_termios_t);
            } else if (req == TTY_TIOCGWINSZ) {
                arg_size = sizeof(tty_winsize_t);
            } else if (req == TTY_TIOCSPGRP) {
                arg_size = sizeof(int32_t);
            } else if (req == TTY_TIOCGPGRP) {
                arg_size = sizeof(int32_t);
                arg_writable = 1;
            } else if (req == TTY_TIOCSWINSZ) {
                arg_size = sizeof(tty_winsize_t);
            } else if (req == TTY_TIOCGPTN) {
                arg_size = sizeof(int32_t);
                arg_writable = 1;
            } else if (req == TTY_TIOCSPTLCK) {
                arg_size = sizeof(int32_t);
            }
            if (arg_size && !user_buffer_valid(argp, arg_size, arg_writable)) {
                ret = (uint64_t)-VFS_EFAULT;
                break;
            }
            if (req == TTY_TIOCSCTTY) {
                pcb_t *current = process_current();
                tty_t *controlling = fds[fd].tty;
                if (controlling->flags & TTY_FLAG_MASTER) {
                    int index = tty_pair_index(controlling);
                    tty_pair_t *pair = index >= 0
                        ? tty_pair_get((uint32_t)index) : NULL;
                    if (!pair || !pair->slave) {
                        ret = (uint64_t)-VFS_EIO;
                        break;
                    }
                    controlling = pair->slave;
                }
                if (current) current->controlling_tty = controlling;
                /* FASE6-fg: põe o chamador em foreground (o handler
                 * antigo em tty.c nunca era alcançado daqui). Sem fg,
                 * VINTR/SIGINT nunca disparava e o dash travava. */
                if (current) controlling->foreground_pgrp = current->pid;
                ret = 0;
                break;
            }
            if (req == TTY_TIOCNOTTY) {
                pcb_t *current = process_current();
                if (current) current->controlling_tty = NULL;
                ret = 0;
                break;
            }
            ret = (uint64_t)tty_ioctl(fds[fd].tty, req, argp);
            break;
        }
        /* ~~ Linux VT/console ioctls (pro Xorg) ~~
         * O Xorg abre /dev/tty0 e /dev/ttyN e espera a maquina VT do
         * Linux. Aqui aceitamos tudo e devolvemos valores plausiveis
         * (nao temos VT de verdade no TipOS~) */
        if (req == 0x5604) { /* VT_OPENQRY -> devolve um VT livre */
            int *v = (int *)argp;
            if (v) *v = 1;  /* tty1, sempre livre~ */
            ret = 0; break;
        }
        if (req == 0x5603) { /* VT_GETSTATE -> struct vt_stat {v_active,v_signal,v_state; int v_mode} */
            unsigned short *vs = (unsigned short *)argp;
            if (vs) { vs[0] = 1; vs[1] = 0; vs[2] = 0; }
            ret = 0; break;
        }
        if (req == 0x5601) { /* VT_GETMODE -> struct vt_mode {char mode,waitv,relsig,acqsig,frsig; int frsig_pending} */
            unsigned char *vm = (unsigned char *)argp;
            if (vm) { vm[0] = 0; vm[1] = 0; vm[2] = 0; vm[3] = 0; vm[4] = 0; }
            ret = 0; break;
        }
        if (req == 0x5602 || req == 0x5606 || req == 0x5607) { /* VT_SETMODE / VT_ACTIVATE / VT_WAITACTIVE */
            ret = 0; break;
        }
        if (req == 0x4B3A || req == 0x4B3B) { /* KDSETMODE / KDGETMODE */
            serial_puts("[KDSETMODE] arg=0x"); serial_puthex((uint32_t)(uintptr_t)argp);
            serial_puts(" fd="); serial_puthex((uint32_t)fd);
            serial_puts("\r\n");
            /* ~~ KDSETMODE: o Xorg chama com arg=1 (KD_GRAPHICS) pra
             * assumir o fbdev; com arg=0 (KD_TEXT) pra devolver o fb
             * ao console. Usamos g_console_fbdev_enabled como "modo
             * texto ativo" e limpamos a tela antes de entregar. ~~ */
            extern int g_console_fbdev_enabled;
            extern void vesa_fill_screen(unsigned int color);
            if (req == 0x4B3B) {           /* KDGETMODE */
                int *m = (int *)argp;
                if (m) *m = g_console_fbdev_enabled ? 0 : 1;
                ret = 0; break;
            }
            /* KDSETMODE */
            if ((uint64_t)(uintptr_t)argp == 1) {  /* KD_GRAPHICS */
                if (g_console_fbdev_enabled) {
                    vesa_fill_screen(0x000000);
                    g_console_fbdev_enabled = 0;
                    serial_puts("[KD] GRAPHICS\r\n");
                }
            } else {                        /* KD_TEXT */
                if (!g_console_fbdev_enabled) {
                    g_console_fbdev_enabled = 1;
                    serial_puts("[KD] TEXT\r\n");
                }
            }
            ret = 0; break;
        }
        if (req == 0x4B44) { /* KDGKBMODE -> devolve K_RAW (1) */
            int *m = (int *)argp;
            if (m) *m = 1;
            ret = 0; break;
        }
        if (req == 0x4B45) { /* KDSKBMODE -> aceita qualquer modo */
            ret = 0; break;
        }
        if (req == 0x4B31 || req == 0x4B32) { /* KDGETLED / KDSETLED */
            if (req == 0x4B31) { int *l = (int *)argp; if (l) *l = 0; }
            ret = 0; break;
        }
        if (req == 0x4B46) { /* KDSKBENT ~ set keycode->keysym entry
             * struct kbkeycode { unsigned int kb_table, kb_index, kb_value; }
             * O driver kbd do Xorg chama isso em loop no setup do keymap.
             * Aceita e descarta: o mapping real e feito do lado do userspace. */
            ret = 0; break;
        }
        if (req == 0x4B47) { /* KDGKBENT ~ le entry do keymap */
            unsigned int *kk = (unsigned int *)argp;
            if (kk) kk[2] = 0; /* kb_value = 0 */
            ret = 0; break;
        }
        if (req == 0x4B48) { /* KDSKBSENT ~ set keybinding string */
            ret = 0; break;
        }
        if (req == 0x4B49) { /* KDGKBSENT ~ le keybinding string (0 chars) */
            ret = 0; break;
        }
        if (req == 0x4B4A) { /* KDGKBDIACR ~ le acentos mortos */
            ret = 0; break;
        }
        if (req == 0x4B4B) { /* KDSKBDIACR ~ set acentos mortos */
            ret = 0; break;
        }
        if (req == 0x4600) { /* FBIOGET_VSCREENINFO (pro driver fbdev do Xorg) */
            extern framebuffer_t g_fb;
            uint32_t *v = (uint32_t *)argp;
            if (!v) { ret = -1; break; }
            uint32_t W = g_fb.width, H = g_fb.height;
            /* ~~ Timings estilo VESA 1024x768@60 escalados pela resolucao ~~
             * Antes voltava tudo zerado: o modo "current" do driver fbdev
             * falhava na validacao do xf86 e o Xorg dava free() no nome
             * estatico dele ("current" em .rodata!) — crash no malloc~
             * Com timing de verdade o modo valida e ninguem e liberado~ */
            uint32_t lm = W * 24 / 1024, rm = W * 160 / 1024,
                     hs = W * 136 / 1024, um = H * 3 / 768,
                     bmm = H * 29 / 768, vs = H * 6 / 768;
            if (!lm) lm = 24; if (!rm) rm = 160; if (!hs) hs = 136;
            if (!um) um = 3; if (!bmm) bmm = 29; if (!vs) vs = 6;
            uint32_t htotal = W + lm + rm + hs;
            uint32_t vtotal = H + um + bmm + vs;
            uint64_t pixclk = 1000000000000ULL / (60ULL * htotal * vtotal);
            v[0]  = W;                       /* xres */
            v[1]  = H;                       /* yres */
            v[2]  = W;                       /* xres_virtual */
            v[3]  = H;                       /* yres_virtual */
            v[4]  = 0;                       /* xoffset */
            v[5]  = 0;                       /* yoffset */
            v[6]  = 32;                      /* bits_per_pixel — 32bpp TrueColor 8-8-8 (VESA é 32bpp; 16 mente pro fbdev e força shadow) */
            v[7]  = 0;                       /* grayscale */
            /* fb_bitfield {offset, length, msb_right} — 32bpp 8-8-8 */
            v[8]  = 16; v[9]  = 8;  v[10] = 0;  /* red */
            v[11] = 8;  v[12] = 8;  v[13] = 0;  /* green */
            v[14] = 0;  v[15] = 8;  v[16] = 0;  /* blue */
            v[17] = 24; v[18] = 8;  v[19] = 0;  /* transp */
            v[20] = 0;                       /* nonstd */
            v[21] = 0;                       /* activate */
            v[22] = W * 304 / 1024;          /* height (mm) */
            v[23] = H * 228 / 768;           /* width (mm) */
            v[24] = 0;                       /* accel_flags */
            v[25] = (uint32_t)pixclk;        /* pixclock (ps) */
            v[26] = lm; v[27] = rm;          /* left/right margin */
            v[28] = um; v[29] = bmm;         /* upper/lower margin */
            v[30] = hs; v[31] = vs;          /* hsync/vsync len */
            v[32] = 0; v[33] = 0;            /* sync, vmode=NONINTERLACED */
            v[34] = 0;                       /* rotate */
            v[35] = 0;                       /* colorspace */
            ret = 0; break;
        }
        if (req == 0x4601) { /* FBIOPUT_VSCREENINFO -> aceita e ignora */
            ret = 0; break;
        }
        if (req == 0x4602) { /* FBIOGET_FSCREENINFO (pro driver fbdev do Xorg) */
            extern framebuffer_t g_fb;
            uint8_t *f = (uint8_t *)argp;
            if (!f) { ret = -1; break; }
            for (int i = 0; i < 16; i++) f[i] = 0;
            const char *id = "TipOS fb";
            for (int i = 0; i < 16 && id[i]; i++) f[i] = (uint8_t)id[i];
            *(uint64_t *)(f + 16) = g_fb.addr;        /* smem_start */
            *(uint32_t *)(f + 24) = g_fb.pitch * g_fb.height; /* smem_len */
            *(uint32_t *)(f + 28) = 0;                /* type = FB_TYPE_PACKED_PIXELS */
            *(uint32_t *)(f + 32) = 0;                /* type_aux */
            *(uint32_t *)(f + 36) = 2;                /* visual = FB_VISUAL_TRUECOLOR */
            *(uint16_t *)(f + 40) = 0;                /* xpanstep */
            *(uint16_t *)(f + 42) = 0;                /* ypanstep */
            *(uint16_t *)(f + 44) = 0;                /* ywrapstep */
            // line_length offset is 48 (2-byte pad after ywrapstep on 64-bit)
            *(uint16_t *)(f + 46) = 0;                /* pad */
            *(uint32_t *)(f + 48) = g_fb.pitch;       /* line_length */
            *(uint64_t *)(f + 56) = 0;                /* mmio_start */
            *(uint32_t *)(f + 64) = 0;                /* mmio_len */
            *(uint32_t *)(f + 68) = 0;                /* accel */
            *(uint16_t *)(f + 72) = 0;                /* capabilities */
            ret = 0; break;
        }
        if (req == 0x5413) { /* TIOCGWINSZ */
            /* struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; } */
            unsigned short *ws = (unsigned short *)argp;
            if (ws) {
                ws[0] = 45; /* ws_row — 45 linhas ~ cabe mais que VGA! */
                ws[1] = 80; /* ws_col — 80 colunas, padrao~ */
                ws[2] = 640; /* ws_xpixel */
                ws[3] = 720; /* ws_ypixel */
            }
            ret = 0; break;
        }
        if (req == 0x5401) { /* TCGETS */
            /* ~~ struct termios do kernel Linux = 4*u32 flags + c_line
             * + c_cc[19] = 36 bytes EXATOS (é o struct __kernel_termios
             * que o tcgetattr do glibc usa)~
             * OTCGETS antigo escrevia 11 uints = 44 bytes: o tcgetattr
             * do glibc reserva 0x38 de frame e guarda o STACK CANÁRIO em
             * rsp+0x28 (offset 40) — escrevíamos 4 bytes de VMIN POR CIMA
             * do canário (0x00000001 no lugar de 0xBEEFCA00) →
             * *** stack smashing detected *** → exit 134 em TODO
             * programa glibc dinâmico (HELLO_DYN, Xorg, xterm...). ~~ */
            uint8_t *t = (uint8_t *)argp;
            if (t) {
                uint32_t flags[4] = { 0x000005FD, 0, 0, 0 }; /* iflag oflag cflag lflag */
                for (int i = 0; i < 4; i++) *(uint32_t *)(t + i * 4) = flags[i];
                t[16] = 0;                    /* c_line */
                for (int i = 0; i < 19; i++) t[17 + i] = 0;   /* c_cc[0..18] */
                t[17 + 0] = 3;                /* VINTR  = ^C */
                t[17 + 3] = 0x7F;             /* VKILL */
                t[17 + 6] = 1;                /* VMIN  = 1 */
            }
            ret = 0; break;
        }
        if (req == 0x5402 || req == 0x5403 || req == 0x5404) { /* TCSANOW / TCSETSW / TCSETSF */
            ret = 0; break;
        }
        ret = 0;
        break;
    }

    case SYS_poll: {
        struct tipos_pollfd { int fd; short events; short revents; } *pollfds = (void *)a1;
        int nfds = (int)a2;
        int timeout = (int)a3;
        if (nfds <= 0) { ret = 0; break; }
        if (nfds > MAX_FDS || !pollfds) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        if (timeout < -1) { ret = (uint64_t)-VFS_EINVAL; break; }
        uint64_t deadline = timeout > 0
            ? ticks_after_milliseconds((uint64_t)timeout) : timer_ticks;
        for (;;) {
            int ready = 0;
            for (int i = 0; i < nfds; i++) {
                pollfds[i].revents = fd_readiness(pollfds[i].fd,
                                                   pollfds[i].events);
                if (pollfds[i].revents) ready++;
            }
            /* ~~ debug fase 7: o que o poll do fvwm/xterm viu ~~ */
            {
                extern int g_tipos_trace_pid;
                pcb_t *tpp = process_current();
                static int poll_dbg_left = 600;
                if (ready && poll_dbg_left > 0 && g_tipos_trace_pid > 0 &&
                    tpp && tpp->pid == g_tipos_trace_pid) {
                    poll_dbg_left--;
                    serial_puts("[poll] pid="); serial_puthex((uint32_t)tpp->pid);
                    for (int i = 0; i < nfds && i < 8; i++) {
                        serial_puts(" fd="); serial_puthex((uint32_t)pollfds[i].fd);
                        serial_puts(" ev="); serial_puthex((uint32_t)(uint16_t)pollfds[i].events);
                        serial_puts(" rev="); serial_puthex((uint32_t)(uint16_t)pollfds[i].revents);
                        serial_puts(" ty="); serial_puthex((uint32_t)((pollfds[i].fd >= 3 && pollfds[i].fd < MAX_FDS) ? fds[pollfds[i].fd].type : 99));
                        serial_puts(" us="); serial_puthex((uint32_t)((pollfds[i].fd >= 0 && pollfds[i].fd < MAX_FDS) ? fds[pollfds[i].fd].used : 0));
                        if (pollfds[i].fd >= 3 && pollfds[i].fd < MAX_FDS &&
                            fds[pollfds[i].fd].used && fds[pollfds[i].fd].type == 4) {
                            int ps = fds[pollfds[i].fd].sock_idx;
                            serial_puts(" s="); serial_puthex((uint32_t)ps);
                            if (ps >= 0 && ps < MAX_SOCKS) {
                                serial_puts(" r="); serial_puthex(socks[ps].rpos);
                                serial_puts(" w="); serial_puthex(socks[ps].wpos);
                                serial_puts(" peer="); serial_puthex((uint32_t)socks[ps].peer);
                                serial_puts(" pused=");
                                serial_puthex((socks[ps].peer >= 0 && socks[ps].peer < MAX_SOCKS &&
                                               socks[socks[ps].peer].used) ? 1 : 0);
                            }
                        }
                        serial_puts("\r\n");
                    }
                }
            }
            if (ready || timeout == 0 ||
                (timeout > 0 && timer_ticks >= deadline)) {
                ret = ready;
                break;
            }
            schedule();
        }
        break;
    }

    /* ~~ select (23) ~ igual poll, mas com fd_set ~~
     * args Linux: rdi=nfds, rsi=readfds, rdx=writefds, rcx=exceptfds
     * fd_set = 1024 bits (128 bytes), FD_SETSIZE=1024 */
    case SYS_select: {
        int nfds = (int)a1;
        uint8_t *rd_set = (uint8_t *)a2;
        uint8_t *wr_set = (uint8_t *)a3;
        uint8_t *ex_set = (uint8_t *)a4;
        struct tipos_timeval { uint64_t tv_sec; uint64_t tv_usec; } *timeout = (void *)regs[5];
        if (nfds < 0 || nfds > MAX_FDS) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        if (timeout && timeout->tv_usec >= 1000000ULL) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        uint64_t deadline = timer_ticks;
        if (timeout) {
            uint64_t milliseconds = timeout->tv_sec * 1000ULL +
                                    timeout->tv_usec / 1000ULL;
            deadline = ticks_after_milliseconds(milliseconds);
        }
        for (;;) {
            int ready = 0;
            for (int fd = 0; fd < nfds; fd++) {
                uint8_t mask = (uint8_t)(1U << (fd & 7));
                uint8_t *rd_byte = rd_set ? &rd_set[fd >> 3] : NULL;
                uint8_t *wr_byte = wr_set ? &wr_set[fd >> 3] : NULL;
                uint8_t *ex_byte = ex_set ? &ex_set[fd >> 3] : NULL;
                int wants_read = rd_byte && (*rd_byte & mask);
                int wants_write = wr_byte && (*wr_byte & mask);
                int wants_except = ex_byte && (*ex_byte & mask);
                short readiness = fd_readiness(fd, POLLIN | POLLOUT | POLLERR);
                int fd_ready = 0;
                if (wants_read && !(readiness & POLLIN)) *rd_byte &= (uint8_t)~mask;
                else if (wants_read) { fd_ready = 1; }
                if (wants_write && !(readiness & POLLOUT)) *wr_byte &= (uint8_t)~mask;
                else if (wants_write) { fd_ready = 1; }
                if (wants_except && !(readiness & (POLLERR | POLLHUP))) *ex_byte &= (uint8_t)~mask;
                else if (wants_except) { fd_ready = 1; }
                if (fd_ready) ready++;
            }
            if (ready || (timeout && timer_ticks >= deadline)) {
                ret = ready;
                break;
            }
            schedule();
        }
        break;
    }

    /* ~~ pselect6 (270) ~ select com sigmask (xterm usa com WINCH bloqueado) ~~ */
    case 270: {
        // Linux pselect6: rdi=nfds, rsi=readfds, rdx=writefds, rcx=exceptfds,
        // r8=timespec*, r9=sigmask* (via stacked args: regs[6]=sigmask).
        // TipOS select ja tem o loop com deadline; aqui apenas delegamos
        // para a mesma logica, ignorando sigmask (ja tratado via sigprocmask).
        // Reusa select: mesma assinatura + timeout em timespec (sec,nsec) vs timeval.
        int nfds = (int)a1;
        uint8_t *rd_set = (uint8_t *)a2;
        uint8_t *wr_set = (uint8_t *)a3;
        uint8_t *ex_set = (uint8_t *)a4;
        uint64_t *tsp = (uint64_t *)regs[5]; /* r8 = timespec {sec, nsec} */
        if (nfds < 0 || nfds > MAX_FDS) { ret = (uint64_t)-VFS_EINVAL; break; }
        uint64_t deadline = timer_ticks;
        int has_timeout = 0;
        if (tsp) {
            // timespec nsec pode ser NULL-timeout (tsp==0 ja checado)
            uint64_t sec = tsp[0];
            uint64_t nsec = tsp[1];
            if (nsec >= 1000000000ULL) { ret = (uint64_t)-VFS_EINVAL; break; }
            uint64_t ms = sec * 1000ULL + nsec / 1000000ULL;
            deadline = ticks_after_milliseconds(ms);
            has_timeout = 1;
        }
        for (;;) {
            int ready = 0;
            for (int fd = 0; fd < nfds; fd++) {
                uint8_t mask = (uint8_t)(1U << (fd & 7));
                uint8_t *rd_byte = rd_set ? &rd_set[fd >> 3] : NULL;
                uint8_t *wr_byte = wr_set ? &wr_set[fd >> 3] : NULL;
                uint8_t *ex_byte = ex_set ? &ex_set[fd >> 3] : NULL;
                int wants_read = rd_byte && (*rd_byte & mask);
                int wants_write = wr_byte && (*wr_byte & mask);
                int wants_except = ex_byte && (*ex_byte & mask);
                short readiness = fd_readiness(fd, POLLIN | POLLOUT | POLLERR);
                int fd_ready = 0;
                if (wants_read && !(readiness & POLLIN)) *rd_byte &= (uint8_t)~mask;
                else if (wants_read) fd_ready = 1;
                if (wants_write && !(readiness & POLLOUT)) *wr_byte &= (uint8_t)~mask;
                else if (wants_write) fd_ready = 1;
                if (wants_except && !(readiness & (POLLERR | POLLHUP))) *ex_byte &= (uint8_t)~mask;
                else if (wants_except) fd_ready = 1;
                if (fd_ready) ready++;
            }
            if (ready || (has_timeout && timer_ticks >= deadline)) { ret = ready; break; }
            schedule();
        }
        break;
    }

    /* ~~ ppoll (271) ~ poll com signal mask ~~
     * struct pollfd* + nfds + timespec* + sigset_t* + sizet
     * ignora a máscara (não temos sinais de verdade) */
    case 271: {
        uint64_t *fds_arr = (uint64_t *)a1;
        int nfds = (int)a2;
        int ready = 0;
        for (int i = 0; i < nfds && i < 16; i++) {
            int pfd_fd = *((int *)fds_arr + i * 2);
            short *revents = (short *)fds_arr + i * 4 + 3; /* offset 8i+6 */
            *revents = 0;
            if (pfd_fd == 0 && keyboard_avail()) {
                *revents = 1;
                ready++;
            }
            if (pfd_fd >= 3 && pfd_fd < MAX_FDS && fds[pfd_fd].used && fds[pfd_fd].type == 1 && fds[pfd_fd].counter > 0) {
                *revents = 1;
                ready++;
            }
            if (fd_readable(pfd_fd)) {
                *revents = 1;
                ready++;
            }
        }
        ret = ready;
        break;
    }

    /* ~~ eventfd2 (290) ~ contador 8 bytes, leitura consuma ~~
     * cria um fd virtual que só conta — suficiente pro
     * wayland/event loop avisar "tem coisa pra processar" */
    case 290: {
        if (a1 == 0 || a1 == 1) { /* eventfd_create(initval, flags) */
            int fd = alloc_fd();
            if (fd < 0) { ret = -1; break; }
            fds[fd].used = 1;
            fds[fd].type = 1;
            fds[fd].counter = a1; /* initval */
            ret = fd;
        } else {
            ret = -1;
        }
        break;
    }

    /* ~~ timerfd_create (283) ~~
     * clockid=1 (CLOCK_MONOTONIC), retorna fd */
    case 283: {
        int fd = alloc_fd();
        if (fd < 0) { ret = -1; break; }
        fds[fd].used = 1;
        fds[fd].type = 2;
        fds[fd].counter = 0;
        fds[fd].tfd_expiry_ms = 0;    /* FASE5: nasce desarmado */
        fds[fd].tfd_interval_ms = 0;
        /* FASE5: honra TFD_NONBLOCK/TFD_CLOEXEC (= O_NONBLOCK/O_CLOEXEC) */
        fds[fd].flags = (int)((uint64_t)a2 & (O_NONBLOCK | O_CLOEXEC));
        ret = fd;
        break;
    }

    case 319: { // memfd_create
        const char *name = (const char*)a1;
        unsigned int flags = (unsigned int)a2;
        // find slot
        int slot=-1; for(int i=0;i<MAX_MEMFD;i++) if(!g_memfds[i].used){slot=i; break;}
        if (slot < 0) {
            /* FASE6-poolchurn: sem slot livre, tenta reciclar mortos */
            extern void memfd_gc(void);
            memfd_gc();
            for(int i=0;i<MAX_MEMFD;i++) if(!g_memfds[i].used){slot=i; break;}
        }
        if(slot<0){ ret=(uint64_t)-12; break; }
        size_t init = 0x1000;
        struct vm_object *obj = vm_object_create(init);
        if(!obj){ ret=(uint64_t)-12; break; }
        g_memfds[slot].used=1; g_memfds[slot].obj=obj; g_memfds[slot].size=init;
        int ii=0; if(name){ while(name[ii]&&ii<63){g_memfds[slot].name[ii]=name[ii]; ii++;} } g_memfds[slot].name[ii]=0;
        int fd3=alloc_fd(); if(fd3<0){ vm_object_destroy(obj); g_memfds[slot].used=0; ret=(uint64_t)-12; break;}
        fds[fd3].used=1; fds[fd3].type=FD_MEMFD; fds[fd3].memfd_idx=slot; fds[fd3].pos=0;
        fds[fd3].flags = (flags & MFD_CLOEXEC) ? O_CLOEXEC : 0;
        int k3=0; if(name){ while(name[k3]&&k3<255){fds[fd3].name[k3]=name[k3]; k3++;} } fds[fd3].name[k3]=0;
        fds[fd3].path[0]=0;
        fds[fd3].stat_valid=1; fds[fd3].stat_size=(uint32_t)init; fds[fd3].stat_attr=0x20; fds[fd3].stat_ino=stat_inode_for_path(name?name:"memfd");
        ret=fd3; break;
    }

    /* ~~ timerfd_settime (286) ~ armazena intervalo (ms) ~~
     * itimerspec: {it_interval{sec,nsec}, it_value{sec,nsec}} */
    case 286: {
        int fd = (int)a1;
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 2) {
            uint64_t *spec = (uint64_t *)a3; /* new_value */
            uint64_t ival_ms = 0, val_ms = 0;
            if (spec) {
                ival_ms = spec[0] * 1000 + spec[1] / 1000000ULL; /* interval */
                val_ms  = spec[2] * 1000 + spec[3] / 1000000ULL; /* value */
            }
            /* FASE5: value==0 desarma (antes: qualquer settime armava
             * com "1" — o disarm do foot (stop_repeater) não valia). */
            extern volatile uint64_t timer_ticks;
            fds[fd].counter = val_ms ? val_ms : ival_ms;
            fds[fd].tfd_interval_ms = ival_ms;
            fds[fd].tfd_expiry_ms = val_ms ? timer_ticks * 10ULL + val_ms : 0;
        }
        ret = 0;
        break;
    }

    /* ~~ timerfd_gettime (287) ~ devolve o que sobrou ~~
     * sem timer real: devolve o intervalo como "restante" */
    case 287: {
        int fd = (int)a1;
        if (fd >= 3 && fd < MAX_FDS && fds[fd].used && fds[fd].type == 2 && a2) {
            /* FASE5: restante real (expiry - now) + intervalo. */
            extern volatile uint64_t timer_ticks;
            uint64_t now_ms = timer_ticks * 10ULL;
            uint64_t rem = (fds[fd].tfd_expiry_ms && fds[fd].tfd_expiry_ms > now_ms)
                         ? fds[fd].tfd_expiry_ms - now_ms : 0;
            uint64_t *spec = (uint64_t *)a2;
            spec[0] = fds[fd].tfd_interval_ms / 1000;                 /* interval */
            spec[1] = (fds[fd].tfd_interval_ms % 1000) * 1000000ULL;
            spec[2] = rem / 1000;                                    /* value */
            spec[3] = (rem % 1000) * 1000000ULL;
        }
        ret = 0;
        break;
    }

    /* ~~ epoll_create (213) / epoll_create1 (291) ~~
     * retorna um fd fake; ctl/wait são no-ops */
    case 213:
    case 291: {
        int epoll_index = -1;
        for (int index = 0; index < MAX_EPOLL; index++) {
            if (!epolls[index].used) { epoll_index = index; break; }
        }
        if (epoll_index < 0) { ret = (uint64_t)-VFS_ENOSPC; break; }
        int fd = alloc_fd();
        if (fd < 0) { ret = -1; break; }
        epolls[epoll_index].used = 1;
        for (int index = 0; index < MAX_FDS; index++)
            epolls[epoll_index].watches[index].used = 0;
        fds[fd].used = 1;
        fds[fd].type = FD_EPOLL;
        fds[fd].sock_idx = epoll_index;
        fds[fd].name[0] = '\0';
        ret = fd;
        break;
    }

    case 233: { /* epoll_ctl */
        int epfd = (int)a1;
        int operation = (int)a2;
        int target_fd = (int)a3;
        linux_epoll_event_t *event = (linux_epoll_event_t *)(uintptr_t)a4;
        if (epfd < 3 || epfd >= MAX_FDS || !fds[epfd].used ||
            fds[epfd].type != FD_EPOLL || target_fd < 0 ||
            target_fd >= MAX_FDS || !fds[target_fd].used) {
            ret = (uint64_t)-VFS_EBADF;
            break;
        }
        int epoll_index = fds[epfd].sock_idx;
        if (epoll_index < 0 || epoll_index >= MAX_EPOLL ||
            !epolls[epoll_index].used) {
            ret = (uint64_t)-VFS_EBADF;
            break;
        }
        epoll_watch_t *watch = &epolls[epoll_index].watches[target_fd];
        /* DBG-TEMP crash dispatch: loga ADD/DEL/MOD (baixo volume) */
        {
            extern void serial_puts(const char *s);
            extern void serial_puthex(uint32_t v);
            extern int process_current_pid(void);
            serial_puts("[epctl] pid="); serial_puthex((uint32_t)process_current_pid());
            serial_puts(" op="); serial_puthex((uint32_t)operation);
            serial_puts(" tfd="); serial_puthex((uint32_t)target_fd);
            serial_puts(" ev="); serial_puthex(event ? event->events : 99);
            serial_puts(" data="); serial_puthex(event ? (uint32_t)(event->data >> 32) : 99);
            serial_puthex(event ? (uint32_t)event->data : 99);
            serial_puts("\r\n");
        }
        if (operation == 2) {
            if (!watch->used) { ret = (uint64_t)-VFS_ENOENT; break; }
            watch->used = 0;
            ret = 0;
            break;
        }
        if ((operation != 1 && operation != 3) || !event) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        if (operation == 1 && watch->used) {
            ret = (uint64_t)-VFS_EEXIST;
            break;
        }
        watch->used = 1;
        watch->target_fd = target_fd;
        watch->events = event->events;
        watch->data = event->data;
        ret = 0;
        break;
    }

    case 232: /* epoll_wait */
    case 281: { /* epoll_pwait */
        int epfd = (int)a1;
        linux_epoll_event_t *events = (linux_epoll_event_t *)(uintptr_t)a2;
        int maxevents = (int)a3;
        int timeout = (int)a4;
        if (epfd < 3 || epfd >= MAX_FDS || !fds[epfd].used ||
            fds[epfd].type != FD_EPOLL || !events || maxevents <= 0) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        int epoll_index = fds[epfd].sock_idx;
        if (epoll_index < 0 || epoll_index >= MAX_EPOLL ||
            !epolls[epoll_index].used) {
            ret = (uint64_t)-VFS_EBADF;
            break;
        }
        uint64_t deadline = timeout >= 0
            ? ticks_after_milliseconds((uint64_t)timeout) : UINT64_MAX;
        for (;;) {
            int ready = 0;
            for (int watch_index = 0;
                 watch_index < MAX_FDS && ready < maxevents; watch_index++) {
                epoll_watch_t *watch = &epolls[epoll_index].watches[watch_index];
                if (!watch->used) continue;
                short state = fd_readiness(watch->target_fd,
                                           (short)(watch->events | POLLERR | POLLHUP));
                if (!state) continue;
                events[ready].events = (uint32_t)state;
                events[ready].data = watch->data;
                ready++;
            }
            if (ready || timeout == 0 || timer_ticks >= deadline) {
                ret = (uint64_t)ready;
                /* DBG-TEMP: só anomalia (count > n° plausível de watches
                 * ou data selvagem) — prova se o kernel entrega lixo */
                if (ready > 8) {
                    extern void serial_puts(const char *s);
                    extern void serial_puthex(uint32_t v);
                    serial_puts("[epwait-WILD] n=");
                    serial_puthex((uint32_t)ready);
                    serial_puts("\r\n");
                } else {
                    for (int wi = 0; wi < ready; wi++) {
                        uint64_t dd = events[wi].data;
                        if (dd && dd < 0x10000ULL) {
                            extern void serial_puts(const char *s);
                            extern void serial_puthex(uint32_t v);
                            serial_puts("[epwait-WILD] data=");
                            serial_puthex((uint32_t)dd);
                            serial_puts("\r\n");
                        }
                    }
                }
                break;
            }
            schedule();
        }
        break;
    }

    case SYS_sigaction:
        ret = signal_sigaction((int)a1,
            (const tipos_sigaction_t *)(uintptr_t)a2,
            (tipos_sigaction_t *)(uintptr_t)a3, a4);
        break;

    case SYS_gettimeofday: {
        struct timeval *tv = (struct timeval *)a1;
        if (tv) {
            uint64_t t = timer_ticks;
            /* FASE8.1-clock: epoch do RTC + uptime (antes: uptime puro) */
            if (!boot_epoch_done) boot_epoch_init(t);
            tv->tv_sec = g_boot_epoch + t / 100;
            tv->tv_usec = (t % 100) * 10000;
        }
        ret = 0;
        break;
    }

    /* ~~ SYS_disp_get_fb (200) ~ "Me da o framebuffer!!" ~~
     * Mapeia o framebuffer VESA/virtio no espaco do usuario e devolve
     * as dimensoes. CUIDADO: a ordem dos parametros eh:
     *   a1 (RDI) = &fb_addr  (uint64_t*)
     *   a2 (RSI) = &width    (uint32_t*)  <-- ANTES tava como pitch!!!
     *   a3 (RDX) = &height   (uint32_t*)
     *   a4 (RCX) = &pitch    (uint32_t*)  <-- ANTES tava como width!!!
     * Sim, eu troquei a2 e a4. O debug de 3 horas foi culpa disso. Desculpa~
     * Adicionei guard pra g_fb.width==0 tbm, pq o VESA as vezes acorda
     * desligado e nao quer passar largura. */
    case SYS_disp_get_fb: {
        extern int g_fb_active;
        extern framebuffer_t g_fb;
        if (!g_fb_active || g_fb.bpp != 32) { ret = -1; break; }
        if (g_fb.width == 0 || g_fb.height == 0) { ret = -1; break; }
        uint64_t user_fb_va = 0xFFFFFFFF80000000ULL;
        uint64_t fb_size = (uint64_t)g_fb.pitch * g_fb.height;
        fb_size = (fb_size + 0x1FFFFF) & ~0x1FFFFF;
        pcb_t *cur = process_current();
        int r = pml4_map_phys(cur->pml4, user_fb_va, g_fb.addr, fb_size, 1);
        if (r < 0) { ret = -1; break; }
        uint64_t *addr   = (uint64_t *)a1;
        uint32_t *width  = (uint32_t *)a2;
        uint32_t *height = (uint32_t *)a3;
        uint32_t *pitch  = (uint32_t *)a4;
        if (addr)   *addr   = user_fb_va;
        if (width)  *width  = g_fb.width;
        if (height) *height = g_fb.height;
        if (pitch)  *pitch  = g_fb.pitch;
        ret = 0;
        break;
    }

    /* ~~ SYS_mouse_read (202) ~ "Mexe o mouse, baka!" ~~
     * Le o acumulado do mouse (dx, dy, botoes) e devolve pro usuario.
     * Implementei de verdade agora (antes tava so retornando 0, o que
     * fazia o cursor ficar parado igual estátua. Triste.) */
    case SYS_mouse_read: {
        int *dx      = (int *)a1;
        int *dy      = (int *)a2;
        int *buttons = (int *)a3;
        mouse_read_delta(dx, dy, buttons);
        ret = 0;
        break;
    }

    /* ~~ SYS_disp_flush (201) ~ "Joga o backbuffer na tela!" ~~
     * Copia o backbuffer inteiro pro framebuffer usando rep movsl.
     * Nao usa SSE aqui porque o kernel nao salva xmm registers.
     * rep movsl é otimizado pelo CPU (ERMSB) e pelo QEMU (memcpy host). */
    case SYS_disp_flush: {
        extern int g_fb_active;
        extern framebuffer_t g_fb;
        extern int g_virtio_active;
        void *backbuffer = (void *)a1;
        if (!g_fb_active || !backbuffer) break;
        uint64_t fb_va = g_virtio_active ? g_fb.addr : 0xFFFFFFFF80000000ULL;
        uint32_t *fb_virt = (uint32_t *)fb_va;
        size_t pixels = (size_t)g_fb.pitch * g_fb.height / 4;
        __asm__ volatile (
            "rep movsl"
            : "+D"(fb_virt), "+S"(backbuffer), "+c"(pixels)
            : : "memory"
        );
        if (g_virtio_active)
            virtio_gpu_flush();
        ret = 0;
        break;
    }

    /* ~~ SYS_disp_flush_rect (205) ~ "So uma parte, nao tudo!" ~~
     * Versao otimizada do flush: so copia um retangulo do backbuffer.
     * Embrulha x,y em a2 e w,h em a3 (cada um em 16 bits). Sim, é
     * uma gambiarra, mas economiza registrador~ >_< */
    case SYS_disp_flush_rect: {
        extern int g_fb_active;
        extern framebuffer_t g_fb;
        extern int g_virtio_active;
        void *backbuffer = (void *)a1;
        int x = (int)(a2 & 0xFFFF);
        int y = (int)((a2 >> 16) & 0xFFFF);
        int w = (int)(a3 & 0xFFFF);
        int h = (int)((a3 >> 16) & 0xFFFF);
        if (!g_fb_active || !backbuffer || w <= 0 || h <= 0) break;
        uint64_t fb_va = g_virtio_active ? g_fb.addr : 0xFFFFFFFF80000000ULL;
        uint32_t stride_px = g_fb.pitch / 4;
        uint32_t *src = (uint32_t *)backbuffer + (uint32_t)y * stride_px + (uint32_t)x;
        uint32_t *dst = (uint32_t *)fb_va + (uint32_t)y * stride_px + (uint32_t)x;
        for (int row = 0; row < h; row++) {
            uint32_t *row_src = src;
            uint32_t *row_dst = dst;
            int cw = w;
            __asm__ volatile ("rep movsl" : "+D"(row_dst), "+S"(row_src), "+c"(cw) : : "memory");
            src += stride_px;
            dst += stride_px;
        }
        if (g_virtio_active)
            virtio_gpu_flush();
        ret = 0;
        break;
    }

    /* ~~ SYS_kb_mod (203) ~ "Shift? Ctrl? Me diz ai!" ~~
     * Devolve o estado das teclas modificadoras (shift, ctrl).
     * Pro disp-wm saber se o usuario quer abrir menu ou matar janela~ */
    case SYS_kb_mod: {
        extern volatile int shift_pressed;
        extern volatile int ctrl_pressed;
        uint32_t mods = 0;
        if (shift_pressed) mods |= 1;
        if (ctrl_pressed)  mods |= 2;
        ret = mods;
        break;
    }

    /* ~~ SYS_readdir (207) ~ "Lista os arquivos, por favor!" ~~
     * Le o diretorio FAT32 e preenche um array de entradas.
     * Salva/restaura o CWD pra nao atrapalhar o processo chamador.
     * Se o path nao existir, retorna -1 e chora baixinho~ */
    case SYS_readdir: {
        int fd = (int)a1;
        uint8_t *buffer = (uint8_t *)a2;
        uint32_t count = (uint32_t)a3;
        if (!buffer || count == 0) { ret = (uint64_t)-VFS_EFAULT; break; }
        if (fd < 0 || fd >= MAX_FDS || !fds[fd].used) {
            ret = (uint64_t)-VFS_EBADF;
            break;
        }
        if (fds[fd].type != 0 || !fds[fd].stat_valid ||
            !(fds[fd].stat_attr & 0x10)) {
            ret = (uint64_t)-VFS_ENOTDIR;
            break;
        }

        const char *directory = fds[fd].path[0] ? fds[fd].path : fds[fd].name;
        uint32_t written = 0;
        while (written < count) {
            vfs_dirent_t entry;
            int n = vfs_readdir(directory, fds[fd].dir_cookie, &entry, 1);
            if (n < 0) {
                ret = written ? written : (uint64_t)n;
                break;
            }
            if (n == 0) {
                ret = written;
                break;
            }
            if (entry.name_len == 0 || entry.name_len >= sizeof(entry.name)) {
                ret = written ? written : (uint64_t)-VFS_EIO;
                break;
            }
            uint32_t reclen = (uint32_t)((19 + entry.name_len + 1 + 7) & ~7U);
            if (reclen > count - written) {
                ret = written ? written : (uint64_t)-VFS_EINVAL;
                break;
            }
            uint8_t *record = buffer + written;
            for (uint32_t index = 0; index < reclen; index++) record[index] = 0;
            *(uint64_t *)(record + 0) = entry.ino;
            *(int64_t *)(record + 8) = (int64_t)entry.next_cookie;
            *(uint16_t *)(record + 16) = (uint16_t)reclen;
            record[18] = entry.type;
            for (uint16_t index = 0; index < entry.name_len; index++)
                record[19 + index] = (uint8_t)entry.name[index];
            record[19 + entry.name_len] = 0;
            written += reclen;
            if (entry.next_cookie <= fds[fd].dir_cookie) {
                ret = written;
                break;
            }
            fds[fd].dir_cookie = entry.next_cookie;
            ret = written;
        }
        break;
    }

    /* ~~ SYS_execve (208) ~ "Executa um programa!" ~~
     * Carrega um binario Mach-O do FAT32 e substitui o processo atual.
     * Faz a separacao do path em diretorio + nome (sim, eu implementei
     * dirname na mao pq nao tinha libc no kernel). Le o arquivo, passa
     * pro mach_o_load, e configura o iretq stack frame pro entry point.
     * É tipo um fork+exec mas sem o fork~ economia de recursos! >_< */
    /* ~~ FORK REAL (issue #72): pai recebe PID, filho recebe RAX=0 ~~ */
    case SYS_fork_real: {
        int cpid = proc_fork(regs);
        if (cpid < 0) { ret = (uint64_t)-1; break; }
        ret = (uint64_t)cpid;
        break;
    }

    case SYS_execve: {
        /* ~~ EXECVE REAL (issue #72) ~ substitui a imagem do processo~~
         * O Popen do Xorg: fork → dup2 → execl("/bin/sh","sh","-c",cmd)~
         * Carregamos o ELF novo num PML4 fresco, montamos pilha Linux,
         * trocamos CR3 e REESCREVEMOS o kframe — o iretq da syscall já
         * pula pro código novo~ (sucesso = nunca retorna!)~ */
        #define EXECVE_STACK_SIZE 1048576
        const char *upath = (const char *)a1;
        char **uargv = (char **)a2;
        char **uenvp = (char **)a3;
        if (!upath || !upath[0]) { ret = -1; break; }

        /* 1. copia path/argv/envp do user ANTES de destruir o espaço~ */
        static char kpath[512];
        {
            int i = 0;
            while (upath[i] && i < 511) { kpath[i] = upath[i]; i++; }
            kpath[i] = '\0';
        }
        static char kargs[16][256];
        static char *kargv[17];
        int kargc = 0;
        if (uargv) {
            while (kargc < 16 && uargv[kargc]) {
                const char *s = uargv[kargc];
                int j = 0;
                while (s[j] && j < 255) { kargs[kargc][j] = s[j]; j++; }
                kargs[kargc][j] = '\0';
                kargv[kargc] = kargs[kargc];
                kargc++;
            }
        }
        kargv[kargc] = 0;
        static char kenvs[32][256];
        static char *kenvp[33];
        int kenvc = 0;
        if (uenvp) {
            while (kenvc < 32 && uenvp[kenvc]) {
                const char *s = uenvp[kenvc];
                int j = 0;
                while (s[j] && j < 255) { kenvs[kenvc][j] = s[j]; j++; }
                kenvs[kenvc][j] = '\0';
                kenvp[kenvc] = kenvs[kenvc];
                kenvc++;
            }
        }
        kenvp[kenvc] = 0;

        /* 2. resolve com cwd do processo + PATH-like /bin~ */
        pcb_t *me = process_current();
        {
            char absp[VFS_MAX_PATH];
            char tmp[VFS_MAX_PATH];
            int i = 0;
            while (kpath[i] && i < VFS_MAX_PATH - 1) { tmp[i] = kpath[i]; i++; }
            tmp[i] = '\0';
            if (vfs_abs_path(me ? me->cwd : "/", tmp, absp, sizeof(absp)) == 0) {
                int j = 0;
                while (absp[j] && j < 511) { kpath[j] = absp[j]; j++; }
                kpath[j] = '\0';
            }
        }
        if (kpath[0] != '/') {
            char fb[128];
            fb[0]='/'; fb[1]='b'; fb[2]='i'; fb[3]='n'; fb[4]='/';
            int fi = 0;
            while (kpath[fi] && fi < 120) { fb[5+fi] = kpath[fi]; fi++; }
            fb[5+fi] = '\0';
            int j = 0;
            while (fb[j]) { kpath[j] = fb[j]; j++; }
            kpath[j] = '\0';
        }

        /* 3. lê via VFS e valida ELF~ */
        uint32_t fsize2; uint8_t attr2;
        if (vfs_stat_size(kpath, &fsize2, &attr2) < 0 || fsize2 < 64) {
            serial_puts("[execve] stat falhou: "); serial_puts(kpath); serial_puts("\r\n");
            ret = -1; break;
        }
        uint8_t *buf = kmalloc(fsize2 + 1);
        if (!buf) { ret = -1; break; }
        if (vfs_read_file(kpath, buf, fsize2) < 0 ||
            !(buf[0]==0x7F && buf[1]=='E' && buf[2]=='L' && buf[3]=='F')) {
            serial_puts("[execve] ELF invalido: "); serial_puts(kpath); serial_puts("\r\n");
            kfree(buf); ret = -1; break;
        }

        /* 4. PML4 novo + U/S nos segmentos (receita do cmd_exec)~ */
        uint64_t child_pml4 = clone_identity_tables();
        if (!child_pml4) { kfree(buf); ret = -1; break; }
        Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
        uint64_t elf_base = (ehdr->e_type == 3) ? elf64_pie_base() : 0;
        uint64_t elf_phdr = 0, elf_phent = ehdr->e_phentsize;
        uint64_t elf_phnum = ehdr->e_phnum, elf_bss_end = 0;
        {
            Elf64_Phdr *pp = (Elf64_Phdr *)(buf + ehdr->e_phoff);
            for (uint32_t k = 0; k < elf_phnum; k++) {
                if (pp->p_type == PT_LOAD) {
                    if (elf_phdr == 0)
                        elf_phdr = elf_base + pp->p_vaddr + (ehdr->e_phoff - pp->p_offset);
                    /* FASE5: sem pml4_add_user por chunk aqui — o loader
                     * (elf64_load_into_pml4, 0x87) já dá U/S nos frames
                     * da imagem; expor a janela 2MB inteira liberava
                     * frames livres do buddy como user (heap poison). */
                    if (elf_base + pp->p_vaddr + pp->p_memsz > elf_bss_end)
                        elf_bss_end = elf_base + pp->p_vaddr + pp->p_memsz;
                }
                pp = (Elf64_Phdr *)((uint8_t *)pp + elf_phent);
            }
            elf_bss_end = (elf_bss_end + 0xFFF) & ~0xFFFULL;
        }

        void *entry = elf64_load_into_pml4(buf, fsize2, child_pml4);
        uint64_t main_entry = (uint64_t)(uintptr_t)entry;
        uint64_t interp_base = 0;
        int is_dynamic = 0;
        char interp_path[256];
        int interp_len = elf64_find_interp(buf, fsize2, interp_path, sizeof(interp_path));
        if (!entry) {
            kfree(buf);
            pml4_destroy(child_pml4);
            ret = -1;
            break;
        }
        if (interp_len > 0) {
            uint32_t interp_size = 0;
            uint8_t interp_attr = 0;
            if (vfs_stat_size(interp_path, &interp_size, &interp_attr) < 0 || !interp_size) {
                kfree(buf);
                pml4_destroy(child_pml4);
                serial_puts("[execve] interp stat falhou: ");
                serial_puts(interp_path);
                serial_puts("\r\n");
                ret = -1;
                break;
            }
            unsigned char *interp_data = (unsigned char *)kmalloc(interp_size);
            if (!interp_data || vfs_read_file(interp_path, interp_data, interp_size) < 0) {
                if (interp_data) kfree(interp_data);
                kfree(buf);
                pml4_destroy(child_pml4);
                serial_puts("[execve] interp read falhou: ");
                serial_puts(interp_path);
                serial_puts("\r\n");
                ret = -1;
                break;
            }
            interp_base = 0x60000000ULL;
            void *interp_entry = elf64_load_at(interp_data, interp_size,
                                               child_pml4, interp_base);
            kfree(interp_data);
            if (!interp_entry) {
                kfree(buf);
                pml4_destroy(child_pml4);
                serial_puts("[execve] interp load falhou\r\n");
                ret = -1;
                break;
            }
            entry = interp_entry;
            is_dynamic = 1;
        }
        kfree(buf);

        /* 5. stack novo mapeado U/S no pml4 NOVO~ */
        void *ustack = mmap_user(0, EXECVE_STACK_SIZE, 3, 0);
        if (!ustack) { pml4_destroy(child_pml4); ret = -1; break; }
        /* U/S só nas páginas do stack — a janela identity de 2MB
         * também mora frame de kernel ali (ver pml4_add_user_range) */
        pml4_add_user_range(child_pml4, (uint64_t)ustack,
                            (uint64_t)ustack + EXECVE_STACK_SIZE);

        /* 6. PCB: pml4/break novos, fds >= 3 fecham (POSIX execve)~ */
        uint64_t old_pml4 = 0;
        if (me) {
            old_pml4 = me->pml4;
            me->pml4 = child_pml4;
            if (elf_bss_end) {
                me->program_break = elf_bss_end;
                me->heap_start = elf_bss_end;
            }
            me->fs_base = 0x70000000ULL + ((uint64_t)me->pid * 0x1000ULL);
            if (map_user_4kb(me->pml4, me->fs_base) < 0) me->fs_base = 0x1000ULL;
            /* ~~ o zéro do TLS ainda é escrito pelo CR3 ANTIGO (o espaço
             * do fork, trocado só no passo 7). Em filho de fork o
             * fs_base herdado é outro e 0x70000000+pid*0x1000 não está
             * mapeado aqui → PF@0x7000A000 infinito e o exec nunca
             * terminava. Mapeia no espaço antigo também. ~~ */
            if (old_pml4 && old_pml4 != me->pml4)
                map_user_4kb(old_pml4, me->fs_base);
            volatile uint64_t *tls_page = (volatile uint64_t *)(uintptr_t)me->fs_base;
            tls_page[0] = 0; tls_page[1] = 0; tls_page[2] = 0; tls_page[3] = 0;
        }
        for (int fd = 3; fd < MAX_FDS; fd++) fds[fd].used = 0;

        /* 7. CR3 JÁ — kernel stack é identity em todo PML4~ */
        __asm__ volatile("mov %0, %%cr3" :: "r"(child_pml4) : "memory");

        /* 8. pilha Linux nova (via CR3 novo!)~ */
        uint64_t newtop = (uint64_t)ustack + EXECVE_STACK_SIZE;
        uint64_t newrsp;
        if (is_dynamic) {
            newrsp = setup_linux_user_stack_dyn(me ? me : 0, newtop,
                                                elf_phdr, elf_phent, elf_phnum,
                                                elf_base, kargv, kargc, kenvp, kenvc,
                                                interp_base, main_entry, 1);
        } else {
            newrsp = setup_linux_user_stack(me ? me : 0, newtop,
                                           elf_phdr, elf_phent, elf_phnum, elf_base,
                                           kargv, kargc, kenvp, kenvc, main_entry);
        }
        /* 8.5 ~~ user_rsp do PCB ainda aponta pro stack do espaço ANTIGO
         * (destruído nos passos 6-8). O fim do handler faz
         * current_rsp0 = syscall_process->user_rsp → o xchg de saída em
         * syscall_entry.asm joga o rsp em cima do stack velho (não
         * mapeado no PML4 novo) e o `mov [rsp-40], rcx` do frame de iret
         * dá #PF em kernel com rsp de user → #PF sobre #PF → DF → triple
         * fault (QEMU morre logo depois do [execve] ok). Aponta pro
         * stack novo que a setup_linux_user_stack acabou de montar. ~~ */
        if (me) me->user_rsp = newrsp;
        {
            extern void serial_puthex(uint32_t v);
            serial_puts("[exec-stk] ustack="); serial_puthex((uint32_t)ustack);
            serial_puts(" newtop="); serial_puthex((uint32_t)newtop);
            serial_puts(" newrsp="); serial_puthex((uint32_t)newrsp);
            serial_puts(" kstack="); serial_puthex((uint32_t)(me ? me->rsp0 : 0));
            serial_puts("\r\n");
        }

        if (old_pml4) pml4_destroy(old_pml4);

        /* 9. reescreve o kframe: iretq pula pro ELF novo!~ */
        for (int i = 1; i < 15; i++) regs[i] = 0;
        regs[14] = 0;
        regs[15] = (uint64_t)entry; /* RIP stash */
        regs[16] = 0x202;           /* RFLAGS stash (IF=1) */
        serial_puts("[execve] ok: "); serial_puts(kpath); serial_puts("\r\n");
        ret = 0;
        break;
    }

    /* ~~ SYS_shell_cmd (209) ~ "Roda um comando e pega a saida!" ~~
     * Executa um comando do shell e captura a saida num buffer.
     * Usa o console_set_output_buffer pra redirecionar a saida.
     * Depois volta ao normal. É tipo um popen() mas mais ~gambiarra~ */
    case SYS_shell_cmd: {
        const char *cmd = (const char *)a1;
        char *outbuf = (char *)a2;
        int outsize = (int)a3;
        if (!cmd || !cmd[0] || !outbuf || outsize <= 0) { ret = -1; break; }
        int outlen = 0;
        console_set_output_buffer(outbuf, &outlen, outsize);
        execute(cmd);
        console_set_output_buffer(NULL, NULL, 0);
        ret = outlen;
        break;
    }

    /* ~~ SYS_spawn (210) ~ "Cria um processo filho!" ~~
     * A syscall mais nova do pedaço! Carrega um binario, cria uma PML4
     * propria pro processo filho (isolamento de verdade!), aloca pilha,
     * e usa o proc_spawn pra criar o processo. O filho vai ter o proprio
     * espaco de enderecamento, entao se ele crashar, o pai nao morre junto~
     * (Diferente do SYS_execve que substitui o processo atual.)
     * Usei clone_identity_tables() pra copiar as tabelas de paginacao
     * e mach_o_load_into_pml4 pra carregar o binario na PML4 do filho. */
    case SYS_spawn: {
        const char *fname = (const char *)a1;
        if (!fname || !fname[0]) { ret = -1; break; }
        uint32_t fsize; uint8_t attr;
        if (fat32_stat(fname, &fsize, &attr, 0, 0) < 0) { ret = -1; break; }
        uint8_t *buf = kmalloc(fsize + 1);
        if (!buf) { ret = -1; break; }
        if (fat32_read_file(fname, buf, fsize) < 0) { kfree(buf); ret = -1; break; }
        uint64_t child_pml4 = clone_identity_tables();
        if (!child_pml4) { kfree(buf); ret = -1; break; }

        /* ~~ ELF vs Mach-O: dois jeitos de ser um binary ~~
         * ELF = Linux padrao, Mach-O = nosso formato legado~
         * Se tiver os 4 bytes magicos "\x7fELF", é um~ */
        uint64_t dyn_interp_entry = 0;
        uint64_t dyn_interp_base = 0;
        int dyn_is_dynamic = 0;
        int is_elf = (fsize >= 4 && buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F');
        uint64_t elf_phdr = 0, elf_phent = 0, elf_phnum = 0, elf_bss_end = 0;
        void *entry;
        uint64_t elf_base = 0;
        if (is_elf) {
            Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
            elf_phent = ehdr->e_phentsize;
            elf_phnum = ehdr->e_phnum;
            elf_base = (ehdr->e_type == 3) ? elf64_pie_base() : 0; /* ET_DYN = PIE */
            /* ~~ Calcula o endereco virtual da tabela PHDR + fim do BSS ~~
             * O BSS end vira o program_break inicial (o musl chama
             * brk(0) pra descobrir onde o heap começa — e antes o
             * kernel respondia 0x201000 pra todo mundo, um chute~) */
            if (elf_phnum) {
                Elf64_Phdr *pp = (Elf64_Phdr *)(buf + ehdr->e_phoff);
                for (uint32_t k = 0; k < elf_phnum; k++) {
                    if (pp->p_type == PT_LOAD) {
                        if (!elf_phdr)
                            elf_phdr = elf_base + pp->p_vaddr + (ehdr->e_phoff - pp->p_offset);
                        uint64_t seg_end = elf_base + pp->p_vaddr + pp->p_memsz;
                        if (seg_end > elf_bss_end) elf_bss_end = seg_end;
                    }
                    pp = (Elf64_Phdr *)((uint8_t *)pp + elf_phent);
                }
                elf_bss_end = (elf_bss_end + 0xFFF) & ~0xFFFULL;
            }
            entry = elf64_load_into_pml4(buf, fsize, child_pml4);

            /* ~~ ELF DINÂMICO (#71): se tem PT_INTERP, carrega o ld.so~~
             * O interpretador roda PRIMEIRO (entry dele), resolve as
             * libs do DT_NEEDED e só depois pula pro entry do binário.
             * AT_BASE passa a apontar pro INTERPRETADOR~ */
            char interp_path[256];
            int ilen = -1;
            if (is_elf)
                ilen = elf64_find_interp(buf, fsize, interp_path, sizeof(interp_path));
            if (ilen > 0) {
                serial_puts("[dyn] interp=");
                serial_puts(interp_path);
                serial_puts("\r\n");
                unsigned char *idata = kmalloc(4 * 1024 * 1024);
                if (idata && vfs_read_file(interp_path, idata, 4 * 1024 * 1024) > 0) {
                    const uint64_t INTERP_BASE = 0x60000000ULL; /* longe do PIE(0x40000000) e do mmap(0x70000000) */
                    void *ientry = elf64_load_at(idata, 4 * 1024 * 1024, child_pml4, INTERP_BASE);
                    if (ientry) {
                        dyn_interp_entry = (uint64_t)ientry;
                        dyn_interp_base = INTERP_BASE;
                        dyn_is_dynamic = 1;
                        serial_puts("[dyn] ld.so no ar\r\n");
                    } else {
                        serial_puts("[dyn] falha ao mapear interp\r\n");
                    }
                } else {
                    serial_puts("[dyn] interp não encontrado no FS\r\n");
                }
                if (idata) kfree(idata);
            }
        } else {
            entry = mach_o_load_into_pml4(buf, fsize, child_pml4);
        }
        kfree(buf);
        if (!entry) { ret = -1; break; }
        if (dyn_is_dynamic) entry = (void *)dyn_interp_entry;
        void *user_stack = mmap_user(0, 1048576, 3, 0);
        if (!user_stack) { ret = -1; break; }
        /* ~~ Torna a pilha visivel pro usuario ~~
         * clone_identity_tables tirou o U/S da memoria identitaria~
         * Só nas páginas DO STACK (janela de 2MB mora com frames do
         * kernel — ver pml4_add_user_range). ~~ */
        pml4_add_user_range(child_pml4, (uint64_t)user_stack,
                            (uint64_t)user_stack + 1048576);
        int pid = proc_spawn(fname, entry, (uint8_t *)user_stack + 1048576);
        if (pid < 0) { munmap_user(user_stack, 1048576); ret = -1; break; }
        for (int i = 0; i < MAX_PROC; i++) {
            if (pcb_table[i].pid == pid) {
                pml4_destroy(pcb_table[i].pml4);
                pcb_table[i].pml4 = child_pml4;
                /* ~~ Setup do vetor auxiliar do Linux ~~
                 * argc, argv, envp, e os AT_* pros binarios ELF~
                 * Assim o codigo do usuario acha os argumentos~ */
                if (is_elf) {
                    /* ~~ Program break começa no fim do BSS do ELF ~~
                     * (era 0x201000 hardcoded do proc_spawn — o musl
                     *  brk(0) respondia 2MB e o heap ia parar lá~) */
                    if (elf_bss_end) {
                        pcb_table[i].program_break = elf_bss_end;
                        pcb_table[i].heap_start = elf_bss_end;
                    }
                    uint64_t *kframe = (uint64_t *)pcb_table[i].kernel_rsp;
                    uint64_t old_rsp = kframe[18];
                    /* ~~ AT_BASE: dinâmico → base do ld.so (interp)~~
                     * AT_ENTRY(9): entry do binário principal pro ld.so~ */
                    kframe[18] = setup_linux_user_stack_dyn(&pcb_table[i], old_rsp,
                                                           elf_phdr, elf_phent, elf_phnum,
                                                           elf_base, NULL, 0, NULL, 0,
                                                           dyn_interp_base, (uint64_t)entry,
                                                           dyn_is_dynamic);
                }
            }
        }
        ret = pid;
        break;
    }

    /* ~~ SYS_spawn_shared (211) ~ spawn com PML4 clonada ~~
     * Cria um processo filho com uma COPIA da PML4 do pai.
     * O filho enxerga os mesmos buffers/paginas que o pai
     * (incluindo 0x500000 e buffers alocados via mmap),
     * mas tem suas proprias tabelas de paginacao. Isso significa
     * que quando carregamos um binario DIFERENTE (ex: TERM em
     * vez do compositor), o codigo do pai nao eh sobrescrito.
     *
     * ANTES usava mach_o_load() que copiava o binario direto
     * em 0x10000000 — o MESMO endereco onde o compositor vive.
     * O compositor morria na volta da syscall porque o codigo
     * dele tinha virado codigo do TERM. affs, 3 dias de debug
     * por causa disso. >_<
     *
     * AGORA: clona a PML4, carrega o binario na PML4 do filho
     * (mach_o_load_into_pml4), e o pai continua intacto. O filho
     * ve as mesmas paginas de memoria compartilhada (0x500000,
     * buffer de pixel) porque a PML4 clonada copia todas as
     * entradas de paginacao, incluindo o mapeamento identitario.
     * rssrsrs, quem diria que clonar a PML4 resolve TUDO. */
    case SYS_spawn_shared: {
        const char *fname = (const char *)a1;
        if (!fname || !fname[0]) { serial_puts("spawn: nome vazio\n"); ret = -1; break; }
        serial_puts("spawn: path="); serial_puts(fname); serial_puts("\n");

        /* ~~ Path resolution: separa diretorio do nome do arquivo ~~
         * O FAT32 so procura nomes simples (sem '/') no diretorio
         * atual. O shell mudou pra /BIN antes de rodar DISP, entao
         * current_dir_cluster aponta pra /BIN, nao pra raiz. E a
         * funcao find_entry() nao entende paths com '/'. Entao
         * precisamos fazer o chdir manualmente antes de stat/read. */
        uint32_t saved_cwd = fat32_get_cwd();
        char _p[256]; int _i = 0;
        while (fname[_i] && _i < 255) { _p[_i] = fname[_i]; _i++; }
        _p[_i] = '\0';
        char *_base = _p, *_last_slash = 0, *_s = _p;
        if (*_base == '/') { fat32_change_dir("/"); _base++; _s = _base; }
        while (*_s) { if (*_s == '/') _last_slash = _s; _s++; }
        char *_file = _base;
        if (_last_slash) {
            *_last_slash = '\0';
            _file = _last_slash + 1;
            /* Walk each directory component */
            char *_w = _base;
            while (_w && *_w) {
                char *_n = _w;
                while (*_n && *_n != '/') _n++;
                int _end = (*_n == '\0');
                if (*_n) *_n = '\0';
                if (_w[0] && fat32_change_dir(_w) < 0) {
                    serial_puts("spawn: chdir "); serial_puts(_w); serial_puts(" falhou\n");
                    ret = -1; break;
                }
                if (_end) break;
                _w = _n + 1;
            }
        }
        if (ret < 0) { fat32_set_cwd(saved_cwd); break; }
        serial_puts("spawn: procurando '"); serial_puts(_file); serial_puts("'\n");
        uint32_t fsize; uint8_t attr;
        if (fat32_stat(_file, &fsize, &attr, 0, 0) < 0) {
            serial_puts("spawn: stat falhou\n");
            fat32_set_cwd(saved_cwd); ret = -1; break;
        }
        serial_puts("spawn: size="); serial_puthex(fsize); serial_puts("\n");
        uint8_t *buf = kmalloc(fsize + 1);
        if (!buf) { serial_puts("spawn: sem kmem\n"); fat32_set_cwd(saved_cwd); ret = -1; break; }
        if (fat32_read_file(_file, buf, fsize) < 0) {
            serial_puts("spawn: read falhou\n"); kfree(buf);
            fat32_set_cwd(saved_cwd); ret = -1; break;
        }
        fat32_set_cwd(saved_cwd);
        uint64_t child_pml4 = clone_identity_tables();
        if (!child_pml4) { kfree(buf); serial_puts("spawn: clone_pml4 falhou\n"); ret = -1; break; }
        serial_puts("spawn: child_pml4="); serial_puthex((uint32_t)child_pml4); serial_puts("\n");

        /* Detect ELF vs Mach-O */
        int is_elf = (fsize >= 4 && buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F');
        uint64_t elf_phdr = 0, elf_phent = 0, elf_phnum = 0;
        uint64_t elf_base = 0;
        void *entry;
        if (is_elf) {
            Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
            elf_phent = ehdr->e_phentsize;
            elf_phnum = ehdr->e_phnum;
            elf_base = (ehdr->e_type == 3) ? elf64_pie_base() : 0; /* ET_DYN = PIE */
            if (elf_phnum) {
                Elf64_Phdr *pp = (Elf64_Phdr *)(buf + ehdr->e_phoff);
                for (uint32_t k = 0; k < elf_phnum; k++) {
                    if (pp->p_type == PT_LOAD) {
                        elf_phdr = elf_base + pp->p_vaddr + (ehdr->e_phoff - pp->p_offset);
                        break;
                    }
                    pp = (Elf64_Phdr *)((uint8_t *)pp + elf_phent);
                }
            }
            entry = elf64_load_into_pml4(buf, fsize, child_pml4);
        } else {
            entry = mach_o_load_into_pml4(buf, fsize, child_pml4);
        }
        kfree(buf);
        if (!entry) { serial_puts("spawn: load_into_pml4 falhou\n"); ret = -1; break; }
        serial_puts("spawn: entry="); serial_puthex((uint32_t)(uintptr_t)entry); serial_puts("\n");
        uint8_t *user_stack = kmalloc(65536);
        if (!user_stack) { serial_puts("spawn: sem stack\n"); ret = -1; break; }
        /* FASE5: sem pml4_add_user de janela inteira (0x500000, 0xA00000,
         * 0x6000000, 0xE00000) — expunha RAM livre do buddy como user e
         * envenenava a freelist (EXC#0x0D no mmap do foot). Imagem tem
         * U/S pelo loader; stack pelo range; heap pelo map_user_4kb. */
        int pid = proc_spawn(fname, entry, user_stack + 65536);
        if (pid < 0) { kfree(user_stack); serial_puts("spawn: proc_spawn falhou\n"); ret = -1; break; }
        serial_puts("spawn: pid="); serial_puthex((uint32_t)pid); serial_puts("\n");
        for (int i = 0; i < MAX_PROC; i++) {
            if (pcb_table[i].pid == pid) {
                pml4_destroy(pcb_table[i].pml4);
                pcb_table[i].pml4 = child_pml4;
                if (is_elf) {
                    uint64_t *kframe = (uint64_t *)pcb_table[i].kernel_rsp;
                    uint64_t old_rsp = kframe[18];
                    kframe[18] = setup_linux_user_stack(&pcb_table[i], old_rsp,
                                                          elf_phdr, elf_phent, elf_phnum,
                                                          elf_base, NULL, 0, NULL, 0,
                                                          (uint64_t)(uintptr_t)entry);
                }
                break;
            }
        }
        ret = pid;
        break;
    }

    /* ~~ Linux arch_prctl (158) ~ TLS pra musl! ~~
     * ARCH_SET_FS (0x1002): seta o FS.base pro TLS do usuário~
     * ARCH_GET_FS (0x1003): devolve o FS.base atual~
     * (GS a gente nem usa, então ARCH_SET_GS/GET_GS são só um
     *  tapinha nas costas e retornam 0 sem fazer nada~ hihi)
     * Se o processo não existir, retorna -1 (você não tem corpo pra
     *  ter TLS, baka! >_<) */
    case SYS_arch_prctl: {
        int code = (int)a1;
        pcb_t *p = process_current();
        if (!p) { ret = -1; break; }
        switch (code) {
        case 0x1002: { /* ARCH_SET_FS — grava o FS.base! */
            p->fs_base = a2;
            __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((uint32_t)a2), "d"((uint32_t)(a2 >> 32)));
            ret = 0;
            break;
        }
        case 0x1003: { /* ARCH_GET_FS — lê o FS.base! */
            uint64_t *out = (uint64_t *)a2;
            if (out) *out = p->fs_base;
            ret = 0;
            break;
        }
        case 0x1001: /* ARCH_SET_GS — nope, next~ */ ret = 0; break;
        case 0x1004: /* ARCH_GET_GS — tbm nope~ */ ret = 0; break;
        default: ret = -1; break;
        }
        break;
    }

    /* ~~ Linux brk (231) ~ "Me mais memoria!" ~~
     * Se arg for 0, retorna o break atual sem mudar nada~
     * Se arg > break, atualiza (assume que deu certo~ confia)
     * Musl chama brk(0) só pra saber onde o heap começa~
     * (tipo quando você pergunta "que horas são?" e o relógio
     *  responde "sim" — utilidade duvidosa mas todo mundo usa) */
    case SYS_brk: {
        pcb_t *p = process_current();
        if (!p) { ret = -1; break; }
        if (a1 == 0) {
            ret = p->program_break;
        } else if (a1 > p->program_break) {
            /* ~~ Crescendo: mapeia as páginas novas ANTES de aceitar! ~~ */
            serial_puts("[brk] ");
            serial_puthex((uint32_t)p->program_break);
            serial_puts(" -> ");
            serial_puthex((uint32_t)a1);
            serial_puts("\r\n");
            uint64_t start = (p->program_break + 0xFFF) & ~0xFFFULL;
            uint64_t end = (a1 + 0xFFF) & ~0xFFFULL;
            int ok = 1;
            for (uint64_t va = start; va < end; va += 0x1000) {
                if (map_user_4kb(p->pml4, va) < 0) {
                    serial_puts("[brk] MAPFAIL va=");
                    serial_puthex((uint32_t)va);
                    serial_puts("\r\n");
                    ok = 0; break;
                }
            }
            if (ok) {
                p->program_break = a1;
                ret = a1;
            } else {
                /* ~~ Linux devolve o break antigo quando falha ~~ */
                ret = p->program_break;
            }
        } else if (a1 < p->heap_start) {
            ret = p->program_break;
        } else {
            /* ~~ Shrinking brk (raro mas permitido) ~~
             * Tipo devolver um pedaço do bolo depois de já
             * ter comido — estranho, mas a gente deixa~ */
            p->program_break = a1;
            ret = p->program_break;
        }
        break;
    }

    /* ~~ Linux exit_group (212) ~ "Mata todo mundo!" ~~
     * No nosso caso sem thread groups, vira um exit normal~
     * (você é grupo de um só, hihi~ solidão mode on) */
    case SYS_exit_group: {
        pcb_t *me_g = process_current();
        if (me_g && me_g->th_parent_pid >= 0) {
            proc_thread_exit((int)a1);
            for (;;) __asm__ volatile("hlt");
        }
        if (me_g) {
            /* ~~ mata threads irmãs (mesmo espaço) silenciosamente ~~ */
            proc_kill_siblings(me_g);
        }
        process_exit_current((int)a1);
        for (;;) __asm__ volatile("hlt");
        break;
    }

    /* ~~ Linux set_tid_address (218) ~ "Aqui, guarda meu TID!" ~~
     * Musl passa o endereço de uma variável onde o kernel
     * deveria escrever o TID quando a thread morre~
     * Mas como somos um kernel ~fofo~ e sem threads de verdade,
     * a gente só devolve o PID e ignora o endereço~ */
    case 186: /* gettid — musl pthread usa p/ self tid */
        ret = process_current_pid();
        break;

    case SYS_set_tid_address:
        ret = process_current_pid();
        break;

    /* ~~ threads e sinais (issue #50) ~~
     * weston/Xorg usam pthread (libwayland) e sinais pra encerrar.
     * Sem futex/robust_list a musl aborta em pthread_create.
     * Como ainda não temos threads de verdade (sem clone), os
     * stubs são "realistas": futex WAIT/WAKE real (pro lock do
     * malloc em memória compartilhada), sinais aceitam mas não
     * entregam, e wait4 liga no proc_waitpid que já existe~ */

    /* ~~ Linux wait4 (61) ~ "Espera o filho morrer~" ~~
     * Args: pid, &status, options, rusage.
     * Proc_waitpid já bloqueia até o filho virar zombie. */
    case SYS_wait4: {
        int pid = (int)a1;
        int *status = (int *)(uintptr_t)a2;
        int options = (int)a3;
        int wstatus = 0;
        if (options & 1) {
            int child_found = 0;
            for (int child_index = 0; child_index < MAX_PROC; child_index++) {
                pcb_t *child = &pcb_table[child_index];
                if (child->state == PROC_EMPTY || child->parent_pid != process_current_pid() ||
                    (pid > 0 && child->pid != pid)) continue;
                child_found = 1;
                if (child->state == PROC_ZOMBIE) {
                    wstatus = child->exit_code << 8;
                    if (status) *status = wstatus;
                    ret = child->pid;
                    serial_puts("[wait4-nohang] pid=");
                    serial_puthex((uint32_t)ret);
                    serial_puts(" status=");
                    serial_puthex((uint32_t)wstatus);
                    serial_puts("\r\n");
                    munmap_user((void *)(uintptr_t)(child->rsp0 - STACK_SIZE), STACK_SIZE);
                    pml4_destroy(child->pml4);
                    child->state = PROC_EMPTY;
                    break;
                }
            }
            if (ret == (uint64_t)-1) ret = child_found ? 0 : (uint64_t)-10;
            break;
        }
        int r = proc_waitpid(pid, &wstatus);
        if (r >= 0 && status) *status = wstatus << 8;
        serial_puts("[wait4-block] pid=");
        serial_puthex((uint32_t)r);
        serial_puts(" raw=");
        serial_puthex((uint32_t)wstatus);
        serial_puts(" status=");
        serial_puthex((uint32_t)(wstatus << 8));
        serial_puts("\r\n");
        ret = r;
        break;
    }

    /* ~~ Linux kill (62) / tgkill (234) / tkill (200) ~
     * "Mata o processo!" — mas sem thread de verdade, aceita e
     * ignora (retornar 0 é aceitável p/ boot, diz a issue~) */
    case SYS_kill:
        ret = signal_kill((int)a1, (int)a2);
        break;
    case SYS_tgkill:
        ret = signal_tgkill((int)a1, (int)a2, (int)a3);
        break;
    case 95: /* tkill */
        ret = signal_tgkill(0, (int)a1, (int)a2);
        break;

    /* ~~ Linux rt_sigprocmask (14) ~ "Bloqueia/desbloqueia sinal" ~~
     * Stub realista: aceita e retorna 0 (sinais não são entregues
     * de verdade, então a máscara não muda nada~) */
    case SYS_rt_sigprocmask:
        ret = signal_sigprocmask((int)a1, (const uint64_t *)(uintptr_t)a2,
                                 (uint64_t *)(uintptr_t)a3, a4);
        break;

    /* ~~ Linux rt_sigreturn (15) ~ "Volta do handler de sinal" ~~
     * Sem handlers de verdade, nunca é chamado — mas musl pode
     * passar por aqui, então retorna 0~ */
    case SYS_rt_sigreturn:
        ret = signal_rt_sigreturn(regs);
        break;

    /* ~~ Linux sigaltstack (131) ~ "Stack alternativo p/ handler" ~~
     * Stub: aceita e retorna 0~ */
    case SYS_sigaltstack:
        ret = 0;
        break;

    /* ~~ Linux prctl (157) ~ "Opções de processo" ~~
     * PR_SET_NAME (15) / PR_SET_DUMPABLE (4) são os que o weston usa.
     * Aceita e retorna 0~ */
    case SYS_prctl:
        ret = 0;
        break;

    case 112:
        ret = signal_setsid();
        break;
    case 111:
        ret = signal_getpgrp();
        break;
    case 109:
        ret = signal_setpgid((int)a1, (int)a2);
        break;

    /* ~~ Linux set_robust_list (273) / get_robust_list (274) ~~
     * Lista de futexes pra cancelamento de thread.
     * Guarda o ponteiro pra devolver quando pedirem~ */
    case SYS_set_robust_list:
        ret = 0;
        break;
    case SYS_get_robust_list: {
        uint64_t *head = (uint64_t *)(uintptr_t)a2;
        if (head) *head = 0; /* lista vazia — sem threads~ */
        ret = 0;
        break;
    }

    /* ~~ Linux rseq (334) ~ "Restartable sequences" ~~
     * glibc usa; musl nem precisa. Se retornar ENOSYS, a libc
     * desativa sozinha. Retornar 0 é o caminho mais seguro~ */
    case SYS_rseq:
        ret = 0;
        break;

    /* ~~ Linux futex (202→96) ~ "Sincronização de threads" ~~
     * op: 0=FUTEX_WAIT, 1=FUTEX_WAKE, 2=FUTEX_FD, 4=REQUEUE...
     * Sem threads de verdade, o lock do malloc da musl em
     * memória compartilhada é o caso mais comum: WAIT compara
     * *uaddr com val, WAKE acorda n. Como processos rodam em
     * round-robin num core só, a memória compartilhada já é
     * "sequencial" — então só conferir o valor resolve~ */
    case SYS_futex: {
        uint32_t *uaddr = (uint32_t *)(uintptr_t)a1;
        int op = (int)a2;
        int val = (int)a3;
        int op_part = op & 0x7F; /* ignora FLAGS/PRIVATE bits */
        (void)val;
        if (op_part == 0 && uaddr) { /* FUTEX_WAIT: spin cooperativo */
            /* ~~ poll-yield: enquanto *uaddr==val, cede a CPU~~
             * pthread_join espera assim até o clear_tid zerar! */
            int spins = 0;
            while (*(volatile uint32_t *)uaddr == (uint32_t)val) {
                schedule();
                if (++spins > 4000000) break; /* safety net~ */
            }
            ret = 0;
            break;
        }
        if (op_part == 1) { /* FUTEX_WAKE: acorda n threads */
            ret = 0; /* ninguém pra acordar~ */
            break;
        }
        ret = 0; /* outras ops: aceita e ignora~ */
        break;
    }

    /* ~~ Linux clock_gettime (228) ~ "Que horas são?" ~~
     * Recebe clock_id (a1) e struct timespec *tp (a2).
     * Ignora o clock_id (todo relógio é relógio quando se é
     *  um kernel minimalista~) e usa timer_ticks.
     * timespec: { tv_sec, tv_nsec } — sim, segundos e nanossegundos~
     * Pode não ser preciso mas pelo menos não é monotônico~ */
    case SYS_clock_gettime: {
        struct { uint64_t tv_sec; uint64_t tv_nsec; } *tp = (void *)a2;
        if (tp) {
            uint64_t t = timer_ticks;
            /* FASE8.1-clock: epoch do RTC + uptime (antes: uptime puro) */
            if (!boot_epoch_done) boot_epoch_init(t);
            tp->tv_sec = g_boot_epoch + t / 100;
            tp->tv_nsec = (t % 100) * 10000000ULL;
        }
        ret = 0;
        break;
    }

    case SYS_nanosleep: {
        struct tipos_timespec {
            int64_t tv_sec;
            int64_t tv_nsec;
        } *request = (void *)a1;
        struct tipos_timespec *remaining = (void *)a2;
        if (!request) {
            ret = (uint64_t)-VFS_EFAULT;
            break;
        }
        if (request->tv_sec < 0 || request->tv_nsec < 0 ||
            request->tv_nsec >= 1000000000LL) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }

        uint64_t seconds = (uint64_t)request->tv_sec;
        if (seconds > (UINT64_MAX - 999ULL) / 1000ULL) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        uint64_t milliseconds = seconds * 1000ULL +
                                (uint64_t)request->tv_nsec / 1000000ULL;
        uint64_t ticks = (milliseconds + 9ULL) / 10ULL;
        if (ticks > UINT64_MAX - timer_ticks) {
            ret = (uint64_t)-VFS_EINVAL;
            break;
        }
        uint64_t deadline = timer_ticks + ticks;
        while (timer_ticks < deadline) schedule();

        if (remaining) {
            remaining->tv_sec = 0;
            remaining->tv_nsec = 0;
        }
        ret = 0;
        break;
    }

    /* ~~ Linux uname (63) ~ "Quem sou eu?" ~~
     * Preenche struct utsname com identidade do sistema.
     * sysname=Linux (pra libc não reclamar), release=TipOS~
     * machine=x86_64 (que é a verdade mesmo~ kyun) */
    case SYS_uname: {
        char *uts = (char *)a1;
        if (!uts) { ret = -1; break; }
        const char *fields[] = {
            "Linux", "tipos", "6.1-tipos", "TipOS 1.0", "x86_64", "(none)"
        };
        /* cada campo tem 65 bytes (__UTS_LEN+1) */
        for (int f = 0; f < 6; f++) {
            const char *s = fields[f];
            char *dst = uts + f * 65;
            int i = 0;
            while (s[i] && i < 64) { dst[i] = s[i]; i++; }
            dst[i] = '\0';
        }
        ret = 0;
        break;
    }

    /* ~~ Linux getrandom (318) ~ "Me da sorte aleatória!" ~~
     * Preenche o buffer com bytes do xorshift (PRNG do kernel)~
     * Não é criptográfico de verdade, mas pro boot do weston
     * (que usa pra seeds) já serve~ confia~ */
    case SYS_getrandom: {
        uint8_t *buf = (uint8_t *)a1;
        size_t len = (size_t)a2;
        static uint64_t x = 0x9E3779B97F4A7C15ULL;
        /* ~~ FIX canário: os PRIMEIROS 16 bytes são EXATAMENTE os
         * mesmos que setup_linux_user_stack escreve em AT_RANDOM.
         * ld.so (getrandom) e __libc_start_main (AT_RANDOM) assim
         * leem o mesmo canário → fs:0x28 bate ~ */
        static const uint8_t at_random[16] = {
            0xCE,0xCA,0xFB,0xEE,0x0D,0xF0,0xAD,0xBA,  /* 0xBAADF00DBEEFCACE */
            0xBE,0xBA,0xFE,0xCA,0xEF,0xBE,0xAD,0xDE   /* 0xDEADBEEFCAFEBABE */
        };
        if (!buf || len == 0) { ret = 0; break; }
        for (size_t i = 0; i < len; i++) {
            if (i < 16) { buf[i] = at_random[i]; continue; }
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            buf[i] = (uint8_t)x;
        }
        serial_puts("[getrandom] len=");
        serial_puthex((uint32_t)len);
        serial_puts(" b0=");
        serial_puthex((uint32_t)buf[0]);
        serial_puts("\r\n");
        ret = (int)len;
        break;
    }

    /* ~~ Linux clock_getres (229) ~ "Quão preciso é teu relógio?" ~~
     * PIT a 100Hz → resolução de 10ms. Devidamente humilde~ */
    case SYS_clock_getres: {
        struct { uint64_t tv_sec; uint64_t tv_nsec; } *tp = (void *)a2;
        if (tp) {
            tp->tv_sec = 0;
            tp->tv_nsec = 10000000ULL; /* 10ms */
        }
        ret = 0;
        break;
    }

    /* ~~ Linux getrusage (98) ~ "Quanto CPU eu usei?" ~~
     * Zerado por preguiça — sem preempção real, ninguém mede~ */
    case SYS_getrusage: {
        uint8_t *ru = (uint8_t *)a2;
        if (ru) {
            /* struct rusage tem 18 longs (144 bytes) */
            for (int i = 0; i < 144; i++) ru[i] = 0;
        }
        ret = 0;
        break;
    }

    /* ~~ Linux times (100) ~ "Ticks do processo!" ~~
     * struct tms { tms_utime, tms_stime, tms_cutime, tms_cstime }
     * Devolve o relógio global e ticks zerados~ */
    case SYS_times: {
        uint64_t *tms = (uint64_t *)a1;
        if (tms) {
            tms[0] = timer_ticks; /* utime */
            tms[1] = 0;           /* stime */
            tms[2] = 0;           /* cutime */
            tms[3] = 0;           /* cstime */
        }
        ret = timer_ticks;
        break;
    }

    /* ~~ Linux sysinfo (239) ~ "Quanta RAM tem?" ~~
     * struct sysinfo do Linux x86-64 tem 112 bytes; o ld.so do glibc
     * lê totalram/freeram na inicialização — se a syscall responder
     * ENOSYS o HELLO_DYN morre antes do main~ */
    case SYS_sysinfo: {
        uint8_t *si = (uint8_t *)(uintptr_t)a1;
        if (si) {
            memset(si, 0, 112);
            uint64_t *q = (uint64_t *)si;
            q[0] = timer_ticks / 100;   /* uptime (s) */
            q[4] = 512ULL * 1024 * 1024; /* totalram (qemu -m 512M) */
            q[5] = 384ULL * 1024 * 1024; /* freeram */
            *(uint16_t *)(si + 80) = 1;  /* procs */
            *(uint32_t *)(si + 104) = 1; /* mem_unit */
        }
        ret = 0;
        break;
    }

    /* ~~ Linux getppid (110) ~ "Meu pai é o quê?" ~~
     * O PCB guarda parent_pid, é só devolver~ */
    case SYS_getppid: {
        pcb_t *p = process_current();
        ret = p ? p->parent_pid : 1;
        break;
    }

    /* ~~ Linux getpgid (121) ~ "Meu grupo?" ~~
     * Sem grupos de processo de verdade, cada um é grupo de si~ */
    case SYS_getpgid: {
        pcb_t *p = process_current();
        ret = p ? p->pid : 0;
        break;
    }

    /* ~~ Linux umask (95) ~ "Máscara de permissão!" ~~
     * Guarda e devolve a anterior. Fácil~ */
    case SYS_umask: {
        static int cur_umask = 0x1FF; /* 0777 — sem restrição */
        int old = cur_umask;
        cur_umask = (int)a1 & 0x1FF;
        ret = old;
        break;
    }

    /* ~~ Linux fcntl (72) ~ "Configura fd!" ~~
     * Mínimo: F_GETFD (1), F_SETFD (2), F_GETFL (3), F_SETFL (4).
     * F_SETFL O_NONBLOCK é o que weston pede no stdin~
     * (código 0x800 no Linux x86_64) */
    case SYS_fcntl: {
        int fd = (int)a1;
        int cmd = (int)a2;
        int arg = (int)a3;
        if (fd < 0 || fd >= MAX_FDS) { ret = -1; break; }
        switch (cmd) {
        case 0: /* F_DUPFD */
        case 1030: /* F_DUPFD_CLOEXEC */ {
            if (!fds[fd].used) { ret = (uint64_t)-VFS_EBADF; break; }
            if (arg < 0 || arg >= MAX_FDS) { ret = (uint64_t)-VFS_EINVAL; break; }
            int nfd = -1;
            for (int i = arg; i < MAX_FDS; i++) if (!fds[i].used) { nfd = i; break; }
            if (nfd < 0) { ret = (uint64_t)-VFS_ENOMEM; break; }
            fds[nfd] = fds[fd];
            if (cmd == 1030) fds[nfd].flags |= O_CLOEXEC;
            else fds[nfd].flags &= ~O_CLOEXEC;
            ret = (uint64_t)nfd;
            break;
        }
        case 1: /* F_GETFD */ ret = fds[fd].flags & O_CLOEXEC ? 1 : 0; break;
        case 2: /* F_SETFD */ fds[fd].flags = (fds[fd].flags & ~O_CLOEXEC) | (arg & O_CLOEXEC); ret = 0; break;
        case 3: /* F_GETFL */ ret = fds[fd].flags; break;
        case 4: /* F_SETFL */ fds[fd].flags = arg; ret = 0; break;
        /* FASE5: seals do memfd (foot exige p/ SHM pool). Sem enforcement
         * de seals no kernel: aceita e ignora (get sempre 0). */
        case 1033: /* F_ADD_SEALS */ ret = 0; break;
        case 1034: /* F_GET_SEALS */ ret = 0; break;
        default: ret = -1; break;
        }
        break;
    }

    /* ~~ Linux dup (32) ~ "Clona um fd!" ~~
     * Procura o menor fd livre e copia o apontamento~ */
    case SYS_dup: {
        int oldfd = (int)a1;
        if (oldfd < 0 || oldfd >= MAX_FDS || (!fds[oldfd].used && oldfd >= 3)) { ret = -1; break; }
        int nfd = alloc_fd();
        if (nfd < 0) { ret = -1; break; }
        fds[nfd] = fds[oldfd];
        ret = nfd;
        break;
    }

    /* ~~ Linux dup2 (91) ~ "Duplica num fd específico!" ~~
     * Se newfd já tá aberto, fecha antes~ (Linux semantics) */
    case SYS_dup2: {
        int oldfd = (int)a1;
        int newfd = (int)a2;
        if (oldfd < 0 || oldfd >= MAX_FDS || (!fds[oldfd].used && oldfd >= 3)) { ret = -1; break; }
        if (newfd < 0 || newfd >= MAX_FDS) { ret = -1; break; }
        if (newfd >= 3 && fds[newfd].used) {
            if (fds[newfd].type == 3) {
                int p = fds[newfd].pipe_idx;
                if (p >= 0 && p < MAX_PIPES) pipes[p].used = 0;
            }
            fds[newfd].used = 0;
        }
        fds[newfd] = fds[oldfd];
        ret = newfd;
        break;
    }

    /* ~~ Linux dup3 (292) ~ "dup2 + O_CLOEXEC!" ~~
     * Mesma coisa do dup2, guarda o flag de CLOEXEC~ */
    case SYS_dup3: {
        int oldfd = (int)a1;
        int newfd = (int)a2;
        int flags = (int)a3;
        if (oldfd < 0 || oldfd >= MAX_FDS || (!fds[oldfd].used && oldfd >= 3)) { ret = -1; break; }
        if (newfd < 0 || newfd >= MAX_FDS) { ret = -1; break; }
        if (newfd >= 3 && fds[newfd].used) {
            if (fds[newfd].type == 3) {
                int p = fds[newfd].pipe_idx;
                if (p >= 0 && p < MAX_PIPES) pipes[p].used = 0;
            }
            fds[newfd].used = 0;
        }
        fds[newfd] = fds[oldfd];
        fds[newfd].flags = flags;
        ret = newfd;
        break;
    }

    /* ~~ Linux pipe (22) ~ "Um canudinho pra conversar!" ~~
     * Cria um par de fds (leitura/escrita) com buffer compartilhado~ */
    case SYS_pipe: {
        int *out = (int *)a1;
        if (!out) { ret = -1; break; }
        int pi = -1;
        for (int i = 0; i < MAX_PIPES; i++) if (!pipes[i].used) { pi = i; break; }
        if (pi < 0) { ret = -1; break; }
        /* ~~ marca used ANTES do próximo alloc_fd — senão os dois
         * recebem o MESMO fd (rfd=wfd=3 e o pipe vira monoculo!)~~ */
        int rfd = alloc_fd();
        if (rfd < 0) { ret = -1; break; }
        fds[rfd].used = 1;
        int wfd = alloc_fd();
        if (wfd < 0) { fds[rfd].used = 0; ret = -1; break; }
        fds[wfd].used = 1;
        pipes[pi].used = 1;
        pipes[pi].rpos = pipes[pi].wpos = 0;
        for (int i = 0; i < 4096; i++) pipes[pi].buf[i] = 0;
        fds[rfd].type = 3; fds[rfd].pipe_idx = pi; fds[rfd].flags = 0; fds[rfd].wend = 0;
        fds[wfd].type = 3; fds[wfd].pipe_idx = pi; fds[wfd].flags = 0; fds[wfd].wend = 1;
        serial_puts("[pipe] tab=");
        serial_puthex((uint32_t)(uintptr_t)fdtab);
        serial_puts(" rfd="); serial_puthex((uint32_t)rfd);
        serial_puts(" wfd="); serial_puthex((uint32_t)wfd);
        serial_puts("\r\n");
        out[0] = rfd;
        out[1] = wfd;
        ret = 0;
        break;
    }

    /* ~~ Linux pipe2 (293) ~ "pipe + O_NONBLOCK/O_CLOEXEC!" ~~
     * Mesma coisa do pipe, aplica os flags~ */
    case SYS_pipe2: {
        int *out = (int *)a1;
        int flags = (int)a2;
        if (!out) { ret = -1; break; }
        int pi = -1;
        for (int i = 0; i < MAX_PIPES; i++) if (!pipes[i].used) { pi = i; break; }
        if (pi < 0) { ret = -1; break; }
        int rfd = alloc_fd();
        if (rfd < 0) { ret = -1; break; }
        fds[rfd].used = 1;
        int wfd = alloc_fd();
        if (wfd < 0) { fds[rfd].used = 0; ret = -1; break; }
        fds[wfd].used = 1;
        pipes[pi].used = 1;
        pipes[pi].rpos = pipes[pi].wpos = 0;
        for (int i = 0; i < 4096; i++) pipes[pi].buf[i] = 0;
        fds[rfd].type = 3; fds[rfd].pipe_idx = pi; fds[rfd].flags = flags; fds[rfd].wend = 0;
        fds[wfd].type = 3; fds[wfd].pipe_idx = pi; fds[wfd].flags = flags; fds[wfd].wend = 1;
        out[0] = rfd;
        out[1] = wfd;
        ret = 0;
        break;
    }

    /* ~~ sockets AF_UNIX (issue #49) ~~
     * A base pro wayland/X11: eles conversam por unix socket
     * (/run/wayland-0, /tmp/.X11-unix/X0). A implementação é um
     * "socket em memória": cada socket tem um buffer (como um pipe)
     * e um peer. write grava no buffer do peer, read lê o próprio.
     * SCM_RIGHTS (passar fd) é aceito mas não passa fd de verdade
     * (fds são por-processo — num kernel single-user nem precisamos). */

    case SYS_socket: {
        int domain = (int)a1;
        int type = (int)a2;
        int proto = (int)a3;
        (void)proto;
        if (domain != AF_UNIX) { ret = -1; break; }
        int s = sock_alloc();
        if (s < 0) { ret = -1; break; }
        int fd = alloc_fd();
        if (fd < 0) { socks[s].used = 0; ret = -1; break; }
        socks[s].used = 1;
        socks[s].listening = 0;
        socks[s].peer = -1;
        socks[s].nonblock = 0;
        socks[s].npending = 0;
        socks[s].has_fd = 1;
        socks[s].anc_n = 0;
        socks[s].rpos = socks[s].wpos = 0;
        socks[s].path[0] = '\0';
        fds[fd].used = 1;
        fds[fd].type = 4;
        fds[fd].sock_idx = s;
        fds[fd].flags = (type & O_NONBLOCK) ? O_NONBLOCK : 0;
        ret = fd;
        break;
    }

    case SYS_bind: {
        int fd = (int)a1;
        const char *path = sock_path_from_addr(a2, a3);
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used || fds[fd].type != 4 || !path) {
            ret = -1; break;
        }
        int s = fds[fd].sock_idx;
        if (s < 0 || s >= MAX_SOCKS) { ret = -1; break; }
        int i = 0;
        while (path[i] && i < 255) { socks[s].path[i] = path[i]; i++; }
        socks[s].path[i] = '\0';
        ret = 0;
        break;
    }

    case SYS_listen: {
        int fd = (int)a1;
        int backlog = (int)a2;
        (void)backlog;
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used || fds[fd].type != 4) {
            ret = -1; break;
        }
        int s = fds[fd].sock_idx;
        if (s >= 0 && s < MAX_SOCKS) socks[s].listening = 1;
        ret = 0;
        break;
    }

    case SYS_connect: {
        int fd = (int)a1;
        const char *path = sock_path_from_addr(a2, a3);
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used || fds[fd].type != 4 || !path) {
            ret = -1; break;
        }
        int s = fds[fd].sock_idx;
        int srv = sock_by_path(path);
        serial_puts("[sock] connect path="); serial_puts(path);
        serial_puts(" cli="); serial_puthex((uint32_t)s);
        serial_puts(" srv="); serial_puthex((uint32_t)srv);
        serial_puts("\r\n");
        if (s < 0 || s >= MAX_SOCKS || srv < 0) { ret = -1; break; }
        if (socks[srv].npending >= SOCK_QUEUE) { ret = -1; break; }
        /* ~~ buffer POR CONEXAO: o lado do servidor ganha um slot
         * proprio AGORA, no connect. Sem isso, o cliente gravava no
         * buffer do LISTENER — que e compartilhado por todos os
         * pending — e os writes de varios clientes se misturavam
         * (o Xorg lia os 12 bytes do setup trocados e derrubava a
         * conexao: XOpenDisplay travava no fvwm2 e no xterm). ~~ */
        int ss = sock_alloc();
        if (ss < 0) { ret = -1; break; }
        socks[ss].used = 1;
        socks[ss].listening = 0;
        socks[ss].has_fd = 0;          /* vira 1 no accept */
        socks[ss].peer = s;
        socks[ss].nonblock = 0;
        socks[ss].npending = 0;
        socks[ss].anc_n = 0;
        socks[ss].rpos = socks[ss].wpos = 0;
        socks[ss].path[0] = '\0';
        socks[s].peer = ss;            /* cliente ja fala com o slot */
        socks[srv].pending[socks[srv].npending++] = ss;
        serial_puts("[sock] pending ss="); serial_puthex((uint32_t)ss);
        serial_puts(" -> cli="); serial_puthex((uint32_t)s);
        serial_puts(" srv="); serial_puthex((uint32_t)srv);
        serial_puts(" npend="); serial_puthex((uint32_t)socks[srv].npending);
        serial_puts(" lis="); serial_puthex((uint32_t)socks[srv].listening);
        serial_puts("\r\n");
        ret = 0;
        break;
    }

    case SYS_accept4:
    case SYS_accept: {
        int fd = (int)a1;
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used || fds[fd].type != 4) {
            { static int a1c; if (a1c < 8) { a1c++;
                extern void serial_puthex(uint32_t v);
                serial_puts("[acc-1] fd="); serial_puthex((uint32_t)fd);
                serial_puts(" used="); serial_puthex(fd>=3&&fd<MAX_FDS?(uint32_t)fds[fd].used:9);
                serial_puts(" type="); serial_puthex(fd>=3&&fd<MAX_FDS?(uint32_t)fds[fd].type:9);
                serial_puts("\r\n"); } }
            ret = -1; break;
        }
        int s = fds[fd].sock_idx;
        if (s < 0 || s >= MAX_SOCKS || !socks[s].listening) {
            { static int a2c; if (a2c < 8) { a2c++;
                extern void serial_puthex(uint32_t v);
                serial_puts("[acc-2] fd="); serial_puthex((uint32_t)fd);
                serial_puts(" s="); serial_puthex((uint32_t)s);
                serial_puts("\r\n"); } }
            ret = -1; break;
        }
        /* ~~ accept bloqueante: espera conexao na fila ate alguem
         * conectar (ou O_NONBLOCK -> EAGAIN)~~ */
        while (socks[s].npending <= 0) {
            if (fds[fd].flags & O_NONBLOCK) {
                { static int a3c; if (a3c < 8) { a3c++;
                    extern void serial_puthex(uint32_t v);
                    serial_puts("[acc-3 eagain] fd="); serial_puthex((uint32_t)fd);
                    serial_puts(" s="); serial_puthex((uint32_t)s);
                    serial_puts("\r\n"); } }
                ret = (uint64_t)-11; break;
            }
            schedule();
        }
        if (socks[s].npending <= 0) break;
        /* tira o primeiro cliente da fila (ja e um slot proprio!) */
        int ns = socks[s].pending[0];
        if (ns < 0 || ns >= MAX_SOCKS || !socks[ns].used) {
            { static int a4c; if (a4c < 8) { a4c++;
                extern void serial_puthex(uint32_t v);
                serial_puts("[acc-4 ns]"); serial_puthex((uint32_t)ns);
                serial_puts(" npend="); serial_puthex((uint32_t)socks[s].npending);
                serial_puts("\r\n"); } }
            ret = -1; break;
        }
        int nfd = alloc_fd();
        if (nfd < 0) {
            { static int a5c; if (a5c < 8) { a5c++;
                extern void serial_puthex(uint32_t v);
                serial_puts("[acc-5 nofd]\r\n"); } }
            ret = -1; break;
        }   /* falha: continua na fila */
        for (int i = 1; i < socks[s].npending; i++) socks[s].pending[i - 1] = socks[s].pending[i];
        socks[s].npending--;
        socks[ns].has_fd = 1;
        int cli = socks[ns].peer;
        serial_puts("[sock] accept listener="); serial_puthex((uint32_t)s);
        serial_puts(" cli="); serial_puthex((uint32_t)cli);
        serial_puts(" ss="); serial_puthex((uint32_t)ns);
        serial_puts(" lbuf_r="); serial_puthex(socks[s].rpos);
        serial_puts(" lbuf_w="); serial_puthex(socks[s].wpos);
        serial_puts(" npending="); serial_puthex((uint32_t)socks[s].npending);
        serial_puts(" cli_r="); serial_puthex(socks[ns].rpos);
        serial_puts(" cli_w="); serial_puthex(socks[ns].wpos);
        serial_puts("\r\n");
        /* o slot do servidor ja existe (alocado no connect) e ja tem
         * os bytes do cliente no proprio buffer — so falta o fd. */
        fds[nfd].used = 1;
        fds[nfd].type = 4;
        fds[nfd].sock_idx = ns;
        fds[nfd].flags = (num == SYS_accept4 && (a4 & O_NONBLOCK)) ? O_NONBLOCK : 0;
        ret = nfd;
        break;
    }

    case SYS_sendto:
    case SYS_sendmsg: {
        int fd = (int)a1;
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used || fds[fd].type != 4) {
            ret = -1; break;
        }
        int s = fds[fd].sock_idx;
        if (s < 0 || s >= MAX_SOCKS) { ret = -1; break; }
        int peer = socks[s].peer;
        if (num == SYS_sendmsg) {
            struct linux_iovec { void *base; uint64_t len; };
            struct linux_msghdr {
                void *name;
                uint32_t namelen;
                uint32_t pad;
                struct linux_iovec *iov;
                uint64_t iovlen;
                void *control;
                uint64_t controllen;
                uint32_t flags;
                uint32_t tail_pad;
            } *message = (void *)a2;
            int msg_flags = (int)a3;
            int is_nonblock_send = (fds[fd].flags & O_NONBLOCK) || (msg_flags & MSG_DONTWAIT);
            if (!message || !message->iov || message->iovlen > 16) {
                ret = (uint64_t)-VFS_EINVAL;
                break;
            }
            // SCM_RIGHTS: 1 fd por mensagem (enfileirado; ver struct)
            {   /* FASE5-DBG temporário: por que o fd do pool não chega? */
                static int nscm = 0;
                if (nscm < 10) {
                    nscm++;
                    extern void serial_puts(const char *s);
                    extern void serial_puthex(uint32_t v);
                    serial_puts("[scm] ctl="); serial_puthex((uint32_t)(uintptr_t)message->control);
                    serial_puts(" clen="); serial_puthex((uint32_t)message->controllen);
                    if (message->control && message->controllen >= 16) {
                        struct { size_t l; int lv; int ty; } *cg = (void*)message->control;
                        serial_puts(" cglen="); serial_puthex((uint32_t)cg->l);
                        serial_puts(" lv="); serial_puthex((uint32_t)cg->lv);
                        serial_puts(" ty="); serial_puthex((uint32_t)cg->ty);
                        int sfd = *(int*)((uint8_t*)cg + 16);
                        serial_puts(" sfd="); serial_puthex((uint32_t)sfd);
                        serial_puts(" ftype="); serial_puthex((sfd>=0&&sfd<MAX_FDS&&fds[sfd].used)?(uint32_t)fds[sfd].type:99);
                        serial_puts(" peer="); serial_puthex((uint32_t)peer);
                        if (sfd>=0&&sfd<MAX_FDS&&fds[sfd].used&&fds[sfd].type==FD_MEMFD) {
                            int mzz = fds[sfd].memfd_idx;
                            serial_puts(" mi="); serial_puthex((uint32_t)mzz);
                            serial_puts(" mused="); serial_puthex((mzz>=0&&mzz<MAX_MEMFD)?(uint32_t)g_memfds[mzz].used:99);
                            serial_puts(" pu="); serial_puthex((peer>=0&&peer<MAX_SOCKS)?(uint32_t)socks[peer].used:99);
                        }
                    }
                    serial_puts("\r\n");
                }
            }
            if (message->control && message->controllen >= 16) {
                struct { size_t cmsg_len; int cmsg_level; int cmsg_type; } *cmsg = (void*)message->control;
                /* FASE5-fix: 1 sendmsg pode carregar N fds (foot manda 2
                 * create_pool num flush só: clen=24 = 2 fds). Antes só o
                 * fdarr[0] era enfileirado e o 2º pool morria com
                 * "file descriptor expected". Enfileira todos. */
                if (cmsg->cmsg_level==1 && cmsg->cmsg_type==1 && cmsg->cmsg_len >= 20) {
                    size_t nfds = (cmsg->cmsg_len - 16) / 4;
                    size_t maxfds = ((size_t)message->controllen - 16) / 4;
                    if (nfds > maxfds) nfds = maxfds;
                    int *fdarr = (int*)((uint8_t*)cmsg + 16);
                    for (size_t fi = 0; fi < nfds; fi++) {
                    int sendfd = fdarr[fi];
                    if (sendfd>=0 && sendfd<MAX_FDS && fds[sendfd].used) {
                        int peer2 = peer;
                        if (peer2>=0 && peer2<MAX_SOCKS && socks[peer2].used) {
                            if (fds[sendfd].type==FD_MEMFD) {
                                int mi=fds[sendfd].memfd_idx;
                                if(mi>=0&&mi<MAX_MEMFD&&g_memfds[mi].used){
                                    if (socks[peer2].anc_n < SOCK_ANC_N) {
                                        socks[peer2].anc_mi[socks[peer2].anc_n++] = mi;
                                        /* FASE5-shm: sem share aqui — o
                                         * objeto do memfd é global e o
                                         * close de fd não o destrói; o
                                         * share antigo só vazava um
                                         * snapshot por fd enviado. A
                                         * posse do receptor nasce no
                                         * mmap (apelido) do lado de lá. */
                                    } else {
                                        extern void serial_puts(const char *s);
                                        serial_puts("[sock] anc cheio, descartando\r\n");
                                    }
                                }
                            }
                        }
                    }
                    } /* for fi */
                }
            }
            int nonblk = is_nonblock_send;
            uint64_t total = 0;
            int failed = 0;
            for (uint64_t index = 0; index < message->iovlen; index++) {
                int sent = sock_write_all(peer,
                    (const uint8_t *)message->iov[index].base,
                    (int)message->iov[index].len, nonblk);
                if (sent < 0) { failed = sent; break; }
                total += (uint32_t)sent;
                if ((uint64_t)sent < message->iov[index].len) break;
            }
            ret = total ? total : (uint64_t)failed;
        } else {
            ret = sock_write_all(peer, (const uint8_t *)a2, (int)a3,
                                 fds[fd].flags & O_NONBLOCK);
        }
        break;
    }

    case SYS_recvfrom:
    case SYS_recvmsg: {
        int fd = (int)a1;
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used || fds[fd].type != 4) {
            ret = -1; break;
        }
        int s = fds[fd].sock_idx;
        if (s < 0 || s >= MAX_SOCKS) { ret = -1; break; }
        int peer_ok = socks[s].peer >= 0 && socks[s].peer < MAX_SOCKS &&
                      socks[socks[s].peer].used;
        if (num == SYS_recvmsg) {
            struct linux_iovec { void *base; uint64_t len; };
            struct linux_msghdr {
                void *name;
                uint32_t namelen;
                uint32_t pad;
                struct linux_iovec *iov;
                uint64_t iovlen;
                void *control;
                uint64_t controllen;
                uint32_t flags;
                uint32_t tail_pad;
            } *message = (void *)a2;
            {   /* FASE5-DBG temporário: o SCM chega na leitura? */
                static int nrr = 0;
                if (nrr < 60) {
                    nrr++;
                    extern void serial_puts(const char *s);
                    extern void serial_puthex(uint32_t v);
                    serial_puts("[rr] anc_n="); serial_puthex((uint32_t)socks[s].anc_n);
                    serial_puts(" ctl="); serial_puthex((uint32_t)(uintptr_t)message->control);
                    serial_puts(" clen_in="); serial_puthex((uint32_t)message->controllen);
                    serial_puts(" rpos="); serial_puthex((uint32_t)socks[s].rpos);
                    serial_puts(" wpos="); serial_puthex((uint32_t)socks[s].wpos);
                    serial_puts("\r\n");
                }
            }
            int msg_flags = (int)a3;
            int is_nonblock = (fds[fd].flags & O_NONBLOCK) || (msg_flags & MSG_DONTWAIT);
            if (!message || !message->iov || message->iovlen > 16) {
                ret = (uint64_t)-VFS_EINVAL;
                break;
            }
            while (socks[s].rpos == socks[s].wpos && peer_ok && !is_nonblock)
                schedule();
            if (socks[s].rpos == socks[s].wpos) {
                ret = is_nonblock ? (uint64_t)-11 : 0;
                break;
            }
            uint64_t total = 0;
            for (uint64_t index = 0; index < message->iovlen; index++) {
                int received = sock_pop(s,
                    (uint8_t *)message->iov[index].base,
                    (int)message->iov[index].len);
                if (received < 0) { ret = total ? total : (uint64_t)received; break; }
                total += (uint32_t)received;
                if ((uint64_t)received < message->iov[index].len) break;
            }
            if (socks[s].anc_n > 0 && message->control && message->controllen >= 20) {
                struct { size_t cmsg_len; int cmsg_level; int cmsg_type; } *cmsg2 = (void*)message->control;
                /* FASE5-fix: entrega TODOS os pendentes que couberem no
                 * buffer do leitor (antes: 1 fd por recvmsg — com 2 fds
                 * num sendmsg só e leitura em 1 recvmsg, o 2º pool do
                 * foot ficava sem fd). cmsg_len = 16+4*n. */
                size_t room = ((size_t)message->controllen - 16) / 4;
                int want = socks[s].anc_n;
                if ((size_t)want > room) want = (int)room;
                cmsg2->cmsg_len = 16;
                cmsg2->cmsg_level = 1;
                cmsg2->cmsg_type = 1;
                int *fdarr2 = (int*)((uint8_t*)cmsg2 + 16);
                int got = 0;
                int fail = 0;
                for (int gi = 0; gi < want; gi++) {
                    int recvfd2 = alloc_fd();
                    if (recvfd2 < 0) { if (got == 0) fail = 1; break; }
                    /* FIFO: o fd mais antigo da fila pertence a ESTA msg */
                    int mi2 = socks[s].anc_mi[0];
                    for (int aq = 1; aq < socks[s].anc_n; aq++)
                        socks[s].anc_mi[aq - 1] = socks[s].anc_mi[aq];
                    socks[s].anc_n--;
                    {   /* FASE5-DBG temporário: o que preenchemos no cmsg? */
                        extern void serial_puts(const char *s);
                        extern void serial_puthex(uint32_t v);
                        serial_puts("[rr-fill] mi="); serial_puthex((uint32_t)mi2);
                        serial_puts(" recvfd="); serial_puthex((uint32_t)recvfd2);
                        serial_puts(" clen_in="); serial_puthex((uint32_t)message->controllen);
                        serial_puts("\r\n");
                    }
                    if (mi2 < 0 || mi2 >= MAX_MEMFD || !g_memfds[mi2].used) {
                        fds[recvfd2].used = 0;
                        fail = 1; got = 0; break;
                    }
                    fds[recvfd2].used=1; fds[recvfd2].type=FD_MEMFD; fds[recvfd2].memfd_idx=mi2;
                    fds[recvfd2].pos=0; fds[recvfd2].flags=0;
                    int kk2=0; const char *nm2=g_memfds[mi2].name; while(nm2[kk2]&&kk2<63){fds[recvfd2].name[kk2]=nm2[kk2]; kk2++;} fds[recvfd2].name[kk2]=0;
                    fds[recvfd2].path[0]=0;
                    fds[recvfd2].stat_valid=1; fds[recvfd2].stat_size=(uint32_t)g_memfds[mi2].size; fds[recvfd2].stat_attr=0x20;
                    fdarr2[got]=recvfd2;
                    got++;
                }
                if (fail || got == 0) {
                    message->controllen = 0;
                } else {
                    cmsg2->cmsg_len = 16 + (size_t)got * 4;
                    message->controllen = cmsg2->cmsg_len;
                }
                {   /* FASE5-DBG: releitura do que preenchemos */
                    extern void serial_puts(const char *s);
                    extern void serial_puthex(uint32_t v);
                    serial_puts("[rr-rd] l="); serial_puthex((uint32_t)cmsg2->cmsg_len);
                    serial_puts(" lv="); serial_puthex((uint32_t)cmsg2->cmsg_level);
                    serial_puts(" ty="); serial_puthex((uint32_t)cmsg2->cmsg_type);
                    serial_puts(" n="); serial_puthex((uint32_t)got);
                    serial_puts(" ctl_out="); serial_puthex((uint32_t)message->controllen);
                    serial_puts("\r\n");
                }
            } else {
                if (message->control) message->controllen = 0;
            }
            message->flags = 0;
            if (message->name && message->namelen) message->namelen = 0;
            ret = total;
        } else {
            char *buf = (char *)a2;
            int count = (int)a3;
            while (socks[s].rpos == socks[s].wpos && peer_ok &&
                   !(fds[fd].flags & O_NONBLOCK))
                schedule();
            if (socks[s].rpos == socks[s].wpos &&
                (fds[fd].flags & O_NONBLOCK))
                ret = (uint64_t)-11;
            else
                ret = sock_pop(s, (uint8_t *)buf, count);
        }
        break;
    }

    case SYS_socketpair: {
        int domain = (int)a1;
        int type = (int)a2;
        int *out = (int *)(uintptr_t)a4;
        (void)type;
        if (domain != AF_UNIX || !out) { ret = -1; break; }
        int s1 = sock_alloc();
        int s2 = sock_alloc();
        /* ~~ used ANTES do próximo alloc (mesmo fix do pipe!)~~ */
        int fd1 = alloc_fd();
        if (fd1 < 0) { ret = -1; break; }
        fds[fd1].used = 1;
        int fd2 = alloc_fd();
        if (fd2 < 0) { fds[fd1].used = 0; ret = -1; break; }
        fds[fd2].used = 1;
        if (s1 < 0 || s2 < 0 || fd1 < 0 || fd2 < 0) { ret = -1; break; }
        socks[s1].used = 1; socks[s1].listening = 0; socks[s1].peer = s2;
        socks[s1].nonblock = 0; socks[s1].npending = 0; socks[s1].has_fd = 1; socks[s1].anc_n = 0;
        socks[s1].rpos = socks[s1].wpos = 0; socks[s1].path[0] = '\0';
        socks[s2].used = 1; socks[s2].listening = 0; socks[s2].peer = s1;
        socks[s2].nonblock = 0; socks[s2].npending = 0; socks[s2].has_fd = 1; socks[s2].anc_n = 0;
        socks[s2].rpos = socks[s2].wpos = 0; socks[s2].path[0] = '\0';
        fds[fd1].used = 1; fds[fd1].type = 4; fds[fd1].sock_idx = s1; fds[fd1].flags = 0;
        fds[fd2].used = 1; fds[fd2].type = 4; fds[fd2].sock_idx = s2; fds[fd2].flags = 0;
        out[0] = fd1;
        out[1] = fd2;
        ret = 0;
        break;
    }

    case SYS_shutdown: {
        int fd = (int)a1;
        (void)fd;
        /* só aceita e ignora — sem conexão de verdade pra derrubar~ */
        ret = 0;
        break;
    }

    case SYS_getsockopt: {
        int fd = (int)a1;
        int level = (int)a2;
        int optname = (int)a3;
        void *optval = (void*)a4;
        uint32_t *optlen_ptr = (uint32_t*)(uintptr_t)regs[5];
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used) { ret = (uint64_t)-9; break; }
        if (level == 1 && optname == 17 && optval && optlen_ptr) {
            uint32_t *u = (uint32_t*)optval;
            u[0] = 0; u[1] = 0; u[2] = 0;
            *optlen_ptr = 12;
            ret = 0;
        } else {
            ret = 0;
        }
        break;
    }

    case SYS_setsockopt: {
        ret = 0;
        break;
    }

    case SYS_getsockname: {
        int fd = (int)a1;
        if (fd < 3 || fd >= MAX_FDS || !fds[fd].used || fds[fd].type != 4) {
            ret = -1; break;
        }
        int s = fds[fd].sock_idx;
        /* devolve sockaddr_un { AF_UNIX, path } no buffer do caller */
        uint64_t addr = a2;
        if (addr && s >= 0 && s < MAX_SOCKS) {
            char *dst = (char *)(uintptr_t)addr;
            dst[0] = AF_UNIX;
            dst[1] = 0;
            int i = 0;
            while (socks[s].path[i] && i < 254) { dst[2 + i] = socks[s].path[i]; i++; }
            dst[2 + i] = '\0';
        }
        ret = 0;
        break;
    }

    default:
        ret = -1;
        break;
    }

    if (num == 3) {
        int _fd = (int)regs[4];
        if (_fd >= 3 && _fd < MAX_FDS && fds[_fd].used &&
            str_equal(fds[_fd].name, "/share/X11/xkb/compiled/def.xkm")) {
            static int n9 = 0;
            if (n9 < 8) {
                n9++;
                serial_puts("[rkmret] ");
                serial_puthex((uint32_t)(uint64_t)ret);
                serial_puts("\r\n");
            }
        }
    }
    extern volatile int need_sched;
    if (need_sched && syscall_process && syscall_process->is_user) {
        need_sched = 0;
        schedule();
    }
    g_in_syscall = 0;
    /* ~~ debug fase 7: retorno das syscalls de socket do processo
     * tracejado (fvwm2/xterm) — achar onde o X client desiste ~~ */
    {
        extern int g_tipos_trace_pid;
        pcb_t *tp2 = process_current();
        if (g_tipos_trace_pid > 0 && tp2 && tp2->pid == g_tipos_trace_pid &&
            (num == SYS_read || num == SYS_write || num == SYS_poll ||
             num == SYS_recvfrom || num == SYS_sendto || num == SYS_ioctl ||
             num == SYS_accept || num == 92 || num == 46 || num == 409)) {
            serial_puts("[ret] n="); serial_puthex((uint32_t)num);
            serial_puts(" ret="); serial_puthex((uint32_t)ret);
            serial_puts("\r\n");
        }
    }
    regs[0] = ret;
    if (syscall_process && syscall_process->is_user)
        current_rsp0 = syscall_process->user_rsp;
    if (num != SYS_rt_sigreturn) signal_dispatch_syscall(regs);
    if (syscall_process && syscall_process->is_user)
        current_rsp0 = syscall_process->user_rsp;
}

/* ♥ syscall.c ~ arquivo fofinho do OvsbMkM! kyun~ <3 */
