#include "tty.h"
#include "../../fs/vfs.h"
#include "serial.h"
#include "signal.h"

#define TTY_ENOTTY 25
#define TTY_EAGAIN 11

static tty_t tty_pool[MAX_TTYS * 2];
static tty_pair_t tty_pairs[MAX_TTY_PAIRS];
static volatile int tty_pool_lock;
static volatile int tty_pair_pool_lock;

static void lock_acquire(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1))
        __asm__ volatile("pause");
    __asm__ volatile("" ::: "memory");
}

static void lock_release(volatile int *lock) {
    __asm__ volatile("" ::: "memory");
    __sync_lock_release(lock);
}

static void zero_bytes(void *ptr, size_t size) {
    uint8_t *bytes = (uint8_t *)ptr;
    for (size_t i = 0; i < size; i++)
        bytes[i] = 0;
}

static void ring_reset(tty_ring_t *ring) {
    ring->head = 0;
    ring->tail = 0;
    ring->count = 0;
}

/* FASE6.2-nav: põe 1 byte no read_buffer (chamador segura o lock).
 * Retorna 1 se coube, 0 se cheio. */
static int ring_putc(tty_ring_t *ring, uint8_t ch) {
    if (ring->count >= TTY_BUFFER_SIZE) return 0;
    ring->data[ring->tail] = ch;
    ring->tail = (ring->tail + 1) % TTY_BUFFER_SIZE;
    ring->count++;
    return 1;
}

static tty_t *tty_alloc_role(uint32_t role) {
    lock_acquire(&tty_pool_lock);
    for (uint32_t i = 0; i < MAX_TTYS * 2; i++) {
        tty_t *tty = &tty_pool[i];
        if (tty->flags & TTY_FLAG_ALLOC)
            continue;

        zero_bytes(tty, sizeof(*tty));
        tty->flags = TTY_FLAG_ALLOC | role;
        tty->refcount = 1;
        tty->pair_index = -1;
        if (role == TTY_FLAG_SLAVE) {
            tty->termios.c_lflag = TTY_ICANON | TTY_ECHO | TTY_ISIG;
            /* FASE6-enter: ICRNL por padrão (POSIX/stty sane). O foot
             * faz tcgetattr + OR IUTF8 + tcsetattr — ou seja, HERDA o
             * default; sem ICRNL aqui, o '\r' do Enter nunca virava
             * '\n' e o shell ficava preso (só redesenhava a linha). */
            tty->termios.c_iflag = TTY_ICRNL;
            /* FASE6.2-nav: OPOST|ONLCR por padrão (stty sane/Linux).
             * Sem isso o '\n' do shell/echo chega cru no master e o
             * foot só desce a linha sem voltar pra coluna 0. O foot
             * faz tcgetattr/OR/tcsetattr, então respeita o que ele
             * pedir — mas o default precisa ser o sane. */
            tty->termios.c_oflag = TTY_OPOST | TTY_ONLCR;
            tty->termios.c_cc[0] = 3;
            tty->termios.c_cc[2] = 127;
            tty->termios.c_cc[3] = 21;
            tty->termios.c_cc[4] = 4;
            tty->termios.c_cc[5] = 0;
            tty->termios.c_cc[6] = 1;
            tty->termios.c_cc[TTY_VSUSP] = 26;
        }
        tty->winsize.ws_row = 24;
        tty->winsize.ws_col = 80;
        lock_release(&tty_pool_lock);
        return tty;
    }
    lock_release(&tty_pool_lock);
    return NULL;
}

tty_t *tty_alloc(void) {
    return tty_alloc_role(TTY_FLAG_SLAVE);
}

void tty_free(tty_t *tty) {
    if (!tty || (uintptr_t)tty < (uintptr_t)tty_pool ||
        (uintptr_t)tty >= (uintptr_t)(tty_pool + MAX_TTYS * 2))
        return;

    lock_acquire(&tty_pool_lock);
    if (tty->flags & TTY_FLAG_ALLOC) {
        if (tty->refcount > 1) {
            tty->refcount--;
        } else {
            tty->flags = 0;
            tty->refcount = 0;
            ring_reset(&tty->read_buffer);
            ring_reset(&tty->write_buffer);
        }
    }
    lock_release(&tty_pool_lock);
}

