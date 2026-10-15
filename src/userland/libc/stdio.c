/* moe moe kyun <3 */
/* moe moe kyun <3 */
/* ♥ userland ~ programinha de ring 3, longe do kernel!
 * arquivo: stdio.c ~ funcoes anotadas: 30
 */

// ~~ stdio.c ~~ Libc: entrada e saída padrão~
// Implementa tudo via int 0x80 (syscall). Sem bufferização complexa,
// sem FILE* mágico - só o fd e fé~ ♡
//
// Syscalls usados:
//   read(3), write(4), open(5), close(6), lseek(199)
//   stat(188), fstat(189), kbhit(198), mkdir(136), rmdir(137)

/* ~*~ stdio.c ~*~
 * Hihi, olha esse arquivo aqui~ Que lindo, né? >_<
 * Escrito com muito amor (e gambiarras) pela equipe TipOS!
 * Se quebrar, a culpa é sua~ <3
 *~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>

int errno;

FILE __stdin_file  = { .fd = 0 };
FILE __stdout_file = { .fd = 1 };
FILE __stderr_file = { .fd = 2 };
static FILE _file_pool[16];
static unsigned int _file_cursor;

// ~~ _syscall ~~
// Chamada de syscall via int 0x80 (Linux-style).
// num = número do syscall (traduzido pelo kernel~)
// a1-a4 = argumentos em rdi, rsi, rdx, rcx
// retorno em rax. Usa constraint "=a" pra pegar o resultado~ ☆
/* ~ essa demorou pra debugar, respeita ~ */
static long _syscall(long num, long a1, long a2, long a3, long a4) {
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "c"(a4)
        : "r11", "memory"
    );
    return ret;
}

// ~~ open ~~
// Syscall open(2): abre arquivo no path com flags.
// Traduzido pelo kernel: nosso syscall 5 = TipOS open~
/* ~~ open ~~ */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
int open(const char *path, int flags, ...) {
    return _syscall(2, (long)path, flags, 0, 0);
}

// ~~ close ~~
// System call close(2): libera o file descriptor.
// Importante chamar pra não vazar fd~ (recursos são finitos!)
/* ~~ close ~~ */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int close(int fd) {
    return _syscall(3, fd, 0, 0, 0);
}

/* ~~ Lendo coisinhas~ toma cuidado pra não ler lixo! */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
int read(int fd, void *buf, int count) {
    return _syscall(0, fd, (long)buf, count, 0);
}

/* ~~ Escrevendo~~ não vai corromper nada, vai? >_< */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
int write(int fd, const void *buf, int count) {
    return _syscall(1, fd, (long)buf, count, 0);
}

/* ~~ lseek ~~ */
/* ~ essa demorou pra debugar, respeita ~ */
int lseek(int fd, int offset, int whence) {
    return _syscall(8, fd, offset, whence, 0);
}

/* ~~ unlink ~~ */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int unlink(const char *path) {
    return _syscall(87, (long)path, 0, 0, 0);
}

int remove(const char *path) {
    return unlink(path);
}

int rename(const char *old_path, const char *new_path) {
    return _syscall(264, -100, (long)old_path, -100, (long)new_path);
}

/* ~~ mkdir ~~ */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
int mkdir(const char *path, int mode) {
    (void)mode;
    return _syscall(83, (long)path, mode, 0, 0);
}

/* ~~ rmdir ~~ */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
int rmdir(const char *path) {
    return _syscall(84, (long)path, 0, 0, 0);
}

/* ~~ stat ~~ */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int stat(const char *path, struct stat *buf) {
    return _syscall(4, (long)path, (long)buf, 0, 0);
}

/* ~~ fstat ~~ */
/* ~ essa demorou pra debugar, respeita ~ */
int fstat(int fd, struct stat *buf) {
    return _syscall(5, fd, (long)buf, 0, 0);
}

/* ~~ kbhit ~~ */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int kbhit(void) {
    return _syscall(198, 0, 0, 0, 0);
}

