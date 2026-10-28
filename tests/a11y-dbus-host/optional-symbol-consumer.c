/* Test-only optional-symbol hiding shim; production runtime never uses it. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
static void *optional_dlsym(void *handle,const char *name){
 const char *hidden=getenv("A11Y_TEST_HIDE_DIAGNOSTIC_SYMBOL");
 return hidden&&!strcmp(hidden,name)?NULL:dlsym(handle,name);
}
#define dlsym optional_dlsym
#include "../../userland/tests/a11y-dbus/terminal-dbus.c"
