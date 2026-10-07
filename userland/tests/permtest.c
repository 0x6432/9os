/* M31 users and permissions: DAC checks (read/write/search, owner/group/other classes,
 * supplementary groups), umask, sticky directories, chmod/chown rules, setgid directories,
 * setuid/setgid stripping on write and chown, setuid/setgid exec with AT_SECURE, --x
 * binaries, capabilities (capget/capset, dropping CAP_DAC_OVERRIDE/CAP_CHOWN/CAP_KILL,
 * bounding set), the set*id family with saved ids, access(2) real vs effective ids,
 * privileged syscalls refused to users, /proc/self/status, SO_PEERCRED, pty ownership,
 * protected hardlinks and /proc/<pid> link protection.
 * usage: permtest [DIR]   (DIR: world-writable sticky directory, default /tmp; run as root) */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/un.h>
#include <sys/fsuid.h>
#include <time.h>
#define CAP_SYS_BOOT_NR 22

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("permtest: FAIL " __VA_ARGS__); printf(" (errno %d) [%s:%d]\n", errno, __func__, __LINE__); fails++; } } while (0)
#define ERR(call, e) (errno = 0, (call) == -1 && errno == (e))
static char base[200];
static char *P(const char *rel) { static char b[4][300]; static int k; k = (k + 1) % 4; snprintf(b[k], sizeof b[k], "%s/%s", base, rel); return b[k]; }

/* run fn in a child with the given identity; the child's failures count in the parent */
static void as_user(uid_t uid, gid_t gid, int ngroups, const gid_t *groups, void (*fn)(void)) {
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        fails = 0;
        if (setgroups(ngroups, groups) || setgid(gid) || setuid(uid)) { printf("permtest: FAIL drop to %d\n", uid); _exit(99); }
        fn();
        fflush(stdout);
        _exit(fails > 50 ? 50 : fails);
    }
    int st;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st)) { printf("permtest: FAIL child killed by %d\n", WTERMSIG(st)); fails++; }
    else fails += WEXITSTATUS(st);
}
static void as_root_child(void (*fn)(void)) {
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) { fails = 0; fn(); fflush(stdout); _exit(fails > 50 ? 50 : fails); }
    int st;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st)) { printf("permtest: FAIL child killed by %d\n", WTERMSIG(st)); fails++; }
    else fails += WEXITSTATUS(st);
}
#define USER(fn) as_user(1000, 1000, 2, (gid_t[]){ 100, 1000 }, fn)
#define OTHER(fn) as_user(1001, 1001, 0, NULL, fn)

static int mkfile(const char *path, mode_t mode, uid_t u, gid_t g) {
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    write(fd, "data\n", 5);
    close(fd);
    if (chown(path, u, g) || chmod(path, mode)) return -1;
    return 0;
}
static mode_t mode_of(const char *path) { struct stat st; return stat(path, &st) ? (mode_t)-1 : st.st_mode & 07777; }