int tty_read(tty_t *tty, void *buffer, size_t count) {
    if (!tty || !(tty->flags & TTY_FLAG_ALLOC) || (count && !buffer))
        return -VFS_EINVAL;
    if (!count) return 0;

    uint8_t *out = (uint8_t *)buffer;
    lock_acquire(&tty->lock);
    if ((tty->flags & TTY_FLAG_SLAVE) &&
        (tty->termios.c_lflag & TTY_ICANON) && tty->canonical_eof &&
        tty->read_buffer.count == 0) {
        tty->canonical_eof = 0;
        lock_release(&tty->lock);
        return 0;
    }
    if (tty->read_buffer.count == 0) {
        lock_release(&tty->lock);
        return -TTY_EAGAIN;
    }

    size_t copied = 0;
    while (copied < count && tty->read_buffer.count > 0) {
        out[copied++] = tty->read_buffer.data[tty->read_buffer.head];
        tty->read_buffer.head = (tty->read_buffer.head + 1) % TTY_BUFFER_SIZE;
        tty->read_buffer.count--;
    }
    lock_release(&tty->lock);
    return (int)copied;
}

int tty_write(tty_t *tty, const void *buffer, size_t count) {
    if (!tty || !(tty->flags & TTY_FLAG_ALLOC) || (count && !buffer))
        return -VFS_EINVAL;
    if (!count) return 0;

    const uint8_t *in = (const uint8_t *)buffer;
    lock_acquire(&tty->lock);
    if (tty->write_buffer.count == TTY_BUFFER_SIZE) {
        lock_release(&tty->lock);
        return -TTY_EAGAIN;
    }

    size_t copied = 0;
    while (copied < count && tty->write_buffer.count < TTY_BUFFER_SIZE) {
        tty->write_buffer.data[tty->write_buffer.tail] = in[copied++];
        tty->write_buffer.tail = (tty->write_buffer.tail + 1) % TTY_BUFFER_SIZE;
        tty->write_buffer.count++;
    }
    lock_release(&tty->lock);
    return (int)copied;
}

int tty_ioctl(tty_t *tty, uint64_t request, void *argument) {
    serial_puts("[tty_ioctl] request=");
    serial_puthex((uint32_t)request);
    serial_puts(" tty=");
    serial_puthex((uint32_t)(uintptr_t)tty);
    serial_puts("\r\n");
    if (!tty || !(tty->flags & TTY_FLAG_ALLOC))
        return -VFS_EINVAL;
    if (request == TTY_TIOCSCTTY || request == TTY_TIOCNOTTY)
        return 0;
    if (request == TTY_TIOCGPTN) {
        if (!(tty->flags & TTY_FLAG_MASTER)) return -TTY_ENOTTY;
        if (!argument) return -VFS_EFAULT;
        int index = tty_pair_index(tty);
        if (index < 0) return -VFS_EIO;
        *(int32_t *)argument = index;
        return 0;
    }
    if (request == TTY_TIOCSPTLCK) {
        if (!(tty->flags & TTY_FLAG_MASTER)) return -TTY_ENOTTY;
        if (!argument) return -VFS_EFAULT;
        return tty_pair_set_locked(tty, *(int32_t *)argument != 0);
    }

    tty_t *target = tty;
    if (tty->flags & TTY_FLAG_MASTER) {
        int index = tty_pair_index(tty);
        tty_pair_t *pair = index >= 0 ? tty_pair_get((uint32_t)index) : NULL;
        if (!pair || !pair->slave) return -VFS_EIO;
        target = pair->slave;
    }

    if (request == TTY_TCGETS) {
        if (!argument) return -VFS_EFAULT;
        lock_acquire(&target->lock);
        *(tty_termios_t *)argument = target->termios;
        lock_release(&target->lock);
        return 0;
    }
    if (request == TTY_TCSETS || request == TTY_TCSETSW ||
        request == TTY_TCSETSF) {
        if (!argument) return -VFS_EFAULT;
        tty_termios_t value = *(const tty_termios_t *)argument;
        return tty_set_termios(target, &value);
    }
    if (request == TTY_TIOCGWINSZ) {
        if (!argument) return -VFS_EFAULT;
        lock_acquire(&target->lock);
        *(tty_winsize_t *)argument = target->winsize;
        lock_release(&target->lock);
        return 0;
    }
    if (request == TTY_TIOCSWINSZ) {
        if (!argument) return -VFS_EFAULT;
        tty_winsize_t value = *(const tty_winsize_t *)argument;
        return tty_set_winsize(target, &value);
    }
    if (request == TTY_TIOCSPGRP) {
        if (!argument) return -VFS_EFAULT;
        return tty_set_fg_pgrp(target, *(const int32_t *)argument);
    }
    if (request == TTY_TIOCGPGRP) {
        if (!argument) return -VFS_EFAULT;
        lock_acquire(&target->lock);
        *(int32_t *)argument = target->foreground_pgrp;
        lock_release(&target->lock);
        return 0;
    }
    return -TTY_ENOTTY;
}

