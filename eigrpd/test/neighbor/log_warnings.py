from pathlib import Path
import subprocess, textwrap, sys
ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())

def test_neighbor_warning_rate_limit_with_fake_monotonic_time(tmp_path):
    c=tmp_path/'t.c'; o=tmp_path/'n.o'; b=tmp_path/'t'
    c.write_text(textwrap.dedent(r'''
    #include <assert.h>
    #include <string.h>
    #include <arpa/inet.h>
    #define EIGRP_TESTING 1
    #include "eigrpd.h"
    #include "eigrp_structs.h"
    #include "eigrp_neighbor.h"
    static void addr(eigrp_nbr_t *n,const char *s){memset(n,0,sizeof(*n));n->src.afi=AF_INET;assert(inet_pton(AF_INET,s,&n->src.ip.v4)==1);}
    int main(void){eigrp_instance_t a={0},b={0};eigrp_nbr_t n1,n2;addr(&n1,"192.0.2.1");addr(&n2,"192.0.2.2");a.log_neighbor_warnings=b.log_neighbor_warnings=true;a.log_neighbor_warning_interval=b.log_neighbor_warning_interval=10;
    assert(eigrp_nbr_warning_should_emit_test(&a,&n1,"maximum-prefix threshold",1000));
    assert(!eigrp_nbr_warning_should_emit_test(&a,&n1,"maximum-prefix threshold",5000));
    assert(eigrp_nbr_warning_should_emit_test(&a,&n1,"maximum-prefix threshold",11000));
    assert(eigrp_nbr_warning_should_emit_test(&a,&n2,"maximum-prefix threshold",11001));
    assert(eigrp_nbr_warning_should_emit_test(&a,&n1,"authentication failure",11002));
    assert(eigrp_nbr_warning_should_emit_test(&b,&n1,"maximum-prefix threshold",11003));
    a.log_neighbor_warnings=false;assert(!eigrp_nbr_warning_should_emit_test(&a,&n1,"new reason",20000));a.log_neighbor_warnings=true;
    eigrp_nbr_warning_state_clear(&a);assert(a.neighbor_warning_state==0);assert(eigrp_nbr_warning_should_emit_test(&a,&n1,"maximum-prefix threshold",20001));eigrp_nbr_warning_state_clear(&a);eigrp_nbr_warning_state_clear(&b);return 0;}
    '''))
    flags=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-DEIGRP_DISABLE_SNMP','-DEIGRP_TESTING','-ffunction-sections','-fdata-sections',f'-I{ROOT}/eigrpd/code']
    r=subprocess.run(flags+['-c',str(ROOT/'eigrpd/code/eigrp_neighbor.c'),'-o',str(o)],cwd=ROOT,text=True,capture_output=True); assert r.returncode==0,r.stderr
    dead_strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    r=subprocess.run(flags+[str(c),str(o),dead_strip,'-o',str(b)],cwd=ROOT,text=True,capture_output=True); assert r.returncode==0,r.stderr
    r=subprocess.run([str(b)],cwd=ROOT,text=True,capture_output=True); assert r.returncode==0,r.stderr

def test_warning_configuration_and_teardown():
    n=(ROOT/'eigrpd/code/eigrp_neighbor.c').read_text(); body=n[n.index('eigrp_result_t eigrp_nbr_log_update'):n.index('void eigrp_nbr_policy_delete_all')]
    assert 'EIGRP_RESULT_NOT_IMPLEMENTED' not in body
    assert 'log_neighbor_warning_interval = 10' in body
    assert 'eigrp_nbr_warning_state_clear(context->runtime)' in body
    assert '"maximum-prefix threshold"' in n
    assert 'eigrp_nbr_warning_state_clear(eigrp);' in (ROOT/'eigrpd/code/eigrpd.c').read_text()
