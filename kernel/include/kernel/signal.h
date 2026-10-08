#pragma once
#include <kernel/types.h>
#include <kernel/list.h>

#define NSIG 65
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30
#define SIGSYS 31

#define SIG_DFL 0UL
#define SIG_IGN 1UL

#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIGBIT(s) (1ULL << ((s) - 1))

struct k_sigaction {          /* layout used by the rt_sigaction syscall */
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

/* si_code values */
#define SI_USER 0
#define SI_KERNEL 0x80
#define SI_QUEUE (-1)
#define SI_TIMER (-2)
#define SI_MESGQ (-3)
#define SI_ASYNCIO (-4)
#define SI_SIGIO (-5)
#define SI_TKILL (-6)
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define TRAP_BRKPT 1
#define TRAP_TRACE 2
#define ILL_ILLOPC 1
#define FPE_INTDIV 1
#define BUS_ADRALN 1

#include <kernel/siginfo.h>

struct process;
struct thread;
void signal_send(struct process *p, int sig);
void signal_send_info(struct process *p, const struct ksiginfo *ki);
int signal_thread_info(struct thread *t, const struct ksiginfo *ki);   /* -EAGAIN: queue full */
void signal_force_info(struct thread *t, int sig, int code, uint64_t addr);
void signal_send_chld(struct process *parent, struct process *child, int code, int status);
void siginfo_to_user(const struct ksiginfo *ki, void *out128);
void siginfo_from_user(struct ksiginfo *ki, const void *in128);
int signal_dequeue(struct thread *t, uint64_t mask, struct ksiginfo *out);  /* 0 = none */
void sigq_flush_thread(struct thread *t);
void sigq_flush_proc(struct process *p);
void ksiginfo_user(struct ksiginfo *ki, int sig, int code);  /* sender = current */
void signal_send_pgrp(int pgid, int sig);
void signal_force(struct thread *t, int sig);   /* synchronous fault signal */
bool signal_pending(struct thread *t);
struct trap_frame;
void signal_deliver(struct trap_frame *f);      /* on return to user mode */
