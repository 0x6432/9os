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
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
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
/* Keep the exited child unreaped: /proc/PID still exists but its fs context
 * has gone away. A process enumerator must get ENOENT, not dereference NULL. */
static void zombie_cwd(void){
 int gate[2];int r=pipe(gate);CHECK(r==0);if(r)return;
 pid_t child=fork();CHECK(child>=0);
 if(child==0){close(gate[1]);char c;while(read(gate[0],&c,1)<0&&errno==EINTR){}close(gate[0]);_exit(17);}
 close(gate[0]);if(child<0){close(gate[1]);return;}
 char path[100],cwd[4096],link[4096];struct stat st;
 snprintf(path,sizeof path,"/proc/%d/cwd",child);
 CHECK(getcwd(cwd,sizeof cwd)!=NULL);
 ssize_t n=readlink(path,link,sizeof link-1);CHECK(n>0);
 if(n>0){link[n]=0;CHECK(!strcmp(link,cwd));}
 CHECK(close(gate[1])==0);
 siginfo_t info={0};CHECK(waitid(P_PID,child,&info,WEXITED|WNOWAIT)==0);
 CHECK(info.si_pid==child&&info.si_code==CLD_EXITED&&info.si_status==17);
 CHECK(lstat(path,&st)==0&&S_ISLNK(st.st_mode));
 errno=0;CHECK(readlink(path,link,sizeof link)==-1&&errno==ENOENT);
 errno=0;CHECK(open(path,O_RDONLY|O_DIRECTORY)==-1&&errno==ENOENT);
 int status;CHECK(waitpid(child,&status,0)==child);
 CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==17);
 errno=0;CHECK(lstat(path,&st)==-1&&errno==ENOENT);
}
/* Proc FD names are canonical unsigned decimal, not wrapping integers.
 * Keep a safe old-kernel control: 2^32 + a live fd must not alias that fd.
 * Full malformed cases run only on the fixed kernel and the Linux oracle. */
static void fd_names(int wrap_only){
 int fd=open("/dev/null",O_RDONLY);CHECK(fd>=0);if(fd<0)return;
 char dirs[3][100],path[512],link[256],name[100];struct stat st;
 snprintf(dirs[0],sizeof dirs[0],"/proc/self/fd");
 snprintf(dirs[1],sizeof dirs[1],"/proc/self/task/%ld/fd",syscall(SYS_gettid));
 snprintf(dirs[2],sizeof dirs[2],"/proc/%ld/fd",syscall(SYS_gettid));
 const char *bad[]={"00","01","+1","-1","1x","2147483647","2147483648",
  "4294967295","4294967296","8589934592","9999999999999999999999999999999999999999"};
 for(unsigned d=0;d<3;d++){
  snprintf(path,sizeof path,"%s/%d",dirs[d],fd);
  ssize_t n=readlink(path,link,sizeof link-1);CHECK(n>0);
  if(n>0){link[n]=0;CHECK(!strcmp(link,"/dev/null"));}
  CHECK(lstat(path,&st)==0&&S_ISLNK(st.st_mode));
  snprintf(name,sizeof name,"%llu",4294967296ULL+(unsigned)fd);
  snprintf(path,sizeof path,"%s/%s",dirs[d],name);
  errno=0;CHECK(readlink(path,link,sizeof link)==-1&&errno==ENOENT);
  if(wrap_only)continue;
  snprintf(name,sizeof name,"0%d",fd);
  for(unsigned i=0;i<=sizeof bad/sizeof bad[0];i++){
   snprintf(path,sizeof path,"%s/%s",dirs[d],i==sizeof bad/sizeof bad[0]?name:bad[i]);
   errno=0;CHECK(readlink(path,link,sizeof link)==-1&&errno==ENOENT);
   errno=0;CHECK(lstat(path,&st)==-1&&errno==ENOENT);
   errno=0;int opened=open(path,O_RDONLY);CHECK(opened==-1&&errno==ENOENT);
   if(opened>=0)close(opened);
  }
 }
 CHECK(close(fd)==0);
}
int main(int argc,char **argv){alarm(30);if(argc==2&&!strcmp(argv[1],"--fd-wrap-control")){fd_names(1);}else{fd_names(0);procstat();zombie_cwd();fs("/proc",0x9fa0);fs("/sys",0x62656572);fs("/sys/class",0x62656572);char a[16384],b[16384];int f=open("/proc/cmdline",O_RDONLY);CHECK(f>=0);ssize_t n=read(f,a,sizeof a);CHECK(n>0&&n<(ssize_t)sizeof a);if(n>0){CHECK(a[n-1]=='\n');CHECK(memchr(a,0,n)==0);CHECK(read(f,b,sizeof b)==0);CHECK(lseek(f,0,SEEK_SET)==0);CHECK(read(f,b,sizeof b)==n);CHECK(memcmp(a,b,n)==0);}CHECK(close(f)==0);}printf("procinfotest: %d checks, %d failures\n",checks,fails);return fails?1:0;}
