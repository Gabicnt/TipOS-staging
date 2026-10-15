#include "signal.h"
#include "process.h"
#include "drivers/tty.h"
#include "serial.h"

extern uint64_t current_rsp0;

typedef struct {
    uint64_t return_address;
    uint64_t magic;
    uint64_t registers[15];
    uint64_t user_rsp;
    uint64_t user_rip;
    uint64_t user_rflags;
    uint64_t old_mask;
} tipos_rt_sigframe_t;

#define TIPOS_SIGFRAME_MAGIC 0x5349474652414D45ULL

static int valid_signal(int signal_number) {
    return signal_number >= 1 && signal_number <= TIPOS_NSIG;
}

static uint64_t signal_bit(int signal_number) {
    return 1ULL << (signal_number - 1);
}

static int valid_user_range(uint64_t address, uint64_t size) {
    return address >= 0x10000ULL && address < 0x0000800000000000ULL &&
           size <= 0x0000800000000000ULL - address;
}

static void copy_bytes(void *destination, const void *source, uint64_t size) {
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (uint64_t index = 0; index < size; index++) out[index] = in[index];
}

int signal_sigaction(int signal_number, const tipos_sigaction_t *action,
                     tipos_sigaction_t *old_action, uint64_t sigset_size) {
    pcb_t *process = process_current();
    if (!process || !valid_signal(signal_number) || sigset_size != sizeof(uint64_t) ||
        signal_number == TIPOS_SIGKILL || signal_number == TIPOS_SIGSTOP)
        return -22;
    if (action && !valid_user_range((uint64_t)(uintptr_t)action, sizeof(*action)))
        return -14;
    if (old_action && !valid_user_range((uint64_t)(uintptr_t)old_action, sizeof(*old_action)))
        return -14;

    tipos_sigaction_t *slot = &process->actions[signal_number - 1];
    if (old_action) copy_bytes(old_action, slot, sizeof(*slot));
    if (action) {
        tipos_sigaction_t next;
        copy_bytes(&next, action, sizeof(next));
        if (next.handler > TIPOS_SIG_IGN &&
            !valid_user_range(next.handler, 1)) return -22;
        *slot = next;
    }
    return 0;
}

int signal_sigprocmask(int how, const uint64_t *set, uint64_t *old_set,
                       uint64_t sigset_size) {
    pcb_t *process = process_current();
    if (!process || sigset_size != sizeof(uint64_t)) return -22;
    if (set && !valid_user_range((uint64_t)(uintptr_t)set, sizeof(*set))) return -14;
    if (old_set && !valid_user_range((uint64_t)(uintptr_t)old_set, sizeof(*old_set))) return -14;
    if (old_set) *old_set = process->blocked;
    if (!set) return 0;

    uint64_t mask = *set;
    mask &= ~signal_bit(TIPOS_SIGKILL);
    mask &= ~signal_bit(TIPOS_SIGSTOP);
    if (how == TIPOS_SIG_BLOCK) process->blocked |= mask;
    else if (how == TIPOS_SIG_UNBLOCK) process->blocked &= ~mask;
    else if (how == TIPOS_SIG_SETMASK) process->blocked = mask;
    else return -22;
    return 0;
}

static int queue_signal(pcb_t *process, int signal_number) {
    if (!process || process->state == PROC_EMPTY || process->state == PROC_ZOMBIE)
        return -3;
    if (signal_number == 0) return 0;
    if (!valid_signal(signal_number)) return -22;
    process->pending |= signal_bit(signal_number);
    if (process->state == PROC_BLOCKED) process->state = PROC_READY;
    return 0;
}

int signal_send_pid(int pid, int signal_number) {
    for (int index = 0; index < MAX_PROC; index++) {
        if (pcb_table[index].pid == pid && pcb_table[index].state != PROC_EMPTY)
            return queue_signal(&pcb_table[index], signal_number);
    }
    return -3;
}

int signal_send_pgrp(int pgrp, int signal_number) {
    int found = 0;
    for (int index = 0; index < MAX_PROC; index++) {
        pcb_t *process = &pcb_table[index];
        if (process->state != PROC_EMPTY && process->state != PROC_ZOMBIE &&
            process->pgrp == pgrp) {
            queue_signal(process, signal_number);
            found = 1;
        }
    }
    return found ? 0 : -3;
}

