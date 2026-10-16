/* Read the genuine application's AT-SPI tree through packaged libatspi.
 * No Qt build, application patch, synthetic model, or folder-only DBus oracle.
 * Usage: desktopa11y PID +required-name -forbidden-name ...
 * These stable public C signatures avoid requiring optional desktop dev APKs. */
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef void Accessible;
typedef void Error;
static Accessible *(*get_desktop)(int);
static int (*get_count)(Accessible *, Error **);
static Accessible *(*get_child)(Accessible *, int, Error **);
static char *(*get_name)(Accessible *, Error **);
static unsigned (*get_pid)(Accessible *, Error **);
static void (*unref)(void *), (*free_text)(void *), (*free_error)(Error *);
static int nodes, failed, argc_;static char **argv_;static unsigned seen[32];
static void check_error(Error *e) {
    if (e) { failed=1;free_error(e); }
}
static void walk(Accessible *a, unsigned depth) {
    if (!a || depth>24 || ++nodes>2048) { failed=1;return; }
    Error *e=NULL;char *n=get_name(a,&e);check_error(e);
    if(n) for(int i=2;i<argc_;i++) if(!strcmp(n,argv_[i]+1)) seen[i]++;
    free_text(n);e=NULL;int c=get_count(a,&e);check_error(e);
    if(c<0 || c>512) { failed=1;return; }
    for(int i=0;i<c;i++) {
        e=NULL;Accessible *b=get_child(a,i,&e);check_error(e);
        walk(b,depth+1);if(b)unref(b);
        if(failed)break;
    }
}
#define LOAD(dst,sym) do { *(void **)(&dst)=dlsym(h,sym); \
    if(!dst){fprintf(stderr,"desktopa11y: missing %s\n",sym);return 2;} } while(0)
int main(int argc,char **argv) {
    if(argc<3 || argc>32) {fputs("usage: desktopa11y PID +required -forbidden ...\n",stderr);return 2;}
    char *end;errno=0;unsigned long target=strtoul(argv[1],&end,10);
    if(errno || *end || !target || target>2147483647) return 2;
    for(int i=2;i<argc;i++)if((argv[i][0]!='+' && argv[i][0]!='-') || !argv[i][1])return 2;
    alarm(45);argc_=argc;argv_=argv;
    void *h=dlopen("libatspi.so.0",RTLD_NOW);
    if(!h){fprintf(stderr,"desktopa11y: %s\n",dlerror());return 2;}
    int (*init)(void),(*finish)(void);
    LOAD(init,"atspi_init");LOAD(finish,"atspi_exit");LOAD(get_desktop,"atspi_get_desktop");
    LOAD(get_count,"atspi_accessible_get_child_count");LOAD(get_child,"atspi_accessible_get_child_at_index");
    LOAD(get_name,"atspi_accessible_get_name");LOAD(get_pid,"atspi_accessible_get_process_id");
    LOAD(unref,"g_object_unref");LOAD(free_text,"g_free");LOAD(free_error,"g_error_free");
    if(init())return 2;
    Accessible *d=get_desktop(0);if(!d){finish();return 1;}
    Error *e=NULL;int c=get_count(d,&e);check_error(e);int found=0;
    if(c<0 || c>128)failed=1;
    for(int i=0;i<c && !failed;i++) {
        e=NULL;Accessible *a=get_child(d,i,&e);check_error(e);
        if(!a){failed=1;break;}
        e=NULL;unsigned pid=get_pid(a,&e);check_error(e);
        if(pid==target){found++;walk(a,0);}
        unref(a);
    }
    unref(d);finish();
    for(int i=2;i<argc;i++) {
        printf("A11Y_NAME %s matches=%u\n",argv[i],seen[i]);
        if(argv[i][0]=='+' ? !seen[i] : seen[i]!=0)failed=1;
    }
    printf("desktopa11y: pid=%lu applications=%d nodes=%d %s\n",target,found,nodes,
           !failed && found==1 ? "PASSED" : "NOT_READY");
    return failed || found!=1;
}
