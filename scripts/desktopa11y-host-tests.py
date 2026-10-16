#!/usr/bin/env python3
"""Observer rejection controls using an explicit mock, NOT real GUI evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parent.parent
MOCK=r'''
#include <stdlib.h>
#include <string.h>
static int root,app,model,leaf,menu;
static int mode(const char *s){const char*m=getenv("MOCK");return m&&!strcmp(m,s);}
int atspi_init(void){return 0;}int atspi_exit(void){return 0;}
void atspi_set_timeout(int a,int b){(void)a;(void)b;}
void *atspi_get_desktop(int n){(void)n;return &root;}
int atspi_accessible_get_role(void*a,void**e){(void)e;if(a==&model)return mode("wrong-role")?0:31;if(a==&leaf)return mode("bad-row-role")?0:32;if(a==&menu)return 33;return 0;}
char *atspi_accessible_get_description(void*a,void**e){(void)a;(void)e;return strdup(mode("wrong-directory")?"in location file:///tmp/other/files":mode("directory-prefix")?"in location file:///tmp/test/files-other":mode("directory-suffix")?"in location file:///wrong/tmp/test/files":"in location file:///tmp/test/files");}
int atspi_accessible_get_child_count(void*a,void**e){
 if(a==&root)return 1;
 if(mode("error")&&a==&model){*e=&leaf;return 0;}
 if(mode("oversize")&&a==&model)return 513;
 if(a==&app)return mode("menu-decoy")?2:1;
 if(a==&model)return mode("menu-decoy")?0:1;
 return a==&menu;
}
void *atspi_accessible_get_child_at_index(void*a,int n,void**e){
 (void)e;if(a==&root)return &app;
 if(a==&app){if(mode("cycle"))return &app;return mode("menu-decoy")&&n==0?&menu:&model;}
 return &leaf;
}
unsigned atspi_accessible_get_process_id(void*a,void**e){(void)a;(void)e;return 42;}
char *atspi_accessible_get_name(void*a,void**e){(void)e;return strdup(a==&leaf?"before.txt":a==&model?"files":"application");}
void g_object_unref(void*p){(void)p;}void g_free(void*p){free(p);}void g_error_free(void*p){(void)p;}
'''
with tempfile.TemporaryDirectory(prefix='9os-a11y-unit-') as tmp:
 d=Path(tmp);(d/'mock.c').write_text(MOCK)
 subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra',str(d/'mock.c'),'-o',str(d/'libatspi.so.0')],check=True)
 subprocess.run(['cc','-O2','-Wall','-Wextra',str(ROOT/'userland/tests/desktopa11y.c'),'-ldl','-o',str(d/'test')],check=True)
 cases=[('valid','42','/tmp/test/files',['+before.txt','-after.txt'],0,''),
        ('missing','42','/tmp/test/files',['+after.txt'],1,''),
        ('stale','42','/tmp/test/files',['+before.txt','-before.txt'],1,''),
        ('wrong-pid','43','/tmp/test/files',['+before.txt'],1,''),
        ('prefix-not-equal','42','/tmp/test/files',['+before'],1,''),
        ('invalid-pid','42x','/tmp/test/files',['+before.txt'],2,''),
        ('invalid-directory','42','relative/files',['+before.txt'],2,''),
        ('bad-query','42','/tmp/test/files',['before.txt'],2,''),
        ('remote-error','42','/tmp/test/files',['+before.txt'],1,'error'),
        ('oversized-model','42','/tmp/test/files',['+before.txt'],1,'oversize'),
        ('cyclic-tree','42','/tmp/test/files',['+before.txt'],1,'cycle'),
        ('wrong-role','42','/tmp/test/files',['+before.txt'],1,'wrong-role'),
        ('bad-row-role','42','/tmp/test/files',['+before.txt'],1,'bad-row-role'),
        ('wrong-directory','42','/tmp/test/files',['+before.txt'],1,'wrong-directory'),
        ('directory-prefix','42','/tmp/test/files',['+before.txt'],1,'directory-prefix'),
        ('directory-suffix','42','/tmp/test/files',['+before.txt'],1,'directory-suffix'),
        ('menu-decoy','42','/tmp/test/files',['+before.txt'],1,'menu-decoy')]
 for label,pid,directory,args,rc,mode in cases:
  p=subprocess.run([str(d/'test'),pid,directory,*args],env={**os.environ,'LD_LIBRARY_PATH':tmp,'MOCK':mode},capture_output=True,text=True,timeout=5)
  assert p.returncode==rc,(label,p.returncode,p.stdout,p.stderr)
  print('PASS',label)
 print(f'{len(cases)}/{len(cases)} observer logic controls (mock only; no GUI claim)')