/* ---------------------------------------------------------------- DAC */
static void dac_user(void) {
    CHECK(ERR(open(P("priv/f"), O_RDONLY), EACCES), "search in 0700 dir");
    struct stat st;
    CHECK(ERR(stat(P("priv/f"), &st), EACCES), "stat through 0700 dir");
    CHECK(stat(P("xonly/f"), &st) == 0, "stat through 0711 dir");
    CHECK(opendir(P("xonly")) == NULL && errno == EACCES, "readdir of 0711 dir");
    int fd = open(P("xonly/f"), O_RDONLY);
    CHECK(fd >= 0, "read 0644 file");
    if (fd >= 0) close(fd);
    CHECK(ERR(open(P("xonly/f"), O_WRONLY), EACCES), "write 0644 root file");
    CHECK(ERR(open(P("xonly/f"), O_RDONLY | O_TRUNC), EACCES), "O_TRUNC needs write");
    CHECK(ERR(truncate(P("xonly/f"), 0), EACCES), "truncate needs write");
    fd = open(P("grp"), O_RDWR);                      /* 0660 root:users, users is supplementary */
    CHECK(fd >= 0, "group class via supplementary group");
    if (fd >= 0) close(fd);
    CHECK(ERR(open(P("mine0077"), O_RDONLY), EACCES), "owner class wins over others");
    CHECK(ERR(open(P("xonly/new"), O_WRONLY | O_CREAT, 0644), EACCES), "create in root dir");
    CHECK(ERR(mkdir(P("xonly/d"), 0755), EACCES), "mkdir in root dir");
    CHECK(ERR(unlink(P("xonly/f")), EACCES), "unlink in root dir");
    CHECK(ERR(rename(P("xonly/f"), P("stolen")), EACCES), "rename out of root dir");
    CHECK(ERR(symlink("x", P("xonly/l")), EACCES), "symlink in root dir");
    CHECK(ERR(chdir(P("priv")), EACCES), "chdir into 0700 dir");
    CHECK(ERR(execl(P("xonly/f"), "f", (char *)0), EACCES), "exec of non-executable");
    CHECK(ERR(execl(P("xonly"), "x", (char *)0), EACCES), "exec of a directory");
}
static void dac_tests(void) {
    mkdir(P("priv"), 0700); mkfile(P("priv/f"), 0644, 0, 0);
    mkdir(P("xonly"), 0711); chmod(P("xonly"), 0711); mkfile(P("xonly/f"), 0644, 0, 0);
    mkfile(P("grp"), 0660, 0, 100);
    mkfile(P("mine0077"), 0077, 1000, 1000);
    mkfile(P("zero"), 0000, 1000, 1000);
    USER(dac_user);
    /* root (CAP_DAC_OVERRIDE) reads anything, executes only what is executable by someone */
    int fd = open(P("mine0077"), O_RDWR);
    CHECK(fd >= 0, "root override");
    if (fd >= 0) close(fd);
    mkfile(P("noexec"), 0666, 0, 0);
    CHECK(access(P("noexec"), X_OK) == -1, "root X_OK on 0666 file");
    CHECK(access(P("mine0077"), X_OK) == 0, "root X_OK when anyone may exec");
}

/* ---------------------------------------------------------------- umask */
static void umask_tests(void) {
    mode_t old = umask(077);
    unlink(P("um")); rmdir(P("umd"));
    int fd = open(P("um"), O_WRONLY | O_CREAT, 0666);
    CHECK(fd >= 0 && mode_of(P("um")) == 0600, "umask 077 file mode %o", mode_of(P("um")));
    if (fd >= 0) close(fd);
    CHECK(mkdir(P("umd"), 0777) == 0 && mode_of(P("umd")) == 0700, "umask 077 dir mode %o", mode_of(P("umd")));
    CHECK(umask(old) == 077, "umask returns the previous");
    umask(027);
    unlink(P("um"));
    fd = open(P("um"), O_WRONLY | O_CREAT, 0666);
    CHECK(mode_of(P("um")) == 0640, "umask 027 file mode %o", mode_of(P("um")));
    if (fd >= 0) close(fd);
    umask(old);
    unlink(P("um")); rmdir(P("umd"));
}

