/* Console TTY with a POSIX line discipline (canonical mode, echo, job-control signals). */
#include <kernel/tty.h>
#include <kernel/input.h>
#include <kernel/fbcon.h>
#include <kernel/vfs.h>
#include <kernel/cred.h>
#include <kernel/printk.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/errno.h>
#include <kernel/time.h>
#include <kernel/kmalloc.h>
#include <kernel/syscall.h>

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
static const struct lock_class tty_class = { "tty", LR_TTY, false };
static uint64_t tlock(struct tty *t) { uint64_t f = arch_irq_save(); spin_lock_ipi(&t->lock); return f; }
static void tunlock(struct tty *t, uint64_t f) { spin_unlock(&t->lock); arch_irq_restore(f); }

/* read/write buffers are user pointers for read(2)/write(2), kernel ones for sendfile */
static int copy_out(void *dst, const void *src, size_t n) {
    if ((vaddr_t)dst >= USER_TOP) { memcpy(dst, src, n); return 0; }
    return copy_to_user(dst, src, n);
}
static int copy_in(void *dst, const void *src, size_t n) {
    if ((vaddr_t)src >= USER_TOP) { memcpy(dst, src, n); return 0; }
    return copy_from_user(dst, src, n);
}

static size_t rb_count(struct tty *t) { return t->rtail - t->rhead; }
static void rb_put(struct tty *t, char c) {
    if (rb_count(t) < sizeof t->rbuf) t->rbuf[t->rtail++ % sizeof t->rbuf] = c;
}

static void tty_echo(struct tty *t, const char *s, size_t n) { t->output(t, s, n, false); }
static ssize_t console_out(struct tty *t, const char *s, size_t n, bool may_block) { console_write(s, n); return n; }
static struct tty *tty_of(struct file *f) { return f->priv ? f->priv : &console_tty; }

void tty_init(void) {
    tty_init_struct(&console_tty);
    console_tty.output = console_out;
}

void tty_init_struct(struct tty *t) {
    memset(t, 0, sizeof *t);
    t->t.c_iflag = ICRNL | IXON;
    t->t.c_oflag = OPOST | ONLCR;
    t->t.c_cflag = 0000260 | 0000017;       /* CS8 | CREAD | B38400 */
    t->t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
    const uint8_t cc[NCCS] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0 };
    memcpy(t->t.c_cc, cc, NCCS);
    t->ws = (struct winsize){ 25, 80, 0, 0 };
    wait_queue_init(&t->rq);
    spin_lock_init_class(&t->lock, &tty_class);
}

void fbcon_get_size(int *cols, int *rows);

static void input_ready(struct tty *t) { wake_up(&t->rq); poll_notify(); }

/* t->lock held; returns a job-control signal for the foreground group, or 0 */
static int tty_input_locked(struct tty *t, char c) {
    struct termios *tm = &t->t;
    if (c == '\r') { if (tm->c_iflag & IGNCR) return 0; if (tm->c_iflag & ICRNL) c = '\n'; }
    else if (c == '\n' && (tm->c_iflag & INLCR)) c = '\r';

    if (tm->c_lflag & ISIG) {
        int sig = 0;
        if ((uint8_t)c == tm->c_cc[VINTR]) sig = SIGINT;
        else if ((uint8_t)c == tm->c_cc[VQUIT]) sig = SIGQUIT;
        else if ((uint8_t)c == tm->c_cc[VSUSP]) sig = SIGTSTP;
        if (sig) {
            if (tm->c_lflag & ECHO) { char e[3] = { '^', (char)(c + 64), '\n' }; tty_echo(t, e, 2); }
            t->line_len = 0;
            return sig;
        }
    }

    if (tm->c_lflag & ICANON) {
        if ((uint8_t)c == tm->c_cc[VERASE] || c == '\b') {
            if (t->line_len) {
                t->line_len--;
                if (tm->c_lflag & ECHO) tty_echo(t, "\b \b", 3);
            }
            return 0;
        }
        if ((uint8_t)c == tm->c_cc[VKILL]) {
            while (t->line_len) { t->line_len--; if (tm->c_lflag & ECHO) tty_echo(t, "\b \b", 3); }
            return 0;
        }
        if ((uint8_t)c == tm->c_cc[VWERASE] && (tm->c_lflag & IEXTEN)) {
            while (t->line_len && t->line[t->line_len - 1] == ' ') { t->line_len--; if (tm->c_lflag & ECHO) tty_echo(t, "\b \b", 3); }
            while (t->line_len && t->line[t->line_len - 1] != ' ') { t->line_len--; if (tm->c_lflag & ECHO) tty_echo(t, "\b \b", 3); }
            return 0;
        }
        if ((uint8_t)c == tm->c_cc[VEOF]) {
            for (size_t i = 0; i < t->line_len; i++) rb_put(t, t->line[i]);
            if (!t->line_len) t->eof = true;
            t->line_len = 0;
            input_ready(t);
            return 0;
        }
        if (c == '\n') {
            if (tm->c_lflag & (ECHO | ECHONL)) tty_echo(t, "\n", 1);
            for (size_t i = 0; i < t->line_len; i++) rb_put(t, t->line[i]);
            rb_put(t, '\n');
            t->line_len = 0;
            input_ready(t);
            return 0;
        }
        if (t->line_len < sizeof t->line - 1) {
            t->line[t->line_len++] = c;
            if (tm->c_lflag & ECHO) {
                if ((uint8_t)c < 32 && c != '\t' && (tm->c_lflag & ECHOCTL)) { char e[2] = { '^', (char)(c + 64) }; tty_echo(t, e, 2); }
                else tty_echo(t, &c, 1);
            }
        }
        return 0;
    }
    rb_put(t, c);
    if (tm->c_lflag & ECHO) tty_echo(t, &c, 1);
    input_ready(t);
    return 0;
}

