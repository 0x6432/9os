/*
 * M35: Linux-style virtual terminals. tty1..tty6 each have their own line discipline (struct
 * tty) and text console (fbcon shadow buffer); tty0 opens the foreground VT. Switching follows
 * the Linux VT semantics used by seatd/logind/X/weston:
 *  - VT_AUTO: VT_ACTIVATE (or Alt+Fn / Ctrl+Alt+Fn in K_XLATE/K_UNICODE) switches immediately;
 *  - VT_PROCESS: the owner of the current VT gets relsig and the switch waits for
 *    VT_RELDISP(1) (VT_RELDISP(0) refuses it); the owner of the new VT gets acqsig and
 *    answers VT_RELDISP(VT_ACKACQ). An owner that died is reset to VT_AUTO/KD_TEXT/K_XLATE.
 * KD mode and keyboard mode are per VT; VT_WAITACTIVE sleeps until the VT is in front.
 * /dev/vcs, /dev/vcsN read a console's characters (rows*cols bytes, no newlines).
 */
#include <kernel/vt.h>
#include <kernel/tty.h>
#include <kernel/fbcon.h>
#include <kernel/printk.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/cpu.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/errno.h>
#include <kernel/string.h>
#include <kernel/device.h>
#include <kernel/syscall.h>

#define VT_AUTO 0
#define VT_PROCESS 1
#define VT_ACKACQ 2
#define KD_TEXT 0
#define K_XLATE 1
#define NSIG_MAX 64

struct tty vt_ttys[NR_VT];
struct vt_mode_k { char mode, waitv; short relsig, acqsig, frsig; };
static struct {
    int kd_mode, kbmode;
    struct vt_mode_k mode;
    int owner;               /* VT_PROCESS: pid that gets relsig/acqsig */
    int opens;
} vts[NR_VT];
static int fg, want = -1;
static bool switch_locked;   /* VT_LOCKSWITCH */
static spinlock_t vt_lock = SPINLOCK_INIT;
static struct wait_queue vt_wq;        /* VT_WAITACTIVE */
static struct wait_queue kbd_wq;       /* keyboard switch requests -> vtswitch thread */
static volatile int kbd_req = -1;

int vt_fg(void) { return fg; }
struct tty *vt_fg_tty(void) { return &vt_ttys[fg]; }
int vt_kbmode(void) { return vts[fg].kbmode; }
int vt_index(struct tty *t) {
    if (t >= vt_ttys && t < vt_ttys + NR_VT) return (int)(t - vt_ttys);
    return -1;
}

/* process context: false if the process is gone */
static bool send_sig(int pid, int sig) {
    if (pid <= 0) return false;
    bool take = !bkl_held();
    if (take) bkl_enter();
    struct process *p = process_find(pid);
    bool ok = p && p->state == P_ALIVE;
    if (ok && sig > 0 && sig < NSIG_MAX) signal_send(p, sig);
    if (take) bkl_exit();
    return ok;
}

static void reset_vc(int i) {           /* the controlling process died */
    uint64_t f = spin_lock_irqsave(&vt_lock);
    vts[i].mode = (struct vt_mode_k){ 0 };
    vts[i].owner = 0;
    vts[i].kbmode = K_XLATE;
    vts[i].kd_mode = KD_TEXT;
    spin_unlock_irqrestore(&vt_lock, f);
    fbcon_vt_set_kd_graphics(i, false);
}

static void complete_switch(int n) {
    uint64_t f = spin_lock_irqsave(&vt_lock);
    fg = n; want = -1;
    bool proc = vts[n].mode.mode == VT_PROCESS;
    int pid = vts[n].owner, sig = vts[n].mode.acqsig;
    spin_unlock_irqrestore(&vt_lock, f);
    fbcon_vt_switch(n);
    if (proc && !send_sig(pid, sig)) reset_vc(n);
    wake_up(&vt_wq);
    poll_notify();
}

