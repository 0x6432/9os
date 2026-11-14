/* Same parser/oracle on Linux and 9os. --mutate requires mount privileges. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <poll.h>
#include <sys/epoll.h>
static int checks,fails;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"mountinfotest: FAIL line %d: %s\n",__LINE__,#x);fails++;}}while(0)
struct row { unsigned long long id,parent;unsigned ma,mi;char root[4096],path[4096],type[128],src[4096],opts[512]; };
static struct row rows[2048];static int nr;
static int decode(char *out,size_t cap,const char *p){size_t n=0;for(;*p;p++){unsigned c=(unsigned char)*p;if(c=='\\'){if(p[1]<'0'||p[1]>'7'||p[2]<'0'||p[2]>'7'||p[3]<'0'||p[3]>'7')return -1;c=(p[1]-'0')*64+(p[2]-'0')*8+p[3]-'0';p+=3;if(!c||c>255)return -1;}if(n+1>=cap)return -1;out[n++]=(char)c;}out[n]=0;return 0;}
static void load(const char *path){
 FILE*f=fopen(path,"r");CHECK(f!=NULL);nr=0;if(!f)return;
 char *line=NULL;size_t cap=0;ssize_t n;
 while((n=getline(&line,&cap,f))>0){
  CHECK(line[n-1]=='\n');CHECK(memchr(line,0,(size_t)n)==NULL);
  char*tok[256],*save;int nt=0;for(char*p=strtok_r(line," \n",&save);p&&nt<256;p=strtok_r(NULL," \n",&save))tok[nt++]=p;
  int sep=6;while(sep<nt&&strcmp(tok[sep],"-"))sep++;
  CHECK(sep>=6&&sep+4==nt);if(sep<6||sep+4!=nt)continue;
  CHECK(nr<2048);if(nr==2048)break;struct row*r=&rows[nr++];memset(r,0,sizeof*r);
  char end;CHECK(sscanf(tok[0],"%llu%c",&r->id,&end)==1&&r->id>0);CHECK(sscanf(tok[1],"%llu%c",&r->parent,&end)==1&&r->parent>0);
  CHECK(sscanf(tok[2],"%u:%u%c",&r->ma,&r->mi,&end)==2);
  CHECK(decode(r->root,sizeof r->root,tok[3])==0&&r->root[0]=='/');
  CHECK(decode(r->path,sizeof r->path,tok[4])==0&&r->path[0]=='/');
  snprintf(r->opts,sizeof r->opts,"%s",tok[5]);snprintf(r->type,sizeof r->type,"%s",tok[sep+1]);
  CHECK(decode(r->src,sizeof r->src,tok[sep+2])==0);
  CHECK(!strncmp(r->opts,"rw",2)||!strncmp(r->opts,"ro",2));
  for(int j=0;j<nr-1;j++)CHECK(rows[j].id!=r->id);
 }
 CHECK(!ferror(f));free(line);CHECK(fclose(f)==0);CHECK(nr>0);
}
static struct row *find(const char*path){for(int i=nr-1;i>=0;i--)if(!strcmp(rows[i].path,path))return &rows[i];return NULL;}
static void device(const char*path){struct row*r=find(path);CHECK(r!=NULL);if(!r)return;struct stat st;CHECK(stat(path,&st)==0);CHECK(r->ma==major(st.st_dev)&&r->mi==minor(st.st_dev));}
static int has_id(unsigned long long id){for(int i=0;i<nr;i++)if(rows[i].id==id)return 1;return 0;}
static void quiet(int fd){struct pollfd p={fd,POLLPRI,0};CHECK(poll(&p,1,0)==0);}
static void changed(int fd){struct pollfd p={fd,POLLPRI,0};CHECK(poll(&p,1,0)==1);CHECK((p.revents&(POLLPRI|POLLERR))==(POLLPRI|POLLERR));quiet(fd);}
static int contains_fd(int fd,const char*needle){
 CHECK(lseek(fd,0,SEEK_SET)==0);size_t cap=4096,len=0;char*b=malloc(cap);CHECK(b!=NULL);if(!b)return 0;
 for(;;){if(len+1024+1>cap){cap*=2;char*p=realloc(b,cap);if(!p){free(b);CHECK(0);return 0;}b=p;}ssize_t n=read(fd,b+len,1024);if(n<0){CHECK(0);free(b);return 0;}if(!n)break;len+=(size_t)n;}
 b[len]=0;int yes=strstr(b,needle)!=NULL;free(b);return yes;
}
/* GLib drains EPOLLIN|EPOLLET startup events for these permanently readable
 * proc files. Repeated level events here make its initialization loop forever. */
