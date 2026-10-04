#include <kernel/printk.h>
#include <kernel/string.h>

struct out { char *buf; size_t size, pos; };
static void put(struct out *o, char c) {
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}

static void put_num(struct out *o, uint64_t v, int base, bool upper, int width, bool zero,
                    bool left, bool neg, bool plus) {
    char tmp[24]; int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = digits[v % base]; v /= base; } while (v);
    int len = n + (neg || plus);
    if (!left && !zero) for (; len < width; width--) put(o, ' ');
    if (neg) put(o, '-'); else if (plus) put(o, '+');
    if (!left && zero) for (; len < width; width--) put(o, '0');
    while (n) put(o, tmp[--n]);
    if (left) for (; len < width; width--) put(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        bool zero = false, left = false, plus = false;
        for (;; fmt++) {
            if (*fmt == '0') zero = true;
            else if (*fmt == '-') left = true;
            else if (*fmt == '+') plus = true;
            else break;
        }
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lng = 2; fmt++; }
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, false, width, zero, left, v < 0, plus);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            int base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
            put_num(&o, v, base, *fmt == 'X', width, zero, left, false, false);
            break;
        }
        case 'p':
            put(&o, '0'); put(&o, 'x');
            put_num(&o, (uintptr_t)va_arg(ap, void *), 16, false, 16, true, false, false, false);
            break;
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t l = prec >= 0 ? strnlen(s, prec) : strlen(s);
            if (!left) for (size_t i = l; (int)i < width; i++) put(&o, ' ');
            for (size_t i = 0; i < l; i++) put(&o, s[i]);
            if (left) for (size_t i = l; (int)i < width; i++) put(&o, ' ');
            break;
        }
        case '%': put(&o, '%'); break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (size) buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
