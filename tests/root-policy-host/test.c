#include <stdio.h>
#include <string.h>
#include <kernel/root_policy.h>

static int calls, mock_error;
static char observed[64];
static int mount_mock(const char *source, const char *type) {
    calls++;
    if (strcmp(type, "ext2")) return -99;
    strcpy(observed, source);
    return mock_error;
}
struct testcase { const char *input; int result; const char *root; };
int main(void) {
    const struct testcase cases[] = {
        {"", 0, ""},
        {"root=/dev/vda1", 0, "/dev/vda1"},
        {"notroot_required=1 root=/dev/vda1", 0, "/dev/vda1"},
        {"root_required_extra=1 root=/dev/vda1", 0, "/dev/vda1"},
        {"root_required=1 root=/dev/vda1", 1, "/dev/vda1"},
        {"root_required=1 root=/dev/./vda1", 1, "/dev/./vda1"},
        {"root_required=1 root=relative", 1, "relative"},
        {"root_required=1 root root=/dev/vda1", 1, "/dev/vda1"},
        {"x=y\troot=/dev/vda1\troot_required=1", 1, "/dev/vda1"},
        {"root_required=1", -1, ""},
        {"root_required", -1, ""},
        {"root_required=0 root=/dev/vda1", -1, "/dev/vda1"},
        {"root_required=1 root=", -1, ""},
        {"root_required=1 root=/dev/a root=/dev/b", -1, "/dev/b"},
        {"root_required=1 root_required=1 root=/dev/a", -1, "/dev/a"},
        {"root_required=1 root=abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl", -1, ""},
    };
    for (size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        char root[64];int result=root_policy_parse(cases[i].input,root);
        if (result!=cases[i].result || strcmp(root,cases[i].root)) {
            fprintf(stderr,"case %zu failed: rc=%d root=%s\n",i,result,root);return 1;
        }
    }
    char root[64], args[128]; int error;
    if (root_policy_parse(NULL,root)!=0) return 2;
    strcpy(args,"root_required=1 root=");memset(args+21,'a',63);args[84]='\0';
    if (root_policy_parse(args,root)!=1 || strlen(root)!=63) return 3;
    args[84]='a';args[85]='\0';if (root_policy_parse(args,root)!=-1) return 4;
    calls=0;mock_error=0;error=123;
    if (root_policy_mount("root=/dev/vda1",root,mount_mock,&error)!=0 || calls || error) return 5;
    error=123;
    if (root_policy_mount("root_required=1",root,mount_mock,&error)!=-1 || calls || error) return 6;
    if (root_policy_mount("root_required=1 root=/dev/vda1",root,mount_mock,&error)!=1 || calls!=1 || error || strcmp(observed,"/dev/vda1")) return 7;
    mock_error=-5;
    if (root_policy_mount("root_required=1 root=/dev/vda1",root,mount_mock,&error)!=-2 || calls!=2 || error!=-5) return 8;
    mock_error=5;
    if (root_policy_mount("root_required=1 root=/dev/vda1",root,mount_mock,&error)!=-2 || calls!=3 || error!=5) return 9;
    puts("strict root policy parser and mock mount controls PASS; no guest evidence");
    return 0;
}