int tty_set_termios(tty_t *tty, const tty_termios_t *termios) {
    if (!tty || !termios || !(tty->flags & TTY_FLAG_ALLOC))
        return -VFS_EINVAL;

    lock_acquire(&tty->lock);
    tty->termios = *termios;
    lock_release(&tty->lock);
    return 0;
}

int tty_set_winsize(tty_t *tty, const tty_winsize_t *winsize) {
    if (!tty || !winsize || !(tty->flags & TTY_FLAG_ALLOC))
        return -VFS_EINVAL;

    lock_acquire(&tty->lock);
    int changed = tty->winsize.ws_row != winsize->ws_row ||
        tty->winsize.ws_col != winsize->ws_col ||
        tty->winsize.ws_xpixel != winsize->ws_xpixel ||
        tty->winsize.ws_ypixel != winsize->ws_ypixel;
    if (changed) tty->flags |= TTY_FLAG_SIGWINCH_PENDING;
    tty->winsize = *winsize;
    int32_t fg = tty->foreground_pgrp;
    if (changed) tty->flags &= ~TTY_FLAG_SIGWINCH_PENDING;
    lock_release(&tty->lock);
    if (changed && fg > 0) {
        serial_puts("[sig] WINCH pgrp=");
        serial_puthex((uint32_t)fg);
        serial_puts("\r\n");
        signal_send_pgrp(fg, TIPOS_SIGWINCH);
    }
    return 0;
}

int tty_set_fg_pgrp(tty_t *tty, int32_t pgrp) {
    if (!tty || !(tty->flags & TTY_FLAG_ALLOC))
        return -VFS_EINVAL;

    lock_acquire(&tty->lock);
    tty->foreground_pgrp = pgrp;
    lock_release(&tty->lock);
    return 0;
}

static tty_t *tty_alloc_master(void) {
    return tty_alloc_role(TTY_FLAG_MASTER);
}

tty_t *tty_pair_alloc(void) {
    tty_pair_t *pair = NULL;
    lock_acquire(&tty_pair_pool_lock);
    for (uint32_t i = 0; i < MAX_TTY_PAIRS; i++) {
        if (!(tty_pairs[i].flags & (TTY_PAIR_FLAG_ALLOC | TTY_PAIR_FLAG_RESERVED))) {
            pair = &tty_pairs[i];
            pair->index = i;
            pair->flags = TTY_PAIR_FLAG_RESERVED;
            pair->master = NULL;
            pair->slave = NULL;
            break;
        }
    }
    lock_release(&tty_pair_pool_lock);
    if (!pair) return NULL;

    tty_t *slave = tty_alloc();
    tty_t *master = tty_alloc_master();
    if (!slave || !master) {
        if (slave) tty_free(slave);
        if (master) tty_free(master);
        lock_acquire(&tty_pair_pool_lock);
        pair->flags = 0;
        lock_release(&tty_pair_pool_lock);
        return NULL;
    }

    slave->pair_index = (int32_t)pair->index;
    master->pair_index = (int32_t)pair->index;
    pair->slave = slave;
    pair->master = master;
    __sync_synchronize();
    pair->flags = TTY_PAIR_FLAG_ALLOC;
    return master;
}

