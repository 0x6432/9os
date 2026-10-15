/* Same-source Linux/9os filesystem identity and boot command line contract. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
static int checks,fails;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"procinfotest: FAIL line %d: %s\n",__LINE__,#x);fails++;}}while(0)
static void fs(const char *path,long magic){struct statfs s;CHECK(statfs(path,&s)==0);CHECK((unsigned long)s.f_type==(unsigned long)magic);int f=open(path,O_RDONLY|O_DIRECTORY);CHECK(f>=0);CHECK(fstatfs(f,&s)==0);CHECK((unsigned long)s.f_type==(unsigned long)magic);CHECK(close(f)==0);}
static void procstat(void){
 char buf[4096],old[16];CHECK(prctl(PR_GET_NAME,old)==0);CHECK(prctl(PR_SET_NAME,"proc (edge))")==0);
 int f=open("/proc/self/stat",O_RDONLY);CHECK(f>=0);ssize_t n=read(f,buf,sizeof(buf)-1);CHECK(n>0&&n<(ssize_t)sizeof(buf)-1);CHECK(close(f)==0);
 if(n<=0)return;
 buf[n]=0;CHECK(buf[n-1]=='\n');CHECK(atoi(buf)==getpid());char *end=strrchr(buf,')');CHECK(end!=NULL);
 if(end){CHECK(strstr(buf,"(proc (edge)))")!=NULL);CHECK(end[1]==' '&&end[3]==' ');char *p=end+4,*q;long long fields[49];int count=0;
  while(*p&&count<49){fields[count]=strtoll(p,&q,10);if(q==p)break;count++;p=q;while(*p==' '||*p=='\n')p++;}
  CHECK(count==49);CHECK(*p==0);if(count==49){CHECK(fields[0]==getppid());CHECK(fields[1]==getpgrp());CHECK(fields[2]==getsid(0));CHECK(fields[16]>=1);CHECK(fields[35]>=0);CHECK(fields[36]>=0);CHECK(fields[37]==syscall(SYS_sched_getscheduler,0));}
 }
 CHECK(prctl(PR_SET_NAME,old)==0);
}
int main(void){procstat();fs("/proc",0x9fa0);fs("/sys",0x62656572);fs("/sys/class",0x62656572);char a[16384],b[16384];int f=open("/proc/cmdline",O_RDONLY);CHECK(f>=0);ssize_t n=read(f,a,sizeof a);CHECK(n>0&&n<(ssize_t)sizeof a);if(n>0){CHECK(a[n-1]=='\n');CHECK(memchr(a,0,n)==0);CHECK(read(f,b,sizeof b)==0);CHECK(lseek(f,0,SEEK_SET)==0);CHECK(read(f,b,sizeof b)==n);CHECK(memcmp(a,b,n)==0);}CHECK(close(f)==0);printf("procinfotest: %d checks, %d failures\n",checks,fails);return fails?1:0;}
