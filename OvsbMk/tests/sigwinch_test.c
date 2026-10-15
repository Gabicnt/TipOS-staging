typedef unsigned long u64;

typedef struct {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
} kernel_sigaction_t;

typedef struct {
    unsigned short row, col, xpixel, ypixel;
} winsize_t;

static volatile int window_signal;

static long syscall6(long number, long a1, long a2, long a3,
                     long a4, long a5, long a6) {
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile ("syscall" : "+a"(number)
                      : "D"(a1), "S"(a2), "d"(a3), "r"(r10),
                        "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return number;
}

static long syscall3(long number, long a1, long a2, long a3) {
    return syscall6(number, a1, a2, a3, 0, 0, 0);
}

__attribute__((naked, noreturn)) static void restorer(void) {
    __asm__ volatile ("mov $15, %rax; syscall");
}

static void on_winch(int signal_number) {
    if (signal_number == 28) window_signal++;
}

static void make_slave_path(char *path, int number) {
    const char prefix[] = "/dev/pts/";
    int index = 0;
    while (prefix[index]) { path[index] = prefix[index]; index++; }
    char digits[10];
    int count = 0;
    do { digits[count++] = (char)('0' + number % 10); number /= 10; }
    while (number && count < 10);
    while (count) path[index++] = digits[--count];
    path[index] = 0;
}

int main(void) {
    int master = (int)syscall6(257, (long)-100, (long)"/dev/ptmx", 0x102, 0, 0, 0);
    if (master < 0) return 1;
    int pty_number = -1;
    int unlocked = 0;
    if (syscall3(16, master, 0x80045430, (long)&pty_number) != 0 ||
        syscall3(16, master, 0x40045431, (long)&unlocked) != 0) return 2;

    char path[32];
    make_slave_path(path, pty_number);
    int slave = (int)syscall6(257, (long)-100, (long)path, 0x102, 0, 0, 0);
    if (slave < 0) return 3;
    kernel_sigaction_t action = {
        (u64)on_winch, 0x04000000UL, (u64)restorer, 0
    };
    if (syscall6(13, 28, (long)&action, 0, 8, 0, 0) != 0) return 4;
    long group = syscall3(111, 0, 0, 0);
    if (syscall3(16, slave, 0x5410, (long)&group) != 0) return 5;

    winsize_t size = { 37, 111, 0, 0 };
    if (syscall3(16, slave, 0x5414, (long)&size) != 0 || window_signal != 1)
        return 6;
    syscall3(1, 1, (long)"PASS SIGWINCH/TIOCSWINSZ\n", 24);
    syscall3(3, slave, 0, 0);
    syscall3(3, master, 0, 0);
    return 0;
}

__attribute__((noreturn)) void _start(void) {
    long result = main();
    syscall3(60, result, 0, 0);
    for (;;) __asm__ volatile ("hlt");
}