static void tty_input_ctx(struct tty *t, char c, bool process_ctx) {
    uint64_t f = tlock(t);
    int sig = tty_input_locked(t, c);
    int pg = t->pgrp;
    tunlock(t, f);
    if (!sig || pg <= 0) return;
    /* the process list is BKL-protected; interrupt context cannot take it (as before) */
    if (process_ctx) bkl_enter();
    signal_send_pgrp(pg, sig);
    if (process_ctx) bkl_exit();
}

void tty_input(struct tty *t, char c) { tty_input_ctx(t, c, false); }

void tty_input_str(struct tty *t, const char *s) { while (*s) tty_input(t, *s++); }

/* Lock-free read (file_ops.nobkl): state is sampled under t->lock, sleeps use the
 * sched-lock recheck (wait_until_sl) against input_ready()'s wake_up. */
static ssize_t tty_read(struct file *f, void *buf, size_t n, off_t *off) {
    struct tty *t = tty_of(f);
    if (!n) return 0;
    uint64_t deadline = 0;
    uint64_t fl;
    for (;;) {
        fl = tlock(t);
        if (rb_count(t)) break;                         /* keep the lock */
        bool canon = t->t.c_lflag & ICANON;
        uint8_t vmin = t->t.c_cc[VMIN], vtime = t->t.c_cc[VTIME];
        if (t->hup) { tunlock(t, fl); return 0; }
        if (canon && t->eof) { t->eof = false; tunlock(t, fl); return 0; }
        tunlock(t, fl);
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        if (!canon && vmin == 0) {
            if (!deadline) deadline = time_ns() + (uint64_t)vtime * 100000000ULL + 1;
            if (time_ns() >= deadline) return 0;
            if (signal_pending(current)) return -EINTR;
            sleep_ns(5000000);
            continue;
        }
        int r = wait_until_sl(&t->rq, rb_count(t) || t->eof || t->hup);
        if (r) return r;
    }
    char kb[512];
    size_t got = 0, lim = MIN(n, sizeof kb);
    bool canon = t->t.c_lflag & ICANON;
    while (got < lim && rb_count(t)) {
        char c = t->rbuf[t->rhead++ % sizeof t->rbuf];
        kb[got++] = c;
        if (canon && c == '\n') break;
    }
    tunlock(t, fl);
    if (copy_out(buf, kb, got)) return -EFAULT;
    return got;
}

static ssize_t tty_write(struct file *f, const void *buf, size_t n, off_t *off) {
    struct tty *t = tty_of(f);
    if (t->hup) return -EIO;
    char kb[512];
    size_t done = 0;
    while (done < n) {
        size_t c = MIN(n - done, sizeof kb);
        if (copy_in(kb, (const char *)buf + done, c)) return done ? (ssize_t)done : -EFAULT;
        ssize_t r = t->output(t, kb, c, !(f->flags & O_NONBLOCK));
        if (r < 0) return done ? (ssize_t)done : r;
        done += r;
        if ((size_t)r < c) break;
    }
    return done;
}

static unsigned tty_poll(struct file *f) {
    struct tty *t = tty_of(f);
    unsigned r = POLLOUT | POLLWRNORM;
    if (rb_count(t) || t->eof) r |= POLLIN | POLLRDNORM;
    if (t->hup) r |= POLLHUP | POLLIN;
    return r;
}

