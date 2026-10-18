/* Targeted poll wakeups must retain aliases, conservative nested epoll, and
 * mixed inotify leaves. Exact source runs on Linux. Timings are not perf gates. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
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
struct create_file { char path[256]; int result; };
static void *create_later(void *arg) {
 struct create_file *c=arg;usleep(30000);
 int fd=open(c->path,O_WRONLY|O_CREAT|O_EXCL,0600);
 c->result=fd<0?-errno:close(fd);return NULL;
}
static void inotify_leaf(int direct_epoll) {
 char dir[]="/tmp/pollfilter-inotify-XXXXXX";
 CHECK(mkdtemp(dir)!=NULL);
 int in=inotify_init1(IN_NONBLOCK|IN_CLOEXEC),e=eventfd(0,EFD_NONBLOCK),ep=-1;
 CHECK(in>=0&&e>=0);if(in<0||e<0)goto done;
 if(direct_epoll){
  ep=epoll_create1(EPOLL_CLOEXEC);CHECK(ep>=0);if(ep<0)goto done;
  struct epoll_event ev={.events=EPOLLIN,.data.u64=41};
  CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,in,&ev)==0);
  ev.data.u64=42;CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,e,&ev)==0);
 }
 int wd=inotify_add_watch(in,dir,IN_CREATE);CHECK(wd>=0);if(wd<0)goto done;
 struct create_file c={.result=-1};snprintf(c.path,sizeof c.path,"%s/watch.txt",dir);
 pthread_t t;int made=pthread_create(&t,NULL,create_later,&c)==0;CHECK(made);
 if(made){
  if(direct_epoll){struct epoll_event ev={0};
   CHECK(epoll_pwait(ep,&ev,1,2000,NULL)==1&&ev.events==EPOLLIN&&ev.data.u64==41);
  }else{struct pollfd p[2]={{in,POLLIN,0},{e,POLLIN,0}};
   CHECK(poll(p,2,2000)==1&&(p[0].revents&POLLIN)&&!p[1].revents);}
  CHECK(pthread_join(t,NULL)==0&&c.result==0);
  union {struct inotify_event event;char bytes[512];} b;
  ssize_t n=read(in,b.bytes,sizeof b.bytes);
  CHECK(n>=(ssize_t)sizeof b.event&&b.event.wd==wd&&(b.event.mask&IN_CREATE));
  CHECK(n>(ssize_t)sizeof b.event&&b.event.len>=10&&b.event.len<=sizeof b.bytes-sizeof b.event&&n>=(ssize_t)sizeof b.event+b.event.len&&!strcmp(b.event.name,"watch.txt"));
  CHECK(unlink(c.path)==0);
 }
 CHECK(inotify_rm_watch(in,wd)==0);
 done:
 if(ep>=0)close(ep);
 if(in>=0)close(in);
 if(e>=0)close(e);
 CHECK(rmdir(dir)==0);
}
/* Direct leaves, mixed idle sources and nested fallback. Register only the
 * alias, then close the original descriptor before the second delivery. */
