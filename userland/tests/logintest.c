/* M31 login: busybox login on a pty with /etc/passwd + /etc/shadow (SHA-512 crypt), a wrong
 * password refused, the session runs as the user (ids, groups, HOME, tty ownership), su back
 * to root, and passwd(1) - a setuid-root program run by the user - rewriting /etc/shadow.
 * usage: logintest   (as root; account "user", password "9os") */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("logintest: FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int m = -1;
static pid_t child;
static char buf[16384];
static size_t blen;

static void spawn(const char *const argv[]) {
    m = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(m); unlockpt(m);
    char *sn = ptsname(m);
    child = fork();
    if (child == 0) {
        setsid();
        int s = open(sn, O_RDWR);          /* becomes the controlling tty */
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(m);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    blen = 0;
}
/* read until pat shows up (returns 1) or timeout */
static int expect(const char *pat, int secs) {
    time_t end = time(NULL) + secs;
    for (;;) {
        buf[blen] = 0;
        if (strstr(buf, pat)) return 1;
        int left = (int)(end - time(NULL));
        if (left <= 0) break;
        struct pollfd p = { m, POLLIN, 0 };
        if (poll(&p, 1, left * 1000) <= 0) break;
        ssize_t n = read(m, buf + blen, sizeof buf - 1 - blen);
        if (n <= 0) break;
        if (getenv("LT_DEBUG")) { fwrite(buf + blen, 1, n, stdout); }
        blen += n;
    }
    printf("logintest: waiting for '%s', got: <<%s>>\n", pat, buf);
    return 0;
}
static void say(const char *s) { blen = 0; usleep(100000); write(m, s, strlen(s)); }
static int finish(void) {
    int st = 0;
    for (int i = 0; i < 50; i++) { if (waitpid(child, &st, WNOHANG) == child) { close(m); return st; } usleep(100000); }
    kill(child, SIGKILL); waitpid(child, &st, 0); close(m);
    return -1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getuid()) { printf("logintest: FAIL run as root\n"); return 1; }
    static const char *login[] = { "/bin/login", NULL };
    static const char *getty_[] = { "/sbin/getty", "-n", "-l", "/bin/login", "0", "-", "vt100", NULL };
    (void)getty_;
    char shadow[2048];
    int fd = open("/etc/shadow", O_RDONLY);
    ssize_t sl = read(fd, shadow, sizeof shadow);
    close(fd);

    /* wrong password */
    spawn(login);
    CHECK(expect("login:", 10), "login prompt");
    say("user\n");
    CHECK(expect("Password:", 10), "password prompt");
    say("wrong\n");
    CHECK(expect("incorrect", 15), "wrong password refused");
    kill(child, SIGKILL); finish();

    /* good password: a shell as the user */
    spawn(login);
    CHECK(expect("login:", 10), "login prompt");
    say("user\n");
    CHECK(expect("Password:", 10), "password prompt");
    say("9os\n");
    CHECK(expect("$ ", 15), "user shell prompt");
    say("echo U=$(id -u) G=$(id -g) H=$HOME T=$(stat -c %u.%a $(tty)) X=$(id -G)\n");
    CHECK(expect("U=1000 G=1000 H=/home/user T=1000.", 10), "session identity");
    CHECK(strstr(buf, "100") != NULL, "supplementary groups from /etc/group");
    say("cat /etc/shadow >/dev/null 2>&1 || echo NOSHADOW\n");
    CHECK(expect("NOSHADOW", 10), "user cannot read /etc/shadow");
    say("touch /root/x 2>/dev/null || echo NOROOT\n");
    CHECK(expect("NOROOT", 10), "user cannot write /root");
    say("echo hi > ~/f && cat ~/f && stat -c OWN=%U ~/f\n");
    CHECK(expect("OWN=user", 10), "file in home owned by user");
    /* passwd: setuid busybox rewrites /etc/shadow on the user's behalf */
    say("passwd\n");
    CHECK(expect("ld password:", 10), "passwd old password prompt");
    say("9os\n");
    CHECK(expect("ew password:", 10), "passwd new password prompt");
    say("Tr0ub4dor&3zq\n");
    CHECK(expect("Retype password:", 10), "passwd retype prompt");
    say("Tr0ub4dor&3zq\n");
    CHECK(expect("$ ", 15), "passwd done");
    say("echo PW=$?\n");
    CHECK(expect("PW=0", 10), "passwd succeeded");
    /* su to root (no password on this image) and back */
    say("su -c 'echo SU=$(id -u)' root\n");
    CHECK(expect("SU=0", 15), "su to root");
    say("exit\n");
    int st = finish();
    CHECK(st == 0 || (WIFEXITED(st)), "session exits (%d)", st);

    /* the new password works, the old one does not */
    spawn(login);
    expect("login:", 10); say("user\n"); expect("Password:", 10); say("9os\n");
    CHECK(expect("incorrect", 15), "old password refused after passwd");
    kill(child, SIGKILL); finish();
    spawn(login);
    expect("login:", 10); say("user\n"); expect("Password:", 10); say("Tr0ub4dor&3zq\n");
    CHECK(expect("$ ", 15), "new password accepted");
    say("exit\n");
    finish();
    struct stat sb;
    CHECK(stat("/etc/shadow", &sb) == 0 && sb.st_uid == 0 && !(sb.st_mode & 004), "shadow still root-only (%o)", sb.st_mode);

    fd = open("/etc/shadow", O_WRONLY | O_TRUNC);      /* restore */
    if (fd >= 0 && sl > 0) { write(fd, shadow, sl); close(fd); }
    printf(fails ? "logintest: %d failures\n" : "logintest: all passed\n", fails);
    return fails != 0;
}
