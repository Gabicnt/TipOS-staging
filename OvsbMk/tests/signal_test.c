typedef unsigned long u64;

typedef struct {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
} kernel_sigaction_t;

static volatile int handled;

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

static void on_signal(int signal_number) {
    if (signal_number == 10) handled++;
}

static void write_text(const char *text, long length) {
    syscall3(1, 1, (long)text, length);
}

int main(void) {
    kernel_sigaction_t action = {
        (u64)on_signal, 0x04000000UL, (u64)restorer, 0
    };
    if (syscall6(13, 10, (long)&action, 0, 8, 0, 0) != 0) return 1;
    long pid = syscall3(39, 0, 0, 0);
    long tid = syscall3(186, 0, 0, 0);
    if (syscall3(234, pid, tid, 10) != 0 || handled != 1) return 2;

    u64 mask = 1UL << 9;
    if (syscall6(14, 0, (long)&mask, 0, 8, 0, 0) != 0) return 3;
    if (syscall3(234, pid, tid, 10) != 0 || handled != 1) return 4;
    if (syscall6(14, 1, (long)&mask, 0, 8, 0, 0) != 0 || handled != 2)
        return 5;

    write_text("PASS signal handlers/mask\n", 26);
    return 0;
}

__attribute__((noreturn)) void _start(void) {
    long result = main();
    syscall3(60, result, 0, 0);
    for (;;) __asm__ volatile ("hlt");
}