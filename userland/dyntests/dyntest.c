/* dyntest: a dynamically linked PIE (ld-musl) that also dlopen()s a shared library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <link.h>

int demo_add(int, int);           /* from libdemo.so via DT_NEEDED */
extern __thread int demo_tls;

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void *thr(void *a) { return (void *)(long)(demo_tls + 100); }

static int count_objs(struct dl_phdr_info *i, size_t sz, void *d) { (*(int *)d)++; return 0; }

int main(int argc, char **argv) {
    CHECK(demo_add(2, 3) == 5);                       /* constructor ran, PLT binding works */
    CHECK(demo_tls == 7);                             /* initial-exec/global-dynamic TLS from a .so */
    demo_tls = 9;
    pthread_t t; void *rv;
    CHECK(pthread_create(&t, NULL, thr, NULL) == 0 && pthread_join(t, &rv) == 0);
    CHECK((long)rv == 107);                           /* new thread gets a fresh TLS block */
    void *h = dlopen("libdemo.so", RTLD_NOW);
    CHECK(h != NULL);
    if (h) {
        const char *(*name)(void) = (const char *(*)(void))dlsym(h, "demo_name");
        CHECK(name && !strcmp(name(), "libdemo"));
        CHECK(dlsym(h, "demo_add") == (void *)demo_add);
        CHECK(dlsym(h, "nonexistent") == NULL && dlerror() != NULL);
    }
    void *m = dlopen("libm.so", RTLD_NOW);            /* musl: libm is part of libc.so */
    CHECK(m != NULL);
    int n = 0;
    dl_iterate_phdr(count_objs, &n);
    CHECK(n >= 3);                                    /* main, libdemo, libc */
    char *s = malloc(100); snprintf(s, 100, "%s-%d", "dyn", 42);
    CHECK(!strcmp(s, "dyn-42"));
    printf("dyntest: %d objects loaded; %s (%d checks, %d failures)\n", n, fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