static int tty_ioctl_t(struct tty *t, struct file *f, uint64_t cmd, uint64_t arg);
static int tty_ioctl(struct file *f, uint64_t cmd, uint64_t arg) { return tty_ioctl_t(tty_of(f), f, cmd, arg); }
static int tty_ioctl_t(struct tty *t, struct file *f, uint64_t cmd, uint64_t arg) {
    struct process *p = curproc;
    switch (cmd) {
    case 0x5401: { /* TCGETS */
        uint64_t fl = tlock(t);
        struct termios tm = t->t;
        tunlock(t, fl);
        return copy_to_user((void *)arg, &tm, sizeof tm);
    }
    case 0x5402: case 0x5403: case 0x5404: { /* TCSETS, TCSETSW, TCSETSF */
        struct termios nt;
        if (copy_from_user(&nt, (void *)arg, sizeof nt)) return -EFAULT;
        uint64_t fl = tlock(t);
        if (cmd == 0x5404) { t->rhead = t->rtail; t->line_len = 0; }
        if ((t->t.c_lflag & ICANON) && !(nt.c_lflag & ICANON) && t->line_len) {
            for (size_t i = 0; i < t->line_len; i++) rb_put(t, t->line[i]);
            t->line_len = 0;
            input_ready(t);
        }
        t->t = nt;
        tunlock(t, fl);
        return 0;
    }
    case 0x5413: { /* TIOCGWINSZ */
        if (t == &console_tty) {
            int cols = 80, rows = 25;
            fbcon_get_size(&cols, &rows);
            t->ws.ws_col = cols; t->ws.ws_row = rows;
        }
        return copy_to_user((void *)arg, &t->ws, sizeof t->ws);
    }
    case 0x5414: { /* TIOCSWINSZ */
        struct winsize w;
        if (copy_from_user(&w, (void *)arg, sizeof w)) return -EFAULT;
        bool changed = memcmp(&w, &t->ws, sizeof w) != 0;
        t->ws = w;
        if (changed && t->pgrp > 0) signal_send_pgrp(t->pgrp, SIGWINCH);
        return 0;
    }
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
        if (arg == 0 || arg == 2) { uint64_t fl = tlock(t); t->rhead = t->rtail; t->line_len = 0; tunlock(t, fl); }
        return 0;
    case 0x5409: case 0x540A: case 0x5421: return 0;   /* TCSBRK, TCXONC, FIONBIO */
    }
    if (t != &console_tty) return -ENOTTY;
    /* virtual-terminal / keyboard ioctls used by display servers (seatd, weston, X) */
    static int kd_mode;
    static struct { char mode, waitv; short relsig, acqsig, frsig; } vt_mode;
    switch (cmd) {
    case 0x4B33: { char kb = 2; return copy_to_user((void *)arg, &kb, 1); }      /* KDGKBTYPE: KB_101 */
    case 0x4B3A:                                                                   /* KDSETMODE */
        if (arg > 1) return -EINVAL;
        kd_mode = (int)arg; fbcon_set_graphics(arg == 1);
        return 0;
    case 0x4B3B: return copy_to_user((void *)arg, &kd_mode, sizeof kd_mode);      /* KDGETMODE */
    case 0x4B44: return copy_to_user((void *)arg, &console_kbmode, sizeof(int)); /* KDGKBMODE */
    case 0x4B45:                                                                   /* KDSKBMODE */
        if (arg > 4) return -EINVAL;
        console_kbmode = (int)arg;
        return 0;
    case 0x5600: { int n = 1; return copy_to_user((void *)arg, &n, sizeof n); }  /* VT_OPENQRY */
    case 0x5601: return copy_to_user((void *)arg, &vt_mode, sizeof vt_mode);     /* VT_GETMODE */
    case 0x5602: return copy_from_user(&vt_mode, (void *)arg, sizeof vt_mode);   /* VT_SETMODE */
    case 0x5603: { uint16_t st[3] = { 1, 0, 2 }; return copy_to_user((void *)arg, st, sizeof st); } /* VT_GETSTATE */
    case 0x5605: case 0x5606: case 0x5607: return 0;                             /* VT_RELDISP/ACTIVATE/WAITACTIVE */
    default: return -ENOTTY;
    }
}

/* a session leader without a controlling terminal acquires the tty it opens */
static void maybe_acquire_ctty(struct tty *t, struct file *f) {
    struct process *p = curproc;
    if (p && !(f->flags & O_NOCTTY) && p->pid == p->sid && !p->ctty && t->sid == 0) {
        p->ctty = t;
        t->sid = p->sid;
        t->pgrp = p->pgid;
    }
}

static int tty_open(struct inode *ino, struct file *f) {
    struct process *p = curproc;
    if (ino->rdev == MKDEV(5, 0) && p && p->ctty) { f->priv = p->ctty; return 0; }   /* /dev/tty */
    maybe_acquire_ctty(&console_tty, f);
    return 0;
}

static const struct file_ops tty_ops = {
    .nobkl = true, .open = tty_open, .read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .poll = tty_poll,
};

#include "pty.inc"

void tty_register_devices(void) {
    tty_init();
    pty_register();
    chrdev_register(5, 0, &tty_ops);
    chrdev_register(5, 1, &tty_ops);
    chrdev_register(4, 0, &tty_ops);
    chrdev_register(4, 1, &tty_ops);
    chrdev_register(4, 64, &tty_ops);
}
