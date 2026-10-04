#pragma once
#include <kernel/vfs.h>
struct linux_stat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, __pad0;
    uint64_t st_rdev;
    int64_t st_size, st_blksize, st_blocks;
    int64_t st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime, st_ctime_nsec;
    int64_t __reserved[3];
};
static inline void kstat_to_linux(const struct kstat *k, struct linux_stat *s) {
    *s = (struct linux_stat){
        .st_dev = k->dev, .st_ino = k->ino, .st_nlink = k->nlink, .st_mode = k->mode,
        .st_uid = k->uid, .st_gid = k->gid, .st_rdev = k->rdev, .st_size = k->size,
        .st_blksize = k->blksize, .st_blocks = k->blocks,
        .st_atime = k->atime.tv_sec, .st_atime_nsec = k->atime.tv_nsec,
        .st_mtime = k->mtime.tv_sec, .st_mtime_nsec = k->mtime.tv_nsec,
        .st_ctime = k->ctime.tv_sec, .st_ctime_nsec = k->ctime.tv_nsec,
    };
}
