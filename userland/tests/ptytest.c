/* ptytest: /dev/ptmx + /dev/pts/N, line discipline over a pty, job control session, SIGWINCH, hangup. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("  FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); fails++; } } while (0)

/* read from the master until `want` shows up or timeout */
static int expect(int m, const char *want, char *acc, size_t cap, int ms) {
    size_t len = strlen(acc);
    for (int waited = 0; waited < ms; ) {
        if (strstr(acc, want)) return 1;
        struct pollfd p = { m, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) { waited += 100; continue; }
        ssize_t n = read(m, acc + len, cap - len - 1);
        if (n <= 0) break;
        len += n; acc[len] = 0;
    }
    return strstr(acc, want) != NULL;
}

int main(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m >= 0);
    CHECK(grantpt(m) == 0 && unlockpt(m) == 0);
    char *name = ptsname(m);
    CHECK(name && !strncmp(name, "/dev/pts/", 9));
    printf("ptytest: master %d, slave %s\n", m, name ? name : "?");
    struct winsize ws = { 30, 100, 0, 0 };
    CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0);

    pid_t c = fork();
    if (c == 0) {
        setsid();
        int s = open(name, O_RDWR);              /* becomes the controlling tty */
        if (s < 0) _exit(10);
        dup2(s, 0); dup2(s, 1); dup2(s, 2); if (s > 2) close(s);
        execl("/bin/sh", "sh", "-i", NULL);
        _exit(11);
    }
    char acc[8192] = {0};
    CHECK(expect(m, "# ", acc, sizeof acc, 5000));                 /* prompt */
    acc[0] = 0;
    write(m, "echo $((6*7)); stty size; tty\n", 30);
    CHECK(expect(m, "42\r\n", acc, sizeof acc, 5000));            /* ONLCR applied */
    CHECK(expect(m, "30 100", acc, sizeof acc, 5000));
    CHECK(expect(m, "/dev/pts/", acc, sizeof acc, 5000));
    /* canonical editing: backspace removes a character before the shell sees it */
    acc[0] = 0;
    write(m, "echo abX\x7f" "c\n", 11);
    CHECK(expect(m, "abc\r\n", acc, sizeof acc, 5000));
    /* Ctrl-C interrupts a foreground job */
    acc[0] = 0;
    write(m, "sleep 20\n", 9);
    usleep(300000);
    write(m, "\x03", 1);
    write(m, "echo after\n", 11);
    CHECK(expect(m, "after\r\n", acc, sizeof acc, 5000));
    /* raw mode on the slave side */
    acc[0] = 0;
    write(m, "stty raw -echo; dd bs=1 count=3 2>/dev/null | od -c | head -1; stty sane\n", 74);
    usleep(400000);
    write(m, "xyz", 3);
    CHECK(expect(m, "x   y   z", acc, sizeof acc, 5000));
    /* exit → master sees EIO/HUP once the slave is closed */
    write(m, "exit\n", 5);
    int st; waitpid(c, &st, 0);
    CHECK(WIFEXITED(st));
    struct pollfd p = { m, POLLIN, 0 };
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLHUP));
    close(m);
    printf("ptytest: %s (%d checks, %d failures)\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails != 0;
}