tty_pair_t *tty_pair_get(uint32_t index) {
    if (index >= MAX_TTY_PAIRS ||
        !(tty_pairs[index].flags & TTY_PAIR_FLAG_ALLOC))
        return NULL;
    return &tty_pairs[index];
}

int tty_pair_index(const tty_t *tty) {
    if (!tty || !(tty->flags & TTY_FLAG_ALLOC) || tty->pair_index < 0 ||
        !tty_pair_get((uint32_t)tty->pair_index))
        return -1;
    return tty->pair_index;
}

void tty_pair_free_index(uint32_t index) {
    if (index >= MAX_TTY_PAIRS) return;

    lock_acquire(&tty_pair_pool_lock);
    tty_pair_t *pair = &tty_pairs[index];
    if (!(pair->flags & TTY_PAIR_FLAG_ALLOC)) {
        lock_release(&tty_pair_pool_lock);
        return;
    }
    tty_t *master = pair->master;
    tty_t *slave = pair->slave;
    pair->flags = 0;
    pair->master = NULL;
    pair->slave = NULL;
    lock_release(&tty_pair_pool_lock);

    tty_free(master);
    tty_free(slave);
}

void tty_pair_free(tty_t *master) {
    int index = tty_pair_index(master);
    if (index >= 0 && (master->flags & TTY_FLAG_MASTER))
        tty_pair_free_index((uint32_t)index);
}

int tty_pair_unlock(tty_t *master) {
    return tty_pair_set_locked(master, 0);
}

int tty_pair_set_locked(tty_t *master, int locked) {
    int index = tty_pair_index(master);
    if (index < 0 || !(master->flags & TTY_FLAG_MASTER))
        return -VFS_EINVAL;
    if (locked)
        tty_pairs[index].flags &= ~TTY_PAIR_FLAG_UNLOCKED;
    else
        tty_pairs[index].flags |= TTY_PAIR_FLAG_UNLOCKED;
    return 0;
}

