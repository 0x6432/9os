#!/usr/bin/env python3
"""Actual wake/filter/scan bodies with mock scheduler locks and FD references.
Not a real SMP proof: guest poll/epoll/select and alias tests remain required.
"""
from pathlib import Path
import os,resource,subprocess,tempfile
R=Path(__file__).resolve().parents[2]
v=(R/'kernel/fs/vfs.c').read_text();v=v[v.index('uint64_t poll_seq;'):v.index('struct timespec now_timespec')]
s=(R/'kernel/core/sched.c').read_text();s=s[s.index('void wake_up(struct wait_queue'):s.index('void wake_up_one(')]
f=(R/'kernel/core/sys_fs.c').read_text();f=f[f.index('static int poll_once('):f.index('/* ppoll/pselect mask swap')]
l=(R/'kernel/include/kernel/list.h').read_text().replace('#pragma once','').replace('#include <kernel/types.h>','')
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
typedef uintptr_t vaddr_t;
'''+l+r'''
struct thread {struct list_node run_node;bool poll_filtered;uint64_t poll_filter;int woken;};
struct wait_queue {struct list_node head;};
static struct thread threads[5],*current;
static struct wait_queue poll_wq;
static int locked,enrolls,wait_result,mode;
static bool expected_filtered;
static uint64_t expected_filter;
static uint64_t sl_lock_irqsave(void){assert(!locked);locked=1;return 42;}
static void sl_unlock_irqrestore(uint64_t f){assert(locked&&f==42);locked=0;}
static uint64_t sched_wait_lock(void){return sl_lock_irqsave();}
static void sched_wait_unlock(uint64_t f){sl_unlock_irqrestore(f);}
static void wake_thread_locked(struct thread *t){assert(locked);list_del(&t->run_node);t->woken++;}
static int wait_event_timeout_locked(struct wait_queue *,uint64_t,uint64_t);
struct inode {int id;};
struct file;
struct file_ops {bool nobkl,poll_leaf;unsigned (*poll)(struct file *);};
struct file {struct inode *inode;const struct file_ops *fops;};
static struct file *fd_table[8];
static int gets,puts_,bkl;
static struct file *fd_get_ref(int fd){if(fd<0||fd>=8||!fd_table[fd])return NULL;gets++;return fd_table[fd];}
static void vfs_close(struct file *f){assert(f);puts_++;}
static bool bkl_held(void){return bkl!=0;}
static void bkl_enter(void){assert(!bkl);bkl=1;}
static void bkl_exit(void){assert(bkl);bkl=0;}
static unsigned not_ready(struct file *f){(void)f;return 0;}
static const struct file_ops leaf={true,true,not_ready},wrapper={false,false,not_ready};
static struct file a,b,alias;
'''
post=r'''
static void reset(void){assert(!locked);list_init(&poll_wq.head);memset(threads,0,sizeof threads);for(int i=0;i<5;i++)list_init(&threads[i].run_node);current=&threads[0];enrolls=0;mode=0;}
static int wait_event_timeout_locked(struct wait_queue *q,uint64_t ns,uint64_t irq){
 assert(locked&&ns==123&&irq==42);assert(current->poll_filtered==expected_filtered);
 assert(current->poll_filter==expected_filter);enrolls++;list_add_tail(&q->head,&current->run_node);sched_wait_unlock(irq);
 if(mode==1){poll_notify_file(&a);assert(current->woken==1);}
 else if(mode==2){poll_notify_file(&b);assert(current->woken==0);}
 if(!current->woken){uint64_t f=sched_wait_lock();list_del(&current->run_node);sched_wait_unlock(f);}
 return wait_result;
}
int main(void){int groups=0;reset();
 a=(struct file){(struct inode *)(uintptr_t)0x1000,&leaf};alias=a;b=a;
 uint64_t ak=poll_file_key(&a);
 assert(ak&&!(ak&(ak-1)));assert(poll_file_key(&alias)==ak);
 struct file aliases[16];for(int i=0;i<16;i++){aliases[i]=a;assert(poll_file_key(&aliases[i])==ak);}groups++;
 for(uintptr_t p=0x2000;poll_file_key(&b)==ak;p+=16){assert(p<0x100000);b.inode=(struct inode *)p;}
 uint64_t bk=poll_file_key(&b);assert(bk&&bk!=ak);groups++;
 threads[0].poll_filtered=true;threads[0].poll_filter=ak;
 threads[1].poll_filtered=true;threads[1].poll_filter=bk;
 threads[2].poll_filtered=false;
 threads[3].poll_filtered=true;threads[3].poll_filter=ak|bk;
 for(int i=0;i<4;i++)list_add_tail(&poll_wq.head,&threads[i].run_node);
 uint64_t before=poll_seq;poll_notify_file(&alias);assert(poll_seq==before+1);
 assert(threads[0].woken==1&&threads[1].woken==0&&threads[2].woken==1&&threads[3].woken==1);groups++;
 poll_notify();assert(threads[1].woken==1&&list_empty(&poll_wq.head));groups++;
 reset();before=poll_seq;poll_notify_file(&a);
 assert(poll_wait_seq_masked(before,123,ak)==0&&enrolls==0&&!current->poll_filtered);groups++;
 for(int i=0;i<3;i++){
  reset();expected_filtered=true;expected_filter=ak;mode=i;wait_result=i==0?-ETIMEDOUT:i==1?0:-EINTR;
  assert(poll_wait_seq_masked(poll_seq,123,ak)==wait_result);assert(enrolls==1&&!current->poll_filtered&&list_empty(&poll_wq.head));groups++;
 }
 reset();expected_filtered=false;expected_filter=UINT64_MAX;wait_result=-EINTR;
 assert(poll_wait_seq(poll_seq,123)==-EINTR&&enrolls==1&&!current->poll_filtered);groups++;
 struct file indirect={a.inode,&wrapper};fd_table[0]=&a;fd_table[1]=&b;fd_table[2]=&alias;fd_table[3]=&indirect;
 struct pollfd p[3]={{0,POLLIN,0},{1,POLLIN,0},{2,POLLIN,0}};uint64_t mask=0;
 assert(poll_once(p,3,&mask)==0&&mask==(ak|bk)&&gets==puts_&&!bkl);groups++;
 p[0].fd=3;assert(poll_once(p,3,&mask)==0&&mask==UINT64_MAX&&gets==puts_&&!bkl);groups++;
 p[0].fd=0;p[2].fd=3;assert(poll_once(p,3,&mask)==0&&mask==UINT64_MAX&&gets==puts_&&!bkl);groups++;
 p[0].fd=-1;p[1].fd=7;assert(poll_once(p,2,&mask)==1&&p[1].revents==POLLNVAL&&gets==puts_);groups++;
 assert(poll_once(p,0,&mask)==0&&mask==0);assert(poll_once(p,0,NULL)==0);groups++;
 printf("poll filter exact-function model: %d groups passed (mock locks/FDs only)\n",groups);return 0;
}
'''
def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
normal=s+v+f
mutations={
 'normal':normal,
 'exclude-unfiltered':normal.replace('!t->poll_filtered || (t->poll_filter & key)','(t->poll_filtered && (t->poll_filter & key))',1),
 'wake-unrelated':normal.replace('!t->poll_filtered || (t->poll_filter & key)','true',1),
 'stale-epoch-enrolls':normal.replace('__atomic_load_n(&poll_seq, __ATOMIC_SEQ_CST) != seq','false',1),
 'missing-publication':normal.replace('current->poll_filtered = filter != UINT64_MAX;','current->poll_filtered = false;',1),
 'missing-reset':normal.replace('current->poll_filtered = false;','/* missing reset */',1),
 'unsafe-wrapper':normal.replace('else *filter = UINT64_MAX;','/* missing conservative fallback */',1),
 'file-not-inode-alias-key':normal.replace('(vaddr_t)f->inode','(vaddr_t)f',1),
}
with tempfile.TemporaryDirectory(prefix='9os-poll-filter-') as temp:
 d=Path(temp)
 for name,code in mutations.items():
  assert name=='normal' or code!=normal
  c=d/(name+'.c');binary=d/name;c.write_text(pre+code+post)
  subprocess.run([os.environ.get('CC','cc'),'-O2','-Wall','-Wextra','-Wno-unused-parameter',str(c),'-o',str(binary)],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  result=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=5,preexec_fn=no_core)
  if name=='normal':
   if result.returncode:raise SystemExit(result.stderr.decode())
   print(result.stdout.decode().strip())
  else:
   if result.returncode!=-6:raise SystemExit(f'mutation {name} did not fail an assertion: {result.returncode}')
   print('poll filter model: rejected '+name)
