/* Console TTY with a POSIX line discipline (canonical mode, echo, job-control signals). */
#include <kernel/tty.h>
#include <kernel/vfs.h>
#include <kernel/printk.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/time.h>

#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define ECHOCTL 0001000
#define ECHOKE 0004000
#define IEXTEN 0100000
#define ICRNL 0000400
#define IGNCR 0000200
#define INLCR 0000100
#define IXON 0002000
#define OPOST 0000001
#define ONLCR 0000004
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSUSP 10
#define VWERASE 14

struct tty console_tty;

static size_t rb_count(struct tty *t) { return t->rtail - t->rhead; }
static void rb_put(struct tty *t, char c) {
    if (rb_count(t) < sizeof t->rbuf) t->rbuf[t->rtail++ % sizeof t->rbuf] = c;
}

static void tty_echo(struct tty *t, const char *s, size_t n) { console_write(s, n); }

void tty_init(void) {
    struct tty *t = &console_tty;
    memset(t, 0, sizeof *t);
    t->t.c_iflag = ICRNL | IXON;
    t->t.c_oflag = OPOST | ONLCR;
    t->t.c_cflag = 0000260 | 0000017;       /* CS8 | CREAD | B38400 */
    t->t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
    const uint8_t cc[NCCS] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0 };
    memcpy(t->t.c_cc, cc, NCCS);
    t->ws = (struct winsize){ 25, 80, 0, 0 };
    wait_queue_init(&t->rq);
}

void fbcon_get_size(int *cols, int *rows);

static void input_ready(struct tty *t) { wake_up(&t->rq); poll_notify(); }

void tty_input(struct tty *t, char c) {
    struct termios *tm = &t->t;
    if (c == '\r') { if (tm->c_iflag & IGNCR) return; if (tm->c_iflag & ICRNL) c = '\n'; }
    else if (c == '\n' && (tm->c_iflag & INLCR)) c = '\r';

    if (tm->c_lflag & ISIG) {
        int sig = 0;
        if ((uint8_t)c == tm->c_cc[VINTR]) sig = SIGINT;
        else if ((uint8_t)c == tm->c_cc[VQUIT]) sig = SIGQUIT;
        else if ((uint8_t)c == tm->c_cc[VSUSP]) sig = SIGTSTP;
        if (sig) {
            if (tm->c_lflag & ECHO) { char e[3] = { '^', (char)(c + 64), '\n' }; tty_echo(t, e, 2); }
            t->line_len = 0;
            if (t->pgrp > 0) signal_send_pgrp(t->pgrp, sig);
            return;
        }
    }

    if (tm->c_lflag & ICANON) {
        if ((uint8_t)c == tm->c_cc[VERASE] || c == '\b') {
            if (t->line_len) {
                t->line_len--;
                if (tm->c_lflag & ECHO) tty_echo(t, "\b \b", 3);
            }
            return;
        }
        if ((uint8_t)c == tm->c_cc[VKILL]) {
            while (t->line_len) { t->line_len--; if (tm->c_lflag & ECHO) tty_echo(t, "\b \b", 3); }
            return;
        }
        if ((uint8_t)c == tm->c_cc[VWERASE] && (tm->c_lflag & IEXTEN)) {
            while (t->line_len && t->line[t->line_len - 1] == ' ') { t->line_len--; if (tm->c_lflag & ECHO) tty_echo(t, "\b \b", 3); }
            while (t->line_len && t->line[t->line_len - 1] != ' ') { t->line_len--; if (tm->c_lflag & ECHO) tty_echo(t, "\b \b", 3); }
            return;
        }
        if ((uint8_t)c == tm->c_cc[VEOF]) {
            for (size_t i = 0; i < t->line_len; i++) rb_put(t, t->line[i]);
            if (!t->line_len) t->eof = true;
            t->line_len = 0;
            input_ready(t);
            return;
        }
        if (c == '\n') {
            if (tm->c_lflag & (ECHO | ECHONL)) tty_echo(t, "\n", 1);
            for (size_t i = 0; i < t->line_len; i++) rb_put(t, t->line[i]);
            rb_put(t, '\n');
            t->line_len = 0;
            input_ready(t);
            return;
        }
        if (t->line_len < sizeof t->line - 1) {
            t->line[t->line_len++] = c;
            if (tm->c_lflag & ECHO) {
                if ((uint8_t)c < 32 && c != '\t' && (tm->c_lflag & ECHOCTL)) { char e[2] = { '^', (char)(c + 64) }; tty_echo(t, e, 2); }
                else tty_echo(t, &c, 1);
            }
        }
        return;
    }
    rb_put(t, c);
    if (tm->c_lflag & ECHO) tty_echo(t, &c, 1);
    input_ready(t);
}

void tty_input_str(struct tty *t, const char *s) { while (*s) tty_input(t, *s++); }

