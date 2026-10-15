/* Linux ignores the upper32 bits of the ioctl request on every 64-bit ABI.
 * Prebuilt musl's int request can be sign-extended at the syscall boundary. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>
static int checks,fails;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"ioctlwidthtest: FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);fails++;}}while(0)
static long call(int fd,uint64_t req,void *arg){return syscall(SYS_ioctl,fd,req,arg);}
int main(void){alarm(20);int m=open("/dev/ptmx",O_RDWR|O_NOCTTY);CHECK(m>=0);if(m<0)return 1;
 int n=-1,other=-1;CHECK(ioctl(m,TIOCGPTN,&n)==0);CHECK(n>=0);uint64_t requests[]={ (uint32_t)TIOCGPTN,(uint64_t)(int64_t)(int32_t)TIOCGPTN,0xdeadbeef00000000ULL|(uint32_t)TIOCGPTN };
 for(unsigned i=0;i<sizeof requests/sizeof requests[0];i++){other=-1;CHECK(call(m,requests[i],&other)==0);CHECK(other==n);errno=0;CHECK(call(m,requests[i],NULL)==-1);CHECK(errno==EFAULT);}
 errno=0;CHECK(call(-1,requests[1],&other)==-1);CHECK(errno==EBADF);
 int zero=0;CHECK(ioctl(m,TIOCSPTLCK,&zero)==0);char path[64];snprintf(path,sizeof path,"/dev/pts/%d",n);int s=open(path,O_RDWR|O_NOCTTY);CHECK(s>=0);
 if(s>=0){CHECK(isatty(s)==1);struct termios t;CHECK(tcgetattr(s,&t)==0);cfmakeraw(&t);CHECK(tcsetattr(s,TCSANOW,&t)==0);CHECK(write(s,"pty-ok",6)==6);char b[8]={0};CHECK(read(m,b,sizeof b)==6);CHECK(memcmp(b,"pty-ok",6)==0);struct winsize a={24,80,0,0},bws;CHECK(ioctl(s,TIOCSWINSZ,&a)==0);CHECK(call(m,0xdeadbeef00000000ULL|TIOCGWINSZ,&bws)==0);CHECK(bws.ws_row==24&&bws.ws_col==80);CHECK(close(s)==0);}
 CHECK(close(m)==0);printf("ioctlwidthtest: %d checks, %d failures\n",checks,fails);return fails?1:0;}
