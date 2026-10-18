#define main epoll_consumer_original_main
#include "/data/9os-team-epoll/userland/tests/epollabitest.c"
#undef main
int main(void) { sigset_t a,b; if(sigemptyset(&a))return 2; for(int s=1;s<=64;s++){b=a;if(sigaddset(&b,s))return 2;if(same_mask(&a,&b)||!same_mask(&b,&b))return 1;} puts("full-mask helper: all 64 single-bit differences detected; equal masks accepted");return 0;}