/* ---------------------------------------------------------------- sticky */
static void sticky_create(void) {
    CHECK(mkfile(P("sticky/a"), 0666, 1000, 1000) == 0, "user creates in sticky dir");
    CHECK(mkdir(P("sticky/ad"), 0777) == 0, "user mkdir in sticky dir");
}
static void sticky_other(void) {
    CHECK(ERR(unlink(P("sticky/a")), EPERM), "unlink other's file in sticky dir");
    CHECK(ERR(rename(P("sticky/a"), P("sticky/b")), EPERM), "rename other's file in sticky dir");
    CHECK(ERR(rmdir(P("sticky/ad")), EPERM), "rmdir other's dir in sticky dir");
    int fd = open(P("sticky/a"), O_WRONLY);      /* 0666: writable, just not removable */
    CHECK(fd >= 0, "write other's 0666 file");
    if (fd >= 0) close(fd);
    CHECK(mkfile(P("sticky/o"), 0644, 1001, 1001) == 0 && unlink(P("sticky/o")) == 0, "own file in sticky dir");
}
static void sticky_owner(void) {
    CHECK(rename(P("sticky/a"), P("sticky/b")) == 0, "owner renames in sticky dir");
    CHECK(unlink(P("sticky/b")) == 0, "owner unlinks in sticky dir");
    CHECK(rmdir(P("sticky/ad")) == 0, "owner rmdir in sticky dir");
}
static void sticky_tests(void) {
    mkdir(P("sticky"), 01777); chmod(P("sticky"), 01777);
    USER(sticky_create);
    OTHER(sticky_other);
    USER(sticky_owner);
    rmdir(P("sticky"));
}

/* ---------------------------------------------------------------- chmod/chown */
static void chmod_user(void) {
    CHECK(ERR(chmod(P("rootfile"), 0777), EPERM), "chmod of root's file");
    CHECK(ERR(chown(P("ufile"), 0, -1), EPERM), "give file away");
    CHECK(ERR(chown(P("ufile"), -1, 0), EPERM), "chgrp to a foreign group");
    CHECK(chown(P("ufile"), -1, 100) == 0, "chgrp to a supplementary group");
    CHECK(chown(P("ufile"), 1000, -1) == 0, "chown to self is a no-op");
    CHECK(ERR(chown(P("rootfile"), -1, 1000), EPERM), "chgrp of root's file");
    CHECK(chmod(P("ufile"), 04755) == 0 && mode_of(P("ufile")) == 04755, "owner sets setuid");
    chown(P("ufile"), -1, 1000);
    CHECK(mode_of(P("ufile")) == 0755, "chgrp by owner clears setuid (%o)", mode_of(P("ufile")));
    chown(P("ufile"), -1, 100);
    CHECK(chmod(P("ufile"), 02755) == 0, "chmod g+s");
    chown(P("ufile"), -1, 1000);
    CHECK(chmod(P("ufile"), 02755) == 0 && mode_of(P("ufile")) == 02755, "g+s, own group");
    CHECK(fchmodat(AT_FDCWD, P("ufile"), 0644, 0) == 0, "fchmodat");
    CHECK(ERR(utimensat(AT_FDCWD, P("rootfile"), (struct timespec[]){ { 1, 0 }, { 1, 0 } }, 0), EPERM), "set times of other's file");
    CHECK(utimensat(AT_FDCWD, P("rootfile666"), NULL, 0) == 0, "touch writable file to now");
    CHECK(ERR(utimensat(AT_FDCWD, P("rootfile"), NULL, 0), EACCES), "touch unwritable file");
}
static void chgrp_foreign(void) {   /* user without group 4242 asks for g+s on a 4242 file it owns */
    CHECK(chmod(P("ufile2"), 02755) == 0 && mode_of(P("ufile2")) == 0755, "g+s silently dropped (%o)", mode_of(P("ufile2")));
}
static void chmod_tests(void) {
    mkfile(P("rootfile"), 0644, 0, 0);
    mkfile(P("rootfile666"), 0666, 0, 0);
    mkfile(P("ufile"), 0644, 1000, 1000);
    mkfile(P("ufile2"), 0644, 1000, 4242);
    USER(chmod_user);
    USER(chgrp_foreign);
    /* root's chown clears setuid/setgid(+x) too */
    chmod(P("ufile"), 06755);
    CHECK(mode_of(P("ufile")) == 06755, "root sets 6755");
    chown(P("ufile"), 1001, -1);
    CHECK(mode_of(P("ufile")) == 0755, "chown clears suid/sgid (%o)", mode_of(P("ufile")));
    chmod(P("ufile"), 02745);   /* setgid without group exec = mandatory locking: kept */
    chown(P("ufile"), 1000, -1);
    CHECK(mode_of(P("ufile")) == 02745, "chown keeps g+s without g+x (%o)", mode_of(P("ufile")));
}