int signal_kill(int pid, int signal_number) {
    if (signal_number < 0 || signal_number > TIPOS_NSIG) return -22;
    {
        extern void serial_puts(const char *s);
        extern void serial_puthex(uint32_t v);
        pcb_t *me = process_current();
        serial_puts("[kill] de pid="); serial_puthex(me ? me->pid : 0xFFFFFFFF);
        serial_puts(" para="); serial_puthex((uint32_t)pid);
        serial_puts(" sig="); serial_puthex((uint32_t)signal_number);
        serial_puts("\r\n");
    }
    if (pid > 0) return signal_send_pid(pid, signal_number);
    if (pid == 0) return signal_send_pgrp(process_current()->pgrp, signal_number);
    if (pid < -1) return signal_send_pgrp(-pid, signal_number);
    int found = 0;
    for (int index = 0; index < MAX_PROC; index++) {
        pcb_t *process = &pcb_table[index];
        if (process->pid > 1 && process->state != PROC_EMPTY &&
            process->state != PROC_ZOMBIE) {
            queue_signal(process, signal_number);
            found = 1;
        }
    }
    return found ? 0 : -3;
}

int signal_tgkill(int tgid, int tid, int signal_number) {
    for (int index = 0; index < MAX_PROC; index++) {
        pcb_t *process = &pcb_table[index];
        if (process->pid == tid && process->state != PROC_EMPTY &&
            (tgid == process->pid || tgid == process->parent_pid || tgid == 0))
            return queue_signal(process, signal_number);
    }
    return -3;
}

int signal_setpgid(int pid, int pgrp) {
    pcb_t *current = process_current();
    if (!current) return -3;
    if (!pid) pid = current->pid;
    if (!pgrp) pgrp = pid;
    for (int index = 0; index < MAX_PROC; index++) {
        pcb_t *target = &pcb_table[index];
        if (target->pid != pid || target->state == PROC_EMPTY) continue;
        if (target != current && target->parent_pid != current->pid) return -1;
        if (target->session != current->session) return -1;
        target->pgrp = pgrp;
        return 0;
    }
    return -3;
}

int signal_setsid(void) {
    pcb_t *current = process_current();
    if (!current) return -3;
    for (int index = 0; index < MAX_PROC; index++)
        if (pcb_table[index].state != PROC_EMPTY &&
            pcb_table[index].pgrp == current->pid) return -1;
    current->session = current->pid;
    current->pgrp = current->pid;
    current->controlling_tty = 0;
    return current->session;
}

int signal_getpgrp(void) {
    pcb_t *current = process_current();
    return current ? current->pgrp : -1;
}

static int default_ignored(int signal_number) {
    return signal_number == TIPOS_SIGCHLD || signal_number == TIPOS_SIGURG ||
           signal_number == TIPOS_SIGWINCH || signal_number == TIPOS_SIGCONT;
}

void signal_dispatch_syscall(uint64_t *registers) {
    pcb_t *process = process_current();
    if (!process || !process->is_user) return;
    uint64_t available = process->pending & ~process->blocked;
    if (!available) return;
    int signal_number = 1;
    while (signal_number <= TIPOS_NSIG && !(available & signal_bit(signal_number)))
        signal_number++;
    if (!valid_signal(signal_number)) return;

    process->pending &= ~signal_bit(signal_number);
    tipos_sigaction_t action = process->actions[signal_number - 1];
    if (action.handler == TIPOS_SIG_IGN ||
        (action.handler == TIPOS_SIG_DFL && default_ignored(signal_number))) return;
    if (action.handler == TIPOS_SIG_DFL) {
        if (signal_number == TIPOS_SIGSTOP || signal_number == TIPOS_SIGTSTP ||
            signal_number == TIPOS_SIGTTIN || signal_number == TIPOS_SIGTTOU) {
            process->state = PROC_BLOCKED;
            return;
        }
        process_exit_current(128 + signal_number);
        return;
    }

    uint64_t user_rsp = process->user_rsp;
    /* ~~ Red zone do x86-64 (128 bytes) ~ o gcc -O2 guarda locais abaixo
     * do RSP (ex: `status` em wait4 a rsp-0x2c). Sem pular essa zona o
     * frame do sinal sobrescrevia a variavel e o teste lixa o status~~ */
    uint64_t frame_base = user_rsp - 128;
    uint64_t frame_address = ((frame_base - sizeof(tipos_rt_sigframe_t) - 8) & ~0xFULL) + 8;
    if (!valid_user_range(frame_address, sizeof(tipos_rt_sigframe_t)) || !action.restorer)
        return;
    serial_puts("[SIG:deliver] n=");
    serial_puthex((uint32_t)signal_number);
    serial_puts(" rsp=");
    serial_puthex((uint32_t)frame_address);
    serial_puts(" rip=");
    serial_puthex((uint32_t)action.handler);
    serial_puts(" probe=");
    serial_puthex((uint32_t)*(volatile uint64_t *)(uintptr_t)(user_rsp + 0x88));
    serial_puts("\r\n");
    tipos_rt_sigframe_t *frame = (tipos_rt_sigframe_t *)(uintptr_t)frame_address;
    frame->return_address = action.restorer;
    frame->magic = TIPOS_SIGFRAME_MAGIC;
    for (int index = 0; index < 15; index++) frame->registers[index] = registers[index];
    frame->user_rsp = user_rsp;
    frame->user_rip = registers[15];
    frame->user_rflags = registers[16];
    frame->old_mask = process->blocked;

    process->blocked |= action.mask;
    if (!(action.flags & TIPOS_SA_NODEFER)) process->blocked |= signal_bit(signal_number);
    registers[4] = (uint64_t)signal_number;
    if (action.flags & TIPOS_SA_SIGINFO) {
        registers[3] = 0;
        registers[2] = 0;
    }
    registers[15] = action.handler;
    process->user_rsp = frame_address;
    current_rsp0 = frame_address;
}

