/* small shared library used by dyntest (dlopen + DT_NEEDED + TLS + constructors) */
#include <stdio.h>
static int inited;
__thread int demo_tls = 7;
__attribute__((constructor)) static void demo_init(void) { inited = 42; }
int demo_add(int a, int b) { return a + b + (inited == 42 ? 0 : 1000); }
const char *demo_name(void) { return "libdemo"; }
int demo_tls_get(void) { return demo_tls++; }
