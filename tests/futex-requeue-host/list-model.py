#!/usr/bin/env python3
"""Execute the exact kernel requeue function with mock locks/stack waiters.
List/count controls only: not a substitute for real kernel SMP execution.
"""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=Path(os.environ['FUTEX_REQUEUE_SOURCE']).read_text() if 'FUTEX_REQUEUE_SOURCE' in os.environ else (ROOT/'kernel/core/sys_proc.c').read_text()
a=s.index('static int futex_requeue(');b=s.index('/* lock the bucket that currently holds w',a)
func=s[a:b]
list_code=(ROOT/'kernel/include/kernel/list.h').read_text().replace('#pragma once','').replace('#include <kernel/types.h>','')
preamble=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
struct mm {int tag;};
struct futex_bucket {int lock;struct list_node head;};
'''
# list_node must be defined before the structures, after host C types.
cut=preamble.index('struct mm')
preamble=preamble[:cut]+list_code+preamble[cut:]+r'''
static struct futex_bucket buckets[2];
struct futex_waiter {struct list_node node;struct mm *mm;uint32_t *addr;uint32_t bitset;struct futex_bucket *bucket;};
struct futex_key {struct mm *mm;uint32_t *addr;};
static int locked;static int *last;
static uint64_t arch_irq_save(void){assert(!locked);last=NULL;return 42;}
static void arch_irq_restore(uint64_t f){assert(f==42&&!locked);}
static void spin_lock_ipi(int *l){assert(!*l);if(last)assert(l>last);last=l;*l=1;locked++;}
static void spin_unlock(int *l){assert(*l);*l=0;locked--;}
static struct futex_bucket *futex_bucket(struct mm *mm,uint32_t *addr){(void)mm;return &buckets[((uintptr_t)addr>>2)&1];}
'''
main=r'''
static struct mm mm,other;
static struct futex_waiter w[4];
static struct futex_key key(uintptr_t v){return (struct futex_key){&mm,(uint32_t *)v};}
static int count(struct futex_key k){int n=0,seen=0;struct futex_bucket *b=futex_bucket(k.mm,k.addr);list_for_each(p,&b->head){assert(++seen<=4);struct futex_waiter *x=list_entry(p,struct futex_waiter,node);if(x->mm==k.mm&&x->addr==k.addr)n++;assert(x->bucket==b);}return n;}
static void setup(void){for(int i=0;i<2;i++)list_init(&buckets[i].head);for(int i=0;i<4;i++){w[i]=(struct futex_waiter){.mm=i==3?&other:&mm,.addr=(uint32_t *)0x1000,.bitset=1u<<i,.bucket=&buckets[0]};list_add_tail(&buckets[0].head,&w[i].node);}}
int main(void){struct futex_key a=key(0x1000);int groups=0;
 setup();assert(futex_requeue(a,a,100)==3);assert(count(a)==3);groups++;
 assert(futex_requeue(a,a,2)==2);assert(count(a)==3);groups++;
 assert(futex_requeue(a,a,0)==0);assert(futex_requeue(a,a,-1)==0);groups++;
 for(int collision=0;collision<2;collision++){
  setup();struct futex_key b=key(collision?0x1008:0x1004);
  assert(futex_requeue(a,b,1)==1);assert(count(a)==2&&count(b)==1);groups++;
  assert(futex_requeue(a,b,100)==2);assert(count(a)==0&&count(b)==3);groups++;
  assert(futex_requeue(b,a,100)==3);assert(count(a)==3&&count(b)==0);groups++;
  for(int i=0;i<4;i++)assert(w[i].bitset==(1u<<i));assert(w[3].mm==&other);groups++;
 }
 printf("futex requeue exact-function list controls: %d groups passed (mock locks only)\n",groups);return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(preamble+func+main)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=5)