void signal_dispatch_irq(uint64_t *registers) {
    if ((registers[18] & 3) != 3) return;
    pcb_t *process = process_current();
    if (!process || !process->is_user) return;
    uint64_t available = process->pending & ~process->blocked;
    if (!available) return;
    int signal_number = 1;
    while (signal_number <= TIPOS_NSIG && !(available & signal_bit(signal_number)))
        signal_number++;
    if (!valid_signal(signal_number)) return;

    tipos_sigaction_t *action = &process->actions[signal_number - 1];
    if (action->handler == TIPOS_SIG_DFL && !default_ignored(signal_number))
        return;

    /* ~~ Frame do irq0 (idt.asm): [0..14] GPRs na ordem INVERSA do
     * frame da syscall (irq0 empilha rax..r15 → regs[0]=r15,regs[14]=rax;
     * syscall_entry empilha r15..rax → regs[0]=rax,regs[4]=rdi), e o
     * CPU empurra RIP(15) CS(16) RFLAGS(17) RSP(18) SS(19) — 20 qwords,
     * SEM int_no/err. Por isso:
     *   - invertemos os 15 GPRs ao montar `adapted` (senão o signo ia
     *     parar no rbp e o rt_sigreturn devolvia tudo permutado);
     *   - regs[17] é RFLAGS (não RIP) e regs[18] é RSP ✓.
     * E current_rsp0 NÃO é mexido: aqui não tem xchg de syscall — o
     * [current_rsp0] precisa continuar sendo o TOPO DA KSTACK, senão a
     * próxima syscall rodava o kernel em cima da pilha do usuário. ~~ */
    uint64_t saved_rsp0 = current_rsp0;
    uint64_t adapted[17];
    for (int index = 0; index < 15; index++) adapted[index] = registers[14 - index];
    adapted[15] = registers[17];
    adapted[16] = registers[19];
    process->user_rsp = registers[20];
    signal_dispatch_syscall(adapted);
    for (int index = 0; index < 15; index++) registers[14 - index] = adapted[index];
    registers[17] = adapted[15];
    registers[19] = adapted[16];
    registers[20] = process->user_rsp;
    current_rsp0 = saved_rsp0;
}

int signal_rt_sigreturn(uint64_t *registers) {
    uint64_t frame_address = current_rsp0 - sizeof(uint64_t);
    if (!valid_user_range(frame_address, sizeof(tipos_rt_sigframe_t))) return -14;
    const tipos_rt_sigframe_t *frame = (const tipos_rt_sigframe_t *)(uintptr_t)frame_address;
    if (frame->magic != TIPOS_SIGFRAME_MAGIC) return -22;
    serial_puts("[SIG:return] frame=");
    serial_puthex((uint32_t)frame_address);
    serial_puts(" rip=");
    serial_puthex((uint32_t)frame->user_rip);
    serial_puts(" rsp=");
    serial_puthex((uint32_t)frame->user_rsp);
    serial_puts(" rbp=");
    serial_puthex((uint32_t)frame->registers[10]);
    serial_puts(" probe=");
    serial_puthex((uint32_t)*(volatile uint64_t *)(uintptr_t)(frame->user_rsp + 0x88));
    serial_puts("\r\n");
    pcb_t *process = process_current();
    if (!process) return -3;
    uint64_t stack_guard = *(volatile uint64_t *)(uintptr_t)(frame->user_rsp + 0x88);
    uint64_t tls_guard = *(volatile uint64_t *)(uintptr_t)(process->fs_base + 0x28);
    serial_puts("[SIG:guard] stack=");
    serial_puthex((uint32_t)(stack_guard >> 32));
    serial_puthex((uint32_t)stack_guard);
    serial_puts(" tls=");
    serial_puthex((uint32_t)(tls_guard >> 32));
    serial_puthex((uint32_t)tls_guard);
    serial_puts("\r\n");
    for (int index = 0; index < 15; index++) registers[index] = frame->registers[index];
    registers[15] = frame->user_rip;
    registers[16] = frame->user_rflags;
    process->blocked = frame->old_mask;
    current_rsp0 = frame->user_rsp;
    process->user_rsp = frame->user_rsp;
    return (int)registers[0];
}