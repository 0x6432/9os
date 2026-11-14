#!/usr/bin/env python3
"""Production evdev queue/state/read/poll functions with mock locks/user copies.
Not hardware, SMP or desktop evidence. The real uinput guest test is separate.
"""
from pathlib import Path
import os,resource,subprocess,tempfile
R=Path(__file__).resolve().parents[2]
s=(R/'kernel/drivers/evdev.c').read_text()
def part(a,b):return s[s.index(a):s.index(b,s.index(a))]
structs=part('struct input_event_abi {','static struct input_dev *devs')
funcs=part('static void client_push(', '/* ---------------------------------------------------------------- M34: sysfs */')
read=part('static ssize_t evdev_read(', '/* as Linux\'s evdev_write')
poll=part('static unsigned evdev_poll(', 'static int put_bits(')
l=(R/'kernel/include/kernel/list.h').read_text().replace('#pragma once','').replace('#include <kernel/types.h>','')
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <linux/input.h>
#define nullptr NULL
#define QLEN 512
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
'''+l+r'''
struct input_dev {int lock;bool gone,console_keys;struct list_node clients;void *grab;unsigned npending;uint64_t evbit[2],keybit[16],keystate[16],swbit[2],swstate[2],sndbit[2],sndstate[2];struct input_absinfo abs[ABS_MAX+1];};
static bool input_test_bit(const uint64_t *b,unsigned n){return !!(b[n/64]&(1ull<<(n%64)));}
static void bit(uint64_t *b,unsigned n){b[n/64]|=1ull<<(n%64);}
static uint64_t boot_epoch;
static unsigned wakes,locked;
static int poll_wq;
static uint64_t time_ns(void){return 123456789;}
static uint64_t spin_lock_irqsave(int *l){assert(!*l&&!locked);*l=1;locked=1;return 42;}
static void spin_unlock_irqrestore(int *l,uint64_t f){assert(*l&&locked&&f==42);*l=0;locked=0;}
static void spin_unlock(int *l){spin_unlock_irqrestore(l,42);}
static void arch_irq_restore(uint64_t f){assert(f==42&&!locked);}
static void poll_notify(void){assert(!locked);wakes++;}
static void console_key(struct input_dev *d,unsigned c,int v){(void)d;(void)c;(void)v;assert(!locked);}
static int wait_event_timeout(int *q,uint64_t ns){(void)q;assert(ns==50000000&&!locked);return -EINTR;}
static int copy_to_user(void *a,const void *b,size_t n){memcpy(a,b,n);return 0;}
struct file {struct evdev_client *priv;int flags;};
'''
post=r'''
static struct input_dev d;
static struct evdev_client a,b;
static struct file f,g;
static void reset(void){assert(!locked);memset(&d,0,sizeof d);memset(&a,0,sizeof a);memset(&b,0,sizeof b);list_init(&d.clients);a.dev=b.dev=&d;list_add_tail(&d.clients,&a.node);list_add_tail(&d.clients,&b.node);f=(struct file){&a,O_NONBLOCK};g=(struct file){&b,O_NONBLOCK};bit(d.evbit,EV_SYN);bit(d.evbit,EV_REL);bit(d.evbit,EV_KEY);bit(d.keybit,KEY_F13);wakes=0;}
static void send(unsigned t,unsigned c,int v){input_event(&d,t,c,v);assert(!locked);}
static void sync_(void){send(EV_SYN,SYN_REPORT,0);}
static bool ready(struct file *p){return !!(evdev_poll(p)&POLLIN);}
static ssize_t take(struct file *p,struct input_event_abi *out,size_t count){return evdev_read(p,out,count*sizeof *out,NULL);}
int main(void){int groups=0;struct input_event_abi out[64];reset();
 send(EV_REL,REL_X,1);assert(!ready(&f)&&!ready(&g));assert(take(&f,out,64)==-EAGAIN);groups++;
 sync_();assert(ready(&f));assert(take(&f,out,64)==48&&out[0].value==1&&out[1].type==EV_SYN);assert(!ready(&f)&&ready(&g));groups++;
 assert(take(&g,out,64)==48);send(EV_REL,REL_X,2);sync_();send(EV_REL,REL_Y,3);assert(take(&f,out,64)==48&&out[0].value==2);assert(!ready(&f)&&take(&f,out,64)==-EAGAIN);groups++;
 sync_();assert(take(&f,out,1)==24&&out[0].value==3);assert(ready(&f));assert(take(&f,out,1)==24&&out[0].type==EV_SYN&&!ready(&f));groups++;
 assert(take(&g,out,64)==96);assert(!ready(&g));groups++;
 reset();send(EV_KEY,KEY_F13,1);sync_();assert(take(&f,out,64)==48);unsigned before=wakes;send(EV_KEY,KEY_F13,1);sync_();assert(!ready(&f)&&wakes==before);groups++;
 send(EV_KEY,KEY_F13,2);sync_();assert(take(&f,out,64)==48&&out[0].value==2&&input_test_bit(d.keystate,KEY_F13));send(EV_KEY,KEY_F13,0);sync_();assert(take(&f,out,64)==48);send(EV_KEY,KEY_F13,2);sync_();assert(take(&f,out,64)==48&&out[0].value==2&&!input_test_bit(d.keystate,KEY_F13));groups++;
 before=wakes;send(EV_KEY,KEY_F14,1);sync_();assert(!ready(&f)&&wakes==before);groups++;
 reset();send(EV_REL,REL_X,1);sync_();for(unsigned i=0;i<QLEN;i++)send(EV_REL,REL_X,10+i);assert(!ready(&f)&&take(&f,out,64)==-EAGAIN);sync_();bool dropped=false;int last=-1;while(ready(&f)){ssize_t n=take(&f,out,64);assert(n>0&&n<=32*24);for(int i=0;i<n/24;i++){if(out[i].type==EV_SYN&&out[i].code==SYN_DROPPED)dropped=true;if(out[i].type==EV_REL)last=out[i].value;}}assert(dropped&&last==10+QLEN-1);groups++;
 reset();for(int i=0;i<40;i++)send(EV_REL,REL_X,i+1);sync_();assert(take(&f,out,64)==32*24&&ready(&f));assert(take(&f,out,64)==9*24&&out[8].type==EV_SYN&&!ready(&f));groups++;
 reset();a.head=a.tail=a.packet_tail=UINT_MAX-1;send(EV_REL,REL_X,7);sync_();assert(take(&f,out,64)==48&&out[0].value==7&&!ready(&f));groups++;
 d.gone=true;assert(evdev_poll(&f)==(POLLERR|POLLHUP));assert(take(&f,out,64)==-ENODEV);groups++;
 printf("evdev exact-function model: %d groups passed (mock locks/users only)\n",groups);return 0;
}
'''
normal=funcs+read+poll
mutations={
'normal':normal,
'publish-incomplete':normal.replace('if (type == EV_SYN && code == SYN_REPORT) c->packet_tail = c->tail;','c->packet_tail = c->tail;',1),
'read-incomplete-tail':funcs+read.replace('c->head != c->packet_tail','c->head != c->tail',1)+poll,
'poll-incomplete-tail':funcs+read+poll.replace('c->packet_tail != c->head','c->tail != c->head',1),
'overflow-stale-packet':normal.replace('c->packet_tail = c->head;','/* missing packet reset */',1),
'duplicate-down-repeat':normal.replace('if (!!value == was) goto ignored;','if (!!value == was) value = 2;',1),
'repeat-mutates-state':normal.replace('if (value != 2) {','if (true) {',1),
'unsupported-key':normal.replace('code > KEY_MAX || !input_test_bit(d->keybit, code)','code > KEY_MAX',1),
}
def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
with tempfile.TemporaryDirectory(prefix='9os-evdev-') as tmp:
 for name,body in mutations.items():
  assert name=='normal' or body!=normal
  c=Path(tmp)/(name+'.c');exe=Path(tmp)/name;c.write_text(pre+structs+body+post)
  r=subprocess.run([os.environ.get('CC','clang'),'-O2','-fsanitize=undefined','-fsanitize-trap=all','-Wall','-Wextra','-Wno-unused-parameter',str(c),'-o',str(exe)],capture_output=True)
  if r.returncode:raise SystemExit(r.stderr.decode())
  r=subprocess.run([str(exe)],capture_output=True,timeout=5,preexec_fn=no_core)
  if name=='normal':
   if r.returncode:raise SystemExit(r.stderr.decode())
   print(r.stdout.decode().strip())
  else:
   if r.returncode!=-6:raise SystemExit(f'{name}: expected assertion, got {r.returncode}')
   print('evdev model: rejected '+name)
