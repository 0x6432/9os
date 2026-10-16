#!/usr/bin/env python3
"""Unit-test observer rejection logic with an explicit mock, NOT GUI evidence."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MOCK = r'''
#include <stdlib.h>
#include <string.h>
static int root,app,leaf;
int atspi_init(void){return 0;}int atspi_exit(void){return 0;}
void atspi_set_timeout(int a,int b){(void)a;(void)b;}
void *atspi_get_desktop(int n){(void)n;return &root;}
int atspi_accessible_get_child_count(void *a,void **e){
 const char *m=getenv("MOCK");
 if(a==&root)return 1;
 if(m&&!strcmp(m,"error")&&a==&app){*e=&leaf;return 0;}
 if(m&&!strcmp(m,"oversize")&&a==&app)return 513;
 if(m&&!strcmp(m,"cycle")&&a!=&root)return 1;
 return a==&app;
}
void *atspi_accessible_get_child_at_index(void *a,int n,void **e){
 (void)n;(void)e;return a==&root?&app:&leaf;
}
unsigned atspi_accessible_get_process_id(void *a,void **e){(void)a;(void)e;return 42;}
char *atspi_accessible_get_name(void *a,void **e){(void)e;return strdup(a==&leaf?"before.txt":"application");}
void g_object_unref(void *p){(void)p;}void g_free(void*p){free(p);}void g_error_free(void*p){(void)p;}
'''
with tempfile.TemporaryDirectory(prefix='9os-a11y-unit-') as tmp:
    d=Path(tmp);(d/'mock.c').write_text(MOCK)
    subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra',str(d/'mock.c'),'-o',str(d/'libatspi.so.0')],check=True)
    subprocess.run(['cc','-O2','-Wall','-Wextra',str(ROOT/'userland/tests/desktopa11y.c'),'-ldl','-o',str(d/'test')],check=True)
    cases=[('valid',['42','+before.txt','-after.txt'],0,''),
           ('missing',['42','+after.txt'],1,''),
           ('stale',['42','+before.txt','-before.txt'],1,''),
           ('wrong-pid',['43','+before.txt'],1,''),
           ('prefix-not-equal',['42','+before'],1,''),
           ('invalid-pid',['42x','+before.txt'],2,''),
           ('bad-query',['42','before.txt'],2,''),
           ('remote-error',['42','+before.txt'],1,'error'),
           ('oversized-tree',['42','+before.txt'],1,'oversize'),
           ('cyclic-tree',['42','+before.txt'],1,'cycle')]
    for label,args,rc,mode in cases:
        env={**os.environ,'LD_LIBRARY_PATH':tmp,'MOCK':mode}
        p=subprocess.run([str(d/'test'),*args],env=env,capture_output=True,text=True,timeout=5)
        assert p.returncode==rc,(label,p.returncode,p.stdout,p.stderr)
        print('PASS',label)
    print(f'{len(cases)}/{len(cases)} observer logic controls (mock only; no GUI claim)')