static void direct_sample(int kind) {
 int e=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC),alias=-1,ep=-1,outer=-1,in=-1,sv[2]={-1,-1};
 CHECK(e>=0);if(e<0)return;
 alias=dup(e);ep=epoll_create1(EPOLL_CLOEXEC);CHECK(alias>=0&&ep>=0);
 if(alias<0||ep<0)goto done;
 struct epoll_event ev={.events=EPOLLIN,.data.u64=71},got[2]={{0}};
 CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,alias,&ev)==0);
 int waitfd=ep;uint64_t tag=71;
 if(kind==1){
  in=inotify_init1(IN_CLOEXEC|IN_NONBLOCK);CHECK(in>=0);
  CHECK(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,sv)==0);
  ev.data.u64=72;CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,in,&ev)==0);
  ev.data.u64=73;CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,sv[0],&ev)==0);
 }
 if(kind==2){
  outer=epoll_create1(EPOLL_CLOEXEC);CHECK(outer>=0);
  ev.data.u64=74;CHECK(epoll_ctl(outer,EPOLL_CTL_ADD,ep,&ev)==0);waitfd=outer;tag=74;
 }
 for(int round=0;round<2;round++){
  struct put w={round?alias:e,(uint64_t)(11+round),0};pthread_t t;
  int made=pthread_create(&t,NULL,later,&w)==0;CHECK(made);
  if(made){
   int n=round?epoll_wait(waitfd,got,2,2000):epoll_pwait(waitfd,got,2,2000,NULL);
   CHECK(n==1&&got[0].events==EPOLLIN&&got[0].data.u64==tag);
   CHECK(pthread_join(t,NULL)==0&&w.result==8);
   uint64_t value=0;CHECK(read(alias,&value,8)==8&&value==w.value);
  }
  CHECK(epoll_wait(waitfd,got,2,10)==0);
  if(!round){CHECK(close(e)==0);e=-1;}
 }
 if(in>=0){char bytes[256];errno=0;CHECK(read(in,bytes,sizeof bytes)==-1&&errno==EAGAIN);}
 if(sv[0]>=0){char byte;errno=0;CHECK(read(sv[0],&byte,1)==-1&&errno==EAGAIN);}
 /* The masked direct wait must not leak a filter into unfiltered select. */
 struct put w={alias,19,0};pthread_t t;int made=pthread_create(&t,NULL,later,&w)==0;CHECK(made);
 if(made){fd_set rd;FD_ZERO(&rd);FD_SET(alias,&rd);struct timeval tv={2,0};uint64_t value=0;
  CHECK(select(alias+1,&rd,NULL,NULL,&tv)==1&&FD_ISSET(alias,&rd));
  CHECK(pthread_join(t,NULL)==0&&w.result==8);CHECK(read(alias,&value,8)==8&&value==19);}
 done:
 if(outer>=0)close(outer);
 if(ep>=0)close(ep);
 if(alias>=0)close(alias);
 if(e>=0)close(e);
 if(in>=0)close(in);
 if(sv[0]>=0)close(sv[0]);
 if(sv[1]>=0)close(sv[1]);
}
struct ctl_put {int ep,fd,op,result;ssize_t wrote;};
static void *ctl_later(void *arg){
 struct ctl_put *c=arg;usleep(30000);uint64_t value=23;
 c->wrote=write(c->fd,&value,8);
 struct epoll_event ev={.events=EPOLLIN,.data.u64=81};
 c->result=epoll_ctl(c->ep,c->op,c->fd,&ev);return NULL;
}
static void topology(int modify){
 int ep=epoll_create1(EPOLL_CLOEXEC),idle=eventfd(0,EFD_NONBLOCK),target=eventfd(0,EFD_NONBLOCK);
 CHECK(ep>=0&&idle>=0&&target>=0);if(ep<0||idle<0||target<0)goto done;
 struct epoll_event ev={.events=EPOLLIN,.data.u64=80},got={0};
 CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,idle,&ev)==0);
 if(modify){ev.events=0;ev.data.u64=82;CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,target,&ev)==0);}
 struct ctl_put c={ep,target,modify?EPOLL_CTL_MOD:EPOLL_CTL_ADD,-1,0};pthread_t t;
 int made=pthread_create(&t,NULL,ctl_later,&c)==0;CHECK(made);
 if(made){
  CHECK(epoll_pwait(ep,&got,1,2000,NULL)==1&&got.events==EPOLLIN&&got.data.u64==81);
  CHECK(pthread_join(t,NULL)==0&&c.wrote==8&&c.result==0);
  uint64_t value=0;CHECK(read(target,&value,8)==8&&value==23);
 }
 CHECK(epoll_ctl(ep,EPOLL_CTL_DEL,target,NULL)==0);
 CHECK(epoll_wait(ep,&got,1,10)==0);
 done:
 if(ep>=0)close(ep);
 if(idle>=0)close(idle);
 if(target>=0)close(target);
}
static void edge_oneshot(void){
 int ep=epoll_create1(EPOLL_CLOEXEC),e=eventfd(0,EFD_NONBLOCK);CHECK(ep>=0&&e>=0);
 if(ep<0||e<0)goto done;
 struct epoll_event ev={.events=EPOLLIN|EPOLLONESHOT,.data.u64=91},got={0};
 CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,e,&ev)==0);
 uint64_t value=1,out=0;CHECK(write(e,&value,8)==8);
 CHECK(epoll_wait(ep,&got,1,2000)==1&&got.events==EPOLLIN&&got.data.u64==91);
 CHECK(epoll_wait(ep,&got,1,10)==0);
 CHECK(epoll_ctl(ep,EPOLL_CTL_MOD,e,&ev)==0);
 CHECK(epoll_wait(ep,&got,1,2000)==1&&got.events==EPOLLIN&&got.data.u64==91);
 CHECK(read(e,&out,8)==8&&out==1);
 CHECK(epoll_ctl(ep,EPOLL_CTL_DEL,e,NULL)==0);
 ev.events=EPOLLIN|EPOLLET;CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,e,&ev)==0);
 for(int i=0;i<2;i++){
  struct put w={e,(uint64_t)(31+i),0};pthread_t t;int made=pthread_create(&t,NULL,later,&w)==0;CHECK(made);
  if(made){
   CHECK(epoll_pwait(ep,&got,1,2000,NULL)==1&&got.events==EPOLLIN&&got.data.u64==91);
   CHECK(pthread_join(t,NULL)==0&&w.result==8);CHECK(read(e,&out,8)==8&&out==w.value);
  }
  CHECK(epoll_wait(ep,&got,1,10)==0);
 }
 done:if(ep>=0)close(ep);if(e>=0)close(e);
}
static volatile sig_atomic_t caught;
static void on_signal(int sig){if(sig==SIGUSR1)caught++;}
struct signal_put {pthread_t target;int result;};
static void *signal_later(void *arg){struct signal_put *s=arg;usleep(30000);s->result=pthread_kill(s->target,SIGUSR1);return NULL;}
static void signal_mask(void){
 int ep=epoll_create1(EPOLL_CLOEXEC),e=eventfd(0,EFD_NONBLOCK);CHECK(ep>=0&&e>=0);
 if(ep<0||e<0)goto done;
 struct epoll_event ev={.events=EPOLLIN,.data.u64=101},got={0};CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,e,&ev)==0);
 struct sigaction action={.sa_handler=on_signal},old_action;sigemptyset(&action.sa_mask);
 int installed=sigaction(SIGUSR1,&action,&old_action)==0;CHECK(installed);if(!installed)goto done;
 sigset_t block,oldmask,temporary,after;sigemptyset(&block);sigaddset(&block,SIGUSR1);
 int blocked=pthread_sigmask(SIG_BLOCK,&block,&oldmask)==0;CHECK(blocked);
 if(blocked){
  temporary=oldmask;sigdelset(&temporary,SIGUSR1);caught=0;
  struct signal_put s={pthread_self(),-1};pthread_t t;int made=pthread_create(&t,NULL,signal_later,&s)==0;CHECK(made);
  if(made){
   errno=0;CHECK(epoll_pwait(ep,&got,1,2000,&temporary)==-1&&errno==EINTR&&caught==1);
   CHECK(pthread_join(t,NULL)==0&&s.result==0);
   CHECK(pthread_sigmask(SIG_SETMASK,NULL,&after)==0&&sigismember(&after,SIGUSR1)==1);
   CHECK(epoll_wait(ep,&got,1,10)==0);
  }
  CHECK(pthread_sigmask(SIG_SETMASK,&oldmask,NULL)==0);
 }
 CHECK(sigaction(SIGUSR1,&old_action,NULL)==0);
 done:if(ep>=0)close(ep);if(e>=0)close(e);
}
int main(void){alarm(30);setvbuf(stdout,NULL,_IONBF,0);for(int i=0;i<4;i++)sample(i);inotify_leaf(0);
 for(int i=0;i<3;i++)direct_sample(i);
 inotify_leaf(1);topology(0);topology(1);edge_oneshot();signal_mask();alarm(0);
 printf("pollfiltertest: %s (%d checks, %d failures)\n",failed?"FAILED":"PASSED",checks,failed);return failed!=0;}
