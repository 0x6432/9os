#pragma once
#include <kernel/string.h>

/* Optional strict root argument parser. Returns 0 for legacy behaviour, 1 for
 * one valid required root, and -1 for malformed/ambiguous strict inputs. */
static inline int root_policy_parse(const char *cmdline, char root[64]) {
    unsigned flags = 0, roots = 0;
    int invalid_flag = 0, invalid_root = 0;
    root[0] = '\0';
    if (!cmdline) return 0;
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t n = (size_t)(p - start);
        if (n >= 13 && !strncmp(start, "root_required", 13) &&
            (n == 13 || start[13] == '=')) {
            flags++;
            if (n != 15 || start[13] != '=' || start[14] != '1') invalid_flag = 1;
        }
        if (n >= 5 && !strncmp(start, "root=", 5)) {
            roots++;
            size_t length = n - 5;
            if (!length || length >= 64) invalid_root = 1;
            else {
                for (size_t i = 0; i < length; i++) root[i] = start[5 + i];
                root[length] = '\0';
            }
        }
    }
    if (!flags) return 0;
    if (invalid_flag || flags != 1 || invalid_root || roots != 1) return -1;
    return 1;
}

/* Result: -1 malformed policy, -2 required mount failed, 0 legacy, 1 mounted.
 * This enforces a mount attempt, not an authenticated device identity. */
static inline int root_policy_mount(const char *cmdline, char root[64],
                                    int (*mount_root)(const char *, const char *),
                                    int *mount_error) {
    *mount_error = 0;
    int policy = root_policy_parse(cmdline, root);
    if (policy <= 0) return policy;
    *mount_error = mount_root(root, "ext2");
    return *mount_error ? -2 : 1;
}
