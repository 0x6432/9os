/* Independent FAILURE-ONLY AT-SPI query diagnostic. No acceptance substitution.
 * Public opaque GLib/GIO C ABI: no private object layouts, no Qt build/patch.
 * Protocol: at-spi2-core 2.56.2 Accessible GetChildren/GetRole/GetState;
 * obtain the bus through org.a11y.Bus.GetAddress and authenticate application
 * references using org.freedesktop.DBus.GetConnectionUnixProcessID on that bus.
 * Bulk GetChildren avoids ChildCount plus a separate call for every child.
 * Every signature, bound and remote error fails closed. No cache GetItems.
 * GetRole/GetChildren are independent read-only calls: submit both before
 * waiting, then drain BOTH callbacks before releasing their stack storage.
 * This reduces serialized round trips, not any acceptance requirement/bound.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
typedef void Obj;
typedef void Error;
static Obj *(*bus_get)(int,Obj *,Error **);
static Obj *(*connect_address)(const char *,int,Obj *,Obj *,Error **);
static Obj *(*call_sync)(Obj *,const char *,const char *,const char *,const char *,Obj *,const Obj *,int,int,Obj *,Error **);
static void (*call_async)(Obj *,const char *,const char *,const char *,const char *,Obj *,const Obj *,int,int,Obj *,void (*)(Obj *,Obj *,void *),void *);
static Obj *(*call_finish)(Obj *,Obj *,Error **);
static int (*context_iteration)(Obj *,int);
static Obj *(*variant_child)(Obj *,size_t);
static size_t (*variant_count)(Obj *);
static const char *(*variant_type)(Obj *);
static const char *(*variant_string)(Obj *,size_t *);
static uint32_t (*variant_uint)(Obj *);
static Obj *(*variant_new)(const char *,...);
static void (*variant_unref)(Obj *),(*object_unref)(Obj *),(*error_free)(Error *);
static Obj *bus;
static unsigned target,nodes,models;
static int failed;
static const char *accessible="org.a11y.atspi.Accessible";
struct Ref {char name[256],path[1024];};
static unsigned long long mono(void){struct timespec t={0};if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (unsigned long long)t.tv_sec*1000000000ull+(unsigned long long)t.tv_nsec;}
static void err(const char *reason){failed=1;printf("DBUS_A11Y_ERROR reason=%s\n",reason);}
static Obj *query(Obj *connection,const struct Ref *r,const char *iface,const char *method,Obj *args,const char *sig){
 if(failed)return NULL;
 Error *e=NULL;
 printf("DBUS_A11Y_QUERY method=%s phase=begin bus=%s path=%s mono_ns=%llu\n",method,r->name,r->path,mono());
 Obj *v=call_sync(connection,r->name,r->path,iface,method,args,NULL,0,1500,NULL,&e);
 printf("DBUS_A11Y_QUERY method=%s phase=end ok=%d mono_ns=%llu\n",method,v!=NULL&&e==NULL,mono());
 if(e){error_free(e);err("remote-call");}
 if(!v){if(!failed)err("missing-reply");return NULL;}
 if(failed||strcmp(variant_type(v),sig)){variant_unref(v);err("reply-signature");return NULL;}
 return v;
}
static int reference(Obj *v,struct Ref *r){
 if(strcmp(variant_type(v),"(so)")||variant_count(v)!=2){err("reference-signature");return 0;}
 Obj *n=variant_child(v,0),*p=variant_child(v,1);size_t nl=0,pl=0;
 const char *ns=variant_string(n,&nl),*ps=variant_string(p,&pl);
 if(!ns||!ps||nl<2||nl>=sizeof(r->name)||pl<1||pl>=sizeof(r->path)||ns[0]!=':'||strncmp(ps,"/org/a11y/atspi/accessible/",27))err("reference-value");
 else {memcpy(r->name,ns,nl+1);memcpy(r->path,ps,pl+1);}
 variant_unref(n);variant_unref(p);return !failed;
}
static int owner(const struct Ref *r){
 const struct Ref dbus={"org.freedesktop.DBus","/org/freedesktop/DBus"};
 Obj *v=query(bus,&dbus,"org.freedesktop.DBus","GetConnectionUnixProcessID",variant_new("(s)",r->name),"(u)");
 if(!v)return 0;
 Obj *c=variant_child(v,0);unsigned pid=variant_uint(c);variant_unref(c);variant_unref(v);
 printf("DBUS_A11Y_OWNER bus=%s pid=%u target=%u\n",r->name,pid,target);return pid==target;
}
static Obj *children(const struct Ref *r){
 Obj *v=query(bus,r,accessible,"GetChildren",NULL,"(a(so))");
 if(!v)return NULL;
 Obj *a=variant_child(v,0);variant_unref(v);
 if(variant_count(a)>512){variant_unref(a);err("children-bound");return NULL;}
 return a;
}
struct Pending {const char *method,*signature;Obj *reply;int done;};
static void node_reply(Obj *source,Obj *result,void *data){
 struct Pending *p=data;Error *e=NULL;
 p->reply=call_finish(source,result,&e);
 printf("DBUS_A11Y_QUERY method=%s phase=end ok=%d mono_ns=%llu\n",p->method,p->reply!=NULL&&e==NULL,mono());
 if(e){error_free(e);err("remote-call");}
 if(!p->reply){if(!failed)err("missing-reply");}
 else if(strcmp(variant_type(p->reply),p->signature))err("reply-signature");
 p->done=1;
}
static int node(const struct Ref *r,Obj **role,Obj **child_array){
 struct Pending p[2]={{"GetRole","(u)",NULL,0},{"GetChildren","(a(so))",NULL,0}};
 for(unsigned i=0;i<2;i++){
  printf("DBUS_A11Y_QUERY method=%s phase=begin bus=%s path=%s mono_ns=%llu\n",p[i].method,r->name,r->path,mono());
  call_async(bus,r->name,r->path,accessible,p[i].method,NULL,NULL,0,1500,NULL,node_reply,&p[i]);
 }
 /* Even on error do not return while another callback owns &p[i]. Both
  * public GIO operations keep their original1500ms bound; alarm8 is unchanged. */
 while(!p[0].done||!p[1].done)context_iteration(NULL,1);
 if(failed){
  for(unsigned i=0;i<2;i++)if(p[i].reply)variant_unref(p[i].reply);
  return 0;
 }
 *role=p[0].reply;*child_array=variant_child(p[1].reply,0);variant_unref(p[1].reply);
 if(variant_count(*child_array)>512){
  variant_unref(*role);variant_unref(*child_array);err("children-bound");return 0;
 }
 return 1;
}
static void walk(const struct Ref *r,unsigned depth){
 if(failed||models)return;
 if(depth>24||++nodes>256){err("tree-bound");return;}
 Obj *v,*a;if(!node(r,&v,&a))return;
 Obj *c=variant_child(v,0);unsigned role=variant_uint(c);variant_unref(c);variant_unref(v);
 printf("DBUS_A11Y_NODE depth=%u node=%u role=%u\n",depth,nodes,role);
 if(role==33||role==34||role==35||role==41||role==63){variant_unref(a);return;}
 if(role==60){
  variant_unref(a);
  v=query(bus,r,accessible,"GetState",NULL,"(au)");if(!v)return;
  Obj *a=variant_child(v,0);variant_unref(v);
  if(variant_count(a)!=2){variant_unref(a);err("state-width");return;}
  c=variant_child(a,0);uint32_t bits=variant_uint(c);variant_unref(c);variant_unref(a);
  int focused=(bits>>12)&1,showing=(bits>>25)&1,visible=(bits>>30)&1;
  printf("DBUS_A11Y_TERMINAL focused=%d showing=%d visible=%d\n",focused,showing,visible);
  if(focused&&showing&&visible)models++;
  return;
 }
 for(size_t i=0;i<variant_count(a)&&!failed&&!models;i++){
  c=variant_child(a,i);struct Ref child;int valid=reference(c,&child);variant_unref(c);
  if(!valid)break;
  /* Cross-application references cannot satisfy this PID's terminal query. */
  if(strcmp(r->name,child.name)&&!owner(&child)){if(!failed)err("foreign-child-owner");break;}
  walk(&child,depth+1);
 }
 variant_unref(a);
}
#define LOAD(dst,sym) do{*(void **)(&dst)=dlsym(h,sym);if(!dst){fprintf(stderr,"missing public symbol: %s\n",sym);return 2;}}while(0)
int main(int argc,char **argv){
 if(argc!=2){fputs("usage: terminal-dbus PID (failure-only diagnostic)\n",stderr);return 2;}
 char *end;errno=0;unsigned long p=strtoul(argv[1],&end,10);
 if(errno||end==argv[1]||*end||p==0||p>2147483647ul)return 2;
 target=(unsigned)p;setvbuf(stdout,NULL,_IOLBF,0);alarm(8);
 void *h=dlopen("libgio-2.0.so.0",RTLD_NOW);if(!h){fprintf(stderr,"%s\n",dlerror());return 2;}
 LOAD(bus_get,"g_bus_get_sync");LOAD(connect_address,"g_dbus_connection_new_for_address_sync");LOAD(call_sync,"g_dbus_connection_call_sync");
 LOAD(call_async,"g_dbus_connection_call");LOAD(call_finish,"g_dbus_connection_call_finish");LOAD(context_iteration,"g_main_context_iteration");
 LOAD(variant_child,"g_variant_get_child_value");LOAD(variant_count,"g_variant_n_children");LOAD(variant_type,"g_variant_get_type_string");LOAD(variant_string,"g_variant_get_string");LOAD(variant_uint,"g_variant_get_uint32");LOAD(variant_new,"g_variant_new");LOAD(variant_unref,"g_variant_unref");LOAD(object_unref,"g_object_unref");LOAD(error_free,"g_error_free");
 Error *e=NULL;Obj *session=bus_get(2,NULL,&e);
 if(e){error_free(e);err("session-connect");}if(!session)return 1;
 const struct Ref launcher={"org.a11y.Bus","/org/a11y/bus"};
 Obj *v=query(session,&launcher,"org.a11y.Bus","GetAddress",NULL,"(s)");
 if(!v){object_unref(session);return 1;}
 Obj *c=variant_child(v,0);size_t length=0;const char *address=variant_string(c,&length);e=NULL;
 if(length==0||length>4096){err("bus-address-bound");}else bus=connect_address(address,9,NULL,NULL,&e);
 variant_unref(c);variant_unref(v);object_unref(session);
 if(e){error_free(e);err("accessibility-connect");}if(!bus)return 1;
 const struct Ref registry={"org.a11y.atspi.Registry","/org/a11y/atspi/accessible/root"};
 Obj *a=children(&registry);unsigned applications=0;
 if(a){
  size_t count=variant_count(a);if(count>128)err("applications-bound");
  for(size_t i=0;i<count&&!failed;i++){
   c=variant_child(a,i);struct Ref app;int valid=reference(c,&app);variant_unref(c);if(!valid)break;
   if(owner(&app)){applications++;walk(&app,0);}
  }
  variant_unref(a);
 }
 object_unref(bus);
 int ok=!failed&&applications==1&&models==1;
 printf("terminal-dbus: pid=%u applications=%u models=%u visited=%u %s (diagnostic only)\n",target,applications,models,nodes,ok?"OBSERVED":"NOT_OBSERVED");
 return ok?0:1;
}
