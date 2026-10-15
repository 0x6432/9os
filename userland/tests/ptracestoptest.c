/* Linux oracle + 9os regression: seized job stops and queued ordinary signals.
 * Real strace SEIZE/INTERRUPT after a job-control SIGSTOP used to spin in
 * do_stop's interruptible wait with the BKL held, freezing the whole kernel. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
static int fails, checks;
static volatile sig_atomic_t caught;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"ptracestoptest: FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);fails++; } } while(0)
static void handler(int sig) { (void)sig;caught=1; }
static void watchdog(int sig) { (void)sig;write(2,"ptracestoptest: FAIL watchdog\n",29);_exit(124); }
static pid_t stopped_child(int exec_child) {
    pid_t p=fork();CHECK(p>=0);
    if(!p){signal(SIGUSR1,handler);raise(SIGSTOP);if(exec_child)execl("/bin/true","true",(char*)0);_exit(exec_child?93:caught?0:92);}
    if(p<0)exit(1);
    int st=0;CHECK(waitpid(p,&st,WUNTRACED)==p);CHECK(WIFSTOPPED(st)&&WSTOPSIG(st)==SIGSTOP);return p;
}
static void pending_signal(void) {
    pid_t p=stopped_child(0);int st=0;
    CHECK(kill(p,SIGUSR1)==0);usleep(20000);
    CHECK(waitpid(p,&st,WNOHANG|WUNTRACED)==0);
    CHECK(kill(p,SIGCONT)==0);CHECK(waitpid(p,&st,0)==p);
    CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==0);
}
static void kill_stopped(void) {
    pid_t p=stopped_child(0);int st=0;CHECK(kill(p,SIGKILL)==0);
    CHECK(waitpid(p,&st,0)==p);CHECK(WIFSIGNALED(st)&&WTERMSIG(st)==SIGKILL);
}
static void seize_stopped(void) {
    pid_t p=stopped_child(1);int st=0,stop=0,exec=0,exit_event=0,syscalls=0,exited=0;
    CHECK(ptrace(PTRACE_SEIZE,p,0,(void*)(long)(PTRACE_O_TRACESYSGOOD|PTRACE_O_TRACEEXEC|PTRACE_O_TRACEEXIT))==0);
    CHECK(ptrace(PTRACE_INTERRUPT,p,0,0)==0);CHECK(kill(p,SIGCONT)==0);
    for(int i=0;i<1000;i++){
        pid_t w=waitpid(p,&st,__WALL);CHECK(w==p);if(w!=p)break;
        if(WIFEXITED(st)){CHECK(WEXITSTATUS(st)==0);exited=1;break;}
        if(WIFSIGNALED(st)){CHECK(0);break;}
        CHECK(WIFSTOPPED(st));
        unsigned ev=(unsigned)st>>16;
        if(ev==PTRACE_EVENT_STOP)stop++;
        if(ev==PTRACE_EVENT_EXEC)exec++;
        if(ev==PTRACE_EVENT_EXIT)exit_event++;
        if(WSTOPSIG(st)==(SIGTRAP|0x80))syscalls++;
        CHECK(ptrace(PTRACE_SYSCALL,p,0,0)==0);
    }
    CHECK(exited);CHECK(stop>=1);CHECK(exec==1);CHECK(exit_event==1);CHECK(syscalls>=2);
    if(!exited){kill(p,SIGKILL);ptrace(PTRACE_CONT,p,0,0);waitpid(p,&st,__WALL);}
}
int main(void){
    setvbuf(stdout,0,_IONBF,0);signal(SIGALRM,watchdog);alarm(30);
    pending_signal();puts("ptracestoptest: pending signal/continue done");
    kill_stopped();puts("ptracestoptest: stopped kill done");
    for(int i=0;i<12;i++)seize_stopped();
    alarm(0);printf("ptracestoptest: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
