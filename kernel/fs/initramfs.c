/* Unpack a newc cpio archive (the Limine module "initramfs") into the root tmpfs. */
#include <kernel/vfs.h>
#include <kernel/boot.h>
#include <kernel/string.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>

static uint32_t hex8(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        v = (v << 4) | (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    return v;
}

void initramfs_load(void) {
    struct limine_file *m = boot_module("initramfs");
    if (!m) { pr_warn("initramfs: no module\n"); return; }
    const char *p = m->address, *end = p + m->size;
    int files = 0;
    while (p + 110 <= end && !memcmp(p, "070701", 6)) {
        uint32_t mode = hex8(p + 14), fsize = hex8(p + 54), nsize = hex8(p + 94);
        uint32_t rdmaj = hex8(p + 78), rdmin = hex8(p + 86);
        const char *name = p + 110;
        const char *data = (const char *)ALIGN_UP((uintptr_t)(name + nsize), 4);
        if (!strcmp(name, "TRAILER!!!")) break;
        char path[512];
        snprintf(path, sizeof path, "/%s", name[0] == '.' && name[1] == '/' ? name + 2 : name);
        if (strcmp(name, ".") != 0) {
            if (S_ISDIR(mode)) vfs_mknod_at(nullptr, path, mode, 0);
            else if (S_ISLNK(mode)) {
                char *t = kmalloc(fsize + 1);
                memcpy(t, data, fsize); t[fsize] = 0;
                vfs_symlink_at(nullptr, t, path);
                kfree(t);
            } else if (S_ISREG(mode)) {
                struct file *f;
                vfs_mknod_at(nullptr, path, mode, 0);
                if (!vfs_open(path, O_WRONLY, 0, &f)) {
                    vfs_write(f, data, fsize);
                    vfs_close(f);
                }
                files++;
            } else {
                vfs_mknod_at(nullptr, path, mode, MKDEV(rdmaj, rdmin));
            }
        }
        p = (const char *)ALIGN_UP((uintptr_t)(data + fsize), 4);
    }
    pr_info("initramfs: unpacked %d files (%lu KiB)\n", files, m->size >> 10);
}
