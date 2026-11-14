#!/usr/bin/env python3
"""Execute the exact kernel requeue function with mock locks/stack waiters.
List/count/transaction lock controls: not a substitute for real kernel SMP execution.
"""
from pathlib import Path
import os,signal,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=Path(os.environ['FUTEX_REQUEUE_SOURCE']).read_text() if 'FUTEX_REQUEUE_SOURCE' in os.environ else (ROOT/'kernel/core/sys_proc.c').read_text()
a=s.index('static int futex_requeue(');b=s.index('/* lock the bucket that currently holds w',a)
func=s[a:b]
atomic='const uint32_t *compare_addr' in func
wa=s.index('static int futex_wake_locked(');wb=s.index('static int futex_wake_mask(',wa)
wake_func=s[wa:wb].replace('futex_wake_locked(', 'real_futex_wake_locked(',1)
list_code=(ROOT/'kernel/include/kernel/list.h').read_text().replace('#pragma once','').replace('#include <kernel/types.h>','')
preamble=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
struct mm {int tag;};
struct futex_bucket {int lock;struct list_node head;};
'''
# list_node must be defined before the structures, after host C types.
cut=preamble.index('struct mm')
preamble=preamble[:cut]+list_code+preamble[cut:]+r'''
static struct futex_bucket buckets[2];
struct wait_queue {int unused;};
struct futex_waiter {struct list_node node;struct mm *mm;uint32_t *addr;uint32_t bitset;struct futex_bucket *bucket;bool woken;struct wait_queue wq;};
struct futex_key {struct mm *mm;uint32_t *addr;};
static int locked;static int *last;
static struct futex_bucket *expected_a,*expected_b;
static bool require_transaction, compared, wake_started, bad_address;
static uint32_t expected_value;
static uint64_t futex_wakes;
static int copy_from_user(void *dst,const void *src,size_t n){
 assert(expected_a->lock&&expected_b->lock);assert(n==4);
 if(bad_address)return 1;
 memcpy(dst,src,n);compared=*(uint32_t *)dst==expected_value;return 0;
}
static void wake_up(struct wait_queue *wq){(void)wq;assert(expected_a->lock&&expected_b->lock);}

