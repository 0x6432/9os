/* Real bus auth/routing/ownership/credentials/method calls and descriptor transfer. */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"dbustest: FAIL %d: %s (errno %d)\n",__LINE__,#c,errno); fails++; } } while (0)
static DBusConnection *connect_bus(void) {
    DBusError e; dbus_error_init(&e);
    DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SESSION, &e);
    if (!c) fprintf(stderr,"dbustest: connect: %s\n",e.message ? e.message : "unknown");
    dbus_error_free(&e);
    if (c) dbus_connection_set_exit_on_disconnect(c,FALSE);
    return c;
}
static DBusMessage *call(DBusConnection *c, DBusMessage *q) {
    DBusError e; dbus_error_init(&e);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c,q,5000,&e);
    if (!r) fprintf(stderr,"dbustest: call: %s\n",e.message ? e.message : "unknown");
    dbus_error_free(&e); dbus_message_unref(q); return r;
}
static DBusMessage *request(const char *dest,const char *path,const char *iface,const char *method) {
    return dbus_message_new_method_call(dest,path,iface,method);
}
static void close_bus(DBusConnection *c) { dbus_connection_close(c); dbus_connection_unref(c); }
static int server(int ready) {
    DBusConnection *c = connect_bus(); if (!c) return 10;
    DBusError e; dbus_error_init(&e);
    int owner = dbus_bus_request_name(c,"org.nineos.Test",DBUS_NAME_FLAG_DO_NOT_QUEUE,&e);
    if (owner != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) return 11;
    if (write(ready,"R",1)!=1) return 12; close(ready);
    int quit=0, requests=0, errors=0;
    for (int tries=0; tries<100 && !quit; tries++) {
        if (!dbus_connection_read_write(c,500)) break;
        DBusMessage *q;
        while ((q=dbus_connection_pop_message(c))) {
            DBusMessage *r=NULL;
            if (dbus_message_is_method_call(q,"org.nineos.Test","Echo")) {
                const char *s=NULL;
                if (dbus_message_get_args(q,&e,DBUS_TYPE_STRING,&s,DBUS_TYPE_INVALID)) {
                    r=dbus_message_new_method_return(q); dbus_message_append_args(r,DBUS_TYPE_STRING,&s,DBUS_TYPE_INVALID); requests++;
                } else errors++;
            } else if (dbus_message_is_method_call(q,"org.nineos.Test","TakeFD")) {
                int fd=-1; char b[32]={0}; const char *s=b;
                if (dbus_message_get_args(q,&e,DBUS_TYPE_UNIX_FD,&fd,DBUS_TYPE_INVALID) && fd>=0) {
                    ssize_t n=pread(fd,b,sizeof b-1,0); close(fd);
                    if (n!=9 || strcmp(b,"passed-fd")) errors++;
                    r=dbus_message_new_method_return(q); dbus_message_append_args(r,DBUS_TYPE_STRING,&s,DBUS_TYPE_INVALID); requests++;
                } else errors++;
            } else if (dbus_message_is_method_call(q,"org.nineos.Test","Quit")) {
                r=dbus_message_new_method_return(q); quit=1; requests++;
            }
            if (r) { if (!dbus_connection_send(c,r,NULL)) errors++; dbus_message_unref(r); dbus_connection_flush(c); }
            dbus_message_unref(q); dbus_error_free(&e);
        }
    }
    close_bus(c); return quit && requests==3 && !errors ? 0 : 13;
}
static unsigned credential(DBusConnection *c,const char *name,const char *method) {
    DBusMessage *q=request(DBUS_SERVICE_DBUS,DBUS_PATH_DBUS,DBUS_INTERFACE_DBUS,method);
    dbus_message_append_args(q,DBUS_TYPE_STRING,&name,DBUS_TYPE_INVALID);
    DBusMessage *r=call(c,q); dbus_uint32_t v=~0u; DBusError e; dbus_error_init(&e);
    CHECK(r && dbus_message_get_args(r,&e,DBUS_TYPE_UINT32,&v,DBUS_TYPE_INVALID));
    if (r) dbus_message_unref(r); dbus_error_free(&e); return v;
}
int main(void) {
    CHECK(dbus_threads_init_default());
    int fds[2]; int rc=pipe2(fds,O_CLOEXEC); CHECK(rc==0); if (rc) return 1;
    pid_t pid=fork(); CHECK(pid>=0); if (pid<0) { close(fds[0]); close(fds[1]); return 1; }
    if (!pid) { close(fds[0]); _exit(server(fds[1])); }
    close(fds[1]);
    struct pollfd p={fds[0],POLLIN,0}; char ready=0;
    CHECK(poll(&p,1,10000)==1 && read(fds[0],&ready,1)==1 && ready=='R'); close(fds[0]);
    DBusConnection *c=connect_bus(); CHECK(c!=NULL);
    if (!c) { kill(pid,SIGKILL); waitpid(pid,NULL,0); return 1; }
    const char *unique=dbus_bus_get_unique_name(c);
    CHECK(unique && unique[0]==':' && dbus_connection_get_is_authenticated(c));
    CHECK(dbus_connection_can_send_type(c,DBUS_TYPE_UNIX_FD));
    DBusError e; dbus_error_init(&e);
    CHECK(dbus_bus_name_has_owner(c,"org.nineos.Test",&e));
    CHECK(dbus_bus_request_name(c,"org.nineos.Test",DBUS_NAME_FLAG_DO_NOT_QUEUE,&e)==DBUS_REQUEST_NAME_REPLY_EXISTS);
    CHECK(credential(c,unique,"GetConnectionUnixUser")==getuid());
    CHECK(credential(c,unique,"GetConnectionUnixProcessID")==getpid());
    CHECK(credential(c,"org.nineos.Test","GetConnectionUnixProcessID")==pid);
    DBusMessage *q=request("org.nineos.Test","/org/nineos/Test","org.nineos.Test","Echo");
    const char *text="hello from 9os"; CHECK(dbus_message_append_args(q,DBUS_TYPE_STRING,&text,DBUS_TYPE_INVALID));
    DBusMessage *r=call(c,q); const char *answer=NULL;
    CHECK(r && dbus_message_get_args(r,&e,DBUS_TYPE_STRING,&answer,DBUS_TYPE_INVALID) && !strcmp(answer,text));
    if (r) dbus_message_unref(r);
    int fd=memfd_create("dbus-fd",MFD_CLOEXEC); CHECK(fd>=0 && write(fd,"passed-fd",9)==9);
    q=request("org.nineos.Test","/org/nineos/Test","org.nineos.Test","TakeFD");
    CHECK(dbus_message_append_args(q,DBUS_TYPE_UNIX_FD,&fd,DBUS_TYPE_INVALID));
    CHECK(close(fd)==0); /* libdbus owns its duplicate; receiver must not need original fd */
    r=call(c,q); answer=NULL;
    CHECK(r && dbus_message_get_args(r,&e,DBUS_TYPE_STRING,&answer,DBUS_TYPE_INVALID) && !strcmp(answer,"passed-fd"));
    if (r) dbus_message_unref(r);
    q=request("org.nineos.Missing","/missing","org.nineos.Test","Echo");
    r=dbus_connection_send_with_reply_and_block(c,q,5000,&e);
    CHECK(!r && dbus_error_has_name(&e,DBUS_ERROR_SERVICE_UNKNOWN)); if (r) dbus_message_unref(r); dbus_message_unref(q); dbus_error_free(&e);
    q=request("org.nineos.Test","/org/nineos/Test","org.nineos.Test","Quit"); r=call(c,q);
    CHECK(r && dbus_message_get_type(r)==DBUS_MESSAGE_TYPE_METHOD_RETURN); if (r) dbus_message_unref(r);
    int status=0; CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(!dbus_bus_name_has_owner(c,"org.nineos.Test",&e));
    close_bus(c); dbus_error_free(&e); dbus_shutdown();
    printf("dbustest: %s (%d checks, %d failures)\n",fails ? "FAILED" : "PASSED",checks,fails);
    return fails!=0;
}
