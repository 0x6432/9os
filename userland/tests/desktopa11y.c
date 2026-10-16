/* Read the genuine Dolphin file model through packaged libatspi. No Qt build,
 * app patch, injected model, or folder-only DBus oracle. Public stable C ABI.
 * Usage: desktopa11y PID /absolute/directory +required-name -forbidden-name ...
 * Terminal mode: desktopa11y --terminal PID observes focused/showing state.
 * It never sets focus, sends keys or treats PTY startup as a mapped window.
 * Dolphin25.04.2 KItemListViewAccessible exports role List, basename as Name,
 * directory URL in Description, and actual model rows as direct children. */
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct _AtspiAccessible Accessible;
typedef struct _GError Error;
/* Stable public AtspiRole values, verified against upstream atspi-constants.h. */
enum { ROLE_LIST=31, ROLE_LIST_ITEM=32, ROLE_MENU=33, ROLE_MENU_BAR=34, ROLE_MENU_ITEM=35, ROLE_POPUP_MENU=41, ROLE_TERMINAL=60, ROLE_TOOL_BAR=63 };
static Accessible *(*get_desktop)(int);
static int (*get_count)(Accessible *, Error **), (*get_role)(Accessible *, Error **);
static Accessible *(*get_child)(Accessible *, int, Error **);
static char *(*get_name)(Accessible *, Error **), *(*get_description)(Accessible *, Error **);
static unsigned (*get_pid)(Accessible *, Error **);
/* Public AtspiStateType ABI: FOCUSED=12, SHOWING=25, VISIBLE=30. */
static void *(*get_states)(Accessible *);
static int (*contains_state)(void *, int);
static void (*unref)(void *), (*free_text)(void *), (*free_error)(Error *);
static int nodes, failed, argc_, models, terminal_mode;static char **argv_;static unsigned seen[32];
static void check_error(Error *e) { if(e){failed=1;free_error(e);} }
static void read_model(Accessible *a) {
    Error *e=NULL;int c=get_count(a,&e);check_error(e);
    if(failed || c<0 || c>512){failed=1;return;}
    printf("A11Y_MODEL directory=%s rows=%d\n",argv_[2],c);models++;
    /* Enumerate EVERY direct model row before asserting forbidden names absent.
     * Do not accept a matching label elsewhere in menus/dialogs as a file. */
    for(int i=0;i<c&&!failed;i++) {
        e=NULL;Accessible *b=get_child(a,i,&e);check_error(e);
        if(!b){failed=1;break;}
        e=NULL;int role=get_role(b,&e);check_error(e);
        e=NULL;char *name=get_name(b,&e);check_error(e);
        if(role!=ROLE_LIST_ITEM || !name)failed=1;
        if(name)for(int j=3;j<argc_;j++)if(!strcmp(name,argv_[j]+1))seen[j]++;
        free_text(name);unref(b);
    }
}
static int directory_matches(const char *desc) {
    size_t n=strlen(desc),m=strlen(argv_[2]);if(n<m)return 0;
    const char *p=desc+n-m;
    return !strcmp(p,argv_[2]) && (p==desc || p[-1]==' ' || (p-desc>=7&&!memcmp(p-7,"file://",7)));
}
static void find_model(Accessible *a,unsigned depth) {
    if(!a || depth>24 || ++nodes>256){failed=1;return;}
    Error *e=NULL;int role=get_role(a,&e);check_error(e);if(failed)return;
    if(role==ROLE_MENU || role==ROLE_MENU_BAR || role==ROLE_MENU_ITEM || role==ROLE_POPUP_MENU || role==ROLE_TOOL_BAR)return;
    if(terminal_mode && role==ROLE_TERMINAL) {
        void *states=get_states(a);
        if(!states){failed=1;return;}
        int focused=contains_state(states,12), showing=contains_state(states,25), visible=contains_state(states,30);
        printf("A11Y_TERMINAL focused=%d showing=%d visible=%d\n",focused,showing,visible);
        if(focused && showing && visible)models++;
        unref(states);return;
    }
    if(!terminal_mode && role==ROLE_LIST) {
        e=NULL;char *name=get_name(a,&e);check_error(e);
        const char *base=strrchr(argv_[2],'/');base=base?base+1:argv_[2];
        if(name&&!strcmp(name,base)) {
            e=NULL;char *desc=get_description(a,&e);check_error(e);
            if(desc&&directory_matches(desc)){free_text(desc);free_text(name);if(!failed)read_model(a);return;}
            free_text(desc);
        }
        free_text(name);
    }
    if(failed)return;
    e=NULL;int c=get_count(a,&e);check_error(e);
    if(c<0 || c>512){failed=1;return;}
    for(int i=0;i<c&&!failed&&!models;i++) {
        e=NULL;Accessible *b=get_child(a,i,&e);check_error(e);
        find_model(b,depth+1);if(b)unref(b);
    }
}
#define LOAD(dst,sym) do { *(void **)(&dst)=dlsym(h,sym); \
    if(!dst){fprintf(stderr,"desktopa11y: missing %s\n",sym);return 2;} } while(0)
