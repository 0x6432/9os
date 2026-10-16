/* Linux directory cookies are opaque NEXT-entry positions, not output-buffer
 * offsets. Run this exact consumer on Linux and every 9os architecture.
 * Default: private temporary directory; optional args add stable proc/sys dirs. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
struct linux_dirent64 { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; };
struct saved { int64_t cookie; char name[256]; };
static int checks, failures;
#define CHECK(x) do { checks++; if(!(x)){failures++;printf("FAIL line=%d: %s errno=%d\n",__LINE__,#x,errno);} } while(0)
static int scan(int fd,struct saved *out,int max) {
    /* Tiny buffers force multiple kernel calls. Cookie values must not restart
     * from zero in each output buffer. Bound all parsing and traversal. */
    char buf[128] __attribute__((aligned(8)));int count=0;
    for(int batch=0;batch<256;batch++) {
        long n=syscall(SYS_getdents64,fd,buf,sizeof buf);
        if(n<0)return -1;
        if(!n)return count;
        for(size_t pos=0;pos<(size_t)n;) {
            struct linux_dirent64 *d=(void*)(buf+pos);
            if((size_t)n-pos<20 || d->reclen<20 || d->reclen>(size_t)n-pos ||
               !memchr(d->name,0,d->reclen-19) || count>=max || strlen(d->name)>=256)return -1;
            out[count].cookie=d->off;strcpy(out[count++].name,d->name);pos+=d->reclen;
        }
    }
    return -1;
}
static void test_dir(const char *path) {
    printf("DIRECTORY_COOKIES path=%s\n",path);
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(fd>=0);if(fd<0)return;
    struct saved rows[128],next[128];int n=scan(fd,rows,128);CHECK(n>=2);
    if(n>=2)for(int i=0;i<n;i++) {
        CHECK(lseek(fd,rows[i].cookie,SEEK_SET)==rows[i].cookie);
        int m=scan(fd,next,128);
        CHECK(m==n-i-1);
        if(m==n-i-1) {
            int same=1;
            for(int j=0;j<m;j++)if(strcmp(next[j].name,rows[i+j+1].name)){same=0;break;}
            CHECK(same);
        }
    }
    /* Exercise libc's tell/seek contract independently of raw syscall parsing. */
    CHECK(lseek(fd,0,SEEK_SET)==0);
    DIR *dir=fdopendir(fd);CHECK(dir!=NULL);
    if(dir) {
        struct dirent *a=readdir(dir);CHECK(a!=NULL);
        if(a) {
            long mark=telldir(dir);struct dirent *b=readdir(dir);CHECK(b!=NULL);
            if(b) {
                char expected[256];snprintf(expected,sizeof expected,"%s",b->d_name);
                seekdir(dir,mark);b=readdir(dir);CHECK(b && !strcmp(b->d_name,expected));
            }
        }
        CHECK(closedir(dir)==0);
    } else close(fd);
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);alarm(45);
    char path[256];const char *base=getenv("TMPDIR");if(!base)base="/tmp";
    int len=snprintf(path,sizeof path,"%s/dircookie-XXXXXX",base);
    CHECK(len>0 && (size_t)len<sizeof path);if(failures)return 1;
    CHECK(mkdtemp(path)!=NULL);if(failures)return 1;
    char file[320];
    for(int i=0;i<40;i++) {
        snprintf(file,sizeof file,"%s/entry-with-name-%03d",path,i);
        int fd=open(file,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);if(fd>=0)CHECK(close(fd)==0);
    }
    test_dir(path);
    /* Deleted entries leave gaps in tmpfs cookies and ext2 directory slots. */
    for(int i=0;i<40;i+=3){snprintf(file,sizeof file,"%s/entry-with-name-%03d",path,i);CHECK(unlink(file)==0);}
    test_dir(path);
    for(int i=1;i<argc;i++)test_dir(argv[i]);
    for(int i=0;i<40;i++)if(i%3){snprintf(file,sizeof file,"%s/entry-with-name-%03d",path,i);CHECK(unlink(file)==0);}
    CHECK(rmdir(path)==0);
    printf("dircookietest: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
