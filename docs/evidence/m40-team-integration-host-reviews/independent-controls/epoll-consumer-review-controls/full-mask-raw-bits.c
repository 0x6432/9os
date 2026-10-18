#define main epoll_consumer_original_main
#include "/data/9os-team-epoll/userland/tests/epollabitest.c"
#undef main
#include <string.h>
int main(void) { sigset_t a,b; if(sigemptyset(&a))return 2; for(int s=1;s<=64;s++){b=a;uint64_t bit=UINT64_C(1)<<(s-1);memcpy(&b,&bit,sizeof bit);if(same_mask(&a,&b)||!same_mask(&b,&b)){printf("FAIL bit=%d\n",s);return 1;}} puts("full-mask helper: all 64 single-bit differences detected; equal masks accepted");return 0;}
