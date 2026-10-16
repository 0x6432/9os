/* Same-source Linux/9os real thread enumeration/identity contract for strace -f. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <signal.h>
static int checks,fails;
#define CHECK(x) do { checks++;if(!(x)){fprintf(stderr,"proctasktest: FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);fails++;} } while(0)
static atomic_int ready,done;
static pid_t tids[2];
static volatile sig_atomic_t signal_receiver;
static void caught(int sig){(void)sig;signal_receiver=(sig_atomic_t)syscall(SYS_gettid);}
static void *worker(void *arg){long n=(long)arg;char name[16];snprintf(name,sizeof name,"pt-worker-%ld",n);prctl(PR_SET_NAME,name);tids[n]=(pid_t)syscall(SYS_gettid);atomic_fetch_add(&ready,1);while(!atomic_load(&done))usleep(1000);return NULL;}
static int readtext(const char*path,char*b,size_t cap){b[0]=0;FILE*f=fopen(path,"r");if(!f)return -1;size_t n=fread(b,1,cap-1,f);int bad=ferror(f);b[n]=0;fclose(f);return bad?-1:(int)n;}
static long field(const char*b,const char*key){const char*p=b;size_t n=strlen(key);while(*p){if(!strncmp(p,key,n))return strtol(p+n,NULL,10);p=strchr(p,'\n');if(!p)break;p++;}return -1;}
static int enumerated(const char*path,pid_t id){DIR*d=opendir(path);CHECK(d!=NULL);if(!d)return 0;int found=0;struct dirent*e;while((e=readdir(d)))if(atoi(e->d_name)==id)found++;CHECK(closedir(d)==0);return found;}
static void inspect(pid_t tid,const char*name){
 CHECK(kill(tid,0)==0);
 char base[128],path[180],b[8192];snprintf(base,sizeof base,"/proc/%d/task/%d",getpid(),tid);
 struct stat st={0};CHECK(stat(base,&st)==0&&S_ISDIR(st.st_mode));CHECK(st.st_uid==geteuid());
 snprintf(path,sizeof path,"%s/status",base);CHECK(readtext(path,b,sizeof b)>0);CHECK(field(b,"Tgid:")==getpid());CHECK(field(b,"Pid:")==tid);CHECK(field(b,"PPid:")==getppid());CHECK(field(b,"Threads:")==3);
 char expected[64];snprintf(expected,sizeof expected,"Name:\t%s\n",name);CHECK(strstr(b,expected)!=NULL);
 snprintf(path,sizeof path,"%s/comm",base);CHECK(readtext(path,b,sizeof b)>0);snprintf(expected,sizeof expected,"%s\n",name);CHECK(!strcmp(b,expected));
 snprintf(path,sizeof path,"%s/stat",base);CHECK(readtext(path,b,sizeof b)>0);CHECK(atoi(b)==tid);snprintf(expected,sizeof expected,"(%s)",name);CHECK(strstr(b,expected)!=NULL);
 snprintf(path,sizeof path,"%s/maps",base);CHECK(readtext(path,b,sizeof b)>0&&strchr(b,'-')!=NULL);
 snprintf(path,sizeof path,"/proc/%d/status",tid);CHECK(readtext(path,b,sizeof b)>0);CHECK(field(b,"Tgid:")==getpid());CHECK(field(b,"Pid:")==tid);
 snprintf(path,sizeof path,"%s/../%d/status",base,tid);CHECK(readtext(path,b,sizeof b)>0&&field(b,"Pid:")==tid);
}
int main(int argc,char **argv){
 if(argc>2||(argc==2&&strcmp(argv[1],"--pause"))){fprintf(stderr,"usage: proctasktest [--pause]\n");return 2;}
 alarm(30);char old[16];CHECK(prctl(PR_GET_NAME,old)==0);CHECK(prctl(PR_SET_NAME,"pt-main")==0);
 sigset_t block,oldmask;sigemptyset(&block);sigaddset(&block,SIGUSR1);CHECK(pthread_sigmask(SIG_BLOCK,&block,&oldmask)==0);
 struct sigaction action={.sa_handler=caught},oldaction;sigemptyset(&action.sa_mask);CHECK(sigaction(SIGUSR1,&action,&oldaction)==0);
 pthread_t t[2];int made=0;for(long i=0;i<2;i++){int r=pthread_create(&t[i],NULL,worker,(void*)i);CHECK(r==0);if(r)break;made++;}
 if(made!=2){atomic_store(&done,1);for(int i=0;i<made;i++)pthread_join(t[i],NULL);return 1;}
 while(atomic_load(&ready)!=2)usleep(1000);
 /* kill(TID, SIGUSR1) is process-directed: both workers inherited a block,
  * so only the unblocked main thread may receive it (unlike tgkill). */
 CHECK(pthread_sigmask(SIG_UNBLOCK,&block,NULL)==0);CHECK(kill(tids[0],SIGUSR1)==0);
 for(int n=0;n<1000&&!signal_receiver;n++)usleep(1000);
 CHECK(signal_receiver==getpid());
 if(argc==2){printf("PROCTASK_READY pid=%d tids=%d,%d\n",getpid(),tids[0],tids[1]);fflush(stdout);sleep(5);}
 char task[100];snprintf(task,sizeof task,"/proc/%d/task",getpid());CHECK(enumerated(task,getpid())==1);
 for(int i=0;i<2;i++){CHECK(tids[i]!=getpid());CHECK(enumerated(task,tids[i])==1);CHECK(enumerated("/proc",tids[i])==0);}
 inspect(getpid(),"pt-main");inspect(tids[0],"pt-worker-0");inspect(tids[1],"pt-worker-1");
 char path[160];snprintf(path,sizeof path,"%s/2147483647/status",task);errno=0;CHECK(access(path,F_OK)==-1&&errno==ENOENT);
 snprintf(path,sizeof path,"%s/999999999999999999999999999999999999999",task);errno=0;CHECK(access(path,F_OK)==-1&&errno==ENOENT);
 pid_t c=fork();CHECK(c>=0);if(c==0){char p[160];snprintf(p,sizeof p,"/proc/%d/task/%d/status",getpid(),tids[0]);errno=0;_exit(access(p,F_OK)==-1&&errno==ENOENT?0:2);}
 if(c>0){int status;CHECK(waitpid(c,&status,0)==c);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
 atomic_store(&done,1);for(int i=0;i<2;i++)CHECK(pthread_join(t[i],NULL)==0);
 /* pthread_join may precede final Linux task removal by a short interval. */
 for(int i=0;i<2;i++){snprintf(path,sizeof path,"/proc/%d/status",tids[i]);for(int n=0;n<1000&&access(path,F_OK)==0;n++)usleep(1000);}
 for(int i=0;i<2;i++){errno=0;CHECK(kill(tids[i],0)==-1&&errno==ESRCH);CHECK(enumerated(task,tids[i])==0);snprintf(path,sizeof path,"%s/%d/status",task,tids[i]);errno=0;CHECK(access(path,F_OK)==-1&&errno==ENOENT);snprintf(path,sizeof path,"/proc/%d/status",tids[i]);errno=0;CHECK(access(path,F_OK)==-1&&errno==ENOENT);}
 CHECK(sigaction(SIGUSR1,&oldaction,NULL)==0);CHECK(pthread_sigmask(SIG_SETMASK,&oldmask,NULL)==0);
 CHECK(prctl(PR_SET_NAME,old)==0);printf("proctasktest: %d checks, %d failures\n",checks,fails);return fails!=0;
}
