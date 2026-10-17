/* Explicit synthetic AT-SPI protocol server for PRIVATE native IPC controls.
 * Never graphical evidence; no real application is impersonated in guest tests. */
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *mode,*unique;
static const char *root="/org/a11y/atspi/accessible/root",*app="/org/a11y/atspi/accessible/app",*frame="/org/a11y/atspi/accessible/frame",*terminal="/org/a11y/atspi/accessible/terminal",*menu="/org/a11y/atspi/accessible/menu";
static int is(const char*s){return !strcmp(mode,s);}
static void method(GDBusConnection *conn,const gchar *sender,const gchar *path,const gchar *iface,const gchar *name,GVariant *parameters,GDBusMethodInvocation *inv,gpointer data){
 (void)conn;(void)sender;(void)iface;(void)parameters;(void)data;
 if(!strcmp(name,"GetAddress")){g_dbus_method_invocation_return_value(inv,g_variant_new("(s)",is("bad-address")?"invalid":getenv("DBUS_SESSION_BUS_ADDRESS")));return;}
 if(is("remote-error")&&!strcmp(name,"GetRole")){g_dbus_method_invocation_return_dbus_error(inv,"org.test.QueryError","explicit negative");return;}
 if(!strcmp(name,"GetRole")){
  if(is("blocked-role"))g_usleep(2500000);
  unsigned role=!strcmp(path,app)?75:!strcmp(path,frame)?23:!strcmp(path,terminal)?(is("wrong-role")?61:60):!strcmp(path,menu)?34:0;
  g_dbus_method_invocation_return_value(inv,g_variant_new("(u)",role));return;
 }
 if(!strcmp(name,"GetState")){
  GVariantBuilder b;g_variant_builder_init(&b,G_VARIANT_TYPE("au"));
  guint32 bits=(1u<<12)|(1u<<25)|(1u<<30);
  if(is("unfocused"))bits&=~(1u<<12);if(is("hidden"))bits&=~(1u<<25);if(is("invisible"))bits&=~(1u<<30);
  g_variant_builder_add(&b,"u",bits);if(!is("short-state"))g_variant_builder_add(&b,"u",0u);
  if(is("long-state"))g_variant_builder_add(&b,"u",0u);
  g_dbus_method_invocation_return_value(inv,g_variant_new("(au)",&b));return;
 }
 if(!strcmp(name,"GetChildren")){
  GVariantBuilder b;g_variant_builder_init(&b,G_VARIANT_TYPE("a(so)"));
  if(!strcmp(path,root)){
   unsigned n=is("many-apps")?129:is("duplicate-app")?2:is("empty")?0:1;
   for(unsigned i=0;i<n;i++)g_variant_builder_add(&b,"(so)",unique,app);
  }else if(!strcmp(path,app)){
   unsigned n=is("many-children")?513:1;
   for(unsigned i=0;i<n;i++)g_variant_builder_add(&b,"(so)",unique,is("bad-path")?"/wrong":frame);
  }else if(!strcmp(path,frame)){
   g_variant_builder_add(&b,"(so)",is("foreign-child")?":1.999999":unique,is("cycle")?frame:is("menu-decoy")?menu:terminal);
  }else if(!strcmp(path,menu))g_variant_builder_add(&b,"(so)",unique,terminal);
  g_dbus_method_invocation_return_value(inv,g_variant_new("(a(so))",&b));return;
 }
 g_dbus_method_invocation_return_dbus_error(inv,"org.test.Unknown","unexpected query");
}
static const GDBusInterfaceVTable table={.method_call=method};
static const char xml[]="<node><interface name='org.a11y.Bus'><method name='GetAddress'><arg type='s' direction='out'/></method></interface><interface name='org.a11y.atspi.Accessible'><method name='GetChildren'><arg type='a(so)' direction='out'/></method><method name='GetRole'><arg type='u' direction='out'/></method><method name='GetState'><arg type='au' direction='out'/></method></interface></node>";
int main(int argc,char **argv){
 if(argc!=2)return 2;mode=argv[1];GError *e=NULL;GDBusConnection *c=g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,&e);if(!c){fprintf(stderr,"connect: %s\n",e?e->message:"none");return 2;}
 unique=g_dbus_connection_get_unique_name(c);
 for(int i=0;i<2;i++){
  GVariant *v=g_dbus_connection_call_sync(c,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","RequestName",g_variant_new("(su)",i?"org.a11y.atspi.Registry":"org.a11y.Bus",4u),G_VARIANT_TYPE("(u)"),0,1000,NULL,&e);
  guint32 result=0;if(!v)return 2;g_variant_get(v,"(u)",&result);g_variant_unref(v);if(result!=1)return 2;
 }
 GDBusNodeInfo *info=g_dbus_node_info_new_for_xml(xml,&e);if(!info)return 2;
 if(!g_dbus_connection_register_object(c,"/org/a11y/bus",info->interfaces[0],&table,NULL,NULL,&e))return 2;
 const char *paths[]={root,app,frame,terminal,menu};
 for(unsigned i=0;i<G_N_ELEMENTS(paths);i++)if(!g_dbus_connection_register_object(c,paths[i],info->interfaces[1],&table,NULL,NULL,&e))return 2;
 printf("MOCK_SERVER_READY pid=%ld\n",(long)getpid());fflush(stdout);
 GMainLoop *loop=g_main_loop_new(NULL,FALSE);g_main_loop_run(loop);return 0;
}
