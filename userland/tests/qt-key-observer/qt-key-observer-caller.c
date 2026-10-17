#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define SYMBOL "_ZN22QWindowSystemInterface22handleExtendedKeyEventEP7QWindowmN6QEvent4TypeEi6QFlagsIN2Qt16KeyboardModifierEEjjjRK7QStringbt"
extern bool submit(void *,unsigned long,int,int,int,uint32_t,uint32_t,uint32_t,const void *,bool,unsigned short) __asm__(SYMBOL);
extern unsigned mock_calls(void);
int main(void){
 for(unsigned i=0;i<4100;i++)assert(submit((void *)0x1234,0x1122334455667788UL,6+(int)(i&1),0x44,0x02000000,40,100,0xfedcba98U,(void *)0x5678,(bool)(i&1),0xfffe)==(bool)(i&1));
 assert(mock_calls()==4100);puts("MOCK_ABI_FORWARDING_PASS calls=4100");return 0;
}