/* ---------------------------------------------------------------- setgid dirs */
static void sgid_user(void) {
    struct stat st;
    int fd = open(P("sgd/uf2"), O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "user creates in setgid dir");
    if (fd >= 0) close(fd);
    CHECK(stat(P("sgd/uf2"), &st) == 0 && st.st_gid == 4321 && st.st_uid == 1000, "inherits the dir's group (%d)", st.st_gid);
}
static void sgid_tests(void) {
    mkdir(P("sgd"), 0777); chown(P("sgd"), 0, 4321); chmod(P("sgd"), 02777);
    int fd = open(P("sgd/f"), O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
    struct stat st;
    CHECK(stat(P("sgd/f"), &st) == 0 && st.st_gid == 4321, "file inherits setgid dir group (%d)", st.st_gid);
    CHECK(mkdir(P("sgd/sub"), 0755) == 0 && stat(P("sgd/sub"), &st) == 0 && st.st_gid == 4321 && (st.st_mode & S_ISGID),
          "subdir inherits group and setgid (%o)", st.st_mode);
    USER(sgid_user);
    unlink(P("sgd/f")); unlink(P("sgd/uf")); unlink(P("sgd/uf2")); rmdir(P("sgd/sub")); rmdir(P("sgd"));
}

/* ---------------------------------------------------------------- privileges dropped on write */
static void write_strips(void) {
    int fd = open(P("suidw"), O_WRONLY);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1, "owner writes");
    if (fd >= 0) close(fd);
    CHECK(mode_of(P("suidw")) == 0755, "write clears setuid+setgid (%o)", mode_of(P("suidw")));
    chmod(P("suidw"), 04755);
    fd = open(P("suidw"), O_WRONLY);
    if (fd >= 0) { CHECK(ftruncate(fd, 1) == 0, "ftruncate"); close(fd); }
    CHECK(mode_of(P("suidw")) == 0755, "ftruncate clears setuid (%o)", mode_of(P("suidw")));
}
static void strip_tests(void) {
    mkfile(P("suidw"), 06755, 1000, 1000);
    USER(write_strips);
    chmod(P("suidw"), 04755);
    int fd = open(P("suidw"), O_WRONLY);   /* root has CAP_FSETID: kept */
    if (fd >= 0) { write(fd, "y", 1); close(fd); }
    CHECK(mode_of(P("suidw")) == 04755, "root's write keeps setuid (%o)", mode_of(P("suidw")));
    unlink(P("suidw"));
}