static uint64_t arch_irq_save(void){assert(!locked);last=NULL;return 42;}
static void arch_irq_restore(uint64_t f){assert(f==42&&!locked);}
static void spin_lock_ipi(int *l){assert(!*l);if(last)assert((uintptr_t)l>(uintptr_t)last);last=l;*l=1;locked++;}
static void spin_unlock(int *l){assert(*l);if(require_transaction&&compared)assert(wake_started);*l=0;locked--;}
static struct futex_bucket *futex_bucket(struct mm *mm,uint32_t *addr){(void)mm;return &buckets[((uintptr_t)addr>>2)&1];}
'''
preamble+=wake_func+r'''
static int futex_wake_locked(struct futex_bucket *b,struct mm *mm,uint32_t *addr,int n,uint32_t mask){
 assert(b->lock);if(require_transaction){assert(expected_a->lock&&expected_b->lock);wake_started=true;}
 return real_futex_wake_locked(b,mm,addr,n,mask);
}
'''
main=r'''
static struct mm mm,other;
static struct futex_waiter w[4];
static struct futex_key key(uintptr_t v){return (struct futex_key){&mm,(uint32_t *)v};}
static int count(struct futex_key k){int n=0,seen=0;struct futex_bucket *b=futex_bucket(k.mm,k.addr);list_for_each(p,&b->head){assert(++seen<=4);struct futex_waiter *x=list_entry(p,struct futex_waiter,node);if(x->mm==k.mm&&x->addr==k.addr)n++;assert(x->bucket==b);}return n;}
static void setup(void){require_transaction=compared=wake_started=bad_address=false;for(int i=0;i<2;i++)list_init(&buckets[i].head);for(int i=0;i<4;i++){w[i]=(struct futex_waiter){.mm=i==3?&other:&mm,.addr=(uint32_t *)0x1000,.bitset=1u<<i,.bucket=&buckets[0]};list_add_tail(&buckets[0].head,&w[i].node);}}
int main(void){struct futex_key a=key(0x1000);int groups=0;
 setup();assert(MOVE(a,a,100)==3);assert(count(a)==3);groups++;
 assert(MOVE(a,a,2)==2);assert(count(a)==3);groups++;
 assert(MOVE(a,a,0)==0);assert(MOVE(a,a,-1)==0);groups++;
 for(int collision=0;collision<2;collision++){
  setup();struct futex_key b=key(collision?0x1008:0x1004);
  assert(MOVE(a,b,1)==1);assert(count(a)==2&&count(b)==1);groups++;
  assert(MOVE(a,b,100)==2);assert(count(a)==0&&count(b)==3);groups++;
  assert(MOVE(b,a,100)==3);assert(count(a)==3&&count(b)==0);groups++;
  for(int i=0;i<4;i++)assert(w[i].bitset==(1u<<i));assert(w[3].mm==&other);groups++;
 }
 ATOMIC_GROUPS
 printf("futex requeue exact-function list controls: %d groups passed (mock locks only)\n",groups);return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)
 extra=''
 if atomic:
  adapter='#define MOVE(a,b,n) futex_requeue(a,b,0,n,false,NULL,0)\n'
  extra=r'''
 uint32_t word=7;
 for(int collision=0;collision<2;collision++){
  setup();struct futex_key b=key(collision?0x1008:0x1004);
  expected_a=futex_bucket(a.mm,a.addr);expected_b=futex_bucket(b.mm,b.addr);
  require_transaction=true;expected_value=7;
  assert(futex_requeue(a,b,1,1,true,&word,7)==2);
  assert(compared&&wake_started&&count(a)==1&&count(b)==1&&w[0].woken);groups++;
  setup();require_transaction=true;expected_value=8;
  assert(futex_requeue(a,b,1,100,true,&word,8)==-EAGAIN);
  assert(!wake_started&&count(a)==3&&count(b)==0&&!w[0].woken);groups++;
  bad_address=true;
  assert(futex_requeue(a,b,1,100,true,&word,7)==-EFAULT);
  assert(!wake_started&&count(a)==3&&count(b)==0);
  assert(futex_requeue(a,b,0,0,true,NULL,7)==-EFAULT);groups++;
  setup();require_transaction=true;expected_value=7;
  assert(futex_requeue(a,b,0,0,true,&word,7)==0);assert(compared&&wake_started&&count(a)==3);groups++;
 }
 setup();expected_a=expected_b=futex_bucket(a.mm,a.addr);require_transaction=true;expected_value=7;
 assert(futex_requeue(a,a,1,100,true,&word,7)==3);assert(count(a)==2&&w[0].woken);groups++;
 /* An opaque shared key is independent of the VA read for comparison. */
 setup();struct futex_key shared={&other,(uint32_t *)0x1000},dest={&other,(uint32_t *)0x1004};
 expected_a=futex_bucket(shared.mm,shared.addr);expected_b=futex_bucket(dest.mm,dest.addr);
 require_transaction=true;expected_value=7;
 assert(futex_requeue(shared,dest,0,100,true,&word,7)==1);assert(count(shared)==0&&count(dest)==1&&count(a)==3);groups++;
'''
 else:adapter='#define MOVE(a,b,n) futex_requeue(a,b,n)\n'
 program=preamble+func+adapter+main.replace('ATOMIC_GROUPS',extra)
 (p/'test.c').write_text(program)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-function',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=5)

 if atomic:
  # Model-only mutations of the exact production body: both reproduce the old
  # unsafe transaction boundaries. They MUST be rejected by the lock controls.
  needle='    r = futex_wake_locked(ba, a.mm, a.addr, nwake, UINT32_MAX);'
  gap='    if (l2 != l1) spin_unlock(&l2->lock);\n    spin_unlock(&l1->lock);\n    spin_lock_ipi(&l1->lock);\n    if (l2 != l1) spin_lock_ipi(&l2->lock);\n'
  for label,body in [('compare-unlocked',func.replace('if (copy_from_user(&cur, compare_addr, sizeof cur))','if ((spin_unlock(&ba->lock), copy_from_user(&cur, compare_addr, sizeof cur)))',1)),('compare-wake-gap',func.replace(needle,gap+needle,1))]:
   assert body!=func
   (p/'negative.c').write_text(preamble+body+adapter+main.replace('ATOMIC_GROUPS',extra))
   subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wno-unused-function',str(p/'negative.c'),'-o',str(p/'negative')],check=True)
   r=subprocess.run([str(p/'negative')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=5)
   assert r.returncode==-signal.SIGABRT,(label,r.returncode)
   print('futex requeue transaction negative rejected:',label)
