/* Same-source Linux oracle/9os contract: a successful allocation must be real.
 * Filesystems may explicitly reject unsupported preallocation; false success
 * leaves a short file and makes mmap consumers such as Mesa fault beyond EOF. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
static int checks, fails;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"fallocatetest: FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);fails++; } } while(0)
static void bad(int fd,int mode,off_t off,off_t len,int err){errno=0;CHECK(fallocate(fd,mode,off,len)==-1);CHECK(errno==err);}
int main(void){
 char path[]="/tmp/fallocate-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);if(fd<0)return 1;
 CHECK(write(fd,"sentinel",8)==8);CHECK(lseek(fd,3,SEEK_SET)==3);
 bad(-1,0,0,4096,EBADF);bad(fd,0,-1,4096,EINVAL);bad(fd,0,0,0,EINVAL);bad(fd,0,0,-1,EINVAL);
 int ro=open(path,O_RDONLY);CHECK(ro>=0);bad(ro,0,0,4096,EBADF);close(ro);
 int op=open(path,O_PATH);CHECK(op>=0);bad(op,0,0,4096,EBADF);close(op);
 int pipefd[2];CHECK(pipe(pipefd)==0);bad(pipefd[1],0,0,4096,ESPIPE);close(pipefd[0]);close(pipefd[1]);
 int r=fallocate(fd,0,0,131072),e=errno;struct stat st;CHECK(fstat(fd,&st)==0);
 if(r==0){
  CHECK(st.st_size==131072);CHECK(lseek(fd,0,SEEK_CUR)==3);
  unsigned char *p=mmap(0,131072,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(p!=MAP_FAILED);
  if(p!=MAP_FAILED){CHECK(memcmp(p,"sentinel",8)==0);int zero=1;for(int i=8;i<131072;i++)if(p[i])zero=0;CHECK(zero);p[131071]=0xa5;CHECK(msync(p,131072,MS_SYNC)==0);CHECK(munmap(p,131072)==0);unsigned char v=0;CHECK(pread(fd,&v,1,131071)==1&&v==0xa5);}
  puts("fallocatetest: filesystem reserves and extends the requested range");
 }else{
  CHECK(e==EOPNOTSUPP||e==ENOSYS);CHECK(st.st_size==8);char b[8];CHECK(pread(fd,b,8,0)==8&&memcmp(b,"sentinel",8)==0);CHECK(lseek(fd,0,SEEK_CUR)==3);
  puts("fallocatetest: explicit unsupported result, file unchanged (no false success)");
 }
 CHECK(close(fd)==0);CHECK(unlink(path)==0);printf("fallocatetest: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