/* ---------------------------------------------------------------- setuid exec */
static int copy_self(const char *dst) {
    int in = open("/proc/self/exe", O_RDONLY);
    if (in < 0) return -1;
    unlink(dst);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    char b[65536]; ssize_t n;
    while ((n = read(in, b, sizeof b)) > 0) write(out, b, n);
    close(in); close(out);
    return 0;
}
static int probe_status(const char *prog, const char *mode) {
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) { execl(prog, prog, "--probe", mode, (char *)0); _exit(100 + errno); }
    int st;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200;
}
/* inside the exec'd copy: report as an exit code */
static int probe(const char *mode) {
    uid_t r, e, s; gid_t rg, eg, sg;
    getresuid(&r, &e, &s); getresgid(&rg, &eg, &sg);
    unsigned long sec = getauxval(AT_SECURE);
    if (!strcmp(mode, "suid")) return (r == 1000 && e == 0 && s == 0 && sec == 1 && getauxval(AT_EUID) == 0 && getauxval(AT_UID) == 1000) ? 0 : 1;
    if (!strcmp(mode, "sgid")) return (r == 1000 && e == 1000 && rg == 1000 && eg == 4242 && sg == 4242 && sec == 1) ? 0 : 1;
    if (!strcmp(mode, "plain")) return (r == 1000 && e == 1000 && s == 1000 && sec == 0) ? 0 : 1;
    if (!strcmp(mode, "rootcaps")) {   /* user -> setuid root: full effective caps */
        int fd = open("/etc/shadow", O_RDONLY);
        return fd >= 0 ? 0 : 1;
    }
    if (!strcmp(mode, "nocaps")) {     /* root exec keeps nothing it had dropped from the bounding set */
        return prctl(PR_CAPBSET_READ, CAP_SYS_BOOT_NR, 0, 0, 0) == 0 && syscall(SYS_reboot, 0, 0, 0, 0) == -1 && errno == EPERM ? 0 : 1;
    }
    return 2;
}
static void exec_user(void) {
    CHECK(probe_status(P("sx/plain"), "plain") == 0, "plain exec ids");
    CHECK(probe_status(P("sx/suid"), "suid") == 0, "setuid-root exec: euid 0, AT_SECURE");
    CHECK(probe_status(P("sx/suid"), "rootcaps") == 0, "setuid-root exec gets capabilities");
    CHECK(probe_status(P("sx/sgid"), "sgid") == 0, "setgid exec: egid, AT_SECURE");
    CHECK(probe_status(P("sx/xonly"), "plain") == 0, "--x binary runs");
    CHECK(probe_status(P("sx/noperm"), "plain") == 100 + EACCES, "0700 root binary refused");
    int fd = open(P("sx/xonly"), O_RDONLY);
    CHECK(fd < 0 && errno == EACCES, "--x binary unreadable");
}
static void exec_nocaps(void) {
    CHECK(prctl(PR_CAPBSET_DROP, CAP_SYS_BOOT_NR, 0, 0, 0) == 0, "drop CAP_SYS_BOOT from bounding set");
    CHECK(probe_status(P("sx/plain"), "nocaps") == 0, "bounding set limits exec'd root");
}
static void exec_tests(void) {
    mkdir(P("sx"), 0755); chmod(P("sx"), 0755);
    copy_self(P("sx/plain")); chmod(P("sx/plain"), 0755);
    copy_self(P("sx/suid")); chown(P("sx/suid"), 0, 0); chmod(P("sx/suid"), 04755);
    copy_self(P("sx/sgid")); chown(P("sx/sgid"), 0, 4242); chmod(P("sx/sgid"), 02755);
    copy_self(P("sx/xonly")); chmod(P("sx/xonly"), 0711);
    copy_self(P("sx/noperm")); chmod(P("sx/noperm"), 0700);
    CHECK(mode_of(P("sx/suid")) == 04755 && mode_of(P("sx/sgid")) == 02755, "modes set");
    USER(exec_user);
    as_root_child(exec_nocaps);
    unlink(P("sx/plain")); unlink(P("sx/suid")); unlink(P("sx/sgid")); unlink(P("sx/xonly")); unlink(P("sx/noperm")); rmdir(P("sx"));
}

