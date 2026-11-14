#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define SYMBOL "_ZN22QWindowSystemInterface22handleExtendedKeyEventEP7QWindowmN6QEvent4TypeEi6QFlagsIN2Qt16KeyboardModifierEEjjjRK7QStringbt"
static unsigned calls;
bool mock(void *,unsigned long,int,int,int,uint32_t,uint32_t,uint32_t,const void *,bool,unsigned short) __asm__(SYMBOL);
bool mock(void *window,unsigned long stamp,int type,int key,int mods,uint32_t scan,uint32_t sym,uint32_t native,const void *text,bool repeat,unsigned short count) {
 assert(window==(void *)0x1234 && stamp==0x1122334455667788UL);
 assert(type==6+(int)(calls&1) && key==0x44 && mods==0x02000000);
 assert(scan==40 && sym==100 && native==0xfedcba98U && text==(void *)0x5678);
 assert(repeat==(bool)(calls&1) && count==0xfffeU);calls++;return repeat;
}
unsigned mock_calls(void){return calls;}
