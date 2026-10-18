#!/usr/bin/env python3
"""Exact epoll scanner with mock locks/FD pins; NOT SMP or GUI validation."""
from pathlib import Path
import os, resource, subprocess, tempfile
R = Path(__file__).resolve().parents[2]
a = (R / 'kernel/fs/anonfd.c').read_text()
scan = a[a.index('struct epitem {'):a.index('static unsigned ep_poll(')]
lists = (R / 'kernel/include/kernel/list.h').read_text().replace('#pragma once', '').replace('#include <kernel/types.h>', '')
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <sys/epoll.h>
#define nullptr NULL
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
''' + lists + r'''
struct mutex { bool held; };
struct lock_class { const char *name; int rank; bool recursive; };
#define LR_MUTEX_EPOLL 1
struct inode { uint64_t key; };
struct file;
struct file_ops { bool nobkl, poll_leaf; unsigned poll_always; unsigned (*poll)(struct file *); };
struct file { struct inode *inode; const struct file_ops *fops; void *priv; unsigned ready; int refs; };
struct epoll_event_u { uint32_t events; uint64_t data; };
static struct file *fds[16];
static int gets, puts_, bkl, key_reads;
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; }
static bool mutex_owned(struct mutex *m) { return m->held; }
static bool bkl_held(void) { return bkl; }
static void bkl_enter(void) { assert(!bkl); bkl=1; }
static void bkl_exit(void) { assert(bkl); bkl=0; }
static struct file *fd_get_ref(int fd) { assert(fd>=0&&fd<16); struct file *f=fds[fd]; if(f){f->refs++;gets++;}return f; }
static void vfs_close(struct file *f) { assert(f->refs>0);f->refs--;puts_++; }
static uint64_t poll_file_key(struct file *f) { assert(f->refs>0); key_reads++; return f->inode->key; }
static void kfree(void *p) { free(p); }
static unsigned leaf_poll(struct file *f) { assert(f->refs>0);return f->ready; }
static const struct file_ops leaf={true,true,0,leaf_poll}, slow_leaf={false,true,0,leaf_poll},
 wrapper={false,false,0,leaf_poll}, no_poll={true,false,0,NULL};
/* Production epoll_fops has a non-null ->poll. Without that, ep_file_poll()
 * takes its generic always-ready path before reaching the nested scanner. */
