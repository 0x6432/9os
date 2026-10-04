#pragma once
#include <kernel/types.h>
#include <kernel/sched.h>

#define NCCS 19
struct termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[NCCS];
};
struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

struct tty {
    struct termios t;
    /* output path: console_write for the console, the master's buffer for a pty slave.
     * may_block is false for echo (input context). Returns bytes accepted. */
    ssize_t (*output)(struct tty *t, const char *s, size_t n, bool may_block);
    void *drv;                 /* driver data (struct pty) */
    bool hup;                  /* the other side went away (pty master closed) */
    struct winsize ws;
    char line[1024]; size_t line_len;
    char rbuf[4096]; size_t rhead, rtail;
    bool eof;
    struct wait_queue rq;
    int pgrp, sid;
};

extern struct tty console_tty;
void tty_input(struct tty *t, char c);        /* from interrupt context */
void tty_input_str(struct tty *t, const char *s);
void tty_init(void);
void tty_init_struct(struct tty *t);
