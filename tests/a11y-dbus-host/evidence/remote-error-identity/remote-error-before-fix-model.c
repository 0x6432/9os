/* Tiny native injection model, NOT IPC/GUI evidence. Real opaque GError APIs. */
#include <gio/gio.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
/* Check the ABI number copied into the header-free consumer against the
 * authoritative installed public header, not a guessed DBus enum. */
_Static_assert(G_IO_ERROR_TIMED_OUT == 24, "GIO timeout ABI changed");
static const char *model_mode,*hide_symbol;
static void *model_dlsym(void *,const char *);
#define dlsym model_dlsym
#define main consumer_main
#include "../../userland/tests/a11y-dbus/terminal-dbus.c"
#undef main
#undef dlsym
static Obj *model_bus_get(int type,Obj *cancel,Error **e){
 (void)type;(void)cancel;(void)e;return (Obj *)1;
}
static void model_unref(Obj *o){(void)o;}
static Obj *model_call(Obj *connection,const char *name,const char *path,const char *iface,const char *method,Obj *args,const Obj *reply_type,int flags,int timeout,Obj *cancel,Error **e){
 (void)connection;(void)name;(void)path;(void)iface;(void)method;(void)args;
 (void)reply_type;(void)flags;(void)cancel;
 if(timeout!=1500)abort();
 GError *error;
 if(!strcmp(model_mode,"local-timeout"))error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_TIMED_OUT,"UNPRINTED_SECRET FAIL panic __RC0=9");
 else if(!strcmp(model_mode,"other"))error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"UNPRINTED_SECRET FAIL panic");
 else error=g_dbus_error_new_for_dbus_error(!strcmp(model_mode,"remote-noreply")?"org.freedesktop.DBus.Error.NoReply":"org.test.FAIL", "UNPRINTED_SECRET FAIL panic __RC0=9");
 *e=error;return NULL;
}
static char *model_long_name(const Error *e){
 (void)e;char *s=g_malloc(601);memset(s,'A',600);memcpy(s,"org.test.FAIL",13);s[600]=0;return s;
}
static void *model_dlsym(void *handle,const char *name){
 if(hide_symbol&&strcmp(hide_symbol,name)==0)return NULL;
 if(!strcmp(name,"g_bus_get_sync"))return (void *)model_bus_get;
 if(!strcmp(name,"g_dbus_connection_call_sync"))return (void *)model_call;
 if(!strcmp(name,"g_object_unref"))return (void *)model_unref;
 if(!strcmp(model_mode,"long")&&!strcmp(name,"g_dbus_error_get_remote_error"))return (void *)model_long_name;
 return dlsym(handle,name);
}
int main(int argc,char **argv){
 if(argc<2||argc>3)return 2;
 model_mode=argv[1];hide_symbol=argc==3?argv[2]:NULL;
 char *consumer_argv[]={"model-consumer","1",NULL};
 int status=consumer_main(2,consumer_argv);
 if(!strcmp(model_mode,"long")){
  struct Ref r;memset(r.name,'B',sizeof(r.name)-1);r.name[sizeof(r.name)-1]=0;
  memset(r.path,'C',sizeof(r.path)-1);r.path[sizeof(r.path)-1]=0;
  failed=0;
  query((Obj *)1,&r,"org.test.Interface","MethodFAILabcdefghijklmnopqrstuvwxyz0123456789",NULL,"(u)");
 }
 return status;
}
