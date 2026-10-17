/* Exact Linux/9os evdev frame and key-state regression. No toolkit or timing
 * thresholds. Requires writable /dev/uinput; never sends console keys. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
static int checks, failures;
#define CHECK(x, msg) do { checks++; if (!(x)) { failures++; printf("evdevframetest: FAIL %s (line %d errno %d)\n",msg,__LINE__,errno); } } while (0)
static int device(int u, char *node, size_t cap) {
    char expected[80]; snprintf(expected,sizeof expected,"9os evdev frames %ld",(long)getpid());
    struct uinput_setup s={.id={BUS_VIRTUAL,0x1234,1,1}};
    snprintf(s.name,sizeof s.name,"%s",expected);
    CHECK(ioctl(u,UI_SET_EVBIT,EV_REL)==0,"REL capability");
    CHECK(ioctl(u,UI_SET_RELBIT,REL_X)==0,"X capability");
    CHECK(ioctl(u,UI_SET_RELBIT,REL_Y)==0,"Y capability");
    CHECK(ioctl(u,UI_SET_EVBIT,EV_KEY)==0,"KEY capability");
    CHECK(ioctl(u,UI_SET_KEYBIT,KEY_F13)==0,"F13 capability");
    CHECK(ioctl(u,UI_DEV_SETUP,&s)==0,"device setup");
    if(ioctl(u,UI_DEV_CREATE))return -1;
    for(int tries=0;tries<100;tries++) {
        for(int i=0;i<64;i++) {
            snprintf(node,cap,"/dev/input/event%d",i);
            int e=open(node,O_RDONLY|O_NONBLOCK);if(e<0)continue;
            char name[128]={0};
            if(ioctl(e,EVIOCGNAME(sizeof name),name)>=0&&!strcmp(name,expected))return e;
            close(e);
        }
        usleep(10000);
    }
    return -1;
}
static void event(int u,int type,int code,int value) {
    struct input_event e={.type=type,.code=code,.value=value};
    CHECK(write(u,&e,sizeof e)==sizeof e,"inject event");
}
static void sync_(int u){event(u,EV_SYN,SYN_REPORT,0);}
static int readable(int e,int ms){struct pollfd p={e,POLLIN,0};int r=poll(&p,1,ms);return r>0&&!!(p.revents&POLLIN);}
static void empty(int e) {
    struct input_event b[16];CHECK(!readable(e,0),"incomplete/empty not poll readable");
    errno=0;CHECK(read(e,b,sizeof b)==-1&&errno==EAGAIN,"incomplete/empty nonblocking EAGAIN");
}
static void frame(int e,int type,int code,int value) {
    struct input_event b[16];memset(b,0,sizeof b);
    CHECK(readable(e,1000),"completed frame poll readable");
    ssize_t n=read(e,b,sizeof b);
    CHECK(n==2*(ssize_t)sizeof b[0],"only completed frame returned");
    CHECK(b[0].type==type&&b[0].code==code&&b[0].value==value&&b[1].type==EV_SYN&&b[1].code==SYN_REPORT,"frame payload and SYN_REPORT");
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);alarm(30);
    int u=open("/dev/uinput",O_WRONLY|O_NONBLOCK);CHECK(u>=0,"open uinput");if(u<0)return 1;
    char node[80];int a=device(u,node,sizeof node);CHECK(a>=0,"create/find unique non-console input");if(a<0){close(u);return 1;}
    int b=open(node,O_RDONLY|O_NONBLOCK);CHECK(b>=0,"independent second client");if(b<0)return 1;
    empty(a);empty(b);
    event(u,EV_REL,REL_X,11);empty(a);empty(b);
    sync_(u);CHECK(readable(a,1000),"first frame readable");
    struct input_event one={0};CHECK(read(a,&one,sizeof one)==sizeof one&&one.type==EV_REL&&one.value==11,"partial frame read");
    CHECK(readable(a,0),"SYN remainder stays readable");
    CHECK(read(a,&one,sizeof one)==sizeof one&&one.type==EV_SYN,"partial SYN remainder");
    empty(a);frame(b,EV_REL,REL_X,11);empty(b);
    event(u,EV_REL,REL_X,22);sync_(u);event(u,EV_REL,REL_Y,33);
    frame(a,EV_REL,REL_X,22);frame(b,EV_REL,REL_X,22);empty(a);empty(b);
    sync_(u);frame(a,EV_REL,REL_Y,33);frame(b,EV_REL,REL_Y,33);empty(a);empty(b);
    event(u,EV_KEY,KEY_F13,1);sync_(u);frame(a,EV_KEY,KEY_F13,1);frame(b,EV_KEY,KEY_F13,1);
    event(u,EV_KEY,KEY_F13,1);sync_(u);empty(a);empty(b); /* duplicate down is NOT repeat */
    event(u,EV_KEY,KEY_F13,2);sync_(u);frame(a,EV_KEY,KEY_F13,2);frame(b,EV_KEY,KEY_F13,2);
    event(u,EV_KEY,KEY_F13,0);sync_(u);frame(a,EV_KEY,KEY_F13,0);frame(b,EV_KEY,KEY_F13,0);
    event(u,EV_KEY,KEY_F13,0);sync_(u);empty(a);empty(b);
    event(u,EV_KEY,KEY_F14,1);sync_(u);empty(a);empty(b); /* unsupported key */
    /* A blocking reader must not consume an incomplete packet even when another
     * notification wakes the shared poll queue. Child announces enrollment first. */
    int p[2];CHECK(pipe(p)==0,"blocking report pipe");pid_t child=fork();CHECK(child>=0,"blocking fork");
    if(child==0){close(p[0]);int e=open(node,O_RDONLY);char ready='R';write(p[1],&ready,1);struct input_event ev[4];ssize_t n=read(e,ev,sizeof ev);int ok=n==2*(ssize_t)sizeof ev[0]&&ev[0].type==EV_REL&&ev[0].value==44&&ev[1].type==EV_SYN;write(p[1],&ok,sizeof ok);close(e);_exit(ok?0:1);}
    if(child>0){close(p[1]);char ready=0;CHECK(read(p[0],&ready,1)==1&&ready=='R',"blocking child enrolled");
        event(u,EV_REL,REL_X,44);CHECK(!readable(p[0],50),"blocking read waits for SYN");sync_(u);
        CHECK(readable(p[0],2000),"blocking read wakes on completed frame");int ok=0;CHECK(read(p[0],&ok,sizeof ok)==sizeof ok&&ok,"blocking complete payload");int st=0;CHECK(waitpid(child,&st,0)==child&&WIFEXITED(st)&&WEXITSTATUS(st)==0,"blocking child exits cleanly");close(p[0]);}
    CHECK(ioctl(u,UI_DEV_DESTROY)==0,"destroy input");close(a);close(b);close(u);
    printf("evdevframetest: %s (%d checks, %d failures)\n",failures?"FAILED":"PASSED",checks,failures);return failures?1:0;
}
