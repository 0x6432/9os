/* setfacl [-bkndR?] [{-m|-x} acl_spec] [--set acl_spec] FILE...: modify POSIX ACLs.
 * acl_spec: comma-separated [d[efault]:]{u[ser]|g[roup]|m[ask]|o[ther]}:[name|id][:perms],
 * perms rwxX- letters or one octal digit (X: x if a directory or some x bit is already set). */
#include "acl_common.h"

static int is_dir;
static mode_t cur_mode;

static int parse_perm(const char *s, unsigned *p) {
    *p = 0;
    if (s[0] >= '0' && s[0] <= '7' && !s[1]) { *p = (unsigned)(s[0] - '0'); return 0; }
    for (; *s; s++)
        switch (*s) {
        case 'r': *p |= 4; break;
        case 'w': *p |= 2; break;
        case 'x': *p |= 1; break;
        case 'X': if (is_dir || (cur_mode & 0111)) *p |= 1; break;
        case '-': break;
        default: return -1;
        }
    return 0;
}
static int parse_id(const char *s, int group, uint32_t *id) {
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*s && !*end) { *id = (uint32_t)v; return 0; }
    if (group) { struct group *g = getgrnam(s); if (!g) return -1; *id = g->gr_gid; }
    else { struct passwd *p = getpwnam(s); if (!p) return -1; *id = p->pw_uid; }
    return 0;
}
/* one entry: returns 0, *def = default ACL, need_perm: -m/--set */
static int parse_ent(char *s, struct ent *e, int *def, int need_perm) {
    *def = 0;
    if (!strncmp(s, "default:", 8)) { *def = 1; s += 8; }
    else if (!strncmp(s, "d:", 2)) { *def = 1; s += 2; }
    char *f[3] = { s, NULL, NULL };
    int nf = 1;
    for (char *c = s; *c && nf < 3; c++) if (*c == ':') { *c = 0; f[nf++] = c + 1; }
    const char *t = f[0], *q = nf > 1 ? f[1] : "", *ps = nf > 2 ? f[2] : NULL;
    int tag;
    if (!strcmp(t, "u") || !strcmp(t, "user")) tag = U;
    else if (!strcmp(t, "g") || !strcmp(t, "group")) tag = G;
    else if (!strcmp(t, "m") || !strcmp(t, "mask")) tag = M;
    else if (!strcmp(t, "o") || !strcmp(t, "other")) tag = O;
    else return -1;
    if ((tag == M || tag == O) && nf == 2 && need_perm) { ps = q; q = ""; }   /* m:rwx, o:r */
    e->id = (uint32_t)-1;
    if (tag == U || tag == G) {
        if (!*q) tag = tag == U ? UO : GO;
        else if (parse_id(q, tag == G, &e->id)) return -1;
    } else if (*q) return -1;
    e->tag = (uint16_t)tag;
    unsigned p = 0;
    if (need_perm) { if (!ps || parse_perm(ps, &p)) return -1; }
    e->perm = (uint16_t)p;
    return 0;
}
static void recalc_mask(struct acl *a) {
    int named = 0; unsigned m = 0;
    for (int k = 0; k < a->n; k++) {
        if (a->e[k].tag == U || a->e[k].tag == G) named = 1;
        if (a->e[k].tag == U || a->e[k].tag == G || a->e[k].tag == GO) m |= a->e[k].perm;
    }
    struct ent *me = acl_find(a, M, 0);
    if (!named && !me) return;
    if (!me) { me = &a->e[a->n++]; me->tag = M; me->id = (uint32_t)-1; }
    me->perm = (uint16_t)m;
}
static void put(struct acl *a, const struct ent *e) {
    struct ent *x = acl_find(a, e->tag, e->id);
    if (x) x->perm = e->perm;
    else if (a->n < MAXE) a->e[a->n++] = *e;
}
static void del(struct acl *a, const struct ent *e) {
    struct ent *x = acl_find(a, e->tag, e->id);
    if (x) *x = a->e[--a->n];
}

