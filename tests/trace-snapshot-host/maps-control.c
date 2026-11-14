/* Explicit synthetic procfs I/O controls around the EXACT production scanner.
 * No registers/ptrace/GUI are mocked into an acceptance observation. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
static const char *fixture;
static FILE *fixture_open(const char *path, const char *mode) {
    (void)path; (void)mode;
    if (!fixture) { errno = EACCES; return NULL; }
    return fmemopen((void *)fixture, strlen(fixture), "r");
}
#define fopen fixture_open
#define main snapshot_cli_main
#include "../../userland/tests/trace-snapshot/trace-snapshot.c"
#undef main
#undef fopen
#define CODE "1000-2000 r-xp 00000000 00:01 1 /synthetic/code\n"
#define STACK "3000-4000 rw-p 00000000 00:01 2 /synthetic/stack\n"
int main(void) {
    struct Case {const char *text;uint64_t pc,sp;int ok;} cases[] = {
        {CODE STACK,0x1100,0x3100,1},
        {"1000-2000 rwxp 00000000 00:01 1 /synthetic/shared\n",0x1100,0x1200,1},
        {CODE STACK,0x2000,0x3100,0}, /* end is exclusive */
        {CODE,0x1100,0x3100,0},
        {STACK,0x1100,0x3100,0},
        {"bad map\n",0x1100,0x3100,0},
        {"2000-1000 r-xp 00000000 00:01 1 /synthetic/reversed\n" STACK,0x1100,0x3100,0},
        {CODE CODE STACK,0x1100,0x3100,0},
        {"1000-2000 r--p 00000000 00:01 1 /synthetic/noexec\n" STACK,0x1100,0x3100,0},
        {CODE "3000-4000 ---p 00000000 00:01 2 /synthetic/noread\n",0x1100,0x3100,0},
        {"1000-2000 r-xp 00000000 00:01 1 /synthetic/unterminated",0x1100,0x1200,0},
        {NULL,0x1100,0x3100,0},
    };
    for (unsigned i=0;i<sizeof cases/sizeof cases[0];i++) {
        fixture=cases[i].text;
        if ((maps(123,cases[i].pc,cases[i].sp)==0)!=cases[i].ok) {
            fprintf(stderr,"maps-control: failed synthetic case %u\n",i);
            return 1;
        }
    }
    char long_line[1200];
    memset(long_line,'x',sizeof long_line);
    memcpy(long_line,"1000-2000 r-xp ",14);
    long_line[sizeof long_line-2]='\n';long_line[sizeof long_line-1]=0;
    fixture=long_line;
    if (!maps(123,0x1100,0x1200)) return 1;
    puts("maps-control:13/13 synthetic scanner I/O controls; NOT ptrace/GUI proof");
    return 0;
}