/* ---------------------------------------------------------------- capabilities */
struct caphdr { uint32_t version; int pid; };
struct capdat { uint32_t eff, perm, inh; };
static int capget_(struct capdat *d) { struct caphdr h = { 0x20080522, 0 }; return syscall(SYS_capget, &h, d); }
static int capset_(struct capdat *d) { struct caphdr h = { 0x20080522, 0 }; return syscall(SYS_capset, &h, d); }
static void caps_drop(void) {
    struct capdat d[2];
    CHECK(capget_(d) == 0 && d[0].eff == 0xffffffff && d[1].eff == 0x1ff, "root has all caps (%x %x)", d[0].eff, d[1].eff);
    d[0].eff &= ~((1u << 1) | (1u << 2));          /* DAC_OVERRIDE, DAC_READ_SEARCH */
    CHECK(capset_(d) == 0, "drop DAC caps from effective");
    CHECK(ERR(open(P("zero"), O_RDONLY), EACCES), "root without CAP_DAC_OVERRIDE");
    d[0].eff |= 1u << 1;
    CHECK(capset_(d) == 0, "raise back within permitted");
    int fd = open(P("zero"), O_RDONLY);
    CHECK(fd >= 0, "override again");
    if (fd >= 0) close(fd);
    d[0].perm &= ~(1u << 0); d[0].eff &= ~(1u << 0);  /* CAP_CHOWN gone for good */
    CHECK(capset_(d) == 0, "drop CAP_CHOWN from permitted");
    CHECK(ERR(chown(P("rootfile"), 1000, -1), EPERM), "chown without CAP_CHOWN");
    d[0].perm |= 1u << 0;
    CHECK(ERR(capset_(d), EPERM), "permitted cannot grow");
    d[0].perm &= ~(1u << 0);
    d[0].eff = d[0].perm & ~(1u << 5);                /* no CAP_KILL */
    CHECK(capset_(d) == 0, "drop CAP_KILL");
    CHECK(kill(1, 0) == 0, "same uid needs no CAP_KILL");
}
static void caps_kill(void) {
    struct capdat d[2];
    capget_(d);
    d[0].eff &= ~(1u << 5);
    capset_(d);
    pid_t p = fork();
    if (p == 0) { setuid(1000); pause(); _exit(0); }
    usleep(100000);
    CHECK(ERR(kill(p, SIGTERM), EPERM), "kill another user's process without CAP_KILL");
    d[0].eff |= 1u << 5;
    capset_(d);
    CHECK(kill(p, SIGKILL) == 0, "kill with CAP_KILL");
    waitpid(p, NULL, 0);
}
static void caps_status(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256]; int got = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Uid:", 4)) got |= strstr(line, "1000\t1000\t1000\t1000") ? 1 : 0;
        if (!strncmp(line, "Gid:", 4)) got |= strstr(line, "1000\t1000\t1000\t1000") ? 2 : 0;
        if (!strncmp(line, "CapEff:", 7)) got |= strstr(line, "0000000000000000") ? 4 : 0;
        if (!strncmp(line, "Groups:", 7)) got |= strstr(line, "100 1000") ? 8 : 0;
    }
    if (f) fclose(f);
    CHECK(got == 15, "/proc/self/status ids/caps (%d)", got);
    struct capdat d[2];
    CHECK(capget_(d) == 0 && !d[0].eff && !d[0].perm && !d[1].perm, "user has no caps");
    d[0].perm = d[0].eff = 1;
    CHECK(ERR(capset_(d), EPERM), "user cannot gain caps");
    CHECK(ERR(prctl(PR_CAPBSET_DROP, 1, 0, 0, 0), EPERM), "bset drop needs CAP_SETPCAP");
    CHECK(prctl(PR_CAPBSET_READ, 1, 0, 0, 0) == 1, "bset read");
}
static void caps_tests(void) {
    as_root_child(caps_drop);
    as_root_child(caps_kill);
    USER(caps_status);
}

