#!/usr/bin/env python3
"""Mock AT-SPI ABI controls; not physical focus or guest delivery proof."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'userland/tests/desktopa11y.c'
MOCK = r'''
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
static int mode(const char *s){const char *m=getenv("MOCK_MODE");return m&&!strcmp(m,s);}
int atspi_init(void){return 0;} int atspi_exit(void){return 0;}
void atspi_set_timeout(int a,int b){(void)a;(void)b;}
void *atspi_get_desktop(int n){(void)n;return (void*)1;}
unsigned atspi_accessible_get_process_id(void *a,void **e){(void)a;(void)e;return mode("wrong-pid")?101:100;}
int atspi_accessible_get_child_count(void *a,void **e){(void)e;return mode("oversized")?513:((intptr_t)a<3?1:0);}
void *atspi_accessible_get_child_at_index(void *a,int n,void **e){(void)n;(void)e;return mode("null-child")?0:(void*)((intptr_t)a+1);}
int atspi_accessible_get_role(void *a,void **e){(void)e;if(mode("blocked-role"))sleep(2);return (intptr_t)a==3?(mode("wrong-role")?61:60):0;}
char *atspi_accessible_get_name(void *a,void **e){(void)a;(void)e;return strdup("not a model");}
char *atspi_accessible_get_description(void *a,void **e){(void)a;(void)e;return strdup("");}
void *atspi_accessible_get_state_set(void *a){(void)a;return mode("null-state")?0:(void*)4;}
int atspi_state_set_contains(void *s,int n){(void)s;return !((n==12&&mode("unfocused"))||(n==25&&mode("hidden"))||(n==30&&mode("invisible")));}
void g_object_unref(void *a){(void)a;} void g_free(void *a){free(a);} void g_error_free(void *e){free(e);}
'''

class TerminalFocus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.path=Path(cls.tmp.name)
        (cls.path/'mock.c').write_text(MOCK)
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(SOURCE),'-ldl','-o',str(cls.path/'observe')],check=True)
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(cls.path/'mock.c'),'-o',str(cls.path/'libatspi.so.0')],check=True)
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def run_mode(self, mode, pid='100', trace=False):
        return subprocess.run([str(self.path/'observe'),'--terminal-trace' if trace else '--terminal',pid],env={**os.environ,'LD_LIBRARY_PATH':str(self.path),'MOCK_MODE':mode},capture_output=True,text=True,timeout=5)
    def test_observed_focused_visible_terminal(self):
        r=self.run_mode('ready');self.assertEqual(r.returncode,0,r.stderr);self.assertIn('focused=1 showing=1 visible=1',r.stdout)
    def test_inactive_or_incomplete_observations_rejected(self):
        for mode in ['wrong-pid','wrong-role','unfocused','hidden','invisible','null-state','oversized','null-child']:
            with self.subTest(mode=mode): self.assertEqual(self.run_mode(mode).returncode,1)
    def test_trace_keeps_readiness_criteria(self):
        for mode in ['ready','wrong-pid','wrong-role','unfocused','hidden','invisible','null-state','oversized','null-child']:
            with self.subTest(mode=mode):
                plain=self.run_mode(mode);traced=self.run_mode(mode,trace=True)
                self.assertEqual(plain.returncode,traced.returncode)
                self.assertNotIn('A11Y_TRACE ',plain.stdout)
                self.assertIn('A11Y_TRACE query=init phase=begin',traced.stdout)
                self.assertIn('A11Y_TRACE query=exit phase=end',traced.stdout)
                if mode=='ready':
                    self.assertIn('query=states phase=end depth=1',traced.stdout)
                    self.assertIn('focused=1 showing=1 visible=1',traced.stdout)
    def test_trace_identifies_unfinished_query_not_unfocused_state(self):
        command=[str(self.path/'observe'),'--terminal-trace','100']
        env={**os.environ,'LD_LIBRARY_PATH':str(self.path),'MOCK_MODE':'blocked-role'}
        with subprocess.Popen(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) as p:
            try:
                p.communicate(timeout=.5)
                self.fail('mock query did not remain blocked')
            except subprocess.TimeoutExpired:
                p.kill();out,err=p.communicate()
                self.assertIn('query=role phase=begin',out)
                self.assertNotIn('query=role phase=end',out)
                self.assertNotIn('A11Y_TERMINAL ',out)
    def test_invalid_pid_rejected_before_bus(self):
        for pid in ['', '0', '-1', '100garbage', '2147483648']:
            for trace in [False,True]:
                with self.subTest(pid=pid,trace=trace): self.assertEqual(self.run_mode('ready',pid,trace=trace).returncode,2)

if __name__=='__main__':unittest.main()
