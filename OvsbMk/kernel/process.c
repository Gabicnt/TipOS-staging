/* moe moe kyun <3 */
/* moe moe kyun <3 */
/* ♥ kernel stuff ~ aqui é onde o bicho pega de verdade! kyun~
 * arquivo: process.c ~ funcoes anotadas: 17
 */
/* ♥ process.c ~ feito com carinho (e gambiarras) pela equipe TipOS! ♥
 * Se quebrar, a culpa é sua~ <3
 *~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*/

/* ♥ PROCESS ~ gerenciamento de processos lindo! */
/* ♥ PROCESS ~ Gerenciamento de processos fofinho!
 * Agora com scheduler round-robin! PID 0 = idle, PID 1+ = processos!
 * Estados: EMPTY, READY, RUNNING, BLOCKED, ZOMBIE~
 * Cada processo tem sua propria PML4 (isolamento de memoria)!
 * O timer IRQ0 chama schedule() pra dar vez pros outros~ kyun! */

#include "process.h"
#include "tss.h"
#include "memory.h"
#include "serial.h"
#include "console.h"
#include "syscall.h"
#include "vm_map.h"

extern void fds_cleanup(void);

pcb_t pcb_table[MAX_PROC];
int current_pid = 0;
static int pcb_count = 0;
uint32_t dbg_setup_count = 0;
int next_pid = 1;

extern void context_switch(pcb_t *current, pcb_t *next);