void vt_activate(int n) {
    if (n < 0 || n >= NR_VT) return;
    uint64_t f = spin_lock_irqsave(&vt_lock);
    if (n == fg) { spin_unlock_irqrestore(&vt_lock, f); return; }
    int cur = fg;
    if (vts[cur].mode.mode == VT_PROCESS) {
        want = n;
        int pid = vts[cur].owner, sig = vts[cur].mode.relsig;
        spin_unlock_irqrestore(&vt_lock, f);
        if (send_sig(pid, sig)) return;              /* wait for VT_RELDISP */
        reset_vc(cur);
    } else spin_unlock_irqrestore(&vt_lock, f);
    complete_switch(n);
}

/* keyboard (interrupt context): the switch may signal processes, so a thread does it */
static void vtswitch_thread(void *arg) {
    for (;;) {
        wait_until(&kbd_wq, kbd_req >= 0);
        int n = __atomic_exchange_n(&kbd_req, -1, __ATOMIC_ACQ_REL);
        if (n >= 0) vt_activate(n);
    }
}
void vt_kbd_switch(int n) {
    if (switch_locked || n < 0 || n >= NR_VT) return;
    kbd_req = n;
    wake_up(&kbd_wq);
}

void vt_open(struct tty *t) {
    int i = vt_index(t);
    if (i >= 0) __atomic_add_fetch(&vts[i].opens, 1, __ATOMIC_RELAXED);
}
void vt_close(struct tty *t) {
    int i = vt_index(t);
    if (i >= 0) __atomic_sub_fetch(&vts[i].opens, 1, __ATOMIC_RELAXED);
}

static ssize_t vt_out(struct tty *t, const char *s, size_t n, bool may_block) {
    fbcon_vt_write(vt_index(t), s, n);
    return n;
}

static bool is_root(void) { return !curproc || curproc->cred->euid == 0; }

