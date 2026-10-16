/* Targeted poll wakeups must retain aliases and conservative nested/unknown
 * descriptors. Exact source also runs on Linux. Timings are not perf gates. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static int checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("pollfiltertest: FAIL line %d: %s\n",__LINE__,#x);}}while(0)
struct put {int fd;uint64_t value;ssize_t result;};
static void *later(void *p){struct put *w=p;usleep(30000);w->result=write(w->fd,&w->value,8);return NULL;}
static void sample(int mode) {
 int e=eventfd(0,EFD_NONBLOCK),alias=dup(e),sv[2]={-1,-1};
 CHECK(e>=0&&alias>=0);if(e<0||alias<0)return;
 CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0);
 int ep=-1,outer=-1,ino=-1,target=alias;
 if(mode==2){
  ep=epoll_create1(EPOLL_CLOEXEC);outer=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0&&outer>=0);
  struct epoll_event ev={.events=EPOLLIN,.data.fd=e};CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,alias,&ev)==0);
  ev.data.fd=ep;CHECK(epoll_ctl(outer,EPOLL_CTL_ADD,ep,&ev)==0);target=outer;
 }
 if(mode==3){ino=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);CHECK(ino>=0);}
 struct put w={e,(uint64_t)(mode+3),0};pthread_t t;
 int made=pthread_create(&t,NULL,later,&w)==0;CHECK(made);
 if(!made)goto done;
 struct pollfd p[3]={{target,POLLIN,0},{sv[0],POLLIN,0},{ino,POLLIN,0}};
 int r;
 if(mode==1){struct timespec timeout={2,0};r=ppoll(p,3,&timeout,NULL);}
 else r=poll(p,3,2000);
 CHECK(r==1&&(p[0].revents&POLLIN)&&!p[1].revents&&!p[2].revents);
 CHECK(pthread_join(t,NULL)==0&&w.result==8);
 uint64_t value=0;CHECK(read(alias,&value,8)==8&&value==w.value);
 /* Filter state must not leak into subsequent unfiltered select/epoll calls. */
 w.result=0;made=pthread_create(&t,NULL,later,&w)==0;CHECK(made);
 if(made){fd_set rd;FD_ZERO(&rd);FD_SET(alias,&rd);struct timeval tv={2,0};
  CHECK(select(alias+1,&rd,NULL,NULL,&tv)==1&&FD_ISSET(alias,&rd));
  CHECK(pthread_join(t,NULL)==0&&w.result==8);CHECK(read(e,&value,8)==8&&value==w.value);}
 struct pollfd empty={alias,POLLIN,0};CHECK(poll(&empty,1,10)==0);
 /* A plain blocking eventfd read still wakes after a filtered poll timeout. */
 int block=eventfd(0,0);CHECK(block>=0);w.fd=block;w.result=0;
 made=pthread_create(&t,NULL,later,&w)==0;CHECK(made);
 if(made){CHECK(read(block,&value,8)==8&&value==w.value);CHECK(pthread_join(t,NULL)==0&&w.result==8);}close(block);
 done:
 if(ep>=0)close(ep);
 if(outer>=0)close(outer);
 if(ino>=0)close(ino);
 if(sv[0]>=0)close(sv[0]);
 if(sv[1]>=0)close(sv[1]);
 close(alias);close(e);
}
int main(void){alarm(30);setvbuf(stdout,NULL,_IONBF,0);for(int i=0;i<4;i++)sample(i);alarm(0);
 printf("pollfiltertest: %s (%d checks, %d failures)\n",failed?"FAILED":"PASSED",checks,failed);return failed!=0;}
