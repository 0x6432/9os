/* Same-source Linux/9os filesystem identity and boot command line contract. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/vfs.h>
#include <unistd.h>
static int checks,fails;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"procinfotest: FAIL line %d: %s\n",__LINE__,#x);fails++;}}while(0)
static void fs(const char *path,long magic){struct statfs s;CHECK(statfs(path,&s)==0);CHECK((unsigned long)s.f_type==(unsigned long)magic);int f=open(path,O_RDONLY|O_DIRECTORY);CHECK(f>=0);CHECK(fstatfs(f,&s)==0);CHECK((unsigned long)s.f_type==(unsigned long)magic);CHECK(close(f)==0);}
int main(void){fs("/proc",0x9fa0);fs("/sys",0x62656572);fs("/sys/class",0x62656572);char a[16384],b[16384];int f=open("/proc/cmdline",O_RDONLY);CHECK(f>=0);ssize_t n=read(f,a,sizeof a);CHECK(n>0&&n<(ssize_t)sizeof a);if(n>0){CHECK(a[n-1]=='\n');CHECK(memchr(a,0,n)==0);CHECK(read(f,b,sizeof b)==0);CHECK(lseek(f,0,SEEK_SET)==0);CHECK(read(f,b,sizeof b)==n);CHECK(memcmp(a,b,n)==0);}CHECK(close(f)==0);printf("procinfotest: %d checks, %d failures\n",checks,fails);return fails?1:0;}