int tty_pair_name(const tty_t *tty, char *buffer, size_t size) {
    int index = tty_pair_index(tty);
    if (index < 0 || !buffer || size < 12) return -VFS_EINVAL;

    static const char prefix[] = "/dev/pts/";
    size_t pos = 0;
    while (prefix[pos]) {
        buffer[pos] = prefix[pos];
        pos++;
    }
    char digits[10];
    size_t digit_count = 0;
    uint32_t value = (uint32_t)index;
    do {
        digits[digit_count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && digit_count < sizeof(digits));
    while (digit_count) buffer[pos++] = digits[--digit_count];
    buffer[pos] = '\0';
    return (int)pos;
}

int tty_pair_read(tty_t *tty, void *buffer, size_t count) {
    return tty_read(tty, buffer, count);
}

int tty_pair_write(tty_t *tty, const void *buffer, size_t count) {
    int index = tty_pair_index(tty);
    if (index < 0 || (count && !buffer)) return -VFS_EINVAL;
    if (!count) return 0;

    tty_pair_t *pair = tty_pair_get((uint32_t)index);
    if (!pair || !pair->master || !pair->slave) return -VFS_EIO;

    tty_t *master = pair->master;
    tty_t *slave = pair->slave;
    const uint8_t *in = (const uint8_t *)buffer;
    /* ~~ espelho no serial: tudo que atravessa o PTY (input do usuário
     * escrito no master + output do shell escrito no slave) também vai
     * pro /tmp/tipos.log do host — debug do QEMU sem depender do
     * console no fbdev, que está desligado enquanto o Xorg roda. ~~ */
    for (size_t mi = 0; mi < count; mi++) serial_putc((char)in[mi]);
    if (tty->flags & TTY_FLAG_SLAVE) {
        /* FASE6.2-nav: OPOST/ONLCR na saída (shell -> foot). Expande
         * '\n' -> '\r\n' (CRLF atômico: sem 2 slots, para e o
         * chamador retenta). Sem isso o foot só desce a linha. */
        uint32_t oflag = slave->termios.c_oflag;
        int onlcr = (oflag & TTY_OPOST) && (oflag & TTY_ONLCR);
        lock_acquire(&master->lock);
        size_t copied = 0;
        while (copied < count) {
            uint8_t ch = in[copied];
            if (ch == '\n' && onlcr &&
                master->read_buffer.count + 1 >= TTY_BUFFER_SIZE) break;
            if (ch == '\n' && onlcr) ring_putc(&master->read_buffer, '\r');
            if (!ring_putc(&master->read_buffer, ch)) break;
            copied++;
        }
        lock_release(&master->lock);
        return copied ? (int)copied : -11;
    }

    lock_acquire(&slave->lock);
    tty_termios_t *termios = &slave->termios;
    int canonical = (termios->c_lflag & TTY_ICANON) != 0;
    int echo = (termios->c_lflag & TTY_ECHO) != 0;
    int isig = (termios->c_lflag & TTY_ISIG) != 0;
    size_t consumed = 0;
    for (; consumed < count; consumed++) {
        uint8_t ch = in[consumed];
        /* FASE6-enter: tradução CR/LF (POSIX). O foot manda '\r'
         * no Enter; sem ICRNL a linha nunca completa ('\n') e o
         * shell fica preso (o '\r' ecoado só redesenha a linha). */
        if (ch == '\r') {
            if (termios->c_iflag & TTY_IGNCR) continue;
            if (termios->c_iflag & TTY_ICRNL) ch = '\n';
        } else if (ch == '\n' && (termios->c_iflag & TTY_INLCR)) {
            ch = '\r';
        }
        if (isig && ch == termios->c_cc[TTY_VINTR]) {
            slave->flags |= TTY_FLAG_SIGINT_PENDING;
            int32_t fg = slave->foreground_pgrp;
            slave->flags &= ~TTY_FLAG_SIGINT_PENDING;
            if (fg > 0) signal_send_pgrp(fg, TIPOS_SIGINT);
            continue;
        }
        if (isig && ch == termios->c_cc[TTY_VSUSP]) {
            slave->flags |= TTY_FLAG_SIGTSTP_PENDING;
            int32_t fg = slave->foreground_pgrp;
            slave->flags &= ~TTY_FLAG_SIGTSTP_PENDING;
            if (fg > 0) signal_send_pgrp(fg, TIPOS_SIGTSTP);
            continue;
        }

        if (canonical && ch == termios->c_cc[TTY_VERASE]) {
            if (slave->line_count) {
                slave->line_count--;
                if (echo) {
                    static const uint8_t erase_echo[] = {'\b', ' ', '\b'};
                    lock_acquire(&master->lock);
                    for (size_t i = 0; i < sizeof(erase_echo) &&
                         master->read_buffer.count < TTY_BUFFER_SIZE; i++) {
                        master->read_buffer.data[master->read_buffer.tail] = erase_echo[i];
                        master->read_buffer.tail = (master->read_buffer.tail + 1) % TTY_BUFFER_SIZE;
                        master->read_buffer.count++;
                    }
                    lock_release(&master->lock);
                }
            }
            continue;
        }

        if (canonical && ch == termios->c_cc[TTY_VEOF]) {
            for (uint16_t i = 0; i < slave->line_count; i++) {
                if (slave->read_buffer.count == TTY_BUFFER_SIZE) break;
                slave->read_buffer.data[slave->read_buffer.tail] = slave->line_buffer[i];
                slave->read_buffer.tail = (slave->read_buffer.tail + 1) % TTY_BUFFER_SIZE;
                slave->read_buffer.count++;
            }
            slave->canonical_eof = 1;
            slave->line_count = 0;
            continue;
        }

        if (echo) {
            /* FASE6.2-nav: eco também passa por ONLCR (igual Linux:
             * o '\n' do Enter ecoa como '\r\n', senão o cursor desce
             * sem voltar pra coluna 0). Sem 2 slots, descarta o eco
             * (pressão no buffer — comportamento padrão). */
            lock_acquire(&master->lock);
            uint32_t eflag = termios->c_oflag;
            if (ch == '\n' && (eflag & TTY_OPOST) && (eflag & TTY_ONLCR)) {
                if (master->read_buffer.count + 1 < TTY_BUFFER_SIZE) {
                    ring_putc(&master->read_buffer, '\r');
                    ring_putc(&master->read_buffer, '\n');
                }
            } else {
                ring_putc(&master->read_buffer, ch);
            }
            lock_release(&master->lock);
        }

        if (!canonical) {
            if (slave->read_buffer.count == TTY_BUFFER_SIZE) break;
            slave->read_buffer.data[slave->read_buffer.tail] = ch;
            slave->read_buffer.tail = (slave->read_buffer.tail + 1) % TTY_BUFFER_SIZE;
            slave->read_buffer.count++;
            continue;
        }

        if (slave->line_count < TTY_LINE_BUFFER_SIZE)
            slave->line_buffer[slave->line_count++] = ch;
        if (ch == '\n' || slave->line_count == TTY_LINE_BUFFER_SIZE) {
            for (uint16_t i = 0; i < slave->line_count; i++) {
                if (slave->read_buffer.count == TTY_BUFFER_SIZE) break;
                slave->read_buffer.data[slave->read_buffer.tail] = slave->line_buffer[i];
                slave->read_buffer.tail = (slave->read_buffer.tail + 1) % TTY_BUFFER_SIZE;
                slave->read_buffer.count++;
            }
            slave->line_count = 0;
        }
    }
    lock_release(&slave->lock);
    return consumed ? (int)consumed : -11;
}

int tty_pair_readable(const tty_t *tty) {
    if (!tty || !(tty->flags & TTY_FLAG_ALLOC)) return 0;
    tty_t *mutable_tty = (tty_t *)tty;
    lock_acquire(&mutable_tty->lock);
    int ready = mutable_tty->read_buffer.count != 0;
    lock_release(&mutable_tty->lock);
    return ready;
}

int tty_pair_writable(const tty_t *tty) {
    int index = tty_pair_index(tty);
    if (index < 0) return 0;
    tty_pair_t *pair = tty_pair_get((uint32_t)index);
    tty_t *peer = (tty->flags & TTY_FLAG_MASTER) ? pair->slave : pair->master;
    if (!peer) return 0;
    lock_acquire(&peer->lock);
    int ready = peer->read_buffer.count < TTY_BUFFER_SIZE;
    lock_release(&peer->lock);
    return ready;
}

int tty_pair_readdir(uint64_t cookie, void *entries, uint32_t capacity) {
    if (!entries || !capacity) return -VFS_EINVAL;
    vfs_dirent_t *out = (vfs_dirent_t *)entries;
    uint32_t written = 0;
    for (uint32_t index = (uint32_t)cookie; index < MAX_TTY_PAIRS && written < capacity; index++) {
        if (!tty_pair_get(index)) continue;
        vfs_dirent_t *entry = &out[written++];
        entry->ino = (uint64_t)index + 1;
        entry->next_cookie = (uint64_t)index + 1;
        entry->type = 2;
        uint32_t value = index;
        char reverse[10];
        uint16_t digits = 0;
        do {
            reverse[digits++] = (char)('0' + value % 10);
            value /= 10;
        } while (value && digits < sizeof(reverse));
        entry->name_len = digits;
        for (uint16_t i = 0; i < digits; i++)
            entry->name[i] = reverse[digits - i - 1];
        entry->name[digits] = '\0';
    }
    return (int)written;
}