/* System call dispatch (Linux ABI). Generated from syscall.c.in by scripts/gen-syscalls.py - edit the template. */
#include <kernel/syscall.h>
#include <kernel/printk.h>
#include <kernel/time.h>
#include <kernel/string.h>
#include <arch/syscall.h>

typedef int64_t (*syscall_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#pragma clang diagnostic ignored "-Wdeprecated-non-prototype"
#pragma clang diagnostic ignored "-Wstrict-prototypes"
int64_t sys_accept();
int64_t sys_accept4();
int64_t sys_access();
int64_t sys_alarm();
int64_t sys_arch_prctl_wrap();
int64_t sys_bind();
int64_t sys_brk();
int64_t sys_capget();
int64_t sys_chdir();
int64_t sys_chmod();
int64_t sys_chown();
int64_t sys_chroot();
int64_t sys_clock_getres();
int64_t sys_clock_gettime();
int64_t sys_clock_nanosleep();
int64_t sys_clone();
int64_t sys_close();
int64_t sys_close_range();
int64_t sys_connect();
int64_t sys_creat();
int64_t sys_dup();
int64_t sys_dup2();
int64_t sys_dup3();
int64_t sys_epoll_create();
int64_t sys_epoll_create1();
int64_t sys_epoll_ctl();
int64_t sys_epoll_pwait();
int64_t sys_epoll_pwait2();
int64_t sys_epoll_wait();
int64_t sys_eventfd();
int64_t sys_eventfd2();
int64_t sys_execve();
int64_t sys_exit();
int64_t sys_exit_group();
int64_t sys_faccessat();
int64_t sys_faccessat2();
int64_t sys_fchdir();
int64_t sys_fchmod();
int64_t sys_fchmodat();
int64_t sys_fchown();
int64_t sys_fchownat();
int64_t sys_fcntl();
int64_t sys_fork();
int64_t sys_fstat();
int64_t sys_fstatfs();
int64_t sys_fsync();
int64_t sys_ftruncate();
int64_t sys_futex();
int64_t sys_getcpu();
int64_t sys_getcwd();
int64_t sys_getdents();
int64_t sys_getdents64();
int64_t sys_getegid();
int64_t sys_geteuid();
int64_t sys_getgid();
int64_t sys_getgroups();
int64_t sys_getitimer();
int64_t sys_getpeername();
int64_t sys_getpgid();
int64_t sys_getpgrp();
int64_t sys_getpid();
int64_t sys_getppid();
int64_t sys_getpriority();
int64_t sys_getrandom();
int64_t sys_getresgid();
int64_t sys_getresuid();
int64_t sys_getrlimit();
int64_t sys_getrusage();
int64_t sys_getsid();
int64_t sys_getsockname();
int64_t sys_getsockopt();
int64_t sys_gettid();
int64_t sys_gettimeofday();
int64_t sys_getuid();
int64_t sys_inotify_add_watch();
int64_t sys_inotify_init();
int64_t sys_inotify_init1();
int64_t sys_inotify_rm_watch();
int64_t sys_ioctl();
int64_t sys_kill();
int64_t sys_lchown();
int64_t sys_link();
int64_t sys_linkat();
int64_t sys_listen();
int64_t sys_lseek();
int64_t sys_lstat();
int64_t sys_madvise();
int64_t sys_membarrier();
int64_t sys_memfd_create();
int64_t sys_mkdir();
int64_t sys_mkdirat();
int64_t sys_mknod();
int64_t sys_mknodat();
int64_t sys_mmap();
int64_t sys_mprotect();
int64_t sys_mremap();
int64_t sys_munmap();
int64_t sys_nanosleep();
int64_t sys_newfstatat();
int64_t sys_open();
int64_t sys_openat();
int64_t sys_pause();
int64_t sys_personality();
int64_t sys_pipe();
int64_t sys_pipe2();
int64_t sys_poll();
int64_t sys_ppoll();
int64_t sys_prctl();
int64_t sys_pread64();
int64_t sys_prlimit64();
int64_t sys_pselect6();
int64_t sys_pwrite64();
int64_t sys_read();
int64_t sys_readlink();
int64_t sys_readlinkat();
int64_t sys_readv();
int64_t sys_reboot();
int64_t sys_recvfrom();
int64_t sys_recvmsg();
int64_t sys_rename();
int64_t sys_renameat();
int64_t sys_renameat2();
int64_t sys_riscv_flush_icache();
int64_t sys_rmdir();
int64_t sys_rseq();
int64_t sys_rt_sigaction();
int64_t sys_rt_sigpending();
int64_t sys_rt_sigprocmask();
int64_t sys_rt_sigreturn();
int64_t sys_rt_sigsuspend();
int64_t sys_rt_sigtimedwait();
int64_t sys_sched_get_priority_max();
int64_t sys_sched_get_priority_min();
int64_t sys_sched_getaffinity();
int64_t sys_sched_getparam();
int64_t sys_sched_getscheduler();
int64_t sys_sched_rr_get_interval();
int64_t sys_sched_setaffinity();
int64_t sys_sched_setparam();
int64_t sys_sched_setscheduler();
int64_t sys_sched_yield();
int64_t sys_select();
int64_t sys_sendfile();
int64_t sys_sendmsg();
int64_t sys_sendto();
int64_t sys_set_robust_list();
int64_t sys_set_tid_address();
int64_t sys_setfsgid();
int64_t sys_setfsuid();
int64_t sys_setgid();
int64_t sys_setgroups();
int64_t sys_sethostname();
int64_t sys_setitimer();
int64_t sys_setpgid();
int64_t sys_setpriority();
int64_t sys_setregid();
int64_t sys_setresgid();
int64_t sys_setresuid();
int64_t sys_setreuid();
int64_t sys_setrlimit();
int64_t sys_setsid();
int64_t sys_setsockopt();
int64_t sys_setuid();
int64_t sys_shutdown();
int64_t sys_sigaltstack();
int64_t sys_signalfd();
int64_t sys_signalfd4();
int64_t sys_socket();
int64_t sys_socketpair();
int64_t sys_stat();
int64_t sys_statfs();
int64_t sys_statx();
int64_t sys_symlink();
int64_t sys_symlinkat();
int64_t sys_sync();
int64_t sys_sysinfo();
int64_t sys_syslog();
int64_t sys_tgkill();
int64_t sys_time();
int64_t sys_timerfd_create();
int64_t sys_timerfd_gettime();
int64_t sys_timerfd_settime();
int64_t sys_times();
int64_t sys_tkill();
int64_t sys_truncate();
int64_t sys_umask();
int64_t sys_uname();
int64_t sys_unlink();
int64_t sys_unlinkat();
int64_t sys_utimensat();
int64_t sys_vfork();
int64_t sys_wait4();
int64_t sys_waitid();
int64_t sys_write();
int64_t sys_writev();
int64_t sys_zero();

static const syscall_fn syscall_table[NR_SYSCALLS] = {
#ifdef __NR_getcwd
    [__NR_getcwd] = (syscall_fn)sys_getcwd,
#endif
#ifdef __NR_eventfd2
    [__NR_eventfd2] = (syscall_fn)sys_eventfd2,
#endif
#ifdef __NR_epoll_create1
    [__NR_epoll_create1] = (syscall_fn)sys_epoll_create1,
#endif
#ifdef __NR_epoll_ctl
    [__NR_epoll_ctl] = (syscall_fn)sys_epoll_ctl,
#endif
#ifdef __NR_epoll_pwait
    [__NR_epoll_pwait] = (syscall_fn)sys_epoll_pwait,
#endif
#ifdef __NR_dup
    [__NR_dup] = (syscall_fn)sys_dup,
#endif
#ifdef __NR_dup3
    [__NR_dup3] = (syscall_fn)sys_dup3,
#endif
#ifdef __NR_fcntl
    [__NR_fcntl] = (syscall_fn)sys_fcntl,
#endif
#ifdef __NR_inotify_init1
    [__NR_inotify_init1] = (syscall_fn)sys_inotify_init1,
#endif
#ifdef __NR_inotify_add_watch
    [__NR_inotify_add_watch] = (syscall_fn)sys_inotify_add_watch,
#endif
#ifdef __NR_inotify_rm_watch
    [__NR_inotify_rm_watch] = (syscall_fn)sys_inotify_rm_watch,
#endif
#ifdef __NR_ioctl
    [__NR_ioctl] = (syscall_fn)sys_ioctl,
#endif
#ifdef __NR_flock
    [__NR_flock] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_mknodat
    [__NR_mknodat] = (syscall_fn)sys_mknodat,
#endif
#ifdef __NR_mkdirat
    [__NR_mkdirat] = (syscall_fn)sys_mkdirat,
#endif
#ifdef __NR_unlinkat
    [__NR_unlinkat] = (syscall_fn)sys_unlinkat,
#endif
#ifdef __NR_symlinkat
    [__NR_symlinkat] = (syscall_fn)sys_symlinkat,
#endif
#ifdef __NR_linkat
    [__NR_linkat] = (syscall_fn)sys_linkat,
#endif
#ifdef __NR_renameat
    [__NR_renameat] = (syscall_fn)sys_renameat,
#endif
#ifdef __NR_umount2
    [__NR_umount2] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_statfs
    [__NR_statfs] = (syscall_fn)sys_statfs,
#endif
#ifdef __NR_fstatfs
    [__NR_fstatfs] = (syscall_fn)sys_fstatfs,
#endif
#ifdef __NR_truncate
    [__NR_truncate] = (syscall_fn)sys_truncate,
#endif
#ifdef __NR_ftruncate
    [__NR_ftruncate] = (syscall_fn)sys_ftruncate,
#endif
#ifdef __NR_fallocate
    [__NR_fallocate] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_faccessat
    [__NR_faccessat] = (syscall_fn)sys_faccessat,
#endif
#ifdef __NR_chdir
    [__NR_chdir] = (syscall_fn)sys_chdir,
#endif
#ifdef __NR_fchdir
    [__NR_fchdir] = (syscall_fn)sys_fchdir,
#endif
#ifdef __NR_chroot
    [__NR_chroot] = (syscall_fn)sys_chroot,
#endif
#ifdef __NR_fchmod
    [__NR_fchmod] = (syscall_fn)sys_fchmod,
#endif
#ifdef __NR_fchmodat
    [__NR_fchmodat] = (syscall_fn)sys_fchmodat,
#endif
#ifdef __NR_fchownat
    [__NR_fchownat] = (syscall_fn)sys_fchownat,
#endif
#ifdef __NR_fchown
    [__NR_fchown] = (syscall_fn)sys_fchown,
#endif
#ifdef __NR_openat
    [__NR_openat] = (syscall_fn)sys_openat,
#endif
#ifdef __NR_close
    [__NR_close] = (syscall_fn)sys_close,
#endif
#ifdef __NR_pipe2
    [__NR_pipe2] = (syscall_fn)sys_pipe2,
#endif
#ifdef __NR_getdents64
    [__NR_getdents64] = (syscall_fn)sys_getdents64,
#endif
#ifdef __NR_lseek
    [__NR_lseek] = (syscall_fn)sys_lseek,
#endif
#ifdef __NR_read
    [__NR_read] = (syscall_fn)sys_read,
#endif
#ifdef __NR_write
    [__NR_write] = (syscall_fn)sys_write,
#endif
#ifdef __NR_readv
    [__NR_readv] = (syscall_fn)sys_readv,
#endif
#ifdef __NR_writev
    [__NR_writev] = (syscall_fn)sys_writev,
#endif
#ifdef __NR_pread64
    [__NR_pread64] = (syscall_fn)sys_pread64,
#endif
#ifdef __NR_pwrite64
    [__NR_pwrite64] = (syscall_fn)sys_pwrite64,
#endif
#ifdef __NR_sendfile
    [__NR_sendfile] = (syscall_fn)sys_sendfile,
#endif
#ifdef __NR_pselect6
    [__NR_pselect6] = (syscall_fn)sys_pselect6,
#endif
#ifdef __NR_ppoll
    [__NR_ppoll] = (syscall_fn)sys_ppoll,
#endif
#ifdef __NR_signalfd4
    [__NR_signalfd4] = (syscall_fn)sys_signalfd4,
#endif
#ifdef __NR_readlinkat
    [__NR_readlinkat] = (syscall_fn)sys_readlinkat,
#endif
#ifdef __NR_newfstatat
    [__NR_newfstatat] = (syscall_fn)sys_newfstatat,
#endif
#ifdef __NR_fstat
    [__NR_fstat] = (syscall_fn)sys_fstat,
#endif
#ifdef __NR_sync
    [__NR_sync] = (syscall_fn)sys_sync,
#endif
#ifdef __NR_fsync
    [__NR_fsync] = (syscall_fn)sys_fsync,
#endif
#ifdef __NR_fdatasync
    [__NR_fdatasync] = (syscall_fn)sys_fsync,
#endif
#ifdef __NR_timerfd_create
    [__NR_timerfd_create] = (syscall_fn)sys_timerfd_create,
#endif
#ifdef __NR_timerfd_settime
    [__NR_timerfd_settime] = (syscall_fn)sys_timerfd_settime,
#endif
#ifdef __NR_timerfd_gettime
    [__NR_timerfd_gettime] = (syscall_fn)sys_timerfd_gettime,
#endif
#ifdef __NR_utimensat
    [__NR_utimensat] = (syscall_fn)sys_utimensat,
#endif
#ifdef __NR_capget
    [__NR_capget] = (syscall_fn)sys_capget,
#endif
#ifdef __NR_capset
    [__NR_capset] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_personality
    [__NR_personality] = (syscall_fn)sys_personality,
#endif
#ifdef __NR_exit
    [__NR_exit] = (syscall_fn)sys_exit,
#endif
#ifdef __NR_exit_group
    [__NR_exit_group] = (syscall_fn)sys_exit_group,
#endif
#ifdef __NR_waitid
    [__NR_waitid] = (syscall_fn)sys_waitid,
#endif
#ifdef __NR_set_tid_address
    [__NR_set_tid_address] = (syscall_fn)sys_set_tid_address,
#endif
#ifdef __NR_futex
    [__NR_futex] = (syscall_fn)sys_futex,
#endif
#ifdef __NR_set_robust_list
    [__NR_set_robust_list] = (syscall_fn)sys_set_robust_list,
#endif
#ifdef __NR_get_robust_list
    [__NR_get_robust_list] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_nanosleep
    [__NR_nanosleep] = (syscall_fn)sys_nanosleep,
#endif
#ifdef __NR_getitimer
    [__NR_getitimer] = (syscall_fn)sys_getitimer,
#endif
#ifdef __NR_setitimer
    [__NR_setitimer] = (syscall_fn)sys_setitimer,
#endif
#ifdef __NR_clock_settime
    [__NR_clock_settime] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_clock_gettime
    [__NR_clock_gettime] = (syscall_fn)sys_clock_gettime,
#endif
#ifdef __NR_clock_getres
    [__NR_clock_getres] = (syscall_fn)sys_clock_getres,
#endif
#ifdef __NR_clock_nanosleep
    [__NR_clock_nanosleep] = (syscall_fn)sys_clock_nanosleep,
#endif
#ifdef __NR_syslog
    [__NR_syslog] = (syscall_fn)sys_syslog,
#endif
#ifdef __NR_sched_setparam
    [__NR_sched_setparam] = (syscall_fn)sys_sched_setparam,
#endif
#ifdef __NR_sched_setscheduler
    [__NR_sched_setscheduler] = (syscall_fn)sys_sched_setscheduler,
#endif
#ifdef __NR_sched_getscheduler
    [__NR_sched_getscheduler] = (syscall_fn)sys_sched_getscheduler,
#endif
#ifdef __NR_sched_getparam
    [__NR_sched_getparam] = (syscall_fn)sys_sched_getparam,
#endif
#ifdef __NR_sched_setaffinity
    [__NR_sched_setaffinity] = (syscall_fn)sys_sched_setaffinity,
#endif
#ifdef __NR_sched_getaffinity
    [__NR_sched_getaffinity] = (syscall_fn)sys_sched_getaffinity,
#endif
#ifdef __NR_sched_yield
    [__NR_sched_yield] = (syscall_fn)sys_sched_yield,
#endif
#ifdef __NR_sched_get_priority_max
    [__NR_sched_get_priority_max] = (syscall_fn)sys_sched_get_priority_max,
#endif
#ifdef __NR_sched_get_priority_min
    [__NR_sched_get_priority_min] = (syscall_fn)sys_sched_get_priority_min,
#endif
#ifdef __NR_sched_rr_get_interval
    [__NR_sched_rr_get_interval] = (syscall_fn)sys_sched_rr_get_interval,
#endif
#ifdef __NR_kill
    [__NR_kill] = (syscall_fn)sys_kill,
#endif
#ifdef __NR_tkill
    [__NR_tkill] = (syscall_fn)sys_tkill,
#endif
#ifdef __NR_tgkill
    [__NR_tgkill] = (syscall_fn)sys_tgkill,
#endif
#ifdef __NR_sigaltstack
    [__NR_sigaltstack] = (syscall_fn)sys_sigaltstack,
#endif
#ifdef __NR_rt_sigsuspend
    [__NR_rt_sigsuspend] = (syscall_fn)sys_rt_sigsuspend,
#endif
#ifdef __NR_rt_sigaction
    [__NR_rt_sigaction] = (syscall_fn)sys_rt_sigaction,
#endif
#ifdef __NR_rt_sigprocmask
    [__NR_rt_sigprocmask] = (syscall_fn)sys_rt_sigprocmask,
#endif
#ifdef __NR_rt_sigpending
    [__NR_rt_sigpending] = (syscall_fn)sys_rt_sigpending,
#endif
#ifdef __NR_rt_sigtimedwait
    [__NR_rt_sigtimedwait] = (syscall_fn)sys_rt_sigtimedwait,
#endif
#ifdef __NR_rt_sigreturn
    [__NR_rt_sigreturn] = (syscall_fn)sys_rt_sigreturn,
#endif
#ifdef __NR_setpriority
    [__NR_setpriority] = (syscall_fn)sys_setpriority,
#endif
#ifdef __NR_getpriority
    [__NR_getpriority] = (syscall_fn)sys_getpriority,
#endif
#ifdef __NR_reboot
    [__NR_reboot] = (syscall_fn)sys_reboot,
#endif
#ifdef __NR_setregid
    [__NR_setregid] = (syscall_fn)sys_setregid,
#endif
#ifdef __NR_setgid
    [__NR_setgid] = (syscall_fn)sys_setgid,
#endif
#ifdef __NR_setreuid
    [__NR_setreuid] = (syscall_fn)sys_setreuid,
#endif
#ifdef __NR_setuid
    [__NR_setuid] = (syscall_fn)sys_setuid,
#endif
#ifdef __NR_setresuid
    [__NR_setresuid] = (syscall_fn)sys_setresuid,
#endif
#ifdef __NR_getresuid
    [__NR_getresuid] = (syscall_fn)sys_getresuid,
#endif
#ifdef __NR_setresgid
    [__NR_setresgid] = (syscall_fn)sys_setresgid,
#endif
#ifdef __NR_getresgid
    [__NR_getresgid] = (syscall_fn)sys_getresgid,
#endif
#ifdef __NR_setfsuid
    [__NR_setfsuid] = (syscall_fn)sys_setfsuid,
#endif
#ifdef __NR_setfsgid
    [__NR_setfsgid] = (syscall_fn)sys_setfsgid,
#endif
#ifdef __NR_times
    [__NR_times] = (syscall_fn)sys_times,
#endif
#ifdef __NR_setpgid
    [__NR_setpgid] = (syscall_fn)sys_setpgid,
#endif
#ifdef __NR_getpgid
    [__NR_getpgid] = (syscall_fn)sys_getpgid,
#endif
#ifdef __NR_getsid
    [__NR_getsid] = (syscall_fn)sys_getsid,
#endif
#ifdef __NR_setsid
    [__NR_setsid] = (syscall_fn)sys_setsid,
#endif
#ifdef __NR_getgroups
    [__NR_getgroups] = (syscall_fn)sys_getgroups,
#endif
#ifdef __NR_setgroups
    [__NR_setgroups] = (syscall_fn)sys_setgroups,
#endif
#ifdef __NR_uname
    [__NR_uname] = (syscall_fn)sys_uname,
#endif
#ifdef __NR_sethostname
    [__NR_sethostname] = (syscall_fn)sys_sethostname,
#endif
#ifdef __NR_getrlimit
    [__NR_getrlimit] = (syscall_fn)sys_getrlimit,
#endif
#ifdef __NR_setrlimit
    [__NR_setrlimit] = (syscall_fn)sys_setrlimit,
#endif
#ifdef __NR_getrusage
    [__NR_getrusage] = (syscall_fn)sys_getrusage,
#endif
#ifdef __NR_umask
    [__NR_umask] = (syscall_fn)sys_umask,
#endif
#ifdef __NR_prctl
    [__NR_prctl] = (syscall_fn)sys_prctl,
#endif
#ifdef __NR_getcpu
    [__NR_getcpu] = (syscall_fn)sys_getcpu,
#endif
#ifdef __NR_gettimeofday
    [__NR_gettimeofday] = (syscall_fn)sys_gettimeofday,
#endif
#ifdef __NR_getpid
    [__NR_getpid] = (syscall_fn)sys_getpid,
#endif
#ifdef __NR_getppid
    [__NR_getppid] = (syscall_fn)sys_getppid,
#endif
#ifdef __NR_getuid
    [__NR_getuid] = (syscall_fn)sys_getuid,
#endif
#ifdef __NR_geteuid
    [__NR_geteuid] = (syscall_fn)sys_geteuid,
#endif
#ifdef __NR_getgid
    [__NR_getgid] = (syscall_fn)sys_getgid,
#endif
#ifdef __NR_getegid
    [__NR_getegid] = (syscall_fn)sys_getegid,
#endif
#ifdef __NR_gettid
    [__NR_gettid] = (syscall_fn)sys_gettid,
#endif
#ifdef __NR_sysinfo
    [__NR_sysinfo] = (syscall_fn)sys_sysinfo,
#endif
#ifdef __NR_socket
    [__NR_socket] = (syscall_fn)sys_socket,
#endif
#ifdef __NR_socketpair
    [__NR_socketpair] = (syscall_fn)sys_socketpair,
#endif
#ifdef __NR_bind
    [__NR_bind] = (syscall_fn)sys_bind,
#endif
#ifdef __NR_listen
    [__NR_listen] = (syscall_fn)sys_listen,
#endif
#ifdef __NR_accept
    [__NR_accept] = (syscall_fn)sys_accept,
#endif
#ifdef __NR_connect
    [__NR_connect] = (syscall_fn)sys_connect,
#endif
#ifdef __NR_getsockname
    [__NR_getsockname] = (syscall_fn)sys_getsockname,
#endif
#ifdef __NR_getpeername
    [__NR_getpeername] = (syscall_fn)sys_getpeername,
#endif
#ifdef __NR_sendto
    [__NR_sendto] = (syscall_fn)sys_sendto,
#endif
#ifdef __NR_recvfrom
    [__NR_recvfrom] = (syscall_fn)sys_recvfrom,
#endif
#ifdef __NR_setsockopt
    [__NR_setsockopt] = (syscall_fn)sys_setsockopt,
#endif
#ifdef __NR_getsockopt
    [__NR_getsockopt] = (syscall_fn)sys_getsockopt,
#endif
#ifdef __NR_shutdown
    [__NR_shutdown] = (syscall_fn)sys_shutdown,
#endif
#ifdef __NR_sendmsg
    [__NR_sendmsg] = (syscall_fn)sys_sendmsg,
#endif
#ifdef __NR_recvmsg
    [__NR_recvmsg] = (syscall_fn)sys_recvmsg,
#endif
#ifdef __NR_brk
    [__NR_brk] = (syscall_fn)sys_brk,
#endif
#ifdef __NR_munmap
    [__NR_munmap] = (syscall_fn)sys_munmap,
#endif
#ifdef __NR_mremap
    [__NR_mremap] = (syscall_fn)sys_mremap,
#endif
#ifdef __NR_clone
    [__NR_clone] = (syscall_fn)sys_clone,
#endif
#ifdef __NR_execve
    [__NR_execve] = (syscall_fn)sys_execve,
#endif
#ifdef __NR_mmap
    [__NR_mmap] = (syscall_fn)sys_mmap,
#endif
#ifdef __NR_fadvise64
    [__NR_fadvise64] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_mprotect
    [__NR_mprotect] = (syscall_fn)sys_mprotect,
#endif
#ifdef __NR_msync
    [__NR_msync] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_mlock
    [__NR_mlock] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_munlock
    [__NR_munlock] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_madvise
    [__NR_madvise] = (syscall_fn)sys_madvise,
#endif
#ifdef __NR_accept4
    [__NR_accept4] = (syscall_fn)sys_accept4,
#endif
#ifdef __NR_wait4
    [__NR_wait4] = (syscall_fn)sys_wait4,
#endif
#ifdef __NR_prlimit64
    [__NR_prlimit64] = (syscall_fn)sys_prlimit64,
#endif
#ifdef __NR_syncfs
    [__NR_syncfs] = (syscall_fn)sys_fsync,
#endif
#ifdef __NR_renameat2
    [__NR_renameat2] = (syscall_fn)sys_renameat2,
#endif
#ifdef __NR_getrandom
    [__NR_getrandom] = (syscall_fn)sys_getrandom,
#endif
#ifdef __NR_memfd_create
    [__NR_memfd_create] = (syscall_fn)sys_memfd_create,
#endif
#ifdef __NR_membarrier
    [__NR_membarrier] = (syscall_fn)sys_membarrier,
#endif
#ifdef __NR_statx
    [__NR_statx] = (syscall_fn)sys_statx,
#endif
#ifdef __NR_rseq
    [__NR_rseq] = (syscall_fn)sys_rseq,
#endif
#ifdef __NR_close_range
    [__NR_close_range] = (syscall_fn)sys_close_range,
#endif
#ifdef __NR_faccessat2
    [__NR_faccessat2] = (syscall_fn)sys_faccessat2,
#endif
#ifdef __NR_epoll_pwait2
    [__NR_epoll_pwait2] = (syscall_fn)sys_epoll_pwait2,
#endif
#ifdef __NR_riscv_flush_icache
    [__NR_riscv_flush_icache] = (syscall_fn)sys_riscv_flush_icache,
#endif
#ifdef __NR_open
    [__NR_open] = (syscall_fn)sys_open,
#endif
#ifdef __NR_stat
    [__NR_stat] = (syscall_fn)sys_stat,
#endif
#ifdef __NR_lstat
    [__NR_lstat] = (syscall_fn)sys_lstat,
#endif
#ifdef __NR_poll
    [__NR_poll] = (syscall_fn)sys_poll,
#endif
#ifdef __NR_access
    [__NR_access] = (syscall_fn)sys_access,
#endif
#ifdef __NR_pipe
    [__NR_pipe] = (syscall_fn)sys_pipe,
#endif
#ifdef __NR_select
    [__NR_select] = (syscall_fn)sys_select,
#endif
#ifdef __NR_dup2
    [__NR_dup2] = (syscall_fn)sys_dup2,
#endif
#ifdef __NR_pause
    [__NR_pause] = (syscall_fn)sys_pause,
#endif
#ifdef __NR_alarm
    [__NR_alarm] = (syscall_fn)sys_alarm,
#endif
#ifdef __NR_fork
    [__NR_fork] = (syscall_fn)sys_fork,
#endif
#ifdef __NR_vfork
    [__NR_vfork] = (syscall_fn)sys_vfork,
#endif
#ifdef __NR_getdents
    [__NR_getdents] = (syscall_fn)sys_getdents,
#endif
#ifdef __NR_rename
    [__NR_rename] = (syscall_fn)sys_rename,
#endif
#ifdef __NR_mkdir
    [__NR_mkdir] = (syscall_fn)sys_mkdir,
#endif
#ifdef __NR_rmdir
    [__NR_rmdir] = (syscall_fn)sys_rmdir,
#endif
#ifdef __NR_creat
    [__NR_creat] = (syscall_fn)sys_creat,
#endif
#ifdef __NR_link
    [__NR_link] = (syscall_fn)sys_link,
#endif
#ifdef __NR_unlink
    [__NR_unlink] = (syscall_fn)sys_unlink,
#endif
#ifdef __NR_symlink
    [__NR_symlink] = (syscall_fn)sys_symlink,
#endif
#ifdef __NR_readlink
    [__NR_readlink] = (syscall_fn)sys_readlink,
#endif
#ifdef __NR_chmod
    [__NR_chmod] = (syscall_fn)sys_chmod,
#endif
#ifdef __NR_chown
    [__NR_chown] = (syscall_fn)sys_chown,
#endif
#ifdef __NR_lchown
    [__NR_lchown] = (syscall_fn)sys_lchown,
#endif
#ifdef __NR_getpgrp
    [__NR_getpgrp] = (syscall_fn)sys_getpgrp,
#endif
#ifdef __NR_utime
    [__NR_utime] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_mknod
    [__NR_mknod] = (syscall_fn)sys_mknod,
#endif
#ifdef __NR_arch_prctl
    [__NR_arch_prctl] = (syscall_fn)sys_arch_prctl_wrap,
#endif
#ifdef __NR_time
    [__NR_time] = (syscall_fn)sys_time,
#endif
#ifdef __NR_utimes
    [__NR_utimes] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_futimesat
    [__NR_futimesat] = (syscall_fn)sys_zero,
#endif
#ifdef __NR_eventfd
    [__NR_eventfd] = (syscall_fn)sys_eventfd,
#endif
#ifdef __NR_epoll_create
    [__NR_epoll_create] = (syscall_fn)sys_epoll_create,
#endif
#ifdef __NR_epoll_wait
    [__NR_epoll_wait] = (syscall_fn)sys_epoll_wait,
#endif
#ifdef __NR_inotify_init
    [__NR_inotify_init] = (syscall_fn)sys_inotify_init,
#endif
#ifdef __NR_signalfd
    [__NR_signalfd] = (syscall_fn)sys_signalfd,
#endif
};

static const char *const syscall_names[NR_SYSCALLS] = {
#ifdef __NR_io_setup
    [__NR_io_setup] = "io_setup",
#endif
#ifdef __NR_io_destroy
    [__NR_io_destroy] = "io_destroy",
#endif
#ifdef __NR_io_submit
    [__NR_io_submit] = "io_submit",
#endif
#ifdef __NR_io_cancel
    [__NR_io_cancel] = "io_cancel",
#endif
#ifdef __NR_io_getevents
    [__NR_io_getevents] = "io_getevents",
#endif
#ifdef __NR_setxattr
    [__NR_setxattr] = "setxattr",
#endif
#ifdef __NR_lsetxattr
    [__NR_lsetxattr] = "lsetxattr",
#endif
#ifdef __NR_fsetxattr
    [__NR_fsetxattr] = "fsetxattr",
#endif
#ifdef __NR_getxattr
    [__NR_getxattr] = "getxattr",
#endif
#ifdef __NR_lgetxattr
    [__NR_lgetxattr] = "lgetxattr",
#endif
#ifdef __NR_fgetxattr
    [__NR_fgetxattr] = "fgetxattr",
#endif
#ifdef __NR_listxattr
    [__NR_listxattr] = "listxattr",
#endif
#ifdef __NR_llistxattr
    [__NR_llistxattr] = "llistxattr",
#endif
#ifdef __NR_flistxattr
    [__NR_flistxattr] = "flistxattr",
#endif
#ifdef __NR_removexattr
    [__NR_removexattr] = "removexattr",
#endif
#ifdef __NR_lremovexattr
    [__NR_lremovexattr] = "lremovexattr",
#endif
#ifdef __NR_fremovexattr
    [__NR_fremovexattr] = "fremovexattr",
#endif
#ifdef __NR_getcwd
    [__NR_getcwd] = "getcwd",
#endif
#ifdef __NR_lookup_dcookie
    [__NR_lookup_dcookie] = "lookup_dcookie",
#endif
#ifdef __NR_eventfd2
    [__NR_eventfd2] = "eventfd2",
#endif
#ifdef __NR_epoll_create1
    [__NR_epoll_create1] = "epoll_create1",
#endif
#ifdef __NR_epoll_ctl
    [__NR_epoll_ctl] = "epoll_ctl",
#endif
#ifdef __NR_epoll_pwait
    [__NR_epoll_pwait] = "epoll_pwait",
#endif
#ifdef __NR_dup
    [__NR_dup] = "dup",
#endif
#ifdef __NR_dup3
    [__NR_dup3] = "dup3",
#endif
#ifdef __NR_fcntl
    [__NR_fcntl] = "fcntl",
#endif
#ifdef __NR_inotify_init1
    [__NR_inotify_init1] = "inotify_init1",
#endif
#ifdef __NR_inotify_add_watch
    [__NR_inotify_add_watch] = "inotify_add_watch",
#endif
#ifdef __NR_inotify_rm_watch
    [__NR_inotify_rm_watch] = "inotify_rm_watch",
#endif
#ifdef __NR_ioctl
    [__NR_ioctl] = "ioctl",
#endif
#ifdef __NR_ioprio_set
    [__NR_ioprio_set] = "ioprio_set",
#endif
#ifdef __NR_ioprio_get
    [__NR_ioprio_get] = "ioprio_get",
#endif
#ifdef __NR_flock
    [__NR_flock] = "flock",
#endif
#ifdef __NR_mknodat
    [__NR_mknodat] = "mknodat",
#endif
#ifdef __NR_mkdirat
    [__NR_mkdirat] = "mkdirat",
#endif
#ifdef __NR_unlinkat
    [__NR_unlinkat] = "unlinkat",
#endif
#ifdef __NR_symlinkat
    [__NR_symlinkat] = "symlinkat",
#endif
#ifdef __NR_linkat
    [__NR_linkat] = "linkat",
#endif
#ifdef __NR_renameat
    [__NR_renameat] = "renameat",
#endif
#ifdef __NR_umount2
    [__NR_umount2] = "umount2",
#endif
#ifdef __NR_mount
    [__NR_mount] = "mount",
#endif
#ifdef __NR_pivot_root
    [__NR_pivot_root] = "pivot_root",
#endif
#ifdef __NR_nfsservctl
    [__NR_nfsservctl] = "nfsservctl",
#endif
#ifdef __NR_statfs
    [__NR_statfs] = "statfs",
#endif
#ifdef __NR_fstatfs
    [__NR_fstatfs] = "fstatfs",
#endif
#ifdef __NR_truncate
    [__NR_truncate] = "truncate",
#endif
#ifdef __NR_ftruncate
    [__NR_ftruncate] = "ftruncate",
#endif
#ifdef __NR_fallocate
    [__NR_fallocate] = "fallocate",
#endif
#ifdef __NR_faccessat
    [__NR_faccessat] = "faccessat",
#endif
#ifdef __NR_chdir
    [__NR_chdir] = "chdir",
#endif
#ifdef __NR_fchdir
    [__NR_fchdir] = "fchdir",
#endif
#ifdef __NR_chroot
    [__NR_chroot] = "chroot",
#endif
#ifdef __NR_fchmod
    [__NR_fchmod] = "fchmod",
#endif
#ifdef __NR_fchmodat
    [__NR_fchmodat] = "fchmodat",
#endif
#ifdef __NR_fchownat
    [__NR_fchownat] = "fchownat",
#endif
#ifdef __NR_fchown
    [__NR_fchown] = "fchown",
#endif
#ifdef __NR_openat
    [__NR_openat] = "openat",
#endif
#ifdef __NR_close
    [__NR_close] = "close",
#endif
#ifdef __NR_vhangup
    [__NR_vhangup] = "vhangup",
#endif
#ifdef __NR_pipe2
    [__NR_pipe2] = "pipe2",
#endif
#ifdef __NR_quotactl
    [__NR_quotactl] = "quotactl",
#endif
#ifdef __NR_getdents64
    [__NR_getdents64] = "getdents64",
#endif
#ifdef __NR_lseek
    [__NR_lseek] = "lseek",
#endif
#ifdef __NR_read
    [__NR_read] = "read",
#endif
#ifdef __NR_write
    [__NR_write] = "write",
#endif
#ifdef __NR_readv
    [__NR_readv] = "readv",
#endif
#ifdef __NR_writev
    [__NR_writev] = "writev",
#endif
#ifdef __NR_pread64
    [__NR_pread64] = "pread64",
#endif
#ifdef __NR_pwrite64
    [__NR_pwrite64] = "pwrite64",
#endif
#ifdef __NR_preadv
    [__NR_preadv] = "preadv",
#endif
#ifdef __NR_pwritev
    [__NR_pwritev] = "pwritev",
#endif
#ifdef __NR_sendfile
    [__NR_sendfile] = "sendfile",
#endif
#ifdef __NR_pselect6
    [__NR_pselect6] = "pselect6",
#endif
#ifdef __NR_ppoll
    [__NR_ppoll] = "ppoll",
#endif
#ifdef __NR_signalfd4
    [__NR_signalfd4] = "signalfd4",
#endif
#ifdef __NR_vmsplice
    [__NR_vmsplice] = "vmsplice",
#endif
#ifdef __NR_splice
    [__NR_splice] = "splice",
#endif
#ifdef __NR_tee
    [__NR_tee] = "tee",
#endif
#ifdef __NR_readlinkat
    [__NR_readlinkat] = "readlinkat",
#endif
#ifdef __NR_newfstatat
    [__NR_newfstatat] = "newfstatat",
#endif
#ifdef __NR_fstat
    [__NR_fstat] = "fstat",
#endif
#ifdef __NR_sync
    [__NR_sync] = "sync",
#endif
#ifdef __NR_fsync
    [__NR_fsync] = "fsync",
#endif
#ifdef __NR_fdatasync
    [__NR_fdatasync] = "fdatasync",
#endif
#ifdef __NR_sync_file_range2
    [__NR_sync_file_range2] = "sync_file_range2",
#endif
#ifdef __NR_sync_file_range
    [__NR_sync_file_range] = "sync_file_range",
#endif
#ifdef __NR_timerfd_create
    [__NR_timerfd_create] = "timerfd_create",
#endif
#ifdef __NR_timerfd_settime
    [__NR_timerfd_settime] = "timerfd_settime",
#endif
#ifdef __NR_timerfd_gettime
    [__NR_timerfd_gettime] = "timerfd_gettime",
#endif
#ifdef __NR_utimensat
    [__NR_utimensat] = "utimensat",
#endif
#ifdef __NR_acct
    [__NR_acct] = "acct",
#endif
#ifdef __NR_capget
    [__NR_capget] = "capget",
#endif
#ifdef __NR_capset
    [__NR_capset] = "capset",
#endif
#ifdef __NR_personality
    [__NR_personality] = "personality",
#endif
#ifdef __NR_exit
    [__NR_exit] = "exit",
#endif
#ifdef __NR_exit_group
    [__NR_exit_group] = "exit_group",
#endif
#ifdef __NR_waitid
    [__NR_waitid] = "waitid",
#endif
#ifdef __NR_set_tid_address
    [__NR_set_tid_address] = "set_tid_address",
#endif
#ifdef __NR_unshare
    [__NR_unshare] = "unshare",
#endif
#ifdef __NR_futex
    [__NR_futex] = "futex",
#endif
#ifdef __NR_set_robust_list
    [__NR_set_robust_list] = "set_robust_list",
#endif
#ifdef __NR_get_robust_list
    [__NR_get_robust_list] = "get_robust_list",
#endif
#ifdef __NR_nanosleep
    [__NR_nanosleep] = "nanosleep",
#endif
#ifdef __NR_getitimer
    [__NR_getitimer] = "getitimer",
#endif
#ifdef __NR_setitimer
    [__NR_setitimer] = "setitimer",
#endif
#ifdef __NR_kexec_load
    [__NR_kexec_load] = "kexec_load",
#endif
#ifdef __NR_init_module
    [__NR_init_module] = "init_module",
#endif
#ifdef __NR_delete_module
    [__NR_delete_module] = "delete_module",
#endif
#ifdef __NR_timer_create
    [__NR_timer_create] = "timer_create",
#endif
#ifdef __NR_timer_gettime
    [__NR_timer_gettime] = "timer_gettime",
#endif
#ifdef __NR_timer_getoverrun
    [__NR_timer_getoverrun] = "timer_getoverrun",
#endif
#ifdef __NR_timer_settime
    [__NR_timer_settime] = "timer_settime",
#endif
#ifdef __NR_timer_delete
    [__NR_timer_delete] = "timer_delete",
#endif
#ifdef __NR_clock_settime
    [__NR_clock_settime] = "clock_settime",
#endif
#ifdef __NR_clock_gettime
    [__NR_clock_gettime] = "clock_gettime",
#endif
#ifdef __NR_clock_getres
    [__NR_clock_getres] = "clock_getres",
#endif
#ifdef __NR_clock_nanosleep
    [__NR_clock_nanosleep] = "clock_nanosleep",
#endif
#ifdef __NR_syslog
    [__NR_syslog] = "syslog",
#endif
#ifdef __NR_ptrace
    [__NR_ptrace] = "ptrace",
#endif
#ifdef __NR_sched_setparam
    [__NR_sched_setparam] = "sched_setparam",
#endif
#ifdef __NR_sched_setscheduler
    [__NR_sched_setscheduler] = "sched_setscheduler",
#endif
#ifdef __NR_sched_getscheduler
    [__NR_sched_getscheduler] = "sched_getscheduler",
#endif
#ifdef __NR_sched_getparam
    [__NR_sched_getparam] = "sched_getparam",
#endif
#ifdef __NR_sched_setaffinity
    [__NR_sched_setaffinity] = "sched_setaffinity",
#endif
#ifdef __NR_sched_getaffinity
    [__NR_sched_getaffinity] = "sched_getaffinity",
#endif
#ifdef __NR_sched_yield
    [__NR_sched_yield] = "sched_yield",
#endif
#ifdef __NR_sched_get_priority_max
    [__NR_sched_get_priority_max] = "sched_get_priority_max",
#endif
#ifdef __NR_sched_get_priority_min
    [__NR_sched_get_priority_min] = "sched_get_priority_min",
#endif
#ifdef __NR_sched_rr_get_interval
    [__NR_sched_rr_get_interval] = "sched_rr_get_interval",
#endif
#ifdef __NR_restart_syscall
    [__NR_restart_syscall] = "restart_syscall",
#endif
#ifdef __NR_kill
    [__NR_kill] = "kill",
#endif
#ifdef __NR_tkill
    [__NR_tkill] = "tkill",
#endif
#ifdef __NR_tgkill
    [__NR_tgkill] = "tgkill",
#endif
#ifdef __NR_sigaltstack
    [__NR_sigaltstack] = "sigaltstack",
#endif
#ifdef __NR_rt_sigsuspend
    [__NR_rt_sigsuspend] = "rt_sigsuspend",
#endif
#ifdef __NR_rt_sigaction
    [__NR_rt_sigaction] = "rt_sigaction",
#endif
#ifdef __NR_rt_sigprocmask
    [__NR_rt_sigprocmask] = "rt_sigprocmask",
#endif
#ifdef __NR_rt_sigpending
    [__NR_rt_sigpending] = "rt_sigpending",
#endif
#ifdef __NR_rt_sigtimedwait
    [__NR_rt_sigtimedwait] = "rt_sigtimedwait",
#endif
#ifdef __NR_rt_sigqueueinfo
    [__NR_rt_sigqueueinfo] = "rt_sigqueueinfo",
#endif
#ifdef __NR_rt_sigreturn
    [__NR_rt_sigreturn] = "rt_sigreturn",
#endif
#ifdef __NR_setpriority
    [__NR_setpriority] = "setpriority",
#endif
#ifdef __NR_getpriority
    [__NR_getpriority] = "getpriority",
#endif
#ifdef __NR_reboot
    [__NR_reboot] = "reboot",
#endif
#ifdef __NR_setregid
    [__NR_setregid] = "setregid",
#endif
#ifdef __NR_setgid
    [__NR_setgid] = "setgid",
#endif
#ifdef __NR_setreuid
    [__NR_setreuid] = "setreuid",
#endif
#ifdef __NR_setuid
    [__NR_setuid] = "setuid",
#endif
#ifdef __NR_setresuid
    [__NR_setresuid] = "setresuid",
#endif
#ifdef __NR_getresuid
    [__NR_getresuid] = "getresuid",
#endif
#ifdef __NR_setresgid
    [__NR_setresgid] = "setresgid",
#endif
#ifdef __NR_getresgid
    [__NR_getresgid] = "getresgid",
#endif
#ifdef __NR_setfsuid
    [__NR_setfsuid] = "setfsuid",
#endif
#ifdef __NR_setfsgid
    [__NR_setfsgid] = "setfsgid",
#endif
#ifdef __NR_times
    [__NR_times] = "times",
#endif
#ifdef __NR_setpgid
    [__NR_setpgid] = "setpgid",
#endif
#ifdef __NR_getpgid
    [__NR_getpgid] = "getpgid",
#endif
#ifdef __NR_getsid
    [__NR_getsid] = "getsid",
#endif
#ifdef __NR_setsid
    [__NR_setsid] = "setsid",
#endif
#ifdef __NR_getgroups
    [__NR_getgroups] = "getgroups",
#endif
#ifdef __NR_setgroups
    [__NR_setgroups] = "setgroups",
#endif
#ifdef __NR_uname
    [__NR_uname] = "uname",
#endif
#ifdef __NR_sethostname
    [__NR_sethostname] = "sethostname",
#endif
#ifdef __NR_setdomainname
    [__NR_setdomainname] = "setdomainname",
#endif
#ifdef __NR_getrlimit
    [__NR_getrlimit] = "getrlimit",
#endif
#ifdef __NR_setrlimit
    [__NR_setrlimit] = "setrlimit",
#endif
#ifdef __NR_getrusage
    [__NR_getrusage] = "getrusage",
#endif
#ifdef __NR_umask
    [__NR_umask] = "umask",
#endif
#ifdef __NR_prctl
    [__NR_prctl] = "prctl",
#endif
#ifdef __NR_getcpu
    [__NR_getcpu] = "getcpu",
#endif
#ifdef __NR_gettimeofday
    [__NR_gettimeofday] = "gettimeofday",
#endif
#ifdef __NR_settimeofday
    [__NR_settimeofday] = "settimeofday",
#endif
#ifdef __NR_adjtimex
    [__NR_adjtimex] = "adjtimex",
#endif
#ifdef __NR_getpid
    [__NR_getpid] = "getpid",
#endif
#ifdef __NR_getppid
    [__NR_getppid] = "getppid",
#endif
#ifdef __NR_getuid
    [__NR_getuid] = "getuid",
#endif
#ifdef __NR_geteuid
    [__NR_geteuid] = "geteuid",
#endif
#ifdef __NR_getgid
    [__NR_getgid] = "getgid",
#endif
#ifdef __NR_getegid
    [__NR_getegid] = "getegid",
#endif
#ifdef __NR_gettid
    [__NR_gettid] = "gettid",
#endif
#ifdef __NR_sysinfo
    [__NR_sysinfo] = "sysinfo",
#endif
#ifdef __NR_mq_open
    [__NR_mq_open] = "mq_open",
#endif
#ifdef __NR_mq_unlink
    [__NR_mq_unlink] = "mq_unlink",
#endif
#ifdef __NR_mq_timedsend
    [__NR_mq_timedsend] = "mq_timedsend",
#endif
#ifdef __NR_mq_timedreceive
    [__NR_mq_timedreceive] = "mq_timedreceive",
#endif
#ifdef __NR_mq_notify
    [__NR_mq_notify] = "mq_notify",
#endif
#ifdef __NR_mq_getsetattr
    [__NR_mq_getsetattr] = "mq_getsetattr",
#endif
#ifdef __NR_msgget
    [__NR_msgget] = "msgget",
#endif
#ifdef __NR_msgctl
    [__NR_msgctl] = "msgctl",
#endif
#ifdef __NR_msgrcv
    [__NR_msgrcv] = "msgrcv",
#endif
#ifdef __NR_msgsnd
    [__NR_msgsnd] = "msgsnd",
#endif
#ifdef __NR_semget
    [__NR_semget] = "semget",
#endif
#ifdef __NR_semctl
    [__NR_semctl] = "semctl",
#endif
#ifdef __NR_semtimedop
    [__NR_semtimedop] = "semtimedop",
#endif
#ifdef __NR_semop
    [__NR_semop] = "semop",
#endif
#ifdef __NR_shmget
    [__NR_shmget] = "shmget",
#endif
#ifdef __NR_shmctl
    [__NR_shmctl] = "shmctl",
#endif
#ifdef __NR_shmat
    [__NR_shmat] = "shmat",
#endif
#ifdef __NR_shmdt
    [__NR_shmdt] = "shmdt",
#endif
#ifdef __NR_socket
    [__NR_socket] = "socket",
#endif
#ifdef __NR_socketpair
    [__NR_socketpair] = "socketpair",
#endif
#ifdef __NR_bind
    [__NR_bind] = "bind",
#endif
#ifdef __NR_listen
    [__NR_listen] = "listen",
#endif
#ifdef __NR_accept
    [__NR_accept] = "accept",
#endif
#ifdef __NR_connect
    [__NR_connect] = "connect",
#endif
#ifdef __NR_getsockname
    [__NR_getsockname] = "getsockname",
#endif
#ifdef __NR_getpeername
    [__NR_getpeername] = "getpeername",
#endif
#ifdef __NR_sendto
    [__NR_sendto] = "sendto",
#endif
#ifdef __NR_recvfrom
    [__NR_recvfrom] = "recvfrom",
#endif
#ifdef __NR_setsockopt
    [__NR_setsockopt] = "setsockopt",
#endif
#ifdef __NR_getsockopt
    [__NR_getsockopt] = "getsockopt",
#endif
#ifdef __NR_shutdown
    [__NR_shutdown] = "shutdown",
#endif
#ifdef __NR_sendmsg
    [__NR_sendmsg] = "sendmsg",
#endif
#ifdef __NR_recvmsg
    [__NR_recvmsg] = "recvmsg",
#endif
#ifdef __NR_readahead
    [__NR_readahead] = "readahead",
#endif
#ifdef __NR_brk
    [__NR_brk] = "brk",
#endif
#ifdef __NR_munmap
    [__NR_munmap] = "munmap",
#endif
#ifdef __NR_mremap
    [__NR_mremap] = "mremap",
#endif
#ifdef __NR_add_key
    [__NR_add_key] = "add_key",
#endif
#ifdef __NR_request_key
    [__NR_request_key] = "request_key",
#endif
#ifdef __NR_keyctl
    [__NR_keyctl] = "keyctl",
#endif
#ifdef __NR_clone
    [__NR_clone] = "clone",
#endif
#ifdef __NR_execve
    [__NR_execve] = "execve",
#endif
#ifdef __NR_mmap
    [__NR_mmap] = "mmap",
#endif
#ifdef __NR_fadvise64
    [__NR_fadvise64] = "fadvise64",
#endif
#ifdef __NR_swapon
    [__NR_swapon] = "swapon",
#endif
#ifdef __NR_swapoff
    [__NR_swapoff] = "swapoff",
#endif
#ifdef __NR_mprotect
    [__NR_mprotect] = "mprotect",
#endif
#ifdef __NR_msync
    [__NR_msync] = "msync",
#endif
#ifdef __NR_mlock
    [__NR_mlock] = "mlock",
#endif
#ifdef __NR_munlock
    [__NR_munlock] = "munlock",
#endif
#ifdef __NR_mlockall
    [__NR_mlockall] = "mlockall",
#endif
#ifdef __NR_munlockall
    [__NR_munlockall] = "munlockall",
#endif
#ifdef __NR_mincore
    [__NR_mincore] = "mincore",
#endif
#ifdef __NR_madvise
    [__NR_madvise] = "madvise",
#endif
#ifdef __NR_remap_file_pages
    [__NR_remap_file_pages] = "remap_file_pages",
#endif
#ifdef __NR_mbind
    [__NR_mbind] = "mbind",
#endif
#ifdef __NR_get_mempolicy
    [__NR_get_mempolicy] = "get_mempolicy",
#endif
#ifdef __NR_set_mempolicy
    [__NR_set_mempolicy] = "set_mempolicy",
#endif
#ifdef __NR_migrate_pages
    [__NR_migrate_pages] = "migrate_pages",
#endif
#ifdef __NR_move_pages
    [__NR_move_pages] = "move_pages",
#endif
#ifdef __NR_rt_tgsigqueueinfo
    [__NR_rt_tgsigqueueinfo] = "rt_tgsigqueueinfo",
#endif
#ifdef __NR_perf_event_open
    [__NR_perf_event_open] = "perf_event_open",
#endif
#ifdef __NR_accept4
    [__NR_accept4] = "accept4",
#endif
#ifdef __NR_recvmmsg
    [__NR_recvmmsg] = "recvmmsg",
#endif
#ifdef __NR_arch_specific_syscall
    [__NR_arch_specific_syscall] = "arch_specific_syscall",
#endif
#ifdef __NR_wait4
    [__NR_wait4] = "wait4",
#endif
#ifdef __NR_prlimit64
    [__NR_prlimit64] = "prlimit64",
#endif
#ifdef __NR_fanotify_init
    [__NR_fanotify_init] = "fanotify_init",
#endif
#ifdef __NR_fanotify_mark
    [__NR_fanotify_mark] = "fanotify_mark",
#endif
#ifdef __NR_clock_adjtime
    [__NR_clock_adjtime] = "clock_adjtime",
#endif
#ifdef __NR_syncfs
    [__NR_syncfs] = "syncfs",
#endif
#ifdef __NR_setns
    [__NR_setns] = "setns",
#endif
#ifdef __NR_sendmmsg
    [__NR_sendmmsg] = "sendmmsg",
#endif
#ifdef __NR_process_vm_readv
    [__NR_process_vm_readv] = "process_vm_readv",
#endif
#ifdef __NR_process_vm_writev
    [__NR_process_vm_writev] = "process_vm_writev",
#endif
#ifdef __NR_kcmp
    [__NR_kcmp] = "kcmp",
#endif
#ifdef __NR_finit_module
    [__NR_finit_module] = "finit_module",
#endif
#ifdef __NR_sched_setattr
    [__NR_sched_setattr] = "sched_setattr",
#endif
#ifdef __NR_sched_getattr
    [__NR_sched_getattr] = "sched_getattr",
#endif
#ifdef __NR_renameat2
    [__NR_renameat2] = "renameat2",
#endif
#ifdef __NR_seccomp
    [__NR_seccomp] = "seccomp",
#endif
#ifdef __NR_getrandom
    [__NR_getrandom] = "getrandom",
#endif
#ifdef __NR_memfd_create
    [__NR_memfd_create] = "memfd_create",
#endif
#ifdef __NR_bpf
    [__NR_bpf] = "bpf",
#endif
#ifdef __NR_execveat
    [__NR_execveat] = "execveat",
#endif
#ifdef __NR_userfaultfd
    [__NR_userfaultfd] = "userfaultfd",
#endif
#ifdef __NR_membarrier
    [__NR_membarrier] = "membarrier",
#endif
#ifdef __NR_mlock2
    [__NR_mlock2] = "mlock2",
#endif
#ifdef __NR_copy_file_range
    [__NR_copy_file_range] = "copy_file_range",
#endif
#ifdef __NR_preadv2
    [__NR_preadv2] = "preadv2",
#endif
#ifdef __NR_pwritev2
    [__NR_pwritev2] = "pwritev2",
#endif
#ifdef __NR_pkey_mprotect
    [__NR_pkey_mprotect] = "pkey_mprotect",
#endif
#ifdef __NR_pkey_alloc
    [__NR_pkey_alloc] = "pkey_alloc",
#endif
#ifdef __NR_pkey_free
    [__NR_pkey_free] = "pkey_free",
#endif
#ifdef __NR_statx
    [__NR_statx] = "statx",
#endif
#ifdef __NR_io_pgetevents
    [__NR_io_pgetevents] = "io_pgetevents",
#endif
#ifdef __NR_rseq
    [__NR_rseq] = "rseq",
#endif
#ifdef __NR_kexec_file_load
    [__NR_kexec_file_load] = "kexec_file_load",
#endif
#ifdef __NR_clock_gettime64
    [__NR_clock_gettime64] = "clock_gettime64",
#endif
#ifdef __NR_clock_settime64
    [__NR_clock_settime64] = "clock_settime64",
#endif
#ifdef __NR_clock_adjtime64
    [__NR_clock_adjtime64] = "clock_adjtime64",
#endif
#ifdef __NR_timer_gettime64
    [__NR_timer_gettime64] = "timer_gettime64",
#endif
#ifdef __NR_timer_settime64
    [__NR_timer_settime64] = "timer_settime64",
#endif
#ifdef __NR_timerfd_gettime64
    [__NR_timerfd_gettime64] = "timerfd_gettime64",
#endif
#ifdef __NR_timerfd_settime64
    [__NR_timerfd_settime64] = "timerfd_settime64",
#endif
#ifdef __NR_pidfd_send_signal
    [__NR_pidfd_send_signal] = "pidfd_send_signal",
#endif
#ifdef __NR_io_uring_setup
    [__NR_io_uring_setup] = "io_uring_setup",
#endif
#ifdef __NR_io_uring_enter
    [__NR_io_uring_enter] = "io_uring_enter",
#endif
#ifdef __NR_io_uring_register
    [__NR_io_uring_register] = "io_uring_register",
#endif
#ifdef __NR_open_tree
    [__NR_open_tree] = "open_tree",
#endif
#ifdef __NR_move_mount
    [__NR_move_mount] = "move_mount",
#endif
#ifdef __NR_fsopen
    [__NR_fsopen] = "fsopen",
#endif
#ifdef __NR_fsconfig
    [__NR_fsconfig] = "fsconfig",
#endif
#ifdef __NR_fsmount
    [__NR_fsmount] = "fsmount",
#endif
#ifdef __NR_fspick
    [__NR_fspick] = "fspick",
#endif
#ifdef __NR_pidfd_open
    [__NR_pidfd_open] = "pidfd_open",
#endif
#ifdef __NR_clone3
    [__NR_clone3] = "clone3",
#endif
#ifdef __NR_close_range
    [__NR_close_range] = "close_range",
#endif
#ifdef __NR_openat2
    [__NR_openat2] = "openat2",
#endif
#ifdef __NR_pidfd_getfd
    [__NR_pidfd_getfd] = "pidfd_getfd",
#endif
#ifdef __NR_faccessat2
    [__NR_faccessat2] = "faccessat2",
#endif
#ifdef __NR_process_madvise
    [__NR_process_madvise] = "process_madvise",
#endif
#ifdef __NR_epoll_pwait2
    [__NR_epoll_pwait2] = "epoll_pwait2",
#endif
#ifdef __NR_mount_setattr
    [__NR_mount_setattr] = "mount_setattr",
#endif
#ifdef __NR_quotactl_fd
    [__NR_quotactl_fd] = "quotactl_fd",
#endif
#ifdef __NR_landlock_create_ruleset
    [__NR_landlock_create_ruleset] = "landlock_create_ruleset",
#endif
#ifdef __NR_landlock_add_rule
    [__NR_landlock_add_rule] = "landlock_add_rule",
#endif
#ifdef __NR_landlock_restrict_self
    [__NR_landlock_restrict_self] = "landlock_restrict_self",
#endif
#ifdef __NR_memfd_secret
    [__NR_memfd_secret] = "memfd_secret",
#endif
#ifdef __NR_process_mrelease
    [__NR_process_mrelease] = "process_mrelease",
#endif
#ifdef __NR_futex_waitv
    [__NR_futex_waitv] = "futex_waitv",
#endif
#ifdef __NR_set_mempolicy_home_node
    [__NR_set_mempolicy_home_node] = "set_mempolicy_home_node",
#endif
#ifdef __NR_cachestat
    [__NR_cachestat] = "cachestat",
#endif
#ifdef __NR_fchmodat2
    [__NR_fchmodat2] = "fchmodat2",
#endif
#ifdef __NR_map_shadow_stack
    [__NR_map_shadow_stack] = "map_shadow_stack",
#endif
#ifdef __NR_futex_wake
    [__NR_futex_wake] = "futex_wake",
#endif
#ifdef __NR_futex_wait
    [__NR_futex_wait] = "futex_wait",
#endif
#ifdef __NR_futex_requeue
    [__NR_futex_requeue] = "futex_requeue",
#endif
#ifdef __NR_lsm_get_self_attr
    [__NR_lsm_get_self_attr] = "lsm_get_self_attr",
#endif
#ifdef __NR_lsm_set_self_attr
    [__NR_lsm_set_self_attr] = "lsm_set_self_attr",
#endif
#ifdef __NR_lsm_list_modules
    [__NR_lsm_list_modules] = "lsm_list_modules",
#endif
#ifdef __NR_mseal
    [__NR_mseal] = "mseal",
#endif
#ifdef __NR_setxattrat
    [__NR_setxattrat] = "setxattrat",
#endif
#ifdef __NR_getxattrat
    [__NR_getxattrat] = "getxattrat",
#endif
#ifdef __NR_listxattrat
    [__NR_listxattrat] = "listxattrat",
#endif
#ifdef __NR_removexattrat
    [__NR_removexattrat] = "removexattrat",
#endif
#ifdef __NR_open_tree_attr
    [__NR_open_tree_attr] = "open_tree_attr",
#endif
#ifdef __NR_file_getattr
    [__NR_file_getattr] = "file_getattr",
#endif
#ifdef __NR_file_setattr
    [__NR_file_setattr] = "file_setattr",
#endif
#ifdef __NR_riscv_hwprobe
    [__NR_riscv_hwprobe] = "riscv_hwprobe",
#endif
#ifdef __NR_riscv_flush_icache
    [__NR_riscv_flush_icache] = "riscv_flush_icache",
#endif
#ifdef __NR_open
    [__NR_open] = "open",
#endif
#ifdef __NR_stat
    [__NR_stat] = "stat",
#endif
#ifdef __NR_lstat
    [__NR_lstat] = "lstat",
#endif
#ifdef __NR_poll
    [__NR_poll] = "poll",
#endif
#ifdef __NR_access
    [__NR_access] = "access",
#endif
#ifdef __NR_pipe
    [__NR_pipe] = "pipe",
#endif
#ifdef __NR_select
    [__NR_select] = "select",
#endif
#ifdef __NR_dup2
    [__NR_dup2] = "dup2",
#endif
#ifdef __NR_pause
    [__NR_pause] = "pause",
#endif
#ifdef __NR_alarm
    [__NR_alarm] = "alarm",
#endif
#ifdef __NR_fork
    [__NR_fork] = "fork",
#endif
#ifdef __NR_vfork
    [__NR_vfork] = "vfork",
#endif
#ifdef __NR_getdents
    [__NR_getdents] = "getdents",
#endif
#ifdef __NR_rename
    [__NR_rename] = "rename",
#endif
#ifdef __NR_mkdir
    [__NR_mkdir] = "mkdir",
#endif
#ifdef __NR_rmdir
    [__NR_rmdir] = "rmdir",
#endif
#ifdef __NR_creat
    [__NR_creat] = "creat",
#endif
#ifdef __NR_link
    [__NR_link] = "link",
#endif
#ifdef __NR_unlink
    [__NR_unlink] = "unlink",
#endif
#ifdef __NR_symlink
    [__NR_symlink] = "symlink",
#endif
#ifdef __NR_readlink
    [__NR_readlink] = "readlink",
#endif
#ifdef __NR_chmod
    [__NR_chmod] = "chmod",
#endif
#ifdef __NR_chown
    [__NR_chown] = "chown",
#endif
#ifdef __NR_lchown
    [__NR_lchown] = "lchown",
#endif
#ifdef __NR_getpgrp
    [__NR_getpgrp] = "getpgrp",
#endif
#ifdef __NR_utime
    [__NR_utime] = "utime",
#endif
#ifdef __NR_mknod
    [__NR_mknod] = "mknod",
#endif
#ifdef __NR_arch_prctl
    [__NR_arch_prctl] = "arch_prctl",
#endif
#ifdef __NR_time
    [__NR_time] = "time",
#endif
#ifdef __NR_utimes
    [__NR_utimes] = "utimes",
#endif
#ifdef __NR_futimesat
    [__NR_futimesat] = "futimesat",
#endif
#ifdef __NR_eventfd
    [__NR_eventfd] = "eventfd",
#endif
#ifdef __NR_uselib
    [__NR_uselib] = "uselib",
#endif
#ifdef __NR_ustat
    [__NR_ustat] = "ustat",
#endif
#ifdef __NR_sysfs
    [__NR_sysfs] = "sysfs",
#endif
#ifdef __NR_modify_ldt
    [__NR_modify_ldt] = "modify_ldt",
#endif
#ifdef __NR__sysctl
    [__NR__sysctl] = "_sysctl",
#endif
#ifdef __NR_iopl
    [__NR_iopl] = "iopl",
#endif
#ifdef __NR_ioperm
    [__NR_ioperm] = "ioperm",
#endif
#ifdef __NR_create_module
    [__NR_create_module] = "create_module",
#endif
#ifdef __NR_get_kernel_syms
    [__NR_get_kernel_syms] = "get_kernel_syms",
#endif
#ifdef __NR_query_module
    [__NR_query_module] = "query_module",
#endif
#ifdef __NR_getpmsg
    [__NR_getpmsg] = "getpmsg",
#endif
#ifdef __NR_putpmsg
    [__NR_putpmsg] = "putpmsg",
#endif
#ifdef __NR_afs_syscall
    [__NR_afs_syscall] = "afs_syscall",
#endif
#ifdef __NR_tuxcall
    [__NR_tuxcall] = "tuxcall",
#endif
#ifdef __NR_security
    [__NR_security] = "security",
#endif
#ifdef __NR_set_thread_area
    [__NR_set_thread_area] = "set_thread_area",
#endif
#ifdef __NR_get_thread_area
    [__NR_get_thread_area] = "get_thread_area",
#endif
#ifdef __NR_epoll_create
    [__NR_epoll_create] = "epoll_create",
#endif
#ifdef __NR_epoll_ctl_old
    [__NR_epoll_ctl_old] = "epoll_ctl_old",
#endif
#ifdef __NR_epoll_wait_old
    [__NR_epoll_wait_old] = "epoll_wait_old",
#endif
#ifdef __NR_epoll_wait
    [__NR_epoll_wait] = "epoll_wait",
#endif
#ifdef __NR_vserver
    [__NR_vserver] = "vserver",
#endif
#ifdef __NR_inotify_init
    [__NR_inotify_init] = "inotify_init",
#endif
#ifdef __NR_signalfd
    [__NR_signalfd] = "signalfd",
#endif
#ifdef __NR_name_to_handle_at
    [__NR_name_to_handle_at] = "name_to_handle_at",
#endif
#ifdef __NR_open_by_handle_at
    [__NR_open_by_handle_at] = "open_by_handle_at",
#endif
};

bool syscall_trace;
static uint8_t warned[NR_SYSCALLS];

void signal_deliver(struct trap_frame *f);

/* Runs with interrupts off, with or without the BKL; signal work takes it (recursively). */
void user_return_work(struct trap_frame *f) {
    if (need_resched && current && (trap_from_user(f) || sched_kernel_preemptible())) schedule();
    if (!trap_from_user(f) || !current || !current->proc) return;
    struct process *p = current->proc;
    if ((p->alarm_ns && time_ns() >= p->alarm_ns) || signal_pending(current) || current->restore_mask) {
        bkl_enter();
        if (p->alarm_ns && time_ns() >= p->alarm_ns) { p->alarm_ns = 0; signal_send(p, SIGALRM); }
        for (int i = 0; i < 8 && signal_pending(current); i++) signal_deliver(f);
        if (current->restore_mask) { current->sig_mask = current->saved_mask; current->restore_mask = false; }
        bkl_exit();
    }
    if (need_resched) schedule();
}

/*
 * Syscalls that only touch the calling thread, read-mostly globals or the scheduler (which has its
 * own lock) run without the big kernel lock, so they proceed in parallel on all CPUs. Page faults
 * taken while copying to/from user memory still take the BKL in the trap handler.
 */
static const char *const lockfree_names[] = {
    "getpid", "gettid", "getuid", "geteuid", "getgid", "getegid", "getresuid", "getresgid",
    "clock_gettime", "clock_getres", "gettimeofday", "time", "sched_yield", "nanosleep",
    "clock_nanosleep", "uname", "getcpu", "sched_getaffinity", "read", "write", "futex",
    "sendmsg", "recvmsg", "sendto", "recvfrom", "poll", "ppoll", "select", "pselect6",
    "epoll_ctl", "epoll_wait", "epoll_pwait", "epoll_pwait2", "close", "dup", "dup2", "dup3", "fcntl",
    "pread64", "pwrite64", "readv", "writev", "lseek", nullptr,
};
static uint8_t lockfree[NR_SYSCALLS];
static volatile bool lockfree_ready;
static void lockfree_init(void) {
    for (int i = 0; lockfree_names[i]; i++)
        for (int n = 0; n < NR_SYSCALLS; n++)
            if (syscall_names[n] && !strcmp(syscall_names[n], lockfree_names[i])) lockfree[n] = 1;
    __atomic_store_n(&lockfree_ready, true, __ATOMIC_RELEASE);
}
uint64_t syscalls_lockfree, syscalls_locked;   /* /proc/stat-style counters (approximate) */

void syscall_dispatch(struct trap_frame *f) {
    uint64_t nr = SC_NR(f);
    if (nr < NR_SYSCALLS && lockfree[nr] && !syscall_trace) {
        syscalls_lockfree++;
        current->last_syscall = nr;
        SC_SET_RET(f, syscall_table[nr](SC_ARG0(f), SC_ARG1(f), SC_ARG2(f), SC_ARG3(f), SC_ARG4(f), SC_ARG5(f)));
        if (current->nborrow) fd_borrow_release();
        arch_irq_disable();
        user_return_work(f);
        return;
    }
    bkl_enter();
    syscalls_locked++;
    if (!lockfree_ready) lockfree_init();
    current->last_syscall = nr;
    int64_t ret;
    if (nr < NR_SYSCALLS && syscall_table[nr]) {
        ret = syscall_table[nr](SC_ARG0(f), SC_ARG1(f), SC_ARG2(f), SC_ARG3(f), SC_ARG4(f), SC_ARG5(f));
        if (syscall_trace)
            printk("[%d] %s(%lx, %lx, %lx) = %ld\n", current->tid, syscall_names[nr] ? syscall_names[nr] : "?",
                   SC_ARG0(f), SC_ARG1(f), SC_ARG2(f), ret);
    } else {
        if (nr < NR_SYSCALLS && !warned[nr]) {
            warned[nr] = 1;
            pr_warn("unimplemented syscall %lu (%s) from %s\n", nr, syscall_names[nr] ? syscall_names[nr] : "?", current->name);
        }
        ret = -ENOSYS;
    }
    SC_SET_RET(f, ret);
    if (current->nborrow) fd_borrow_release();
    arch_irq_disable();
    user_return_work(f);
    bkl_exit();
}

/* first return to user mode of a new thread (fork/clone): leaves the kernel */
void thread_first_return(struct trap_frame *f) {
    sched_finish_switch();
    user_return_work(f);
    bkl_exit();
}
