/* Opt-in Qt6.8.3 LP64 Itanium-ABI diagnostic, not an application fix.
 * Opaque Qt references are forwarded untouched. No Qt library compilation.
 * Observes platform event submission, NOT application event consumption.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
_Static_assert(sizeof(unsigned long)==8 && sizeof(void *)==8 && sizeof(bool)==1, "Qt6.8.3 LP64 ABI required");
#define SYMBOL "_ZN22QWindowSystemInterface22handleExtendedKeyEventEP7QWindowmN6QEvent4TypeEi6QFlagsIN2Qt16KeyboardModifierEEjjjRK7QStringbt"
typedef bool (*submit_fn)(void *,unsigned long,int,int,int,uint32_t,uint32_t,uint32_t,const void *,bool,unsigned short);
static submit_fn next;
static _Atomic unsigned sequence;
_Static_assert(ATOMIC_INT_LOCK_FREE==2, "observer sequence must be lock-free");
static void output(const char *s,size_t len) {
 while(len){ssize_t n=write(2,s,len);if(n<0&&errno==EINTR)continue;if(n<=0)return;s+=n;len-=(size_t)n;}
}
static void record(unsigned seq,const char *phase,unsigned long stamp,int type,int key,int mods,uint32_t scan,uint32_t sym,uint32_t native,bool repeat,unsigned short count,int result) {
 struct timespec ts={0};int ok=clock_gettime(CLOCK_MONOTONIC,&ts)==0;char b[512];
 int n=snprintf(b,sizeof(b),"QT_KEY_TRACE seq=%u phase=%s pid=%ld clock_ok=%d mono_ns=%llu source_ms=%lu type=%d key=%d modifiers=%d scan=%u sym=%u native=%u autorepeat=%d count=%u result=%d\n",seq,phase,(long)getpid(),ok,(unsigned long long)ts.tv_sec*1000000000ull+(unsigned long long)ts.tv_nsec,stamp,type,key,mods,scan,sym,native,repeat?1:0,(unsigned)count,result);
 if(n>0&&(size_t)n<sizeof(b))output(b,(size_t)n);
}
__attribute__((constructor))static void init(void) {
 void *symbol=dlsym(RTLD_NEXT,SYMBOL);
 _Static_assert(sizeof(next)==sizeof(symbol), "POSIX function pointer representation");
 memcpy(&next,&symbol,sizeof(next));
 /* Non-Qt children can inherit preload. Never reject their startup. */
 if(next){const char b[]="QT_KEY_TRACE_LOADED abi=Qt6.8.3-LP64 boundary=platform-submission\n";output(b,sizeof(b)-1);}
}
bool observe(void *,unsigned long,int,int,int,uint32_t,uint32_t,uint32_t,const void *,bool,unsigned short) __asm__(SYMBOL);
bool observe(void *window,unsigned long stamp,int type,int key,int mods,uint32_t scan,uint32_t sym,uint32_t native,const void *text,bool repeat,unsigned short count) {
 if(!next)_exit(127);
 /* Saturate after the single truncation witness: unsigned wrap must never
  * restart logging in a long-lived client. No blocking observer-side lock. */
 unsigned seq=atomic_load_explicit(&sequence,memory_order_relaxed);
 while(seq<=4096 && !atomic_compare_exchange_weak_explicit(&sequence,&seq,seq+1,memory_order_relaxed,memory_order_relaxed)) {}
 if(seq<4096)record(seq,"begin",stamp,type,key,mods,scan,sym,native,repeat,count,-1);
 else if(seq==4096){const char b[]="QT_KEY_TRACE_TRUNCATED limit=4096\n";output(b,sizeof(b)-1);}
 bool result=next(window,stamp,type,key,mods,scan,sym,native,text,repeat,count);
 if(seq<4096)record(seq,"end",stamp,type,key,mods,scan,sym,native,repeat,count,result?1:0);
 return result;
}
