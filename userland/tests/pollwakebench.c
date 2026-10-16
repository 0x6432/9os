/* Informational wakeup benchmark plus an actual readiness/progress check.
 * Private idle eventfds must never report readiness for unrelated transfers.
 * No wall-time/context-switch threshold is a correctness gate: Linux and 9os
 * may schedule differently, and emulation speeds vary by architecture. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#define N 24
static int fd[N], ready, failed;static long switches[N];
static long long ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){perror("clock_gettime");exit(2);}return (long long)t.tv_sec*1000000000+t.tv_nsec;}
static void *sleeper(void *arg){
 int i=(int)(intptr_t)arg;struct rusage before={0},after={0};if(getrusage(RUSAGE_THREAD,&before))__atomic_add_fetch(&failed,1,__ATOMIC_RELAXED);
 __atomic_add_fetch(&ready,1,__ATOMIC_RELEASE);
 struct pollfd p={fd[i],POLLIN,0};int r;do{r=poll(&p,1,-1);}while(r<0&&errno==EINTR);
 uint64_t v=0;if(r!=1||!(p.revents&POLLIN)||read(fd[i],&v,8)!=8||v!=1)__atomic_add_fetch(&failed,1,__ATOMIC_RELAXED);
 if(getrusage(RUSAGE_THREAD,&after))__atomic_add_fetch(&failed,1,__ATOMIC_RELAXED);
 switches[i]=after.ru_nvcsw+after.ru_nivcsw-before.ru_nvcsw-before.ru_nivcsw;return NULL;
}
int main(void){
 alarm(45);setvbuf(stdout,NULL,_IOLBF,0);pthread_t t[N];
 for(int i=0;i<N;i++){fd[i]=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);if(fd[i]<0||pthread_create(&t[i],NULL,sleeper,(void*)(intptr_t)i))return 2;}
 while(__atomic_load_n(&ready,__ATOMIC_ACQUIRE)!=N)usleep(1000);
 usleep(20000);int other=eventfd(0,EFD_CLOEXEC);if(other<0)return 2;
 struct rusage a={0},b={0};if(getrusage(RUSAGE_THREAD,&a))__atomic_add_fetch(&failed,1,__ATOMIC_RELAXED);long long start=ns();
 for(int i=0;i<1000;i++){uint64_t one=1,v=0;if(write(other,&one,8)!=8||read(other,&v,8)!=8||v!=1)__atomic_add_fetch(&failed,1,__ATOMIC_RELAXED);}
 long long elapsed=ns()-start;if(getrusage(RUSAGE_THREAD,&b))__atomic_add_fetch(&failed,1,__ATOMIC_RELAXED);
 for(int i=0;i<N;i++){uint64_t one=1;if(write(fd[i],&one,8)!=8)__atomic_add_fetch(&failed,1,__ATOMIC_RELAXED);}
 long total=0;for(int i=0;i<N;i++){if(pthread_join(t[i],NULL))__atomic_add_fetch(&failed,1,__ATOMIC_RELAXED);total+=switches[i];close(fd[i]);}
 close(other);
 printf("pollwakebench: transfers=1000 idle_waiters=%d wall_ms=%lld producer_switches=%ld waiter_switches=%ld failures=%d\n",N,elapsed/1000000,b.ru_nvcsw+b.ru_nivcsw-a.ru_nvcsw-a.ru_nivcsw,total,failed);
 return __atomic_load_n(&failed,__ATOMIC_RELAXED)?1:0;
}