static ssize_t tty_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct tty *t = &console_tty;
    if (!n) return 0;
    bool canon = t->t.c_lflag & ICANON;
    uint8_t vmin = t->t.c_cc[VMIN], vtime = t->t.c_cc[VTIME];
    if (!rb_count(t)) {
        if (canon && t->eof) { t->eof = false; return 0; }
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (!canon && vmin == 0) {
            uint64_t deadline = time_ns() + (uint64_t)vtime * 100000000ULL;
            while (!rb_count(t) && time_ns() < deadline) {
                if (signal_pending(current)) return -EINTR;
                sleep_ns(5000000);
            }
            if (!rb_count(t)) return 0;
        } else {
            int r = wait_until(&t->rq, rb_count(t) || (canon && t->eof));
            if (r) return r;
            if (!rb_count(t)) { t->eof = false; return 0; }
        }
    }
    size_t got = 0;
    char *b = buf;
    uint64_t fl = arch_irq_save();
    while (got < n && rb_count(t)) {
        char c = t->rbuf[t->rhead++ % sizeof t->rbuf];
        b[got++] = c;
        if (canon && c == '\n') break;
    }
    arch_irq_restore(fl);
    return got;
}

static ssize_t tty_write(struct file *f, const void *buf, size_t n, off_t *off) {
    console_write(buf, n);
    return n;
}

static unsigned tty_poll(struct file *f) {
    struct tty *t = &console_tty;
    unsigned r = POLLOUT | POLLWRNORM;
    if (rb_count(t) || t->eof) r |= POLLIN | POLLRDNORM;
    return r;
}

static int tty_ioctl(struct file *f, uint64_t cmd, uint64_t arg) {
    struct tty *t = &console_tty;
    struct process *p = curproc;
    switch (cmd) {
    case 0x5401: /* TCGETS */
        return copy_to_user((void *)arg, &t->t, sizeof t->t);
    case 0x5402: case 0x5403: case 0x5404: { /* TCSETS, TCSETSW, TCSETSF */
        struct termios nt;
        if (copy_from_user(&nt, (void *)arg, sizeof nt)) return -EFAULT;
        if (cmd == 0x5404) { t->rhead = t->rtail; t->line_len = 0; }
        if ((t->t.c_lflag & ICANON) && !(nt.c_lflag & ICANON) && t->line_len) {
            for (size_t i = 0; i < t->line_len; i++) rb_put(t, t->line[i]);
            t->line_len = 0;
        }
        t->t = nt;
        return 0;
    }
    case 0x5413: { /* TIOCGWINSZ */
        int cols = 80, rows = 25;
        fbcon_get_size(&cols, &rows);
        t->ws.ws_col = cols; t->ws.ws_row = rows;
        return copy_to_user((void *)arg, &t->ws, sizeof t->ws);
    }
    case 0x5414: return copy_from_user(&t->ws, (void *)arg, sizeof t->ws);
    case 0x540F: /* TIOCGPGRP */
        return copy_to_user((void *)arg, &t->pgrp, sizeof(int));
    case 0x5410: { /* TIOCSPGRP */
        int pg;
        if (copy_from_user(&pg, (void *)arg, sizeof pg)) return -EFAULT;
        t->pgrp = pg;
        return 0;
    }
    case 0x5429: /* TIOCGSID */
        return copy_to_user((void *)arg, &t->sid, sizeof(int));
    case 0x540E: /* TIOCSCTTY */
        if (p) { p->ctty = t; t->sid = p->sid; t->pgrp = p->pgid; }
        return 0;
    case 0x5422: /* TIOCNOTTY */
        if (p) p->ctty = nullptr;
        return 0;
    case 0x541B: { /* FIONREAD */
        int n = (int)rb_count(t);
        return copy_to_user((void *)arg, &n, sizeof n);
    }
    case 0x540B: /* TCFLSH */
        if (arg == 0 || arg == 2) { t->rhead = t->rtail; t->line_len = 0; }
        return 0;
    case 0x5409: case 0x540A: case 0x5421: return 0;   /* TCSBRK, TCXONC, FIONBIO */
    default: return -ENOTTY;
    }
}

static int tty_open(struct inode *ino, struct file *f) {
    struct process *p = curproc;
    if (p && !(f->flags & O_NOCTTY) && p->pid == p->sid && !p->ctty && console_tty.sid == 0) {
        p->ctty = &console_tty;
        console_tty.sid = p->sid;
        console_tty.pgrp = p->pgid;
    }
    return 0;
}

static const struct file_ops tty_ops = {
    .open = tty_open, .read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .poll = tty_poll,
};

void tty_register_devices(void) {
    tty_init();
    chrdev_register(5, 0, &tty_ops);
    chrdev_register(5, 1, &tty_ops);
    chrdev_register(4, 0, &tty_ops);
    chrdev_register(4, 1, &tty_ops);
    chrdev_register(4, 64, &tty_ops);
}
