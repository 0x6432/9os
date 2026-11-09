/* Host control for the real kmain dispatch, not a guest or isolation test.
 * Every case is a fresh process because kmain uses static boot buffers. */
#include <kernel/boot.h>
#include <kernel/process.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

static jmp_buf done;
static const char *cmdline;
static int mount_result, calls, panicked, reached_init, pressure, loaded, log_error, init_was_null;
static size_t source_len;
static char source_seen[64], type_seen[16], panic_source[64], logged_source[64], init_seen[128];
static int logged_error;
static const char *panic_fmt;
static int panic_error;
uint64_t hhdm_offset;
bool syscall_trace;

const char *boot_cmdline(void) { return cmdline; }
uint64_t boot_revision(void) { return 0; }
struct limine_memmap_response *boot_memmap(void) {
    static struct limine_memmap_response empty;
    return &empty;
}
void initramfs_load(void) { loaded++; }
void mm_pressure_init(void) { pressure++; }
void printk(const char *fmt, ...) {
    if (strstr(fmt, "cannot mount root")) {
        va_list ap; va_start(ap, fmt);
        snprintf(logged_source, sizeof logged_source, "%s", va_arg(ap, const char *));
        logged_error = va_arg(ap, int); va_end(ap); log_error++;
    }
}
int vfs_mount_root(const char *source, const char *type) {
    calls++;
    source_len = strlen(source);
    snprintf(source_seen, sizeof source_seen, "%s", source);
    snprintf(type_seen, sizeof type_seen, "%s", type);
    return mount_result;
}
__noreturn void panic(const char *fmt, ...) {
    va_list ap;
    panicked++;
    panic_fmt = fmt;
    va_start(ap, fmt);
    if (strstr(fmt, "required root %s failed")) {
        snprintf(panic_source, sizeof panic_source, "%s", va_arg(ap, const char *));
        panic_error = va_arg(ap, int);
    }
    va_end(ap);
    longjmp(done, 1);
}
struct process *process_create_init(const char *path) {
    init_was_null = path == nullptr;
    if (path) snprintf(init_seen, sizeof init_seen, "%s", path);
    reached_init++; longjmp(done, 1);
}
void sleep_ns(uint64_t ns) { (void)ns; longjmp(done, 2); }
void kmain(void);

struct test_case { const char *name, *cmdline; int result, calls, panic, init, error; const char *source; };
static const struct test_case cases[] = {
    {"strict-ok", "root_required=1 root=/dev/vda1", 0, 1, 0, 1, 0, "/dev/vda1"},
    {"strict-negative", "root_required=1 root=/dev/vda1", -5, 1, 1, 0, -5, "/dev/vda1"},
    {"strict-positive", "root_required=1 root=/dev/vda1", 5, 1, 1, 0, 5, "/dev/vda1"},
    {"invalid-flag", "root_required=0 root=/dev/vda1", 0, 0, 1, 0, 0, ""},
    {"missing-root", "root_required=1", 0, 0, 1, 0, 0, ""},
    {"duplicate-root", "root_required=1 root=/dev/a root=/dev/b", 0, 0, 1, 0, 0, ""},
    {"duplicate-flag", "root_required=1 root_required=1 root=/dev/a", 0, 0, 1, 0, 0, ""},
    {"legacy-ok", "root=/dev/vda1", 0, 1, 0, 1, 0, "/dev/vda1"},
    {"legacy-fail", "root=/dev/vda1", -5, 1, 0, 1, 0, "/dev/vda1"},
    {"no-root", "quiet", 0, 0, 0, 1, 0, ""},
    {"legacy-substring", "notroot=/dev/vda1", 0, 1, 0, 1, 0, "/dev/vda1"},
    {"legacy-duplicate", "root=/dev/a root=/dev/b", 0, 1, 0, 1, 0, "/dev/a"},
    {"legacy-empty", "root= init=/sbin/start", -5, 1, 0, 1, 0, ""},
    {"legacy-tab", "root=/dev/a\toption=x", 0, 1, 0, 1, 0, "/dev/a\toption=x"},
    {"legacy-truncated", "root=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0, 1, 0, 1, 0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const struct test_case *t = NULL;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (!strcmp(argv[1], cases[i].name)) t = &cases[i];
    if (!t) return 2;
    cmdline = t->cmdline; mount_result = t->result;
    int stop = setjmp(done);
    if (!stop) kmain();
    int bad = stop != 1 || loaded != 1 || calls != t->calls || panicked != t->panic ||
        reached_init != t->init || pressure != t->init || strcmp(source_seen, t->source) ||
        (calls && source_len != strlen(t->source)) ||
        (calls && strcmp(type_seen, "ext2")) || (t->error && (panic_error != t->error || strcmp(panic_source, t->source))) ||
        (t->panic && (!panic_fmt || !strstr(panic_fmt, t->error ? "required root %s failed" : "invalid required root"))) ||
        (log_error != (!t->panic && t->result != 0 && calls != 0)) ||
        (log_error && (strcmp(logged_source, t->source) || logged_error != t->result)) ||
        (t->init && strcmp(init_seen, strstr(t->cmdline, "init=/sbin/start") ? "/sbin/start" : "")) ||
        (t->init && init_was_null != (strstr(t->cmdline, "init=/sbin/start") == nullptr));
    if (bad) fprintf(stderr, "%s FAIL stop=%d loaded=%d calls=%d panic=%d init=%d pressure=%d error=%d source=%s type=%s logs=%d\n",
                     t->name, stop, loaded, calls, panicked, reached_init, pressure, panic_error, source_seen, type_seen, log_error);
    return bad ? 1 : 0;
}