int vt_ioctl(struct tty *t, uint64_t cmd, uint64_t arg) {
    int i = vt_index(t);
    if (i < 0) return -ENOTTY;
    switch (cmd) {
    case 0x4B33: { char kb = 2; return copy_to_user((void *)arg, &kb, 1); }      /* KDGKBTYPE: KB_101 */
    case 0x4B3A:                                                                   /* KDSETMODE */
        if (arg > 1) return -EINVAL;
        vts[i].kd_mode = (int)arg;
        fbcon_vt_set_kd_graphics(i, arg == 1);
        return 0;
    case 0x4B3B: return copy_to_user((void *)arg, &vts[i].kd_mode, sizeof(int)); /* KDGETMODE */
    case 0x4B44: return copy_to_user((void *)arg, &vts[i].kbmode, sizeof(int));  /* KDGKBMODE */
    case 0x4B45:                                                                   /* KDSKBMODE */
        if (arg > 4) return -EINVAL;
        vts[i].kbmode = (int)arg;
        return 0;
    case 0x5600: {                                                                 /* VT_OPENQRY */
        int n = -1;
        for (int k = 0; k < NR_VT; k++) if (!vts[k].opens) { n = k + 1; break; }
        return copy_to_user((void *)arg, &n, sizeof n);
    }
    case 0x5601: return copy_to_user((void *)arg, &vts[i].mode, sizeof vts[i].mode);   /* VT_GETMODE */
    case 0x5602: {                                                                 /* VT_SETMODE */
        struct vt_mode_k m;
        if (copy_from_user(&m, (void *)arg, sizeof m)) return -EFAULT;
        if (m.mode != VT_AUTO && m.mode != VT_PROCESS) return -EINVAL;
        if (m.relsig < 0 || m.relsig >= NSIG_MAX || m.acqsig < 0 || m.acqsig >= NSIG_MAX) return -EINVAL;
        uint64_t f = spin_lock_irqsave(&vt_lock);
        vts[i].mode = m;
        vts[i].owner = m.mode == VT_PROCESS && curproc ? curproc->pid : 0;
        if (i == fg) want = -1;
        spin_unlock_irqrestore(&vt_lock, f);
        return 0;
    }
    case 0x5603: {                                                                 /* VT_GETSTATE */
        uint16_t st[3] = { (uint16_t)(fg + 1), 0, 0 };
        for (int k = 0; k < NR_VT; k++) if (vts[k].opens || k == fg) st[2] |= 1u << (k + 1);
        return copy_to_user((void *)arg, st, sizeof st);
    }
    case 0x5605: {                                                                 /* VT_RELDISP */
        uint64_t f = spin_lock_irqsave(&vt_lock);
        if (vts[i].mode.mode != VT_PROCESS) { spin_unlock_irqrestore(&vt_lock, f); return -EINVAL; }
        if (i == fg && want >= 0) {
            int n = want;
            if (!arg) { want = -1; spin_unlock_irqrestore(&vt_lock, f); return 0; }   /* refused */
            spin_unlock_irqrestore(&vt_lock, f);
            complete_switch(n);
            return 0;
        }
        spin_unlock_irqrestore(&vt_lock, f);
        return arg == VT_ACKACQ ? 0 : -EINVAL;
    }
    case 0x5606:                                                                   /* VT_ACTIVATE */
        if (arg < 1 || arg > NR_VT) return -ENXIO;
        vt_activate((int)arg - 1);
        return 0;
    case 0x5607:                                                                   /* VT_WAITACTIVE */
        if (arg < 1 || arg > NR_VT) return -ENXIO;
        return wait_until(&vt_wq, fg == (int)arg - 1);
    case 0x5608:                                                                   /* VT_DISALLOCATE */
        if (arg > NR_VT) return -ENXIO;
        if (arg && (vts[arg - 1].opens || (int)arg - 1 == fg || arg == 1)) return -EBUSY;
        return 0;
    case 0x560B: case 0x560C:                                                      /* VT_LOCKSWITCH, VT_UNLOCKSWITCH */
        if (!is_root()) return -EPERM;
        switch_locked = cmd == 0x560B;
        return 0;
    }
    return -ENOTTY;
}

/* /dev/vcs (foreground) and /dev/vcsN */
static ssize_t vcs_read(struct file *f, void *buf, size_t n, off_t *off) {
    int minor = (int)(uintptr_t)f->priv;
    char kb[512];
    size_t got = fbcon_vcs_read(minor - 1, kb, MIN(n, sizeof kb), (uint64_t)*off);
    if ((vaddr_t)buf >= USER_TOP) memcpy(buf, kb, got);
    else if (copy_to_user(buf, kb, got)) return -EFAULT;
    *off += got;
    return got;
}
static int vcs_open(struct inode *ino, struct file *f) {
    unsigned mi = MINOR(ino->rdev);
    if (mi > NR_VT) return -ENXIO;
    f->priv = (void *)(uintptr_t)mi;
    return 0;
}
static unsigned vcs_poll(struct file *f) { return POLLIN | POLLRDNORM; }
static const struct file_ops vcs_ops = { .nobkl = true, .open = vcs_open, .read = vcs_read, .poll = vcs_poll };

static int a_active(struct device *d, char *b, size_t n) { return dev_printf(b, n, "tty%d\n", fg + 1); }
const struct dev_attr vt_tty0_attrs[] = { { .name = "active", .mode = 0444, .show = a_active }, { 0 } };

void vt_register_devices(void) {
    wait_queue_init(&vt_wq);
    wait_queue_init(&kbd_wq);
    for (int i = 0; i < NR_VT; i++) {
        if (i) { tty_init_struct(&vt_ttys[i]); vt_ttys[i].output = vt_out; }
        vts[i].kbmode = K_XLATE;
    }
    for (int i = 0; i <= NR_VT; i++) chrdev_register(7, i, &vcs_ops);
}

void vt_late_init(void) { thread_create("vtswitch", vtswitch_thread, nullptr); }