static void epoll_proc(const char *path){
 int fd=open(path,O_RDONLY),ep=epoll_create1(EPOLL_CLOEXEC),outer=epoll_create1(EPOLL_CLOEXEC);
 CHECK(fd>=0&&ep>=0&&outer>=0);if(fd<0||ep<0||outer<0)goto done;
 struct epoll_event ev={.events=EPOLLIN,.data.u64=123},out;
 CHECK(epoll_ctl(ep,EPOLL_CTL_ADD,fd,&ev)==0);
 CHECK(epoll_wait(ep,&out,1,0)==1&&(out.events&EPOLLIN));
 CHECK(epoll_wait(ep,&out,1,0)==1); /* level trigger stays ready */
 ev.events=EPOLLIN|EPOLLET;CHECK(epoll_ctl(ep,EPOLL_CTL_MOD,fd,&ev)==0);
 ev.events=EPOLLIN;CHECK(epoll_ctl(outer,EPOLL_CTL_ADD,ep,&ev)==0);
 CHECK(epoll_wait(outer,&out,1,0)==1);CHECK(epoll_wait(outer,&out,1,0)==1); /* peeking must not consume inner event */
 CHECK(epoll_wait(ep,&out,1,0)==1&&out.data.u64==123&&(out.events&EPOLLIN));
 CHECK(epoll_wait(ep,&out,1,0)==0);CHECK(epoll_wait(outer,&out,1,0)==0);
 char c;CHECK(read(fd,&c,1)==1);CHECK(epoll_wait(ep,&out,1,0)==0);
 CHECK(lseek(fd,0,SEEK_SET)==0);CHECK(epoll_wait(ep,&out,1,0)==0);
 ev.events=EPOLLIN|EPOLLET|EPOLLONESHOT;ev.data.u64=456;CHECK(epoll_ctl(ep,EPOLL_CTL_MOD,fd,&ev)==0);
 CHECK(epoll_wait(outer,&out,1,0)==1);CHECK(epoll_wait(ep,&out,1,0)==1&&out.data.u64==456);
 CHECK(epoll_wait(outer,&out,1,0)==0);CHECK(epoll_wait(ep,&out,1,0)==0);
 CHECK(epoll_ctl(ep,EPOLL_CTL_MOD,fd,&ev)==0);CHECK(epoll_wait(ep,&out,1,0)==1);
 CHECK(epoll_ctl(ep,EPOLL_CTL_DEL,fd,NULL)==0);CHECK(epoll_wait(ep,&out,1,0)==0);
 done: if(fd>=0)CHECK(close(fd)==0);if(ep>=0)CHECK(close(ep)==0);if(outer>=0)CHECK(close(outer)==0);
}
int main(int argc,char**argv){
 epoll_proc("/proc/self/mountinfo");epoll_proc("/proc/self/mounts");epoll_proc("/proc/mounts");
 load("/proc/self/mountinfo");device("/");device("/proc");struct row*r=find("/");unsigned long long rootid=r?r->id:0;
 char path[128];snprintf(path,sizeof path,"/proc/%d/mountinfo",getpid());load(path);r=find("/");CHECK(r&&r->id==rootid);
 int f=open("/proc/self/mountinfo",O_RDONLY);CHECK(f>=0);if(f>=0){char a[113],b[113];ssize_t n=read(f,a,sizeof a);CHECK(n>0);CHECK(lseek(f,0,SEEK_SET)==0);CHECK(n>0&&read(f,b,sizeof b)==n&&memcmp(a,b,(size_t)n)==0);CHECK(close(f)==0);}
 f=open("/proc/self/mounts",O_RDONLY);CHECK(f>=0);if(f>=0){char b[128];CHECK(read(f,b,sizeof b)>0);CHECK(close(f)==0);}
 int watch=open("/proc/self/mountinfo",O_RDONLY),watch2=open("/proc/self/mountinfo",O_RDONLY),mtab=open("/proc/mounts",O_RDONLY);
 CHECK(watch>=0&&watch2>=0&&mtab>=0);if(watch>=0)quiet(watch);if(watch2>=0)quiet(watch2);if(mtab>=0)quiet(mtab);
 if(argc==2&&!strcmp(argv[1],"--mutate")){
  char base[256],inner[280];snprintf(base,sizeof base,"/tmp/mountinfo-%d space\t\\end",getpid());snprintf(inner,sizeof inner,"%s/nested",base);
  CHECK(mkdir(base,0700)==0);
  CHECK(mount("bad",base,"no-such-filesystem",0,NULL)<0);quiet(watch);
  int syncfd[2];CHECK(pipe(syncfd)==0);pid_t child=fork();CHECK(child>=0);
  if(child==0){close(syncfd[0]);struct pollfd p={watch,POLLPRI,0};if(write(syncfd[1],"r",1)!=1)_exit(2);close(syncfd[1]);int n=poll(&p,1,5000);_exit(n==1&&(p.revents&(POLLPRI|POLLERR))==(POLLPRI|POLLERR)?0:3);}
  close(syncfd[1]);char ready;CHECK(read(syncfd[0],&ready,1)==1);close(syncfd[0]);usleep(20000);
  int mounted=mount("test source\t\\end",base,"tmpfs",0,NULL)==0;CHECK(mounted);
  if(child>0){int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
  if(mounted){
   quiet(watch); /* fork shares the file's acknowledged event */
   CHECK(contains_fd(watch2,"test\\040source\\011\\134end"));
   changed(watch2); /* reading did not acknowledge the independent poll event */
   CHECK(contains_fd(mtab,"test\\040source\\011\\134end"));changed(mtab);
   load("/proc/self/mountinfo");r=find(base);CHECK(r!=NULL);unsigned long long outerid=r?r->id:0;
   if(r){CHECK(!strcmp(r->src,"test source\t\\end"));CHECK(!strcmp(r->type,"tmpfs"));CHECK(has_id(r->parent));}
   device(base);CHECK(mkdir(inner,0700)==0);int nested=mount("nested",inner,"tmpfs",0,NULL)==0;CHECK(nested);
   unsigned long long innerid=0;if(nested){
    changed(watch);changed(watch2);changed(mtab);
    CHECK(mount(NULL,inner,NULL,MS_REMOUNT|MS_RDONLY,NULL)==0);changed(watch);load("/proc/self/mountinfo");r=find(inner);CHECK(r&&!strncmp(r->opts,"ro",2));
    CHECK(mount(NULL,inner,NULL,MS_REMOUNT,NULL)==0);changed(watch);
    load("/proc/self/mountinfo");r=find(inner);CHECK(r!=NULL);if(r){innerid=r->id;CHECK(r->parent==outerid);CHECK(innerid!=outerid);}device(inner);
    CHECK(umount(inner)==0);changed(watch);CHECK(!contains_fd(watch," - tmpfs nested "));load("/proc/self/mountinfo");CHECK(!has_id(innerid));}
   CHECK(rmdir(inner)==0);CHECK(umount(base)==0);load("/proc/self/mountinfo");CHECK(!has_id(outerid));CHECK(find(base)==NULL);
   mounted=mount("again",base,"tmpfs",0,NULL)==0;CHECK(mounted);if(mounted){load("/proc/self/mountinfo");r=find(base);CHECK(r&&r->id>0); /* Linux may reuse an unmounted ID. */CHECK(umount(base)==0);}
  }
  CHECK(rmdir(base)==0);
  char parent[128],renamed[128],oldmp[160],newmp[160];
  snprintf(parent,sizeof parent,"/tmp/mi-parent-%d",getpid());snprintf(renamed,sizeof renamed,"/tmp/mi-renamed-%d",getpid());
  snprintf(oldmp,sizeof oldmp,"%s/mount",parent);snprintf(newmp,sizeof newmp,"%s/mount",renamed);
  CHECK(mkdir(parent,0700)==0);CHECK(mkdir(oldmp,0700)==0);
  mounted=mount("rename-source",oldmp,"tmpfs",0,NULL)==0;CHECK(mounted);
  if(mounted){CHECK(contains_fd(watch,oldmp));load("/proc/self/mountinfo");r=find(oldmp);unsigned long long id=r?r->id:0;
   CHECK(rename(parent,renamed)==0);CHECK(contains_fd(watch,newmp));CHECK(!contains_fd(watch,oldmp));
   load("/proc/self/mountinfo");r=find(newmp);CHECK(r&&r->id==id);CHECK(umount(newmp)==0);
   CHECK(rmdir(newmp)==0);CHECK(rmdir(renamed)==0);
  }else{CHECK(rmdir(oldmp)==0);CHECK(rmdir(parent)==0);}
 }
 if(watch>=0)CHECK(close(watch)==0);
 if(watch2>=0)CHECK(close(watch2)==0);
 if(mtab>=0)CHECK(close(mtab)==0);
 printf("mountinfotest: %d checks, %d failures\n",checks,fails);return fails!=0;
}