/* ---------------------------------------------------------------- set*id */
static void ids_saved(void) {
    CHECK(setresuid(1000, 1000, 0) == 0, "root -> ruid/euid 1000, saved 0");
    CHECK(geteuid() == 1000 && ERR(open(P("priv/f"), O_RDONLY), EACCES), "effective 1000 has no override");
    CHECK(seteuid(0) == 0 && geteuid() == 0, "regain euid 0 from saved");
    int fd = open(P("priv/f"), O_RDONLY);
    CHECK(fd >= 0, "capabilities back with euid 0");
    if (fd >= 0) close(fd);
    CHECK(setreuid(-1, 1000) == 0 && geteuid() == 1000, "setreuid effective");
    uid_t r, e, s;
    getresuid(&r, &e, &s);
    CHECK(r == 1000 && e == 1000 && s == 0, "setreuid(-1, ruid) keeps saved (%d %d %d)", r, e, s);
}
static void ids_drop(void) {
    CHECK(setresuid(1000, 1000, 1000) == 0, "full drop");
    CHECK(ERR(setuid(0), EPERM), "no way back");
    CHECK(ERR(seteuid(0), EPERM), "no seteuid back");
    CHECK(ERR(setresuid(-1, 1001, -1), EPERM), "foreign euid");
    CHECK(ERR(setgid(4242), EPERM), "foreign setgid as user");
    gid_t g = 0;
    CHECK(ERR(setgroups(1, &g), EPERM), "setgroups as user");
    CHECK(getgroups(0, NULL) >= 0, "getgroups");
}
static void ids_keepcaps(void) {
    CHECK(prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) == 0 && prctl(PR_GET_KEEPCAPS, 0, 0, 0, 0) == 1, "keepcaps");
    CHECK(setresuid(1000, 1000, 1000) == 0, "drop with keepcaps");
    struct capdat d[2];
    capget_(d);
    CHECK(d[0].perm == 0xffffffff && d[0].eff == 0, "permitted kept, effective cleared (%x %x)", d[0].perm, d[0].eff);
    d[0].eff = 1u << 1;
    CHECK(capset_(d) == 0, "raise DAC_OVERRIDE as uid 1000");
    int fd = open(P("priv/f"), O_RDONLY);
    CHECK(fd >= 0, "uid 1000 with CAP_DAC_OVERRIDE");
    if (fd >= 0) close(fd);
}
static void ids_fsuid(void) {
    CHECK(setfsuid(1000) == 0, "setfsuid returns old");
    CHECK(ERR(open(P("priv/f"), O_RDONLY), EACCES), "fsuid 1000 drops fs caps");
    CHECK(setfsuid(0) == 1000, "back");
    int fd = open(P("priv/f"), O_RDONLY);
    CHECK(fd >= 0, "fs caps back");
    if (fd >= 0) close(fd);
}
static void access_user_eff(void) {   /* real uid 1000, effective 0 (as after a setuid-root exec) */
    CHECK(setresuid(1000, 0, 0) == 0, "ruid 1000 euid 0");
    CHECK(access(P("priv/f"), R_OK) == -1 && errno == EACCES, "access() uses the real uid");
    CHECK(faccessat(AT_FDCWD, P("priv/f"), R_OK, AT_EACCESS) == 0, "AT_EACCESS uses the effective uid");
    int fd = open(P("priv/f"), O_RDONLY);
    CHECK(fd >= 0, "open uses the effective uid");
    if (fd >= 0) close(fd);
}
static void ids_tests(void) {
    as_root_child(ids_saved);
    as_root_child(ids_drop);
    as_root_child(ids_keepcaps);
    as_root_child(ids_fsuid);
    as_root_child(access_user_eff);
}

/* ---------------------------------------------------------------- privileged syscalls */
static void priv_user(void) {
    CHECK(ERR(mount("none", P("xonly"), "tmpfs", 0, NULL), EPERM), "mount");
    CHECK(ERR(umount(P("xonly")), EPERM), "umount");
    CHECK(ERR(syscall(SYS_reboot, 0, 0, 0, 0), EPERM), "reboot");   /* bad magic: never reboots */
    CHECK(ERR(mknod(P("sticky_dev"), S_IFCHR | 0600, makedev(1, 3)), EPERM), "mknod char device");
    CHECK(ERR(chroot("/"), EPERM), "chroot");
    CHECK(ERR(sethostname("evil", 4), EPERM), "sethostname");
    CHECK(ERR(setpriority(PRIO_PROCESS, 0, -5), EACCES), "raise priority");
    CHECK(setpriority(PRIO_PROCESS, 0, 5) == 0, "lower priority");
    CHECK(ERR(setpriority(PRIO_PROCESS, 0, 0), EACCES), "raise back (no RLIMIT_NICE)");
    struct sched_param sp = { 10 };
    CHECK(ERR(syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp), EPERM), "real-time policy");
    CHECK(ERR(kill(1, SIGTERM), EPERM), "kill init");
    CHECK(ERR(setpriority(PRIO_PROCESS, 1, 10), EPERM), "renice init");
    struct timespec ts = { 0, 0 };
    CHECK(ERR(clock_settime(CLOCK_REALTIME, &ts), EPERM), "clock_settime");
    char l[64];
    CHECK(readlink("/proc/1/cwd", l, sizeof l) == -1 && errno == EACCES, "/proc/1/cwd of root's init");
    CHECK(readlink("/proc/self/cwd", l, sizeof l) > 0, "/proc/self/cwd");
    struct stat st;
    CHECK(stat("/proc/self", &st) == 0 && st.st_uid == 1000, "/proc/self owned by the process (%d)", st.st_uid);
    CHECK(ERR(link(P("priv_shadow"), P("sticky_hl")), EPERM), "protected hardlink to root's 0600 file");
    pid_t p = fork();
    if (p == 0) { pause(); _exit(0); }
    CHECK(kill(p, SIGKILL) == 0, "kill own-uid process");
    waitpid(p, NULL, 0);
}
static void priv_tests(void) {
    mkfile(P("priv_shadow"), 0600, 0, 0);
    chmod(base, 01777);
    USER(priv_user);
    unlink(P("priv_shadow")); unlink(P("sticky_hl")); unlink(P("sticky_dev"));
}

