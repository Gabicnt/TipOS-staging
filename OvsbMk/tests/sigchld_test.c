typedef unsigned long u64;

typedef struct {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
} kernel_sigaction_t;

static volatile int child_signal;

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

static void on_child(int signal_number) {
    if (signal_number == 17) child_signal++;
}

int main(void) {
    kernel_sigaction_t action = {
        (u64)on_child, 0x04000000UL, (u64)restorer, 0
    };
    if (syscall6(13, 17, (long)&action, 0, 8, 0, 0) != 0) return 1;
    long child = syscall3(57, 0, 0, 0);
    if (child < 0) return 2;
    if (child == 0) {
        syscall3(60, 42, 0, 0);
        for (;;) __asm__ volatile ("hlt");
    }

    int status = 0;
    long waited = syscall6(61, child, (long)&status, 1, 0, 0, 0);
    if (waited == 0)
        waited = syscall6(61, child, (long)&status, 0, 0, 0, 0);
    if (waited != child) return 3;
    if (status != (42 << 8)) return 4;
    if (child_signal != 1) return 5;
    syscall3(1, 1, (long)"PASS SIGCHLD/wait4\n", 19);
    return 0;
}

__attribute__((noreturn)) void _start(void) {
    long result = main();
    syscall3(60, result, 0, 0);
    for (;;) __asm__ volatile ("hlt");
}