/* ~ cuidado que essa aqui morde ~ */
static void idle_entry(void) {
    serial_puts("[PROCESS] Idle rodando~ kyun!\r\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/* ~ essa demorou pra debugar, respeita ~ */
static pcb_t *alloc_pcb(void) {
    if (pcb_count >= MAX_PROC) return 0;
    return &pcb_table[pcb_count++];
}

/* ~ cuidado que essa aqui morde ~ */
static int find_pid(int pid) {
    for (int i = 0; i < MAX_PROC; i++)
        if (pcb_table[i].pid == pid) return i;
    return -1;
}

/* ~~ Inicializando~~ torce pra não dar panic! */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
void process_init(void) {
    serial_puts("[PROC] init~\r\n");
    fds_cleanup();
    for (int i = 0; i < MAX_PROC; i++) {
        for (int j = 0; j < (int)sizeof(pcb_t); j++)
            ((uint8_t *)&pcb_table[i])[j] = 0;
        pcb_table[i].pid = -1;
        pcb_table[i].th_parent_pid = -1;
        pcb_table[i].clear_tid = 0;
        pcb_table[i].state = PROC_EMPTY;
    }
    pcb_count = 0;
    current_pid = 0;
    next_pid = 1;

    /* Processo idle PID 0 */
    pcb_t *idle = alloc_pcb();
    if (!idle) return;

    void *kstack = kmalloc(STACK_SIZE);
    if (!kstack) return;

    idle->pid = 0;
    idle->state = PROC_READY;
    idle->is_user = 0;
    for (int i = 0; "idle"[i]; i++) idle->name[i] = "idle"[i];

    uint64_t *sp = (uint64_t *)((uint8_t *)kstack + STACK_SIZE);
    *--sp = (uint64_t)idle_entry;
    idle->kernel_rsp = (uint64_t)sp;
    idle->rsp0 = (uint64_t)sp;
    idle->pml4 = pml4_get_current();
    idle->parent_pid = 0;
    idle->exit_code = 0;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static void setup_user_stack(pcb_t *pcb, void *entry, void *ustack, uint64_t ustack_size) {
    uint64_t *sp = (uint64_t *)pcb->kernel_rsp;
    /* iretq frame — pushed first, at highest addresses */
    *--sp = 0x23;                          /* SS (user data) */
    *--sp = (uint64_t)ustack + ustack_size; /* RSP (topo da pilha user) */
    *--sp = 0x202;                          /* RFLAGS (IF=1) */
    *--sp = 0x1B;                           /* CS (user code 64-bit) */
    *--sp = (uint64_t)entry;                /* RIP (entry point) */
    /* 15 registradores — pop order: r15,r14,...,rbp,rbx,r11,...,rax
     * push in reverse so rax is at highest address, r15 at lowest */
    *--sp = 0; /* rax */  *--sp = 0; /* rcx */
    *--sp = 0; /* rdx */  *--sp = 0; /* rsi */
    *--sp = 0; /* rdi */  *--sp = 0; /* r8 */
    *--sp = 0; /* r9 */   *--sp = 0; /* r10 */
    *--sp = 0; /* r11 */  *--sp = 0; /* rbx */
    *--sp = 0; /* rbp */  *--sp = 0; /* r12 */
    *--sp = 0; /* r13 */  *--sp = 0; /* r14 */
    *--sp = 0; /* r15 */
    pcb->kernel_rsp = (uint64_t)sp;
}

/* ♥ process_create_user ~ API antiga, mantida pra compatibilidade!
 * Usa a PML4 atual (compartilhada) ~ sem isolamento total! */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
int process_create_user(const char *name, void *entry, void *user_stack, uint64_t user_stack_size) {
    pcb_t *pcb = alloc_pcb();
    if (!pcb) return -1;

    void *kstack = kmalloc(STACK_SIZE);
    if (!kstack) return -1;

    pcb->pid = next_pid++;
    pcb->state = PROC_READY;
    pcb->is_user = 1;
    pcb->in_kern = 0;
    pcb->user_rsp = (uint64_t)user_stack + user_stack_size;
    for (int i = 0; name[i] && i < PROC_NAME_MAX - 1; i++) pcb->name[i] = name[i];

    uint64_t *ksp = (uint64_t *)((uint8_t *)kstack + STACK_SIZE);
    pcb->kernel_rsp = (uint64_t)ksp;
    pcb->rsp0 = (uint64_t)ksp;
    pcb->pml4 = pml4_get_current();
    pcb_t *parent = process_current();
    pcb->parent_pid = parent ? parent->pid : 0;
    pcb->exit_code = 0;
    pcb->program_break = 0x201000;
    pcb->heap_start = 0x201000;
    pcb->vm_map = (struct vm_map *)kmalloc(sizeof(struct vm_map));
    if (pcb->vm_map) vm_map_init((vm_map_t *)pcb->vm_map, 0x10000, 0x7FFFF000ULL);
    pcb->fs_base = 0;
    pcb->th_parent_pid = -1;
    pcb->clear_tid = 0;
    pcb->pgrp = pcb->pid;
    pcb->session = current_pid >= 0 && current_pid < MAX_PROC &&
                   pcb_table[current_pid].state != PROC_EMPTY
        ? pcb_table[current_pid].session : pcb->pid;

    pcb->cwd[0] = '/'; pcb->cwd[1] = '\0';
    setup_user_stack(pcb, entry, user_stack, user_stack_size);
    return pcb->pid;
}

/* ♥ proc_spawn ~ API nova do TipOS! Cria processo com PML4 propria!
 * entry = endereco virtual do codigo, user_stack_top = topo da pilha user */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int proc_spawn(const char *name, void *entry, void *user_stack_top) {
    int slot = -1;
    for (int i = 0; i < MAX_PROC; i++)
        if (pcb_table[i].state == PROC_EMPTY) { slot = i; break; }
    if (slot < 0) return -1;

    void *kstack = mmap_user(0, STACK_SIZE, 3, 0);
    if (!kstack) return -1;

    uint64_t my_pml4 = pml4_create();
    if (!my_pml4) { munmap_user(kstack, STACK_SIZE); return -1; }

    pcb_t *p = &pcb_table[slot];
    p->pid = next_pid++;
    p->state = PROC_READY;
    p->is_user = 1;
    p->in_kern = 0;

    int ni = 0;
    while (name[ni] && ni < PROC_NAME_MAX - 1) { p->name[ni] = name[ni]; ni++; }
    p->name[ni] = '\0';

    p->rsp0 = (uint64_t)kstack + STACK_SIZE;
    p->user_rsp = (uint64_t)user_stack_top;
    p->pml4 = my_pml4;
    pcb_t *parent = process_current();
    p->parent_pid = parent ? parent->pid : 0;
    p->exit_code = 0;
    p->program_break = 0x201000;
    p->heap_start = 0x201000;
    p->vm_map = (struct vm_map *)kmalloc(sizeof(struct vm_map));
    if (p->vm_map) vm_map_init((vm_map_t *)p->vm_map, 0x10000, 0x7FFFF000ULL);
    p->fs_base = 0x50000000ULL + ((uint64_t)p->pid * 0x1000ULL);
    if (map_user_4kb(p->pml4, p->fs_base) < 0) p->fs_base = 0x1000ULL;
    volatile uint64_t *tls_page = (volatile uint64_t *)(uintptr_t)p->fs_base;
    tls_page[0] = 0; tls_page[1] = 0; tls_page[2] = 0; tls_page[3] = 0;
    p->th_parent_pid = -1;
    p->clear_tid = 0;
    p->pgrp = p->pid;
    p->session = current_pid >= 0 && current_pid < MAX_PROC &&
                 pcb_table[current_pid].state != PROC_EMPTY
        ? pcb_table[current_pid].session : p->pid;
    p->blocked = 0;
    p->pending = 0;
    for (int signal_index = 0; signal_index < 64; signal_index++) {
        p->actions[signal_index].handler = 0;
        p->actions[signal_index].flags = 0;
        p->actions[signal_index].restorer = 0;
        p->actions[signal_index].mask = 0;
    }
    p->cwd[0] = '/'; p->cwd[1] = '\0';

    // proc_spawn nao usa alloc_pcb(), entao precisamos
    // atualizar pcb_count manualmente senao process_current()
    // nao encontra o processo filho (e syscalls como mmap
    // sao chamadas pelo filho via libc). affs 2 horas de debug
    // por causa disso, mas agora ta certo~ rssrsrs
    if (slot >= pcb_count) pcb_count = slot + 1;

    uint64_t *sp = (uint64_t *)p->rsp0;
    *(--sp) = 0x23;
    *(--sp) = (uint64_t)user_stack_top;
    *(--sp) = 0x202;
    *(--sp) = 0x1B;
    *(--sp) = (uint64_t)entry;
    /* 15 registers: push reverse of context_switch pop order */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;  /* rax,rcx,rdx,rsi */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;  /* rdi,r8,r9,r10 */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;                /* r11,rbx,rbp */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;  /* r12,r13,r14,r15 */

    p->kernel_rsp = (uint64_t)sp;
    p->s_rbx = 0; p->s_rbp = 0; p->s_r12 = 0;
    p->s_r13 = 0; p->s_r14 = 0; p->s_r15 = 0;

    return p->pid;
}

/* ♥ process_switch_to ~ API antiga, compatibilidade */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
void process_switch_to(int pid) {
    serial_puts("[PROC] switch_to ");
    serial_puthex((uint32_t)pid);
    serial_puts("\r\n");
    pcb_t *current = process_current();
    if (!current) { serial_puts("[PROC] no current!\r\n"); return; }

    pcb_t *next = 0;
    int next_idx = -1;
    for (int i = 0; i < MAX_PROC; i++) {
        if (pcb_table[i].state != PROC_EMPTY && pcb_table[i].pid == pid) {
            next = &pcb_table[i];
            next_idx = i;
            break;
        }
    }
    if (!next) { serial_puts("[PROC] no next!\r\n"); return; }

    pcb_table[current_pid].state = PROC_BLOCKED;
    current_pid = next_idx;
    next->state = PROC_RUNNING;
    tss_set_rsp0(next->rsp0);

    // Debug: print iretq frame from kernel stack
    if (next->is_user) {
        uint64_t *sp = (uint64_t *)next->kernel_rsp;
        // After 15 pops, the iretq frame is at sp+15
        uint64_t *iretq = sp + 15;
        serial_puts("[PROC] iretq: RIP=");
        serial_puthex((uint32_t)(uint64_t)iretq[0]);
        serial_puts(" CS=");
        serial_puthex((uint32_t)iretq[1]);
        serial_puts(" RFL=");
        serial_puthex((uint32_t)iretq[2]);
        serial_puts(" RSP=");
        serial_puthex((uint32_t)(uint64_t)iretq[3]);
        serial_puts(" SS=");
        serial_puthex((uint32_t)iretq[4]);
        serial_puts("\r\n");
    }

    serial_puts("[PROC] context_switch now\r\n");
    context_switch(current, next);
}

/* ♥ process_exit_current ~ API antiga, usada pela syscall SYS_exit */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
void process_exit_current(int code) {
    pcb_t *p = process_current();
    if (!p) return;
    serial_puts("[PROC] exit pid=");
    serial_puthex((uint32_t)p->pid);
    serial_puts(" code=");
    serial_puthex((uint32_t)code);
    serial_puts("\r\n");
    {
        extern void fds_close_exec(void *);
        fds_close_exec(p->fds_tab);
    }
    /* FASE5: devolve os mapeamentos com objeto (file/wire/SHM) ao buddy.
     * Antes o espaço inteiro vazava no exit (pml4_destroy só solta o
     * topo): com o fontconfig mapeando 100s de MB, o pool esgotava
     * ("ba: no free" no mmap do foot). Só objetos (páginas
     * order-0 exatas); tabelas/PTEs e o resto vazam como antes (o
     * processo nunca mais roda; a troca de CR3 descarrega a TLB).
     * Threads (vm_map compartilhado) NÃO passam por aqui. */
    if (p->th_parent_pid < 0 && p->vm_map) {
        vm_map_remove(p->vm_map, 0, (uint64_t)~0ULL);
    }
    /* FASE5: morte fatal mata o grupo (irmãs p/ EMPTY com clear_tid
     * zerado); sem isso a thread órfã voltava ao scheduler com a
     * memória do main liberada (#GP no switch). */
    proc_kill_siblings(p);
    p->exit_code = code;
    p->state = PROC_ZOMBIE;
    signal_send_pid(p->parent_pid, TIPOS_SIGCHLD);

    proc_wake_parent(p->pid);

    /* Volta pro pai */
    int parent_pid = p->parent_pid;
    int parent_slot = 0;
    for (int slot = 0; slot < MAX_PROC; slot++) {
        if (pcb_table[slot].state != PROC_EMPTY && pcb_table[slot].pid == parent_pid) {
            parent_slot = slot;
            break;
        }
    }
    if (pcb_table[parent_slot].state == PROC_BLOCKED ||
        pcb_table[parent_slot].state == PROC_READY)
        pcb_table[parent_slot].state = PROC_READY;
    /* ~~ pai já morreu (ZOMBIE/EMPTY): NÃO dá resume nele — o
     * context_switch faria pops+iretq num frame salvo que já é lixo
     * (o KRNL_RSP do próprio exit aponta pra pilha C do exit) →
     * CS=lixo → #GP em ring 0 e o QEMU morre. Deixa o scheduler
     * escolher outro processo pronto. ~~ */
    {
        int ps = pcb_table[parent_slot].state;
        if (ps == PROC_ZOMBIE || ps == PROC_EMPTY) {
            serial_puts("[exit] pai morto slot=");
            serial_puthex((uint32_t)parent_slot);
            serial_puts(" -> schedule\r\n");
            schedule();
            for (;;) __asm__ volatile("cli; hlt");
        }
    }
    current_pid = parent_slot;
    pcb_t *next = &pcb_table[parent_slot];
    tss_set_rsp0(next->rsp0);
    serial_puts("[exit] parent=");
    serial_puthex((uint32_t)parent_pid);
    serial_puts(" in_kern=");
    serial_puthex((uint32_t)next->in_kern);
    serial_puts("\r\n");
    context_switch(p, next);
}

/* ♥ proc_exit ~ API nova do TipOS! Exit com schedule */
/* ~ essa demorou pra debugar, respeita ~ */
void proc_exit(int code) {
    if (current_pid < 0 || current_pid >= MAX_PROC) return;
    fds_cleanup();
    pcb_t *p = &pcb_table[current_pid];
    p->exit_code = code;
    p->state = PROC_ZOMBIE;
    signal_send_pid(p->parent_pid, TIPOS_SIGCHLD);
    proc_wake_parent(p->pid);
    schedule();
    for (;;) __asm__ volatile("hlt");
}

/* ~~ Processando~~ ufaaa que trabalhão! */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
void proc_wake_parent(int child_pid) {
    for (int i = 0; i < MAX_PROC; i++) {
        if (pcb_table[i].state == PROC_BLOCKED) {
            for (int j = 0; j < MAX_PROC; j++) {
                if (pcb_table[j].pid == child_pid &&
                    pcb_table[j].parent_pid == pcb_table[i].pid) {
                    pcb_table[i].state = PROC_READY;
                    return;
                }
            }
        }
    }
}

/* ~~ Processando~~ ufaaa que trabalhão! */
/* ~ cuidado que essa aqui morde ~ */
int proc_waitpid(int pid, int *exit_code) {
    for (;;) {
        int found = 0;
        for (int i = 0; i < MAX_PROC; i++) {
            if (pcb_table[i].pid == pid) {
                found = 1;
                if (pcb_table[i].state == PROC_ZOMBIE) {
                    if (exit_code) *exit_code = pcb_table[i].exit_code;
                    munmap_user((void *)(pcb_table[i].rsp0 - STACK_SIZE), STACK_SIZE);
                    pml4_destroy(pcb_table[i].pml4);
                    pcb_table[i].state = PROC_EMPTY;
                    return pid;
                }
                /* ~~ poll-yield cooperativo (sem BLOCKED!): o
                 * context_switch sempre iretqa pra USERSPACE, então
                 * bloquear dentro da syscall quebraria o retorno~
                 * Continuamos READY e cedemos a CPU até o filho virar
                 * zumbi~ simples e funciona~ */
                schedule();
                break;
            }
        }
        if (!found) return -1;
    }
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
pcb_t *process_current(void) {
    if (current_pid < 0 || current_pid >= MAX_PROC ||
        pcb_table[current_pid].state == PROC_EMPTY)
        return 0;
    return &pcb_table[current_pid];
}

/* ~~ Processando~~ ufaaa que trabalhão! */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int process_current_pid(void) {
    pcb_t *current = process_current();
    return current ? current->pid : -1;
}

/* ~ cuidado que essa aqui morde ~ */
static int find_next_ready(void) {
    int start = current_pid;
    /* ~~ Comeca em i=1: NAO se auto-encontre!~~
     * O shell fica READY eterno (o exit acorda ele e ninguem
     * re-marca RUNNING), entao i=0 retornava ele mesmo e o
     * schedule() saia sem trocar — nada em background rodava~ */
    for (int i = 1; i <= MAX_PROC; i++) {
        int idx = (start + i) % MAX_PROC;
        if (pcb_table[idx].state == PROC_READY)
            return idx;
    }
    return -1;
}

/* ♥ schedule ~ Round-robin scheduler chamado pelo timer! */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
void schedule(void) {
    int next_idx = find_next_ready();
    {
        static int dbg_sch = 0;
        if (dbg_sch < 6 && pcb_table[3].state != PROC_EMPTY) {
            dbg_sch++;
            serial_puts("[sch] nxt=");
            serial_puthex((uint32_t)next_idx);
            serial_puts(" cur=");
            serial_puthex((uint32_t)current_pid);
            serial_puts(" s3=");
            serial_puthex((uint32_t)pcb_table[3].state);
            serial_puts("\r\n");
        }
    }
    if (next_idx == -1 || next_idx == current_pid) {
        /* FASE6-stalemate: nada p/ trocar => abre IRQs e dorme um tick.
         * Sem isso, com IF=0 em syscall + todos bloqueados, ninguém
         * volta a userspace, IRQs pendentes nunca atendidas (timer
         * morto, mouse morto) = impasse eterno. sti+hlt: seguro aqui
         * (schedule nunca roda dentro de handler IRQ; handlers não
         * pegam locks de syscall; IRQ pendente entre sti/hlt acorda). */
        __asm__ volatile("sti; hlt");
        return;
    }
    int prev = current_pid;

    if (pcb_table[prev].in_kern) {
        serial_puts("[!] limpando in_kern de pid=");
        serial_puthex((uint32_t)prev);
        serial_puts("\r\n");
    }
    pcb_table[prev].in_kern = 0;   /* caminho normal: sem resume C */

    // [SCHD] debug removed — context switch is stable

    if (pcb_table[prev].state == PROC_RUNNING)
        pcb_table[prev].state = PROC_READY;

    pcb_table[next_idx].state = PROC_RUNNING;
    current_pid = next_idx;
    tss_set_rsp0(pcb_table[next_idx].rsp0);

    /* ~~ bloqueio dentro de syscall → troca que PRESERVA o fluxo C~~
     * O ret do context_switch_kern devolve AQUI quando o processo
     * for re-escalado (loop de waitpid/read continua de onde parou!) */
    {
        extern volatile int g_in_syscall;
        extern void context_switch_kern(pcb_t *cur, pcb_t *next);
        if (g_in_syscall && pcb_table[prev].is_user) {
            context_switch_kern(&pcb_table[prev], &pcb_table[next_idx]);
            /* ~~ fd table POR PROCESSO: religa ao voltar de uma troca ~~
             * `fdtab` (macro `fds`) e global: enquanto o processo A
             * dormia numa syscall bloqueante, o processo B rodava e
             * rebindava a tabela pra ELE. Ao acordar, A continuava
             * lendo os fds de B — o poll do fvwm2 via POLLERR no
             * socket X (fds do Xorg) e o Xlib derrubava a conexão.
             * Refaz o bind antes de voltar pro meio da syscall. ~~ */
            {
                extern void fds_bind_current(void);
                fds_bind_current();
            }
            return;
        }
    }
    {
        /* ~~ debug: frame do proximo processo antes do switch ~~ */
        static int dbg_cs = 0;
        if (dbg_cs < 12 && pcb_table[next_idx].pid >= 3) {
            dbg_cs++;
            serial_puts("[cs] idx="); serial_puthex((uint32_t)next_idx);
            serial_puts(" pid="); serial_puthex((uint32_t)pcb_table[next_idx].pid);
            serial_puts(" st="); serial_puthex((uint32_t)pcb_table[next_idx].state);
            serial_puts(" krsp=");
            serial_puthex((uint32_t)pcb_table[next_idx].kernel_rsp);
            serial_puts(" pml4=");
            serial_puthex((uint32_t)pcb_table[next_idx].pml4);
            serial_puts("\r\n[fr] rip=");
            uint64_t *fr = (uint64_t *)pcb_table[next_idx].kernel_rsp;
            serial_puthex((uint32_t)fr[15]);
            serial_puts(" cs=");
            serial_puthex((uint32_t)fr[16]);
            serial_puts(" rsp=");
            serial_puthex((uint32_t)fr[18]);
            serial_puts("\r\n");
        }
    }
    context_switch(&pcb_table[prev], &pcb_table[next_idx]);
    /* ~~ mesma religação do fd table no caminho não-syscall ~~ */
    {
        extern void fds_bind_current(void);
        fds_bind_current();
    }
}

/* ~~ setup_linux_user_stack_dyn ~ wrapper pro ELF dinâmico (#71) ~~
 * AT_BASE aponta pro INTERPRETADOR (ld-musl); musl ld.so usa AT_PHDR
 * pra achar o Ehdr/e_entry do binário principal (não precisa AT_ENTRY)~ */
uint64_t setup_linux_user_stack_dyn(pcb_t *pcb, uint64_t user_stack_top,
                                    uint64_t phdr, uint64_t phent, uint64_t phnum,
                                    uint64_t elf_base, char **argv, int argc,
                                    char **envp, int envc,
                                    uint64_t interp_base, uint64_t main_entry,
                                    int is_dynamic) {
    uint64_t at_base = is_dynamic ? interp_base : elf_base;
    return setup_linux_user_stack(pcb, user_stack_top,
                                  phdr, phent, phnum,
                                  at_base, argv, argc, envp, envc,
                                  main_entry);
}

/* ♥ proc_yield ~ "Me da a vez, CPU! >_<" */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
void proc_yield(void) {
    schedule();
}






/* ~~ setup_linux_user_stack ~~
 * Monta a pilha do usuário no formato Linux x86_64:
 *   argc, argv[NULL], envp[NULL], auxv[], strings
 * Tudo no estilo que o musl espera~ escreve no topo da pilha
 * (user_stack_top) e devolve o novo RSP apontando pro argc.
 * Se phdr/phent/phnum = 0, omite AT_PHDR/PHENT/PHNUM (ninguém
 *  sente falta mesmo~ mas o musl chora sem AT_PHDR as vezes)
 * ~~ kyun! ~~ */
uint64_t setup_linux_user_stack(pcb_t *pcb, uint64_t user_stack_top,
                                uint64_t phdr, uint64_t phent, uint64_t phnum,
                                uint64_t elf_base,
                                char **argv, int argc,
                                char **envp, int envc,
                                uint64_t at_entry) {
    uint64_t *sp = (uint64_t *)user_stack_top;
    (void)pcb;

    /* ~~ debug: quantas vezes e com que topo~ ~~ */
    extern uint32_t dbg_setup_count;
    dbg_setup_count++;
    serial_puts("[setup] top=");
    serial_puthex((uint32_t)user_stack_top);
    serial_puts(" n=");
    serial_puthex(dbg_setup_count);
    serial_puts("\r\n");
    if (argv && argc > 0 && argv[0] && argv[0][0] == '/' &&
        argv[0][1] == 'B' && argv[0][2] == 'I' && argv[0][3] == 'N' &&
        argv[0][4] == '/' && argv[0][5] == 'X' && argv[0][6] == 'T' &&
        argv[0][7] == 'E' && argv[0][8] == 'R' && argv[0][9] == 'M' &&
        argv[0][10] == '\0') {
        for (int index = 0; index < argc && index < 8; index++) {
            serial_puts("[ARGV] ");
            serial_puthex((uint32_t)index);
            serial_puts("='");
            serial_puts(argv[index] ? argv[index] : "(null)");
            serial_puts("'\r\n");
        }
        for (int index = 0; index < envc && index < 8; index++) {
            serial_puts("[ENV] '");
            serial_puts(envp && envp[index] ? envp[index] : "(null)");
            serial_puts("'\r\n");
        }
    }

    /* ~~ Strings dos argumentos ficam no TOPO da pilha ~~
     * Layout Linux x86_64 (de baixo pra cima):
     *   argc, argv[], NULL, envp[], NULL, auxv[], strings
     * Antes a gente intercalava as strings entre o argv e o
     * environ — e o musl via os bytes de "XORG" como primeiro
     * entry do environ! getenv("PIXMAN_DISABLE") do pixman
     * lia "XORG\0XOR" como ponteiro (não-canônico) e o XORG
     * tomava #GP no strncmp~ rssrsrs */
    uint64_t str_ptrs[16];
    for (int i = argc - 1; i >= 0; i--) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        size_t len = 0;
        while (s[len]) len++;
        sp = (uint64_t *)((uint8_t *)sp - (len + 1));
        for (size_t j = 0; j <= len; j++) ((uint8_t *)sp)[j] = (uint8_t)s[j];
        str_ptrs[i] = (uint64_t)sp;
    }

    /* ~~ Strings do ambiente (mesma danca do argv) ~~
     * O FVWM precisa do DISPLAY=:0 pra achar o servidor!~ */
    uint64_t env_ptrs[16];
    for (int i = envc - 1; i >= 0; i--) {
        const char *s = (envp && envp[i]) ? envp[i] : "";
        size_t len = 0;
        while (s[len]) len++;
        sp = (uint64_t *)((uint8_t *)sp - (len + 1));
        for (size_t j = 0; j <= len; j++) ((uint8_t *)sp)[j] = (uint8_t)s[j];
        env_ptrs[i] = (uint64_t)sp;
    }

    const uint64_t argv_words = (argc > 0 && argv) ? (uint64_t)argc + 2 : 2;
    const uint64_t env_words = (envc > 0) ? (uint64_t)envc + 1 : 1;
    const uint64_t aux_words = (phnum ? 16 : 10) + (at_entry ? 2 : 0) +
                               ((argc > 0 && argv) ? 2 : 0) + 12;
    const uint64_t stack_words = 2 + aux_words + env_words + argv_words;
    const uint64_t sp_addr = (uint64_t)sp;
    /* ~~ ABI SysV: RSP inicial do processo DEVE ser 16-byte aligned ~~
     * O '-8' antigo deixava rsp ≡ 8 (mod 16); o ld.so do glibc acha
     * movaps %xmm, -0x80(%rbp) com rbp desalinhado → #GP err=0 no
     * _dl_start. Sem o -8 o rsp final fica ≡ 0 (mod 16)~ */
    const uint64_t align_pad = (sp_addr - stack_words * sizeof(uint64_t)) & 0xF;
    sp = (uint64_t *)(uintptr_t)(sp_addr - align_pad);

    /* ~~ 16 bytes pseudo-aleatórios pro AT_RANDOM ~~
     * O kernel Linux de verdade pega do /dev/random~
     * A gente pega do /dev/memes — igualmente seguro! */
    sp -= 2;
    sp[0] = 0xBAADF00DBEEFCACEULL;
    sp[1] = 0xDEADBEEFCAFEBABEULL;
    uint64_t random_addr = (uint64_t)sp;

    /* ~~ Vetor auxiliar ~~
     * Layout Linux: cada par é [type][value] na memória (type no
     * endereço MENOR), e o AT_NULL fica no TOPO (último lido).
     * A gente escreve de trás pra frente: AT_NULL primeiro, e
     * cada par com value antes do type (quem manda é o *--sp)~
     * O musl static-pie usa AT_BASE pra se auto-relocar, então
     * sem AT_BASE (ou com ele no lugar errado) ele reloca em 0~
     * e o mundo desaba. Confia na ordem! kyun! */
    *--sp = 0;  *--sp = 0;             /* AT_NULL — topo da pilha de pares */
    if (at_entry) {
        *--sp = at_entry; *--sp = 9;   /* AT_ENTRY — entry do binário principal
                                        * (o ld.so do glibc pula pra cá no fim)*/
    }
    if (phnum) {
        *--sp = phdr;  *--sp = 3;     /* AT_PHDR — onde tao os headers~ */
        *--sp = phent; *--sp = 4;     /* AT_PHENT — tamanho de cada um~ */
        *--sp = phnum; *--sp = 5;     /* AT_PHNUM — quantos program headers~ */
    }
    *--sp = 0;      *--sp = 23;        /* AT_SECURE — "to suave, sem seccomp" */
    *--sp = 0x1000; *--sp = 6;         /* AT_PAGESZ — 4KB, padrao~ */
    *--sp = elf_base; *--sp = 7;       /* AT_BASE — base do PIE pro self-reloc~ */
    *--sp = random_addr; *--sp = 25;   /* AT_RANDOM */
    *--sp = 0xbfebfbffULL; *--sp = 16;  /* AT_HWCAP — x86_64 feature bits (glibc checa) */
    *--sp = 100; *--sp = 17;            /* AT_CLKTCK — 100 Hz (sysconf SC_CLK_TCK) */
    *--sp = 0; *--sp = 11;              /* AT_UID */
    *--sp = 0; *--sp = 12;              /* AT_EUID */
    *--sp = 0; *--sp = 13;              /* AT_GID */
    *--sp = 0; *--sp = 14;              /* AT_EGID */
    if (argc > 0 && argv) {
        *--sp = str_ptrs[0]; *--sp = 31; /* AT_EXECFN */
    }

    /* ~~ Ambiente: ponteiros + NULL terminador ~~
     * ORDEM CRÍTICA: o NULL tem que ser escrito PRIMEIRO (fica no
     * endereço MAIOR) pra subir na memória como [env0..envN,NULL].
     * Com o NULL depois do loop ele ficava em [NULL,env0..] e o glibc
     * parava o envp na hora e lixa o auxv (AT_RANDOM=0 → #PF em 0)~ */
    *--sp = 0;  /* NULL termina o envp[] */
    for (int i = envc - 1; i >= 0; i--) *--sp = env_ptrs[i];

    /* ~~ Argumentos: o array de ponteiros + argc ~~ */
    if (argc > 0 && argv) {
        *--sp = 0;  /* NULL termina o argv[] */
        for (int i = argc - 1; i >= 0; i--) *--sp = str_ptrs[i];
        *--sp = (uint64_t)argc;
    } else {
        *--sp = 0;  /* argv: lista vazia */
        *--sp = 0;  /* argc = 0 */
    }

    uint64_t *auxv = sp + argv_words + env_words;
    for (int i = 0; i < 12; i++) {
        uint64_t type = auxv[i * 2];
        uint64_t value = auxv[i * 2 + 1];
        serial_puts("[AUXV] ");
        serial_puthex((uint32_t)type);
        serial_puts("=");
        serial_puthex((uint32_t)value);
        if (type == 31) {
            serial_puts(" ");
            serial_puts((const char *)(uintptr_t)value);
        }
        serial_puts("\r\n");
        if (type == 0) break;
    }

    if (argv && argc == 3 && argv[0] && argv[0][0] == '/' &&
        argv[0][1] == 'B' && argv[0][2] == 'I' && argv[0][3] == 'N' &&
        argv[0][4] == '/' && argv[0][5] == 'X' && argv[0][6] == 'T' &&
        argv[0][7] == 'E' && argv[0][8] == 'R' && argv[0][9] == 'M' &&
        argv[0][10] == '\0') {
        uint64_t *initial = sp;
        serial_puts("[USTACK] argc="); serial_puthex((uint32_t)initial[0]);
        serial_puts(" argv0='"); serial_puts((const char *)(uintptr_t)initial[1]);
        serial_puts("' argv1='"); serial_puts((const char *)(uintptr_t)initial[2]);
        serial_puts("' argv2='"); serial_puts((const char *)(uintptr_t)initial[3]);
        serial_puts("' env0='");
        serial_puts((const char *)(uintptr_t)initial[5]);
        serial_puts("'\r\n");
    }

    return (uint64_t)sp;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * ♥ FORK REAL (issue #72) ~ o pai clona o filho de verdade!~
 * O Popen do Xorg precisa: pipe → fork → dup2 → execl → waitpid~
 * Sem fork de verdade o xkbcomp nunca roda e o XKB morre~ */
extern int map_user_4kb(uint64_t pml4_pa, uint64_t vaddr);

/* devolve o PA do frame recém-mapeado pro VA no filho (identity!)~
 * ATENÇÃO: o filho herda identity mapping de 2MB (huge pages)! Se
 * não dividir primeiro, o map_user_4kb vê "huge já mapeada" e não
 * aloca frame — e o readback leria o frame como se fosse PT (lixo,
 * write em non-canonical → #GP na cara~ rssrsrs aprendi na prática!) */
extern int split_2mb_pde(uint64_t pml4_pa, uint64_t vaddr);

/* ~~ tabela ainda compartilhada com o pai? Copia ela pro filho ~~
 * pml4_create() copia só as ENTRADAS do pml4 — os níveis de baixo
 * (PDPT/PD/PT) eram os MESMOS frames pros dois processos. O
 * map_user_4kb via o PTE já existente e devolvia o frame DO PAI:
 * o "fork" ficava com memória compartilhada e o filho destroçava o
 * stack do pai (sintoma no fvwm2: *** stack smashing detected ***
 * logo depois do fork do Exec). ~~ */
static uint64_t tbl_clone_if_shared(uint64_t *child_slot,
                                    uint64_t child_tbl,
                                    uint64_t parent_tbl) {
    if (!child_tbl) return 0;
    if (child_tbl != parent_tbl) return child_tbl;   /* já é privada */
    uint64_t *np = page_frame_kernel();
    if (!np) return 0;
    uint64_t *src = (uint64_t *)(uintptr_t)parent_tbl;
    for (int i = 0; i < 512; i++) np[i] = src[i];
    uint64_t newpa = (uint64_t)(uintptr_t)np;
    *child_slot = (*child_slot & 0xFFFULL) | newpa;
    return newpa;
}

/* devolve um frame NOVO (só do filho) pro VA, com a cópia das tabelas
 * em cima. O caller copia os bytes do frame do pai pra cá. */
static uint64_t child_frame_for(uint64_t child_pml4, uint64_t src_pml4,
                                uint64_t va) {
    int i4 = (va >> 39) & 0x1FF;
    int i3 = (va >> 30) & 0x1FF;
    int i2 = (va >> 21) & 0x1FF;
    int i1 = (va >> 12) & 0x1FF;
    uint64_t *p4c = (uint64_t *)(uintptr_t)child_pml4;
    uint64_t *p4s = (uint64_t *)(uintptr_t)src_pml4;
    if (!(p4c[i4] & 1) || !(p4s[i4] & 1)) return 0;

    /* divide 2MB nos DOIS lados (neutro pro pai: mesmo PA + flags) pra
     * as comparações de frame abaixo serem de tabela de verdade */
    split_2mb_pde(child_pml4, va);
    split_2mb_pde(src_pml4, va);

    uint64_t pdpt = tbl_clone_if_shared(&p4c[i4], p4c[i4] & ~0xFFFULL,
                                        p4s[i4] & ~0xFFFULL);
    if (!pdpt) return 0;
    uint64_t *p3c = (uint64_t *)(uintptr_t)pdpt;
    uint64_t *p3s = (uint64_t *)(uintptr_t)(p4s[i4] & ~0xFFFULL);
    if (!(p3c[i3] & 1) || !(p3s[i3] & 1)) return 0;
    if (p3c[i3] & 0x80) return 0;      /* 1GB huge: não mexe */

    uint64_t pd = tbl_clone_if_shared(&p3c[i3], p3c[i3] & ~0xFFFULL,
                                      p3s[i3] & ~0xFFFULL);
    if (!pd) return 0;
    uint64_t *p2c = (uint64_t *)(uintptr_t)pd;
    uint64_t *p2s = (uint64_t *)(uintptr_t)(p3s[i3] & ~0xFFFULL);
    if (!(p2c[i2] & 1) || !(p2s[i2] & 1)) return 0;
    if (p2c[i2] & 0x80) return 0;      /* ainda huge: skip */

    uint64_t pt = tbl_clone_if_shared(&p2c[i2], p2c[i2] & ~0xFFFULL,
                                      p2s[i2] & ~0xFFFULL);
    if (!pt) return 0;
    uint64_t *ptc = (uint64_t *)(uintptr_t)pt;

    uint64_t *frame = mmap_user(0, 4096, 3, 0);
    if (!frame) return 0;
    uint64_t flags = (ptc[i1] & 1) ? (ptc[i1] & 0xFFF) : 0x07;
    ptc[i1] = ((uint64_t)(uintptr_t)frame & ~0xFFFULL) | flags;
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return (uint64_t)(uintptr_t)frame;
}

/* copia página user: fonte lida pelo VA direto (CR3 é o pai na syscall)~
 * skip_lo/skip_hi: faixa de kstack do FILHO — é memória de kernel, o
 * filho tem a dele própria; copiar ia trocar o frame onde o kernel
 * vai montar o iretq-frame do filho (GPF na retomada). */
static void copy_user_space(uint64_t src_pml4_pa, uint64_t dst_pml4_pa,
                            uint64_t skip_lo, uint64_t skip_hi) {
    uint64_t *sp4 = (uint64_t *)(uintptr_t)src_pml4_pa;
    for (int i = 0; i < 512; i++) {
        uint64_t e4 = sp4[i];
        if (!(e4 & 1) || !(e4 & 4)) continue;
        uint64_t *sp3 = (uint64_t *)(uintptr_t)(e4 & 0x000FFFFFFFFFF000ULL);
        for (int j = 0; j < 512; j++) {
            uint64_t e3 = sp3[j];
            if (!(e3 & 1) || !(e3 & 4)) continue;
            if (e3 & 0x80) continue;
            uint64_t *sp2 = (uint64_t *)(uintptr_t)(e3 & 0x000FFFFFFFFFF000ULL);
            for (int k = 0; k < 512; k++) {
                uint64_t e2 = sp2[k];
                if (!(e2 & 1) || !(e2 & 4)) continue;
                uint64_t base = ((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21);
                if (e2 & 0x80) {
                    uint64_t pa2 = e2 & 0x000FFFFFE00000ULL;   /* bits 51:21 */
                    /* ~~ NÃO copia MMIO (≥2GB): RAM toda é <512MB aqui,
                     *  então qualquer pa alto é BAR de dispositivo (GPU,
                     *  e1000…). Ler como linear derruba o fork com #PF
                     *  (VA 0xFE800000 sem identity garantido)~ */
                    if (pa2 >= 0x80000000ULL) continue;
                    for (int n = 0; n < 512; n++) {
                        uint64_t va = base + ((uint64_t)n << 12);
                        if (va >= skip_lo && va < skip_hi) continue;
                        uint64_t src_pa = pa2 + ((uint64_t)n << 12);
                        if (!frame_is_user(src_pa)) continue;  /* kernel: share */
                        uint64_t fr = child_frame_for(dst_pml4_pa, src_pml4_pa, va);
                        if (!fr) {
                            serial_puts("[fork!] huge sem frame va=");
                            serial_puthex((uint32_t)(va >> 12));
                            serial_puts("\r\n");
                            continue;
                        }
                        for (int b = 0; b < 1024; b++)
                            ((uint32_t *)(uintptr_t)fr)[b] =
                                ((uint32_t *)(uintptr_t)src_pa)[b];
                        /* ~~ debug TCB: página que contém o fs_base do forktest ~~ */
                        if (va == 0x40206000ULL) {
                            uint64_t srcv = *(uint64_t *)(uintptr_t)(pa2 + 0x818);
                            uint64_t dstv = *(uint64_t *)(uintptr_t)(fr + 0x818);
                            serial_puts("[fork] tcb src=");
                            serial_puthex((uint32_t)(srcv >> 32));
                            serial_puthex((uint32_t)(srcv & 0xFFFFFFFF));
                            serial_puts(" dst=");
                            serial_puthex((uint32_t)(dstv >> 32));
                            serial_puthex((uint32_t)(dstv & 0xFFFFFFFF));
                            serial_puts(" pa2=");
                            serial_puthex((uint32_t)(pa2 >> 12));
                            serial_puts("\r\n");
                        }
                    }
                } else {
                    uint64_t pt_pa = e2 & 0x000FFFFFFFFFF000ULL;
                    /* ~~ PDE aponta pra fora da RAM (>=2GB) ou é lixo:
                     * desce numa página de dados e o loop lê entradas
                     * não-canônicas → #GP no fork (QEMU morre). ~~ */
                    if (pt_pa >= 0x80000000ULL) {
                        /* dump completo do PD corrompido (1a vez) */
                        static int dumped_pd;
                        if (!dumped_pd) {
                            dumped_pd = 1;
                            int nz = 0;
                            serial_puts("[pd-dump] pa=");
                            serial_puthex((uint32_t)((uint64_t)sp2 >> 32));
                            serial_puthex((uint32_t)(uint64_t)sp2);
                            for (int d = 0; d < 512 && nz < 16; d++) {
                                if (!sp2[d]) continue;
                                nz++;
                                serial_puts(" ["); serial_puthex(d);
                                serial_puts("]=");
                                serial_puthex((uint32_t)(sp2[d] >> 32));
                                serial_puthex((uint32_t)sp2[d]);
                                serial_puts("]");
                            }
                            serial_puts("\r\n");
                        }
                        serial_puts("[fork-bad] i="); serial_puthex(i);
                        serial_puts(" j="); serial_puthex(j);
                        serial_puts(" k="); serial_puthex(k);
                        serial_puts(" pde=");
                        serial_puthex((uint32_t)(e2 >> 32));
                        serial_puthex((uint32_t)e2);
                        serial_puts(" pd=");
                        serial_puthex((uint32_t)((uint64_t)sp2 >> 32));
                        serial_puthex((uint32_t)(uint64_t)sp2);
                        serial_puts(" e3=");
                        serial_puthex((uint32_t)(e3 >> 32));
                        serial_puthex((uint32_t)e3);
                        serial_puts(" pdpt=");
                        serial_puthex((uint32_t)((uint64_t)sp3 >> 32));
                        serial_puthex((uint32_t)(uint64_t)sp3);
                        serial_puts(" e4=");
                        serial_puthex((uint32_t)(e4 >> 32));
                        serial_puthex((uint32_t)e4);
                        serial_puts("\r\n");
                        continue;
                    }
                    uint64_t *spt = (uint64_t *)(uintptr_t)pt_pa;
                    for (int n = 0; n < 512; n++) {
                        uint64_t e1 = spt[n];
                        if (!(e1 & 1) || !(e1 & 4)) continue;
                        uint64_t va = base + ((uint64_t)n << 12);
                        uint64_t pa1 = e1 & 0x000FFFFFFFFFF000ULL;
                        if (pa1 >= 0x80000000ULL) continue;   /* MMIO: não copia */
                        if (va >= skip_lo && va < skip_hi) continue;
                        if (!frame_is_user(pa1)) continue;   /* kernel: share */
                        uint64_t fr = child_frame_for(dst_pml4_pa, src_pml4_pa, va);
                        if (!fr) {
                            serial_puts("[fork!] leaf sem frame va=");
                            serial_puthex((uint32_t)(va >> 12));
                            serial_puts("\r\n");
                            continue;
                        }
                        for (int b = 0; b < 1024; b++)
                            ((uint32_t *)(uintptr_t)fr)[b] =
                                ((uint32_t *)(uintptr_t)pa1)[b];
                    }
                }
            }
        }
    }
}

/* cópia rasa das regiões do vm_map (dados já foram pelo walker)~ */
static void vm_map_copy_to(struct vm_map *dst, struct vm_map *src) {
    vm_map_entry_t *e = src->head;
    while (e) {
        uint64_t size = (e->end > e->start) ? (e->end - e->start) : 0;
        if (size)
            vm_map_fixed((vm_map_t *)dst, NULL, 0, e->start, size, e->prot, e->flags);
        e = e->next;
    }
}

int proc_fork(uint64_t *parent_kframe) {
    pcb_t *par = process_current();
    if (!par || !parent_kframe) return -1;

    int slot = -1;
    for (int i = 0; i < MAX_PROC; i++)
        if (pcb_table[i].state == PROC_EMPTY) { slot = i; break; }
    if (slot < 0) { serial_puts("[fork] sem slot\r\n"); return -1; }

    void *kstack = kmalloc(STACK_SIZE);
    if (!kstack) { serial_puts("[fork] sem kstack\r\n"); return -1; }

    uint64_t child_pml4 = pml4_create();
    if (!child_pml4) { kfree(kstack); serial_puts("[fork] sem pml4\r\n"); return -1; }

    pcb_t *p = &pcb_table[slot];
    p->pid = next_pid++;
    p->state = PROC_READY;
    p->is_user = 1;
    p->in_kern = 0;
    {
        int ni = 0;
        while (par->name[ni] && ni < PROC_NAME_MAX - 5) { p->name[ni] = par->name[ni]; ni++; }
        p->name[ni] = 'f'; p->name[ni+1] = 'k'; p->name[ni+2] = '\0';
    }
    p->rsp0 = (uint64_t)kstack + STACK_SIZE;
    {
        extern void serial_puts(const char *s);
        extern void serial_puthex(uint32_t v);
        serial_puts("[fork] kstack="); serial_puthex((uint32_t)kstack);
        serial_puts(" rsp0="); serial_puthex((uint32_t)p->rsp0);
        serial_puts("\r\n");
    }
    p->pml4 = child_pml4;
    p->parent_pid = par->pid;
    p->th_parent_pid = -1;   /* fork gera PROCESSO, nunca thread (FASE5: lixo
                              * do slot reaproveitado desviava o exit do filho
                              * pra proc_thread_exit e o waitpid nunca colhia) */
    p->exit_code = 0;
    p->program_break = par->program_break;
    p->heap_start = par->heap_start;
    p->vm_map = (struct vm_map *)kmalloc(sizeof(struct vm_map));
    if (p->vm_map) {
        vm_map_init((vm_map_t *)p->vm_map, 0x10000, 0x7FFFF000ULL);
        if (par->vm_map) vm_map_copy_to((struct vm_map *)p->vm_map,
                                        (struct vm_map *)par->vm_map);
    }
    p->vmspace = 0;
    p->fs_base = par->fs_base;
    for (int ci = 0; ci < 256; ci++) p->cwd[ci] = par->cwd[ci];
    for (int signal_index = 0; signal_index < 64; signal_index++)
        p->actions[signal_index] = par->actions[signal_index];
    p->blocked = par->blocked;
    p->pending = 0;
    p->pgrp = par->pgrp;
    p->session = par->session;
    p->controlling_tty = par->controlling_tty;

    if (slot >= pcb_count) pcb_count = slot + 1;

    serial_puts("[fork] copiando memoria par=");
    serial_puthex(par->pid);
    {
        extern void serial_puthex(uint32_t v);
        serial_puts(" rip=");
        serial_puthex((uint32_t)(parent_kframe[17] >> 32));
        serial_puthex((uint32_t)parent_kframe[17]);
        serial_puts(" rsp=");
        serial_puthex((uint32_t)(parent_kframe[20] >> 32));
        serial_puthex((uint32_t)parent_kframe[20]);
        serial_puts(" rflags=");
        serial_puthex((uint32_t)parent_kframe[19]);
    }
    serial_puts("\r\n");
    copy_user_space(par->pml4, child_pml4, (uint64_t)kstack,
                    (uint64_t)kstack + STACK_SIZE);
    serial_puts("[fork] memoria ok\r\n");

    /* ~~ fd table própria pro filho — do POOL estático, no slot dele~~
     * NÃO troca o fdtab global: quem continua rodando é o PAI! */
    {
        extern void fds_dup_into_slot(int child_slot);
        extern void *fds_tab_addr(int slot);
        int cslot = (int)(p - pcb_table);
        fds_dup_into_slot(cslot);          /* copia conteúdo pro pool~ */
        p->fds_tab = fds_tab_addr(cslot);  /* e aponta o PCB pro slot! */
    }

    /* ~~ frame do filho: IRET-frame completo de 20 qwords~~
     * O frame da SYSCALL tem layout próprio (syscall_entry.asm):
     *   kframe[13]=rcx=return RIP, kframe[6]=r11=return RFLAGS,
     *   e o USER RSP tá na variável current_rsp0 (xchg da entry)!
     * O filho recebe TUDO igual ao pai mas com RAX=0~ */
    extern uint64_t current_rsp0;
    uint64_t user_rsp_now = current_rsp0;
    {   /* ~~ debug TLS: lê o FS MSR AO VIVO em vez de confiar no PCB~~ */
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
        uint64_t live_fs = ((uint64_t)hi << 32) | lo;
        serial_puts("[fork] pcb_fs=");
        serial_puthex((uint32_t)(par->fs_base >> 32));
        serial_puthex((uint32_t)(par->fs_base & 0xFFFFFFFF));
        serial_puts(" live_fs=");
        serial_puthex((uint32_t)(live_fs >> 32));
        serial_puthex((uint32_t)(live_fs & 0xFFFFFFFF));
        serial_puts("\r\n");
        p->fs_base = live_fs ? live_fs : par->fs_base;
    }
    uint64_t *sp = (uint64_t *)p->rsp0;
    sp -= 20;
    /* ~~ Converte layout de syscall (parent_kframe: rax..r15) para o
     * layout esperado pelo context_switch (sp: r15..rax) ~~
     * parent_kframe vem de syscall_entry.asm que empilhou r15..rax
     * (regs[0]=rax, regs[14]=r15).
     * context_switch desempilha r15..rax (sp[0]=r15, sp[14]=rax).
     * Sem inverter, sp[4] (RBP) recebia regs[4] (RDI = clone flags),
     * corrompendo os registradores callee-saved da libc e disparando
     * *** stack smashing detected *** no retorno do fork! */
    for (int q = 0; q < 15; q++) sp[q] = parent_kframe[14 - q];
    sp[14] = 0;                     /* RAX = 0 → fork() retorna 0 no filho! */
    /* ~~ return RIP/RFLAGS originais estão no STASH da entry~~
     * (sub rsp,16 antes dos pushes): kframe[15]=RIP, kframe[16]=RFLAGS~
     * O kframe[1] (slot do rcx) foi sobrescrito com o arg4 pelo Zig~ */
    sp[15] = parent_kframe[15];     /* RIP de retorno (stash!) */
    sp[16] = 0x1B;                  /* CS ring3 */
    sp[17] = parent_kframe[16];     /* RFLAGS de retorno (stash!) */
    sp[18] = user_rsp_now;          /* RSP = user rsp do pai na syscall */
    sp[19] = 0x23;                  /* SS ring3 */
    p->kernel_rsp = (uint64_t)sp;
    p->user_rsp = user_rsp_now;
    p->user_rip = parent_kframe[15];
    p->s_rbx = parent_kframe[9];
    p->s_rbp = parent_kframe[10];
    p->s_r12 = parent_kframe[11];
    p->s_r13 = parent_kframe[12];
    p->s_r14 = parent_kframe[13];
    p->s_r15 = parent_kframe[14];

    {
        uint64_t *f = (uint64_t *)p->kernel_rsp;
        serial_puts("[fkf] rax=");
        serial_puthex((uint32_t)f[14]);
        serial_puts(" rip=");
        serial_puthex((uint32_t)(f[15] & 0xFFFFFFFF));
        serial_puts(" rsp=");
        serial_puthex((uint32_t)f[18]);
        serial_puts("\r\n");
    }
    serial_puts("[fork] pid=");
    serial_puthex((uint32_t)p->pid);
    serial_puts(" pronto!\r\n");
    {
        extern int g_trace_window;
        g_trace_window = 60;   /* loga as próximas 60 syscalls completas~ */
    }
    return p->pid;
}



/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * ♥ CLONE (Linux 56) ~ threads estilo POSIX pro musl!~
 * Thread = PCB novo COMPARTILHANDO PML4/vm_map/fd_table com o pai
 * (zero cópia de memória!). O musl prepara o trampolino no `stack`
 * novo — o kernel só troca RSP e devolve 0 no filho~ kyun~ */
int proc_clone_thread(uint64_t flags, uint64_t newsp,
                      uint64_t ptid, uint64_t ctid,
                      uint64_t tls, uint64_t *parent_kframe) {
    pcb_t *par = process_current();
    if (!par || !parent_kframe) return -1;

    /* só threads (o fork "de verdade" já tem caminho próprio)~ */
    #define CLONE_VM_       0x00000100ULL
    #define CLONE_FS_       0x00000200ULL
    #define CLONE_FILES_    0x00000400ULL
    #define CLONE_SIGHAND_  0x00000800ULL
    #define CLONE_THREAD_   0x00010000ULL
    #define CLONE_SYSVSEM_  0x00040000ULL
    #define CLONE_SETTLS_   0x00080000ULL
    #define CLONE_PARENT_SETTID_ 0x00100000ULL
    #define CLONE_CHILD_CLEARTID_ 0x00200000ULL
    #define CLONE_CHILD_SETTID_  0x01000000ULL

    if (!(flags & CLONE_THREAD_) || !(flags & CLONE_VM_)) {
        serial_puts("[clone] flags sem THREAD/VM — EINVAL\r\n");
        return -22; /* -EINVAL: musl não usa clone-fork aqui */
    }

    int slot = -1;
    for (int i = 0; i < MAX_PROC; i++)
        if (pcb_table[i].state == PROC_EMPTY) { slot = i; break; }
    if (slot < 0) { serial_puts("[clone] sem slot\r\n"); return -1; }

    void *kstack = kmalloc(STACK_SIZE);
    if (!kstack) { serial_puts("[clone] sem kstack\r\n"); return -1; }

    pcb_t *p = &pcb_table[slot];
    for (int j = 0; j < (int)sizeof(pcb_t); j++)
        ((uint8_t *)p)[j] = 0;
    p->pid = next_pid++;
    p->state = PROC_READY;
    p->is_user = 1;
    {
        int ni = 0;
        while (par->name[ni] && ni < PROC_NAME_MAX - 4) { p->name[ni] = par->name[ni]; ni++; }
        p->name[ni] = 't'; p->name[ni+1] = 'h'; p->name[ni+2] = '\0';
    }
    p->rsp0 = (uint64_t)kstack + STACK_SIZE;
    /* ~~ COMPARTILHADOS (é isso que faz ser thread!)~~ */
    p->pml4 = par->pml4;
    p->vm_map = par->vm_map;
    p->fds_tab = par->fds_tab;
    p->program_break = par->program_break;
    p->heap_start = par->heap_start;
    p->parent_pid = par->pid;
    p->th_parent_pid = par->pid;
    p->vmspace = 0;
    p->in_kern = 0;
    for (int ci = 0; ci < 256; ci++) p->cwd[ci] = par->cwd[ci];

    if (slot >= pcb_count) pcb_count = slot + 1;

    /* ~~ TLS: FS.base da thread~~ */
    p->fs_base = (flags & CLONE_SETTLS_) ? tls : par->fs_base;

    /* ~~ clear_tid: zera no exit pra acordar o joiner~~ */
    p->clear_tid = (flags & CLONE_CHILD_CLEARTID_) ? (volatile long *)ctid : 0;

    /* ~~ notificações de TID (escrita direta em VA user — CR3 é o pai)~~ */
    if ((flags & CLONE_PARENT_SETTID_) && ptid)
        *(int *)(uintptr_t)ptid = p->pid;
    if ((flags & CLONE_CHILD_SETTID_) && ctid)
        *(int *)(uintptr_t)ctid = p->pid;

    /* ~~ frame do filho: igual ao pai, mas NOVO RSP e RAX=0~~
     * O musl preparou o trampolino (fn/arg) na base do stack novo —
     * quando a CPU der iretq, ele roda lá~ */
    uint64_t *sp = (uint64_t *)p->rsp0;
    sp -= 20;
    for (int q = 0; q < 15; q++) sp[q] = parent_kframe[14 - q];
    sp[14] = 0;                          /* RAX=0: filho "retorna" 0 */
    sp[15] = parent_kframe[15];          /* RIP de retorno (stash)   */
    sp[16] = 0x1B;                       /* CS ring3                 */
    sp[17] = parent_kframe[16];          /* RFLAGS (stash)           */
    extern uint64_t current_rsp0;
    sp[18] = newsp ? newsp : current_rsp0;
    sp[19] = 0x23;                       /* SS                       */
    p->kernel_rsp = (uint64_t)sp;
    p->user_rsp = sp[18];
    p->user_rip = sp[15];
    p->s_rbx = parent_kframe[9];
    p->s_rbp = parent_kframe[10];
    p->s_r12 = parent_kframe[11];
    p->s_r13 = parent_kframe[12];
    p->s_r14 = parent_kframe[13];
    p->s_r15 = parent_kframe[14];

    serial_puts("[clone] tid=");
    serial_puthex((uint32_t)p->pid);
    serial_puts(" fs=");
    serial_puthex((uint32_t)(p->fs_base >> 12));
    serial_puts("\r\n");
    return p->pid;
}

/* ~~ exit de THREAD: NÃO derruba espaço/fds compartilhados~~
 * Zera clear_tid (joiner enxerga via poll) e libera só o PCB.
 * O kernel stack vaza por ora (documentado — barato e raro~) */
void proc_thread_exit(int code) {
    pcb_t *me = process_current();
    if (!me) return;
    if (me->clear_tid) {
        *me->clear_tid = 0;              /* tid=0: joiner solta~ */
        __asm__ volatile("" ::: "memory");
    }
    serial_puts("[thexit] pid=");
    serial_puthex((uint32_t)me->pid);
    serial_puts(" code=");
    serial_puthex((uint32_t)code);
    serial_puts("\r\n");
    me->exit_code = code;
    me->state = PROC_EMPTY;              /* sem zombie: join é por futex */
    schedule();
    for (;;) __asm__ volatile("hlt");
}
/* ~~ mata threads irmãs (mesmo espaço) silenciosamente ~~
 * Morte fatal (exit/exit_group/sinal) mata o GRUPO (POSIX): threads
 * irmãs viram EMPTY (join acorda via clear_tid zerado + timeout do
 * futex). FASE5: sem isso, thread órfã de um main morto era
 * re-escalonada com a pilha/memória liberadas (#GP no switch). */
void proc_kill_siblings(pcb_t *me) {
    if (!me) return;
    for (int i = 0; i < MAX_PROC; i++) {
        if (pcb_table[i].state != PROC_EMPTY &&
            pcb_table[i].pid != me->pid &&
            pcb_table[i].pml4 == me->pml4) {
            if (pcb_table[i].clear_tid)
                *pcb_table[i].clear_tid = 0;
            pcb_table[i].state = PROC_EMPTY;
        }
    }
}
/* ♥ fim clone/threads ♥ */

/* ♥ process.c ~ arquivo fofinho do OvsbMkM! kyun~ <3 */