int main(int argc,char **argv) {
    terminal_mode=argc==3 && !strcmp(argv[1],"--terminal");
    if(!terminal_mode && (argc<4 || argc>32 || argv[2][0]!='/' || !strchr(argv[2]+1,'/'))){fputs("usage: desktopa11y PID /absolute/directory +required -forbidden ... | --terminal PID\n",stderr);return 2;}
    const char *pid_arg=argv[terminal_mode?2:1];
    char *end;errno=0;unsigned long target=strtoul(pid_arg,&end,10);
    if(errno || end==pid_arg || *end || !target || target>2147483647)return 2;
    if(!terminal_mode)for(int i=3;i<argc;i++)if((argv[i][0]!='+'&&argv[i][0]!='-')||!argv[i][1])return 2;
    setvbuf(stdout,NULL,_IOLBF,0);alarm(45);argc_=argc;argv_=argv;
    void *h=dlopen("libatspi.so.0",RTLD_NOW);if(!h){fprintf(stderr,"desktopa11y: %s\n",dlerror());return 2;}
    int (*init)(void),(*finish)(void);void (*set_timeout)(int,int);
    LOAD(init,"atspi_init");LOAD(finish,"atspi_exit");LOAD(set_timeout,"atspi_set_timeout");LOAD(get_desktop,"atspi_get_desktop");
    LOAD(get_count,"atspi_accessible_get_child_count");LOAD(get_child,"atspi_accessible_get_child_at_index");
    LOAD(get_name,"atspi_accessible_get_name");LOAD(get_description,"atspi_accessible_get_description");LOAD(get_role,"atspi_accessible_get_role");
    LOAD(get_pid,"atspi_accessible_get_process_id");LOAD(unref,"g_object_unref");LOAD(free_text,"g_free");LOAD(free_error,"g_error_free");
    if(terminal_mode){LOAD(get_states,"atspi_accessible_get_state_set");LOAD(contains_state,"atspi_state_set_contains");}
    puts("A11Y_PHASE init");if(init())return 2;set_timeout(1500,3000);
    Accessible *d=get_desktop(0);if(!d){finish();return 1;}
    Error *e=NULL;int c=get_count(d,&e);check_error(e);int found=0;if(c<0||c>128)failed=1;
    printf("A11Y_APPLICATIONS=%d\n",c);
    for(int i=0;i<c&&!failed&&!found;i++) {
        e=NULL;Accessible *a=get_child(d,i,&e);check_error(e);if(!a){failed=1;break;}
        e=NULL;unsigned pid=get_pid(a,&e);check_error(e);printf("A11Y_APP_PID=%u\n",pid);
        if(pid==target){found++;find_model(a,0);}unref(a);
    }
    unref(d);finish();
    if(!terminal_mode)for(int i=3;i<argc;i++){printf("A11Y_NAME %s matches=%u\n",argv[i],seen[i]);if(argv[i][0]=='+'?!seen[i]:seen[i]!=0)failed=1;}
    printf("desktopa11y: pid=%lu application=%d models=%d visited=%d %s\n",target,found,models,nodes,
           !failed&&found==1&&models==1?"PASSED":"NOT_READY");
    return failed||found!=1||models!=1;
}