/* ~~ putchar ~~ */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
void putchar(char c) {
    write(1, &c, 1);
}

/* ~~ puts ~~ */
/* ~ essa demorou pra debugar, respeita ~ */
void puts(const char *s) {
    write(1, s, strlen(s));
    write(1, "\n", 1);
}

/* ~~ Pegando o valor~ só confia que ta certo~ */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
char getchar(void) {
    char c;
    read(0, &c, 1);
    return c;
}

/* ~ cuidado que essa aqui morde ~ */
char *gets(char *buf) {
    int i = 0;
    while (1) {
        char c = getchar();
        if (c == '\n') break;
        if (c == '\b') { if (i > 0) i--; continue; }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return buf;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
FILE *fopen(const char *path, const char *mode) {
    (void)mode;
    int fd = open(path, 0);
    if (fd < 0) return 0;
    for (unsigned int i = 0; i < sizeof(_file_pool) / sizeof(_file_pool[0]); i++) {
        unsigned int slot = (_file_cursor + i) % (sizeof(_file_pool) / sizeof(_file_pool[0]));
        if (!_file_pool[slot].in_use) {
            _file_pool[slot].fd = fd;
            _file_pool[slot].eof = 0;
            _file_pool[slot].in_use = 1;
            _file_cursor = (slot + 1) % (sizeof(_file_pool) / sizeof(_file_pool[0]));
            return &_file_pool[slot];
        }
    }
    close(fd);
    return 0;
}

/* ~~ fclose ~~ */
/* ~ essa demorou pra debugar, respeita ~ */
int fclose(FILE *f) {
    if (!f) return -1;
    int result = close(f->fd);
    f->in_use = 0;
    f->eof = 0;
    return result;
}

/* ~~ Lendo coisinhas~ toma cuidado pra não ler lixo! */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int fread(void *buf, unsigned long size, unsigned long count, FILE *f) {
    if (!f || size == 0 || count == 0) return 0;
    int r = read(f->fd, buf, (int)(size * count));
    if (r <= 0) { f->eof = 1; return 0; }
    return (int)((unsigned long)r / size);
}

/* ~~ Escrevendo~~ não vai corromper nada, vai? >_< */
/* ~ essa demorou pra debugar, respeita ~ */
int fwrite(const void *buf, unsigned long size, unsigned long count, FILE *f) {
    if (!f || size == 0 || count == 0) return 0;
    int r = write(f->fd, buf, (int)(size * count));
    if (r <= 0) return 0;
    return (int)((unsigned long)r / size);
}

int fseek(FILE *f, long offset, int whence) {
    if (!f || lseek(f->fd, (int)offset, whence) < 0) return -1;
    f->eof = 0;
    return 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    return lseek(f->fd, 0, 1);
}

int feof(FILE *f) {
    return f ? f->eof : 0;
}

int fflush(FILE *f) {
    (void)f;
    return 0;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
char *fgets(char *buf, int n, FILE *f) {
    int i = 0;
    while (i < n - 1) {
        char c;
        if (read(f->fd, &c, 1) <= 0) { f->eof = 1; break; }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return i > 0 ? buf : 0;
}

/* ~~ fputs ~~ */
/* ~ essa demorou pra debugar, respeita ~ */
int fputs(const char *s, FILE *f) {
    return write(f->fd, s, strlen(s)) > 0 ? 0 : -1;
}

/* ~~ fputc ~~ */
/* ~ essa demorou pra debugar, respeita ~ */
int fputc(int c, FILE *f) {
    char ch = c;
    return write(f->fd, &ch, 1) == 1 ? c : -1;
}

/* ~~ sscanf ~~ */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static int _vsscanf(const char *s, const char *fmt, va_list ap) {
    int matched = 0;
    while (*fmt) {
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            while (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') fmt++;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            continue;
        }
        if (*fmt != '%') {
            if (*s == *fmt) { s++; fmt++; continue; }
            break;
        }
        fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (width == 0) width = 0x7fffffff;
        switch (*fmt) {
            case 'd': case 'i': case 'u': case 'x': case 'X': {
                while (*s == ' ' || *s == '\t' || *s == '\n') s++;
                int sign = 1;
                if (*s == '-') { sign = -1; s++; }
                else if (*s == '+') s++;
                int base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
                unsigned long value = 0;
                int digits = 0;
                while (*s && digits < width) {
                    int digit = -1;
                    if (isdigit(*s)) digit = *s - '0';
                    else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
                    else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
                    if (digit < 0 || digit >= base) break;
                    value = value * (unsigned long)base + (unsigned long)digit;
                    s++; digits++;
                }
                if (!digits) break;
                if (*fmt == 'd' || *fmt == 'i') *va_arg(ap, int *) = sign * (int)value;
                else *va_arg(ap, unsigned *) = (unsigned)value;
                matched++;
                fmt++;
                break;
            }
            case 'c': {
                char *v = va_arg(ap, char *);
                if (!*s) break;
                *v = *s++;
                matched++;
                fmt++;
                break;
            }
            case 's': {
                char *v = va_arg(ap, char *);
                while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
                int count = 0;
                while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && count < width)
                    v[count++] = *s++;
                if (!count) break;
                v[count] = '\0';
                matched++;
                fmt++;
                break;
            }
            case '[': {
                unsigned char accepted[256] = {0};
                int invert = 0;
                char *v = va_arg(ap, char *);
                fmt++;
                if (*fmt == '^') { invert = 1; fmt++; }
                while (*fmt && *fmt != ']') accepted[(unsigned char)*fmt++] = 1;
                if (*fmt == ']') fmt++;
                int count = 0;
                while (*s && count < width && ((accepted[(unsigned char)*s] != 0) != invert))
                    v[count++] = *s++;
                if (!count) break;
                v[count] = '\0';
                matched++;
                break;
            }
            default:
                fmt++;
                break;
        }
    }
    return matched;
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int matched = _vsscanf(s, fmt, ap);
    va_end(ap);
    return matched;
}

int fscanf(FILE *f, const char *fmt, ...) {
    if (!f) return EOF;
    char line[512];
    int length = 0;
    char ch;
    while (length < (int)sizeof(line) - 1 && read(f->fd, &ch, 1) > 0) {
        line[length++] = ch;
        if (ch == '\n') break;
    }
    if (!length) { f->eof = 1; return EOF; }
    line[length] = '\0';
    va_list ap;
    va_start(ap, fmt);
    int result = _vsscanf(line, fmt, ap);
    va_end(ap);
    return result;
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
static void _print_dec(unsigned long n) {
    char buf[20], *p = buf;
    do { *p++ = '0' + (n % 10); n /= 10; } while (n);
    while (p > buf) putchar(*--p);
}

/* ~ essa demorou pra debugar, respeita ~ */
static void _print_hex(unsigned long n) {
    const char *hex = "0123456789abcdef";
    char buf[20], *p = buf;
    do { *p++ = hex[n & 0xF]; n >>= 4; } while (n);
    while (p > buf) putchar(*--p);
}

/* ~~ Mostrando pro mundo~~ lindo demais! */
/* ~ essa demorou pra debugar, respeita ~ */
static void _format_char(char *buf, int capacity, int *length, char value) {
    if (buf && capacity > 0 && *length < capacity - 1) buf[*length] = value;
    (*length)++;
}

int vsnprintf(char *buf, int n, const char *fmt, va_list ap) {
    int length = 0;
    if (n < 0) return -1;
    while (*fmt) {
        if (*fmt != '%') { _format_char(buf, n, &length, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { _format_char(buf, n, &length, *fmt++); continue; }
        int zero_pad = 0, left = 0;
        while (*fmt == '0' || *fmt == '-') {
            if (*fmt == '0') zero_pad = 1;
            if (*fmt == '-') left = 1;
            fmt++;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') precision = precision * 10 + (*fmt++ - '0');
        }
        int long_flag = 0;
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') {
            if (*fmt == 'l' || *fmt == 'z') long_flag = 1;
            fmt++;
        }
        char conversion = *fmt ? *fmt++ : '\0';
        if (conversion == 's') {
            const char *text = va_arg(ap, const char *);
            if (!text) text = "(null)";
            int count = 0;
            while (text[count] && (precision < 0 || count < precision)) count++;
            int padding = width > count ? width - count : 0;
            if (!left) while (padding-- > 0) _format_char(buf, n, &length, ' ');
            for (int i = 0; i < count; i++) _format_char(buf, n, &length, text[i]);
            if (left) while (padding-- > 0) _format_char(buf, n, &length, ' ');
            continue;
        }
        if (conversion == 'c') {
            int padding = width > 1 ? width - 1 : 0;
            if (!left) while (padding-- > 0) _format_char(buf, n, &length, ' ');
            _format_char(buf, n, &length, (char)va_arg(ap, int));
            if (left) while (padding-- > 0) _format_char(buf, n, &length, ' ');
            continue;
        }
        int is_signed = conversion == 'd' || conversion == 'i';
        int base = (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16 :
                   (conversion == 'o' ? 8 : 10);
        if (!is_signed && conversion != 'u' && conversion != 'x' && conversion != 'X' &&
            conversion != 'o' && conversion != 'p') {
            _format_char(buf, n, &length, '%');
            if (conversion) _format_char(buf, n, &length, conversion);
            continue;
        }
        long signed_value = is_signed ? (long_flag ? va_arg(ap, long) : va_arg(ap, int)) : 0;
        unsigned long value = is_signed ? (unsigned long)(signed_value < 0 ? -signed_value : signed_value) :
                              (long_flag ? va_arg(ap, unsigned long) : va_arg(ap, unsigned));
        int negative = is_signed && signed_value < 0;
        char digits[32];
        int digit_count = 0;
        do {
            int digit = (int)(value % (unsigned long)base);
            digits[digit_count++] = (char)(digit < 10 ? '0' + digit : (conversion == 'X' ? 'A' : 'a') + digit - 10);
            value /= (unsigned long)base;
        } while (value);
        while (precision >= 0 && digit_count < precision) digits[digit_count++] = '0';
        int padding = width > digit_count + negative ? width - digit_count - negative : 0;
        if (!left && !(zero_pad && precision < 0)) while (padding-- > 0) _format_char(buf, n, &length, ' ');
        if (negative) _format_char(buf, n, &length, '-');
        if (!left && zero_pad && precision < 0) while (padding-- > 0) _format_char(buf, n, &length, '0');
        while (digit_count > 0) _format_char(buf, n, &length, digits[--digit_count]);
        if (left) while (padding-- > 0) _format_char(buf, n, &length, ' ');
    }
    if (buf && n > 0) buf[length < n ? length : n - 1] = '\0';
    return length;
}

int snprintf(char *buf, unsigned long n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int result = vsnprintf(buf, (int)n, fmt, ap);
    va_end(ap);
    return result;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!f) return -1;
    char buffer[1024];
    int length = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    if (length > (int)sizeof(buffer) - 1) length = sizeof(buffer) - 1;
    return write(f->fd, buffer, length);
}

/* ~~ Mostrando pro mundo~~ lindo demais! */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, 4096, fmt, ap);
    va_end(ap);
    return n;
}

/* ~~ Mostrando pro mundo~~ lindo demais! */
/* ~ cuidado que essa aqui morde ~ */
int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int result = vfprintf(f, fmt, ap);
    va_end(ap);
    return result;
}

/* ~~ Mostrando pro mundo~~ lindo demais! */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int result = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return result;
}




/* ♥ stdio.c ~ feito com carinho (e uma raiva controlada) ~ kyun! */
