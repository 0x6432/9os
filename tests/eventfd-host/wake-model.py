#!/usr/bin/env python3
"""Compile actual eventfd operations with mocked scheduler queues.

This models notification routing and enrolled overflow writers, not real 9os
SMP scheduler behavior. The exact syscall consumer separately runs on Linux
and in guest CI. Negative mutations must be rejected, not accepted as passes.
"""
import os
from pathlib import Path
import resource
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
src=(ROOT/'kernel/fs/anonfd.c').read_text()
body=src[src.index('struct eventfd {'):src.index('static void priv_release')]
pre=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <unistd.h>
typedef uintptr_t vaddr_t;
#define USER_TOP ((uintptr_t)0)
struct wait_queue { pthread_cond_t cond; int waiting; };
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t enrolled=PTHREAD_COND_INITIALIZER;
static struct wait_queue poll_wq={PTHREAD_COND_INITIALIZER,0};
static unsigned notifications;
static void wait_queue_init(struct wait_queue *q) { pthread_cond_init(&q->cond,NULL); q->waiting=0; }
static void wake_up(struct wait_queue *q) { pthread_mutex_lock(&lock); pthread_cond_broadcast(&q->cond); pthread_mutex_unlock(&lock); }
static void poll_notify(void) { __atomic_add_fetch(&notifications,1,__ATOMIC_RELAXED); wake_up(&poll_wq); }
#define wait_until_sl(q,predicate) ({ pthread_mutex_lock(&lock); while(!(predicate)) { (q)->waiting++; pthread_cond_broadcast(&enrolled); pthread_cond_wait(&(q)->cond,&lock); (q)->waiting--; } pthread_mutex_unlock(&lock); 0; })
struct file { void *priv; int flags; };
static int copy_to_user(void *a,const void *b,size_t n) { memcpy(a,b,n); return 0; }
static int copy_from_user(void *a,const void *b,size_t n) { memcpy(a,b,n); return 0; }
'''
post=r'''
struct call { struct file *f; uint64_t value; ssize_t result; };
static void *writer(void *arg) { struct call *c=arg; c->result=efd_write(c->f,&c->value,8,NULL); return NULL; }
static void enroll(struct wait_queue *q) { pthread_mutex_lock(&lock); while(!q->waiting) pthread_cond_wait(&enrolled,&lock); pthread_mutex_unlock(&lock); }
static void *poller(void *arg) {
    struct file *f=arg;
    wait_until_sl(&poll_wq,efd_poll(f)&POLLOUT);
    return NULL;
}
int main(void) {
    alarm(3); struct eventfd e={0}; wait_queue_init(&e.writers); struct file f={&e,0}; uint64_t v;
    /* Ordinary drains must not globally wake unrelated pollers. */
    for(int i=0;i<1000;i++) { v=1; assert(efd_write(&f,&v,8,NULL)==8); assert(efd_read(&f,&v,8,NULL)==8 && v==1); }
    assert(notifications==1000);
    /* Prove model enrollment on the private queue while POLLOUT is true. */
    e.count=5; struct call c={&f,UINT64_MAX-4,0}; pthread_t t;
    assert(!pthread_create(&t,NULL,writer,&c)); enroll(&e.writers);
    assert(efd_poll(&f)&POLLOUT); unsigned n=notifications;
    assert(efd_read(&f,&v,8,NULL)==8 && v==5); assert(!pthread_join(t,NULL)); assert(c.result==8);
    assert(notifications==n+1); /* writer, not the non-saturated reader */
    /* Saturated read must also wake a real modeled poll waiter. */
    e.count=UINT64_MAX-1; n=notifications;
    assert(!pthread_create(&t,NULL,poller,&f)); enroll(&poll_wq);
    assert(efd_read(&f,&v,8,NULL)==8 && v==UINT64_MAX-1); assert(!pthread_join(t,NULL)); assert(notifications==n+1);
    /* Semaphore reads each notify overflow writers, including non-saturated. */
    e.count=UINT64_MAX-1; e.semaphore=true; c.value=2; c.result=0;
    assert(!pthread_create(&t,NULL,writer,&c)); enroll(&e.writers);
    pthread_mutex_lock(&lock); assert(e.writers.waiting==1); pthread_mutex_unlock(&lock);
    assert(efd_read(&f,&v,8,NULL)==8 && v==1);
    assert(efd_read(&f,&v,8,NULL)==8 && v==1);
    assert(!pthread_join(t,NULL)); assert(c.result==8 && efd_count(&e)==UINT64_MAX-1);
    puts("eventfd wake model: 4 routing/enrollment groups passed"); return 0;
}
'''
def no_core():
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
with tempfile.TemporaryDirectory(prefix='9os-eventfd-model-') as temp:
    d=Path(temp)
    mutations={
        'normal':body,
        'missing-private-wake':body.replace('wake_up(&e->writers);','/* missing private wake */',1),
        'only-saturated-writer-wake':body.replace('wake_up(&e->writers);','if (c == UINT64_MAX - 1) wake_up(&e->writers);',1),
        'missing-pollout-wake':body.replace('if (c == UINT64_MAX - 1) poll_notify();','/* missing POLLOUT wake */',1),
        'unconditional-global-drain':body.replace('if (c == UINT64_MAX - 1) poll_notify();','poll_notify();',1),
    }
    for name,code in mutations.items():
        if name!='normal' and code==body:
            raise SystemExit('mutation did not match: '+name)
        source=d/(name+'.c'); binary=d/name
        source.write_text(pre+code+post)
        subprocess.run([os.environ.get('CC','cc'),'-O2','-pthread',str(source),'-o',str(binary)],check=True)
        r=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=5,preexec_fn=no_core)
        if name=='normal':
            if r.returncode: raise SystemExit(r.stderr.decode() or 'normal model failed')
            print(r.stdout.decode().strip())
        else:
            if r.returncode==0: raise SystemExit('mutation survived: '+name)
            if r.returncode not in (-6,-14): raise SystemExit(f'unexpected mutation failure: {name}: {r.returncode}')
            print(f'eventfd wake model: rejected {name} (signal {-r.returncode})')
