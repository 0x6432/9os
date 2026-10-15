/* Linux oracle: inherited sockets report the sender, not the socket creator.
 * eudev completes worker jobs with zero-byte datagrams and SCM_CREDENTIALS. */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
static int checks, fails;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"unixcredtest: FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); fails++; } } while(0)
static void receive(int fd,int flags,pid_t pid,uid_t uid,gid_t gid,size_t len){
 char data[4],ctl[CMSG_SPACE(sizeof(struct ucred))];memset(ctl,0,sizeof ctl);
 struct iovec io={data,sizeof data};struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=ctl,.msg_controllen=sizeof ctl};
 CHECK(recvmsg(fd,&m,flags)==(ssize_t)len);CHECK(!(m.msg_flags&MSG_CTRUNC));
 struct cmsghdr *c=CMSG_FIRSTHDR(&m);CHECK(c&&c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_CREDENTIALS&&c->cmsg_len==CMSG_LEN(sizeof(struct ucred)));
 if(c&&c->cmsg_len>=CMSG_LEN(sizeof(struct ucred))){struct ucred u;memcpy(&u,CMSG_DATA(c),sizeof u);CHECK(u.pid==pid);CHECK(u.uid==uid);CHECK(u.gid==gid);}
}
static void run(int type,int split){
 printf("unixcredtest: type=%d split=%d\n",type,split);fflush(stdout);
 int s[2],on=1;pid_t parent=getpid();uid_t uid=geteuid();gid_t gid=getegid();
 CHECK(socketpair(AF_UNIX,type,0,s)==0);CHECK(setsockopt(s[0],SOL_SOCKET,SO_PASSCRED,&on,sizeof on)==0);
 pid_t child=fork();CHECK(child>=0);if(child<0)exit(1);
 if(!child){close(s[0]);if(split&&(setresgid(1234,1235,0)||setresuid(1234,1235,0)))_exit(77);
  if(write(s[1],"",0)!=0)_exit(2);
  if(write(s[1],"x",1)!=1)_exit(3);
  close(s[1]);_exit(0);}
 close(s[1]);struct ucred peer; socklen_t n=sizeof peer;
 CHECK(getsockopt(s[0],SOL_SOCKET,SO_PEERCRED,&peer,&n)==0);CHECK(n==sizeof peer);CHECK(peer.pid==parent&&peer.uid==uid&&peer.gid==gid);
 if(type==SOCK_DGRAM){receive(s[0],MSG_PEEK,child,split?1234:getuid(),split?1234:getgid(),0);receive(s[0],0,child,split?1234:getuid(),split?1234:getgid(),0);}
 receive(s[0],MSG_PEEK,child,split?1234:getuid(),split?1234:getgid(),1);receive(s[0],0,child,split?1234:getuid(),split?1234:getgid(),1);
 int st;CHECK(waitpid(child,&st,0)==child);CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==0);close(s[0]);
}
static void boundaries(void){
 int s[2],on=1;CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);CHECK(setsockopt(s[0],SOL_SOCKET,SO_PASSCRED,&on,sizeof on)==0);
 pid_t child=fork();CHECK(child>=0);if(child<0)exit(1);
 if(!child){close(s[0]);_exit(write(s[1],"c",1)==1?0:2);}
 int st;CHECK(waitpid(child,&st,0)==child);CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==0);
 CHECK(write(s[1],"p",1)==1);
 receive(s[0],MSG_PEEK,child,getuid(),getgid(),1);receive(s[0],0,child,getuid(),getgid(),1);
 receive(s[0],0,getpid(),getuid(),getgid(),1);close(s[0]);close(s[1]);
}
static void shutdown_errors(void){
 int s[2];signal(SIGPIPE,SIG_IGN);
 for(int which=0;which<3;which++){
  CHECK(socketpair(AF_UNIX,SOCK_DGRAM,0,s)==0);
  if(which==0)CHECK(close(s[1])==0);
  else CHECK(shutdown(s[which==1?1:0],which==1?SHUT_RD:SHUT_WR)==0);
  errno=0;CHECK(write(s[0],"",0)==-1);CHECK(errno==(which==0?ECONNREFUSED:EPIPE));
  close(s[0]);if(which)close(s[1]);
 }
}
int main(void){alarm(20);shutdown_errors();boundaries();run(SOCK_DGRAM,0);run(SOCK_STREAM,0);if(geteuid()==0){run(SOCK_DGRAM,1);run(SOCK_STREAM,1);}else puts("unixcredtest: split real/effective IDs require root");printf("unixcredtest: %d checks, %d failures\n",checks,fails);return fails?1:0;}
