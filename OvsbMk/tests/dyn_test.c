/* dyn_test.c — Fase 4: ELF dinâmico chamando função de .so */
extern int add5(int x);

static long syscall3(long number, long a1, long a2, long a3) {
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile ("syscall" : "+a"(number)
                      : "D"(a1), "S"(a2), "d"(a3), "r"(r10),
                        "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return number;
}

int main(void) {
    if (add5(37) != 42) return 1;
    syscall3(1, 1, (long)"PASS dynlink .so\n", 17);
    return 0;
}

__attribute__((noreturn)) void _start(void) {
    long result = main();
    syscall3(60, result, 0, 0);
    for (;;) __asm__ volatile ("hlt");
}