/* ---------------------------------------------------------------- sockets, ptys */
static void peer_user(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    struct ucred uc; socklen_t l = sizeof uc;
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &uc, &l) == 0 && uc.uid == 1000 && uc.gid == 1000 && uc.pid == getpid(),
          "SO_PEERCRED (%d %d)", uc.uid, uc.gid);
    close(sv[0]); close(sv[1]);
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m >= 0, "open ptmx");
    if (m >= 0) {
        struct stat st;
        char *n = ptsname(m);
        CHECK(n && stat(n, &st) == 0 && st.st_uid == 1000 && st.st_gid == 5 && (st.st_mode & 0777) == 0620,
              "pts owned by opener, tty group, 0620 (%d %d %o)", st.st_uid, st.st_gid, st.st_mode);
        close(m);
    }
}
static void other_connect(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", P("usock/s"));
    CHECK(connect(s, (void *)&a, sizeof a) == -1 && (errno == EACCES), "connect through 0700 dir");
    close(s);
}
static void sock_tests(void) {
    USER(peer_user);
    mkdir(P("usock"), 0700);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", P("usock/s"));
    unlink(a.sun_path);
    CHECK(bind(s, (void *)&a, sizeof a) == 0 && listen(s, 4) == 0, "bind");
    OTHER(other_connect);
    close(s); unlink(a.sun_path); rmdir(P("usock"));
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--probe")) return probe(argv[2]);
    snprintf(base, sizeof base, "%s/permtest.%d", argc > 1 ? argv[1] : "/tmp", getpid());
    if (getuid() != 0) { printf("permtest: FAIL must run as root\n"); return 1; }
    if (mkdir(base, 0755) || chmod(base, 0755)) { printf("permtest: FAIL mkdir %s (errno %d)\n", base, errno); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    dac_tests(); printf("permtest: dac done (%d)\n", fails);
    umask_tests(); printf("permtest: umask done (%d)\n", fails);
    sticky_tests(); printf("permtest: sticky done (%d)\n", fails);
    chmod_tests(); printf("permtest: chmod done (%d)\n", fails);
    sgid_tests(); printf("permtest: setgid dirs done (%d)\n", fails);
    strip_tests(); printf("permtest: suid strip done (%d)\n", fails);
    exec_tests(); printf("permtest: setuid exec done (%d)\n", fails);
    caps_tests(); printf("permtest: caps done (%d)\n", fails);
    ids_tests(); printf("permtest: set*id done (%d)\n", fails);
    priv_tests(); printf("permtest: privileged syscalls done (%d)\n", fails);
    sock_tests(); printf("permtest: sockets/ptys done (%d)\n", fails);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", base);
    system(cmd);
    printf(fails ? "permtest: %d failures\n" : "permtest: all passed\n", fails);
    return fails != 0;
}