int main(int argc, char **argv) {
    enum { MAXOP = 32 };
    struct { char op; char *spec; } ops[MAXOP];
    int nops = 0, opt_b = 0, opt_k = 0, opt_n = 0, opt_d = 0, rc = 0, i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "--set") || !strcmp(a, "-m") || !strcmp(a, "-x")) {
            if (i + 1 >= argc || nops == MAXOP) goto usage;
            ops[nops].op = a[1] == '-' ? 's' : a[1]; ops[nops++].spec = argv[++i];
            continue;
        }
        for (char *c = a + 1; *c; c++)
            switch (*c) {
            case 'b': opt_b = 1; break;
            case 'k': opt_k = 1; break;
            case 'n': opt_n = 1; break;
            case 'd': opt_d = 1; break;
            default: goto usage;
            }
    }
    if (i >= argc || (!nops && !opt_b && !opt_k)) goto usage;
    for (; i < argc; i++) {
        const char *p = argv[i];
        struct stat st;
        if (stat(p, &st)) { fprintf(stderr, "setfacl: %s: %s\n", p, strerror(errno)); rc = 1; continue; }
        is_dir = S_ISDIR(st.st_mode); cur_mode = st.st_mode;
        struct acl acc, def;
        if (acl_read(p, ACC, &acc) < 0 || acl_read(p, DEF, &def) < 0) { fprintf(stderr, "setfacl: %s: %s\n", p, strerror(errno)); rc = 1; continue; }
        if (!acc.n) acl_from_mode(&acc, st.st_mode);
        int acc_ch = 0, def_ch = 0, acc_mask = 0, def_mask = 0, bad = 0;
        if (opt_b) {                      /* keep only the base entries (group = owning group entry) */
            struct acl b; b.n = 0;
            for (int k = 0; k < acc.n; k++) if (acc.e[k].tag == UO || acc.e[k].tag == GO || acc.e[k].tag == O) b.e[b.n++] = acc.e[k];
            acc = b; acc_ch = 1; def.n = 0; def_ch = 1;
        }
        if (opt_k) { def.n = 0; def_ch = 1; }
        for (int o = 0; o < nops && !bad; o++) {
            char *spec = strdup(ops[o].spec), *save = NULL;
            if (ops[o].op == 's') {
                struct acl na = { 0 }, nd = { 0 };
                for (char *tok = strtok_r(spec, ",\n", &save); tok; tok = strtok_r(NULL, ",\n", &save)) {
                    struct ent e; int d;
                    if (parse_ent(tok, &e, &d, 1)) { bad = 1; break; }
                    if (d || opt_d) { put(&nd, &e); def_mask |= e.tag == M; } else { put(&na, &e); acc_mask |= e.tag == M; }
                }
                acc = na; acc_ch = 1;
                if (nd.n || is_dir) { def = nd; def_ch = 1; }
            } else {
                for (char *tok = strtok_r(spec, ",\n", &save); tok; tok = strtok_r(NULL, ",\n", &save)) {
                    struct ent e; int d;
                    if (parse_ent(tok, &e, &d, ops[o].op == 'm')) { bad = 1; break; }
                    d |= opt_d;
                    struct acl *t = d ? &def : &acc;
                    if (d && !def.n && ops[o].op == 'm') {          /* seed the default ACL from the access ACL */
                        for (int k = 0; k < acc.n; k++) if (acc.e[k].tag == UO || acc.e[k].tag == GO || acc.e[k].tag == O) def.e[def.n++] = acc.e[k];
                    }
                    if (ops[o].op == 'm') put(t, &e); else del(t, &e);
                    if (e.tag == M) { if (d) def_mask = 1; else acc_mask = 1; }
                    if (d) def_ch = 1; else acc_ch = 1;
                }
            }
            free(spec);
        }
        if (bad) { fprintf(stderr, "setfacl: %s: invalid ACL specification\n", p); rc = 2; continue; }
        if (!opt_n && acc_ch && !acc_mask) recalc_mask(&acc);
        if (!opt_n && def_ch && !def_mask && def.n) recalc_mask(&def);
        if (def_ch && def.n && !is_dir) { fprintf(stderr, "setfacl: %s: only directories can have default ACLs\n", p); rc = 1; continue; }
        if ((acc_ch && acl_write(p, ACC, &acc)) || (def_ch && is_dir && acl_write(p, DEF, &def))) {
            fprintf(stderr, "setfacl: %s: %s\n", p, strerror(errno)); rc = 1;
        }
    }
    return rc;
usage:
    fprintf(stderr, "usage: setfacl [-bkdn] [{-m|-x} acl_spec] [--set acl_spec] file...\n");
    return 2;
}
