#ifndef TIPOS_SIGNAL_H
#define TIPOS_SIGNAL_H

#include <stdint.h>

#define TIPOS_NSIG 64
#define TIPOS_SIG_BLOCK 0
#define TIPOS_SIG_UNBLOCK 1
#define TIPOS_SIG_SETMASK 2
#define TIPOS_SIG_DFL 0ULL
#define TIPOS_SIG_IGN 1ULL
#define TIPOS_SA_SIGINFO 0x00000004UL
#define TIPOS_SA_NODEFER 0x40000000UL
#define TIPOS_SA_RESTORER 0x04000000UL

#define TIPOS_SIGINT 2
#define TIPOS_SIGKILL 9
#define TIPOS_SIGCHLD 17
#define TIPOS_SIGCONT 18
#define TIPOS_SIGSTOP 19
#define TIPOS_SIGTSTP 20
#define TIPOS_SIGTTIN 21
#define TIPOS_SIGTTOU 22
#define TIPOS_SIGURG 23
#define TIPOS_SIGWINCH 28

typedef struct {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
} tipos_sigaction_t;

int signal_sigaction(int signal_number, const tipos_sigaction_t *action,
                     tipos_sigaction_t *old_action, uint64_t sigset_size);
int signal_sigprocmask(int how, const uint64_t *set, uint64_t *old_set,
                       uint64_t sigset_size);
int signal_send_pid(int pid, int signal_number);
int signal_send_pgrp(int pgrp, int signal_number);
int signal_kill(int pid, int signal_number);
int signal_tgkill(int tgid, int tid, int signal_number);
int signal_setpgid(int pid, int pgrp);
int signal_setsid(void);
int signal_getpgrp(void);
void signal_dispatch_syscall(uint64_t *registers);
void signal_dispatch_irq(uint64_t *registers);
int signal_rt_sigreturn(uint64_t *registers);

#endif