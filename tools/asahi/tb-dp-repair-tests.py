#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real DP retirement/repair bodies with modeled paths, PM and a queue.

This checks ordering and reference balance, not kernel structure layout, hardware,
real runtime-PM locking or a complete Thunderbolt workqueue implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(text, name):
    match = re.search(r'^(?:static\s+)?(?:inline\s+)?(?:int|void|bool)\s+' +
                      name + r'\([^;]*?\)\n\{', text, re.M | re.S)
    if not match:
        raise ValueError('Missing actual function: ' + name)
    pos = text.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[match.start():pos] + '\n'


HEADER = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CHECK line %d: %s\n",__LINE__,#x); exit(77); } } while (0)
struct list_head {int unused;};
struct device {int unused;};
struct tb_switch {struct device dev;};
struct tb_port {struct tb_switch *sw;bool is_dpin;struct list_head list;};
struct work_struct {void(*fn)(struct work_struct*);bool pending;};
struct tb {int lock;struct device dev;void*wq;struct tb_switch*root_switch;};
struct tb_cm {bool hotplug_active;struct work_struct dp_tunnel_work;bool dp_tunnel_pending;};
struct domain {struct tb tb;struct tb_cm cm;};
_Static_assert(offsetof(struct domain,cm)==sizeof(struct tb),"mock private storage adjacency");
#define INIT_WORK(w,handler) ((w)->fn=(handler))
struct kref {int count;};
struct tb_path {bool reserved;};
enum {TB_TUNNEL_DP,TB_TUNNEL_USB3};
struct tb_tunnel {struct kref kref;struct tb*tb;struct tb_port*src_port,*dst_port;int npaths;void(*destroy)(struct tb_tunnel*);struct list_head list;int type;bool dprx_started;void(*callback)(struct tb_tunnel*,void*);void*callback_data;struct tb_path*paths[3];};
static int tb_tunnel_lock,occupied,alloc_calls,alloc_failures,alloc_successes,freed;
static int domain_refs,pm_refs,pm_calls,pm_error,stop_during_pm,requeue_once,lock_depth;
static struct tb_tunnel *current;
static struct domain domain;
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define READ_ONCE(x) (x)
#define fallthrough __attribute__((fallthrough))
#define tb_port_dbg(...) ((void)0)
static struct tb_cm*tb_priv(struct tb*t){return (struct tb_cm*)((char*)t+sizeof(*t));}
static struct tb*tcm_to_tb(struct tb_cm*c){return (struct tb*)((char*)c-sizeof(struct tb));}
static void mutex_lock(int*x){if(x==&domain.tb.lock){CHECK(!lock_depth);lock_depth++;}}
static void mutex_unlock(int*x){if(x==&domain.tb.lock){CHECK(lock_depth==1);lock_depth--;}}
#define lockdep_assert_held(x) CHECK((x)==&domain.tb.lock && lock_depth==1)
static void kref_get(struct kref*k){CHECK(k->count>0);k->count++;}
static void kref_put(struct kref*k,void(*destroy)(struct kref*)){CHECK(k->count>0);if(!--k->count)destroy(k);}
static void tb_path_free(struct tb_path*p){if(p->reserved)occupied--;freed++;free(p);}
#define kfree(x) free(x)
static void list_del(struct list_head*l){current=NULL;}
static void list_del_init(struct list_head*l){}
static bool tb_port_is_dpin(struct tb_port*p){return p->is_dpin;}
static void tb_dp_tunnel_notify(struct tb_tunnel*t,bool active){}
static void tb_tunnel_deactivate(struct tb_tunnel*t){}
static void tb_detach_bandwidth_group(struct tb_port*p){}
static void tb_switch_dealloc_dp_resource(struct tb_switch*s,struct tb_port*p){}
static void tb_configure_sym(struct tb*t,struct tb_port*i,struct tb_port*o,bool active){}
static void pm_runtime_mark_last_busy(struct device*d){}
static void pm_runtime_put_autosuspend(struct device*d){if(d==&domain.tb.dev){CHECK(pm_refs==1);pm_refs--;}}
static int pm_runtime_resume_and_get(struct device*d){
 CHECK(!lock_depth);pm_calls++;
 if(pm_error)return pm_error;
 pm_refs++;
 if(stop_during_pm)domain.cm.hotplug_active=false;
 return 0;
}
static void tb_reclaim_usb3_bandwidth(struct tb*t,struct tb_port*i,struct tb_port*o){}
static struct tb_tunnel*tb_find_tunnel(struct tb*t,int type,struct tb_port*in,struct tb_port*out){return current;}
static void tb_enter_redrive(struct tb_port*p){}
static void tb_recalc_estimated_bandwidth(struct tb*t){}
static struct tb*tb_domain_get(struct tb*t){CHECK(domain_refs>=1);domain_refs++;return t;}
static void tb_domain_put(struct tb*t){CHECK(domain_refs>1);domain_refs--;}
static bool queue_work(void*wq,struct work_struct*w){if(w->pending)return false;w->pending=true;return true;}
static void tb_queue_dp_tunnels(struct tb*tb);
static void tb_tunnel_dp(struct tb*t){
 CHECK(lock_depth==1 && domain.cm.hotplug_active && t->root_switch);
 alloc_calls++;if(occupied)alloc_failures++;else alloc_successes++;
 if(requeue_once){requeue_once=0;tb_queue_dp_tunnels(t);}
}
'''

FOOTER = r'''
static void failed_callback(struct tb_tunnel*t,void*data){tb_dp_resource_unavailable(data,t->src_port,"DPRX negotiation failed");}
static struct tb_tunnel*new_tunnel(struct tb*tb,struct tb_port*in,struct tb_port*out){
 struct tb_tunnel*t=calloc(1,sizeof(*t));CHECK(t);t->tb=tb;t->src_port=in;t->dst_port=out;t->type=TB_TUNNEL_DP;t->kref.count=1;t->npaths=1;
 t->paths[0]=calloc(1,sizeof(struct tb_path));CHECK(t->paths[0]);t->paths[0]->reserved=true;occupied++;t->callback=failed_callback;t->callback_data=tb;current=t;return t;
}
static void flush_queue(void){
 unsigned n=0;
 while(domain.cm.dp_tunnel_work.pending){
  CHECK(++n<8);CHECK(domain.cm.dp_tunnel_work.fn);domain.cm.dp_tunnel_work.pending=false;
  domain.cm.dp_tunnel_work.fn(&domain.cm.dp_tunnel_work);
 }
 CHECK(!lock_depth && !pm_refs && domain_refs==1);
 if(alloc_successes)CHECK(!domain.cm.dp_tunnel_pending);
}
static void queue_repair(unsigned count){
 mutex_lock(&domain.tb.lock);
 for(unsigned i=0;i<count;i++)tb_queue_dp_tunnels(&domain.tb);
 mutex_unlock(&domain.tb.lock);
}
int main(int argc,char**argv){
 CHECK(argc==2);
 struct tb_switch sw={0};struct tb_port in={.sw=&sw,.is_dpin=true},out={.sw=&sw};
 domain_refs=1;domain.tb.root_switch=&sw;domain.tb.wq=&domain;domain.cm.hotplug_active=true;
 init_fixture(&domain.cm);CHECK(domain.cm.dp_tunnel_work.fn);
 const char*c=argv[1];
 if(!strcmp(c,"callback-release-before-repair")){
  struct tb_tunnel*t=new_tunnel(&domain.tb,&in,&out);
  tb_tunnel_get(t);tb_domain_get(&domain.tb);mutex_lock(&domain.tb.lock);
  worker_tail(&domain.tb,t);
  CHECK(domain.cm.dp_tunnel_work.pending && !alloc_calls && !occupied && freed==1 && domain_refs==2);
  flush_queue();CHECK(alloc_calls==1 && !alloc_failures && alloc_successes==1);
 }else if(!strcmp(c,"coalesce")){
  queue_repair(20);CHECK(domain_refs==2);flush_queue();CHECK(alloc_calls==1);
 }else if(!strcmp(c,"stop-before-queue")){
  domain.cm.hotplug_active=false;queue_repair(1);CHECK(!domain.cm.dp_tunnel_work.pending && domain_refs==1);flush_queue();CHECK(!alloc_calls && !pm_calls);
 }else if(!strcmp(c,"stop-after-queue")){
  queue_repair(1);domain.cm.hotplug_active=false;domain.tb.root_switch=NULL;flush_queue();CHECK(!alloc_calls && !pm_calls);
 }else if(!strcmp(c,"stop-during-pm")){
  queue_repair(1);stop_during_pm=1;flush_queue();CHECK(!alloc_calls && pm_calls==1);
 }else if(!strcmp(c,"no-root")){
  queue_repair(1);domain.tb.root_switch=NULL;flush_queue();CHECK(!alloc_calls && pm_calls==1);
 }else if(!strcmp(c,"pm-failure")){
  queue_repair(1);pm_error=-5;flush_queue();CHECK(!alloc_calls && pm_calls==1);
 }else if(!strcmp(c,"requeue-running")){
  queue_repair(1);requeue_once=1;flush_queue();CHECK(alloc_calls==2 && alloc_successes==2);
 }else if(!strcmp(c,"freeze-thaw-repair")){
  queue_repair(1);mutex_lock(&domain.tb.lock);CHECK(!tb_freeze_noirq(&domain.tb));mutex_unlock(&domain.tb.lock);
  flush_queue();CHECK(!alloc_calls && !pm_calls && domain.cm.dp_tunnel_pending);
  mutex_lock(&domain.tb.lock);CHECK(!tb_thaw_noirq(&domain.tb));mutex_unlock(&domain.tb.lock);
  CHECK(domain.cm.dp_tunnel_work.pending && domain_refs==2);flush_queue();CHECK(alloc_successes==1);
 }else if(!strcmp(c,"pm-failure-retry")){
  queue_repair(1);pm_error=-5;flush_queue();CHECK(!alloc_calls && domain.cm.dp_tunnel_pending);
  pm_error=0;mutex_lock(&domain.tb.lock);CHECK(!tb_thaw_noirq(&domain.tb));mutex_unlock(&domain.tb.lock);
  flush_queue();CHECK(alloc_successes==1 && pm_calls==2);
 }else if(!strcmp(c,"ordinary-retirement")){
  new_tunnel(&domain.tb,&in,&out);mutex_lock(&domain.tb.lock);
  tb_dp_resource_unavailable(&domain.tb,&in,"ordinary detach");mutex_unlock(&domain.tb.lock);
  CHECK(!occupied && freed==1 && !alloc_calls);flush_queue();CHECK(alloc_successes==1);
 }else return 78;
 printf("PASS %s\n",c);return 0;
}
'''

CASES = ['callback-release-before-repair', 'coalesce', 'stop-before-queue',
         'stop-after-queue', 'stop-during-pm', 'no-root', 'pm-failure',
         'requeue-running', 'ordinary-retirement', 'freeze-thaw-repair',
         'pm-failure-retry']
MUTANTS = {
    'inline-repair': ('\ttb_recalc_estimated_bandwidth(tb);\n\ttb_queue_dp_tunnels(tb);',
                      '\ttb_recalc_estimated_bandwidth(tb);\n\ttb_tunnel_dp(tb);'),
    'missing-queue-reference': ('\ttb_domain_get(tb);', '\t/* removed get */'),
    'coalescing-leaks-reference': ('\tif (!queue_work(tb->wq, &tcm->dp_tunnel_work))\n\t\ttb_domain_put(tb);',
                                 '\tqueue_work(tb->wq, &tcm->dp_tunnel_work);'),
    'missing-stop-guard': ('\tif (!READ_ONCE(tcm->hotplug_active))', '\tif (false)'),
    'missing-post-resume-guard': ('\tif (tcm->hotplug_active && tb->root_switch)', '\tif (tb->root_switch)'),
    'missing-root-guard': ('\tif (tcm->hotplug_active && tb->root_switch)', '\tif (tcm->hotplug_active)'),
    'missing-pm-error-guard': ('\tif (ret < 0)', '\tif (false)'),
    'missing-pm-put': ('\tpm_runtime_put_autosuspend(&tb->dev);', '\t/* removed PM put */'),
    'missing-probe-init': ('INIT_WORK(&tcm->dp_tunnel_work, tb_dp_tunnel_work);',
                           '/* removed probe initialization */'),
    'missing-pending-request': ('\ttcm->dp_tunnel_pending = true;', '\t/* lost request */'),
    'pending-never-consumed': ('\t\ttcm->dp_tunnel_pending = false;', '\t\t/* pending forever */'),
    'missing-thaw-requeue': ('\tif (tcm->dp_tunnel_pending)\n\t\ttb_queue_dp_tunnels(tb);',
                             '\t/* lost thaw request */'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    tb = (ROOT / 'drivers/thunderbolt/tb.c').read_text()
    tunnel = (ROOT / 'drivers/thunderbolt/tunnel.c').read_text()
    names = ['tb_tunnel_get', 'tb_tunnel_destroy', 'tb_tunnel_put']
    parts = {n: function(tunnel, n) for n in names}
    for name in ['tb_deactivate_and_free_tunnel', 'tb_dp_tunnel_work',
                 'tb_queue_dp_tunnels', 'tb_dp_resource_unavailable',
                 'tb_freeze_noirq', 'tb_thaw_noirq']:
        parts[name] = function(tb, name)
    worker = function(tunnel, 'tb_dp_dprx_work')
    marker = '\ttunnel->dprx_started = false;'
    if worker.count(marker) != 1:
        raise ValueError('DPRX completion tail changed; review extraction')
    tail = worker[worker.index(marker):].rstrip()[:-1]
    production = '\n'.join(parts.values())
    probe = (ROOT / 'drivers/thunderbolt/tb.c').read_text()
    probe = probe[probe.index('struct tb *tb_probe(struct tb_nhi *nhi)'):]
    initializers = re.findall(r'INIT_WORK\(&tcm->\w+,\s*\w+\);', probe)
    production += '\nstatic void init_fixture(struct tb_cm*tcm){\n' + '\n'.join(initializers) + '\n}\n'
    results = {}
    with tempfile.TemporaryDirectory(prefix='tb-dp-repair-') as tmp:
        out = args.output or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        variants = {'real': production}
        if args.mutations:
            for name, (old, new) in MUTANTS.items():
                if production.count(old) != 1:
                    raise ValueError('Mutation site changed: ' + name)
                variants[name] = production.replace(old, new, 1)
        for name, body in variants.items():
            unit = out / (name + '.c')
            unit.write_text(HEADER + '#include <string.h>\n' + body +
                            '\nstatic void worker_tail(struct tb*tb,struct tb_tunnel*tunnel){\n' +
                            tail + '\n}\n' + FOOTER)
            executable = out / name
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', '-Wno-unused-function',
                            '-Wno-unused-but-set-variable', '-g',
                            '-fsanitize=address,undefined', str(unit), '-o', str(executable)],
                           check=True)
            runs = []
            for case in CASES:
                run = subprocess.run([str(executable), case], capture_output=True, text=True)
                runs.append({'case': case, 'rc': run.returncode,
                             'stdout': run.stdout, 'stderr': run.stderr})
            failures = [r for r in runs if r['rc'] != 0]
            if name == 'real' and failures:
                raise RuntimeError(json.dumps(failures))
            if name != 'real' and not any(r['rc'] == 77 for r in failures):
                raise RuntimeError('Mutant not caught by a valid runtime assertion: ' + name)
            results[name] = runs
        receipt = {'scope': __doc__, 'production_bodies': {
            n: hashlib.sha256(body.encode()).hexdigest() for n, body in parts.items()},
            'worker_tail_sha256': hashlib.sha256(tail.encode()).hexdigest(),
            'probe_initializers': initializers,
            'results': results}
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps({'cases': len(CASES), 'passed': True,
                          'mutants_caught': len(variants) - 1, 'output': str(out)}))


if __name__ == '__main__':
    main()