static const struct file_ops epoll_fops={true,false,0,leaf_poll};
'''
post = r'''
static struct inode ia={1},ib={2},iw={8};
static struct file fa={&ia,&leaf,NULL,0,0}, fb={&ib,&leaf,NULL,0,0}, fw={&iw,&wrapper,NULL,0,0};
static void init(struct epoll *e) {
 list_init(&e->items);e->mtx.held=false;
 struct list_node probe;list_init(&probe);list_add(&e->items,&probe);list_del(&probe);assert(list_empty(&e->items));
}
static struct epitem *add(struct epoll *e,int fd,struct file *f,uint32_t events) {
 struct epitem *i=calloc(1,sizeof *i);assert(i);i->fd=fd;i->f=f;i->events=events;i->data=100+fd;
 fds[fd]=f;list_add_tail(&e->items,&i->node);return i;
}
static void clear(struct epoll *e) { list_for_each_safe(i,t,&e->items){list_del(i);free(list_entry(i,struct epitem,node));} }
static int run(struct epoll *e,struct epoll_event_u *out,int max,struct ep_scan_ctx *x) {
 *x=(struct ep_scan_ctx){.depth=1};mutex_lock(&e->mtx);int n=ep_scan_locked(e,out,max,x);mutex_unlock(&e->mtx);
 if(x->took_bkl)bkl_exit();assert(gets==puts_&&!bkl&&fa.refs==0&&fb.refs==0&&fw.refs==0);return n;
}
int main(void) {
 int groups=0;struct epoll e,in;struct ep_scan_ctx x;struct epoll_event_u out[2];init(&e);init(&in);
 add(&e,0,&fa,EPOLLIN);add(&e,1,&fb,EPOLLIN);
 assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==3);clear(&e);groups++;
 add(&e,0,&fa,EPOLLIN);add(&e,2,&fa,EPOLLIN);
 assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==1);clear(&e);groups++;
 for(int order=0;order<2;order++){
  add(&e,0,order?&fa:&fw,EPOLLIN);add(&e,1,order?&fw:&fa,EPOLLIN);
  assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==UINT64_MAX);clear(&e);groups++;
 }
 for(int kind=0;kind<2;kind++){
  struct file f={&iw,kind?NULL:&no_poll,NULL,0,0};add(&e,3,&f,0);
  assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==UINT64_MAX&&f.refs==0);clear(&e);groups++;
 }
 struct epitem *it=add(&e,0,&fa,EPOLLIN);it->disabled=true;
 int before=key_reads;assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==UINT64_MAX&&key_reads==before);clear(&e);groups++;
 assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==UINT64_MAX);groups++;
 add(&e,0,&fa,EPOLLIN);fds[0]=&fb;
 assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==UINT64_MAX&&list_empty(&e.items));groups++;
 add(&in,0,&fa,EPOLLIN|EPOLLONESHOT);
 struct file nested={&iw,&epoll_fops,&in,0,0};add(&e,4,&nested,EPOLLIN);
 assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==UINT64_MAX&&x.depth==1&&nested.refs==0);groups++;
 fa.ready=EPOLLIN;
 assert(run(&e,out,2,&x)==1&&out[0].data==104&&out[0].events==EPOLLIN&&ep_wait_filter(&x)==UINT64_MAX);
 it=list_first(&in.items,struct epitem,node);assert(!it->disabled);clear(&e);clear(&in);fa.ready=0;groups++;
 fa.ready=EPOLLOUT;add(&e,0,&fa,EPOLLOUT|EPOLLET);
 assert(run(&e,out,2,&x)==1&&out[0].events==EPOLLOUT);
 assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==1);clear(&e);fa.ready=0;groups++;
 fa.ready=EPOLLIN;it=add(&e,0,&fa,EPOLLIN|EPOLLONESHOT);
 assert(run(&e,out,2,&x)==1&&it->disabled);
 assert(run(&e,out,2,&x)==0&&ep_wait_filter(&x)==UINT64_MAX);
 it->disabled=false;assert(run(&e,out,2,&x)==1&&out[0].data==100);clear(&e);fa.ready=0;groups++;
 fa.ready=fb.ready=EPOLLIN;it=add(&e,0,&fa,EPOLLIN|EPOLLONESHOT);struct epitem *second=add(&e,1,&fb,EPOLLIN|EPOLLONESHOT);
 assert(run(&e,out,1,&x)==1&&it->disabled&&!second->disabled);clear(&e);fa.ready=fb.ready=0;groups++;
 fa.fops=&slow_leaf;add(&e,0,&fa,EPOLLIN);add(&e,1,&fb,EPOLLIN);
 assert(run(&e,out,2,&x)==0&&x.took_bkl&&ep_wait_filter(&x)==3);clear(&e);fa.fops=&leaf;groups++;
 printf("epoll exact-scanner model: %d groups passed (mock locks/FDs only)\n",groups);return 0;
}
'''
mutations = {
 'normal': scan,
 'overwrite-leaf-union': scan.replace('x->filter |= poll_file_key(f)', 'x->filter = poll_file_key(f)', 1),
 'unsafe-wrapper': scan.replace('else x->filter = UINT64_MAX;', 'else x->filter |= poll_file_key(f);', 1),
 'unsafe-empty': scan.replace('return x->filter ? x->filter : UINT64_MAX;', 'return x->filter;', 1),
 'skip-disabled-guard': scan.replace('if (it->disabled)', 'if (false)', 1),
 'stale-fd-accepted': scan.replace('f && f != it->f', 'false', 1),
 'missing-et-latch': scan.replace('ready &= ~(it->last & latch)', 'ready &= ~0u', 1),
 'missing-oneshot': scan.replace('it->disabled = true', 'it->disabled = false', 1),
}
key_block = '        if (f->fops && f->fops->poll_leaf) x->filter |= poll_file_key(f);\n        else x->filter = UINT64_MAX;\n'
assert scan.count(key_block) == 1
mutations['key-after-release'] = scan.replace(key_block, '', 1).replace('        vfs_close(f);\n        unsigned ready', '        vfs_close(f);\n' + key_block + '        unsigned ready', 1)
def no_core(): resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='9os-epoll-filter-') as tmp:
 for name, source in mutations.items():
  assert name == 'normal' or source != scan
  c=Path(tmp)/(name+'.c');exe=Path(tmp)/name;c.write_text(pre+source+post)
  compile_result=subprocess.run([os.environ.get('CC','cc'),'-O2','-Wall','-Wextra','-Werror','-Wno-unused-const-variable','-Wno-misleading-indentation','-Wno-unused-variable',str(c),'-o',str(exe)],capture_output=True)
  if compile_result.returncode:
   raise SystemExit(f'{name}: compiler rejected exact-scanner fixture\n'+compile_result.stderr.decode())
  result=subprocess.run([str(exe)],capture_output=True,timeout=5,preexec_fn=no_core)
  if name=='normal':
   if result.returncode:raise SystemExit(result.stderr.decode())
   print(result.stdout.decode().strip())
  else:
   if result.returncode!=-6:raise SystemExit(f'{name}: expected assertion failure, got {result.returncode}')
   print('epoll scanner model: rejected '+name)
