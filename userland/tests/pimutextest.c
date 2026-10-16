/* Optional PI mutexes must either work or advertise ENOTSUP, not abort users. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
static int checks,fails;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"pimutextest: FAIL line %d: %s\n",__LINE__,#x);fails++;}}while(0)
int main(void){pthread_mutexattr_t a;pthread_mutex_t m;CHECK(pthread_mutexattr_init(&a)==0);CHECK(pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE)==0);int r=pthread_mutexattr_setprotocol(&a,PTHREAD_PRIO_INHERIT);CHECK(r==0||r==ENOTSUP);if(r==ENOTSUP){CHECK(pthread_mutexattr_setprotocol(&a,PTHREAD_PRIO_NONE)==0);puts("pimutextest: explicit unsupported PI, using normal recursive mutex");}CHECK(pthread_mutex_init(&m,&a)==0);CHECK(pthread_mutex_lock(&m)==0);CHECK(pthread_mutex_lock(&m)==0);CHECK(pthread_mutex_unlock(&m)==0);CHECK(pthread_mutex_unlock(&m)==0);CHECK(pthread_mutex_destroy(&m)==0);CHECK(pthread_mutexattr_destroy(&a)==0);printf("pimutextest: %d checks, %d failures\n",checks,fails);return fails?1:0;}
