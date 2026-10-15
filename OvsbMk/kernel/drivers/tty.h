#ifndef TTY_H
#define TTY_H

#include <stddef.h>
#include <stdint.h>

#define MAX_TTYS 64
#define MAX_TTY_PAIRS MAX_TTYS
#define TTY_BUFFER_SIZE 4096
#define TTY_NCCS 19

#define TTY_FLAG_MASTER 0x01u
#define TTY_FLAG_SLAVE  0x02u
#define TTY_FLAG_ALLOC  0x04u
#define TTY_FLAG_SIGWINCH_PENDING 0x08u
#define TTY_FLAG_SIGINT_PENDING   0x10u
#define TTY_FLAG_SIGTSTP_PENDING  0x20u

#define TTY_PAIR_FLAG_ALLOC     0x01u
#define TTY_PAIR_FLAG_UNLOCKED  0x02u
#define TTY_PAIR_FLAG_RESERVED  0x80000000u

#define TTY_ICANON 0x0002u
#define TTY_ECHO   0x0008u
#define TTY_ISIG   0x0001u
/* FASE6-enter: tradução CR/LF de entrada (valores Linux) */
#define TTY_INLCR  0x0040u
#define TTY_IGNCR  0x0080u
#define TTY_ICRNL  0x0100u
/* FASE6.2-nav: processamento de saída (valores Linux).
 * Sem ONLCR o '\n' do shell chega cru no foot (só desce a linha,
 * sem voltar pra coluna 0 — cursor "pula e volta"). */
#define TTY_OPOST  0x0001u
#define TTY_ONLCR  0x0004u

#define TTY_VINTR  0u
#define TTY_VQUIT  1u
#define TTY_VERASE 2u
#define TTY_VKILL  3u
#define TTY_VEOF   4u
#define TTY_VTIME  5u
#define TTY_VMIN   6u
#define TTY_VSUSP  10u
#define TTY_LINE_BUFFER_SIZE 256

#define TTY_TCGETS    0x5401u
#define TTY_TCSETS    0x5402u
#define TTY_TCSETSW   0x5403u
#define TTY_TCSETSF   0x5404u
#define TTY_TIOCGWINSZ 0x5413u
#define TTY_TIOCSWINSZ 0x5414u
#define TTY_TIOCGPGRP   0x540Fu
#define TTY_TIOCSPGRP   0x5410u
#define TTY_TIOCGPTN   0x80045430u
#define TTY_TIOCSPTLCK 0x40045431u
#define TTY_TIOCSCTTY  0x540Eu
#define TTY_TIOCNOTTY  0x5422u

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[TTY_NCCS];
} tty_termios_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} tty_winsize_t;

typedef struct {
    uint8_t data[TTY_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} tty_ring_t;

typedef struct tty {
    tty_termios_t termios;
    tty_winsize_t winsize;
    int32_t foreground_pgrp;
    tty_ring_t read_buffer;
    tty_ring_t write_buffer;
    uint8_t line_buffer[TTY_LINE_BUFFER_SIZE];
    uint16_t line_count;
    uint8_t canonical_eof;
    uint32_t flags;
    uint32_t refcount;
    int32_t pair_index;
    volatile int lock;
} tty_t;

typedef struct tty_pair {
    tty_t *master;
    tty_t *slave;
    uint32_t index;
    uint32_t flags;
} tty_pair_t;

_Static_assert(sizeof(tty_termios_t) == 36, "Linux x86-64 termios layout");
_Static_assert(sizeof(tty_winsize_t) == 8, "Linux winsize layout");

tty_t *tty_alloc(void);
void tty_free(tty_t *tty);
int tty_read(tty_t *tty, void *buffer, size_t count);
int tty_write(tty_t *tty, const void *buffer, size_t count);
int tty_ioctl(tty_t *tty, uint64_t request, void *argument);
int tty_set_termios(tty_t *tty, const tty_termios_t *termios);
int tty_set_winsize(tty_t *tty, const tty_winsize_t *winsize);
int tty_set_fg_pgrp(tty_t *tty, int32_t pgrp);
tty_t *tty_pair_alloc(void);
void tty_pair_free(tty_t *master);
void tty_pair_free_index(uint32_t index);
tty_pair_t *tty_pair_get(uint32_t index);
int tty_pair_index(const tty_t *tty);
int tty_pair_unlock(tty_t *master);
int tty_pair_set_locked(tty_t *master, int locked);
int tty_pair_name(const tty_t *tty, char *buffer, size_t size);
int tty_pair_read(tty_t *tty, void *buffer, size_t count);
int tty_pair_write(tty_t *tty, const void *buffer, size_t count);
int tty_pair_readable(const tty_t *tty);
int tty_pair_writable(const tty_t *tty);
int tty_pair_readdir(uint64_t cookie, void *entries, uint32_t capacity);

#endif