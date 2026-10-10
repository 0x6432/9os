/* getfacl [-adnc] FILE...: print POSIX ACLs (acl(5) text form) */
#include "acl_common.h"

static int numeric, omit_header;
static void name_of(int group, uint32_t id, char *out, size_t n) {
    if (!numeric) {
        if (group) { struct group *g = getgrgid(id); if (g) { snprintf(out, n, "%s", g->gr_name); return; } }
        else { struct passwd *p = getpwuid(id); if (p) { snprintf(out, n, "%s", p->pw_name); return; } }
    }
    snprintf(out, n, "%u", id);
}
static void print(struct acl *a, const char *pfx, int dir_default) {
    struct ent *mask = acl_find(a, M, 0);
    (void)dir_default;
    for (int k = 0; k < a->n; k++) {
        struct ent *e = &a->e[k];
        char ps[4], nm[64] = "";
        perm_str(e->perm, ps);
        const char *t = e->tag == UO || e->tag == U ? "user" : e->tag == GO || e->tag == G ? "group" : e->tag == M ? "mask" : "other";
        if (e->tag == U || e->tag == G) name_of(e->tag == G, e->id, nm, sizeof nm);
        printf("%s%s:%s:%s", pfx, t, nm, ps);
        if (mask && (e->tag == U || e->tag == GO || e->tag == G) && (e->perm & ~mask->perm)) {
            char es[4]; perm_str(e->perm & mask->perm, es);
            printf("\t#effective:%s", es);
        }
        putchar('\n');
    }
}
int main(int argc, char **argv) {
    int only_acc = 0, only_def = 0, rc = 0, opt;
    while ((opt = getopt(argc, argv, "adnc")) != -1)
        switch (opt) {
        case 'a': only_acc = 1; break;
        case 'd': only_def = 1; break;
        case 'n': numeric = 1; break;
        case 'c': omit_header = 1; break;
        default: fprintf(stderr, "usage: getfacl [-adnc] file...\n"); return 2;
        }
    if (optind >= argc) { fprintf(stderr, "usage: getfacl [-adnc] file...\n"); return 2; }
    for (int i = optind; i < argc; i++) {
        const char *p = argv[i];
        struct stat st;
        if (stat(p, &st)) { fprintf(stderr, "getfacl: %s: %s\n", p, strerror(errno)); rc = 1; continue; }
        struct acl a, d;
        if (acl_read(p, ACC, &a) < 0 || acl_read(p, DEF, &d) < 0) { fprintf(stderr, "getfacl: %s: %s\n", p, strerror(errno)); rc = 1; continue; }
        if (!a.n) acl_from_mode(&a, st.st_mode);
        if (!omit_header) {
            char u[64], g[64];
            name_of(0, st.st_uid, u, sizeof u); name_of(1, st.st_gid, g, sizeof g);
            printf("# file: %s\n# owner: %s\n# group: %s\n", p[0] == '/' ? p + 1 : p, u, g);
            if (st.st_mode & 07000)
                printf("# flags: %c%c%c\n", st.st_mode & S_ISUID ? 's' : '-', st.st_mode & S_ISGID ? 's' : '-', st.st_mode & S_ISVTX ? 't' : '-');
        }
        if (!only_def) print(&a, "", 0);
        if (!only_acc && d.n) print(&d, only_def ? "" : "default:", 1);
        putchar('\n');
    }
    return rc;
}
