# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Actual PM/HCD/mux/probe boundaries using the shared PHY extraction fixture."""
from pathlib import Path
import os, re, subprocess
import phy
from common import block

def ownership(root, usb_unreachable=False):
    ns=phy.parking(root)
    s=ns['atc'];has_pm='static int atcphy_usb3_pm_get(' in s
    prod=ns['production'].replace('struct apple_atcphy {','struct apple_atcphy {\n    struct device_node *np;',1)
    if has_pm: prod=prod.replace('static int atcphy_mux_set(', 'static int atcphy_mux_set_locked(',1)
    # Non-mux fixture entry points continue to exercise actual source. Only the new
    # wrapper owns PM; the fixture's original helpers call it through this declaration.
    prefix=ns['prefix']+'\nstruct device_node;\nstatic int atcphy_mux_set(struct typec_mux_dev *, struct typec_mux_state *);\n'
    prefix=prefix.replace('struct typec_mux_dev { struct apple_atcphy *data; };',
     '''struct typec_mux_dev { struct apple_atcphy *data;
 int (*set)(struct typec_mux_dev *, struct typec_mux_state *); };
 struct typec_mux { unsigned int num_mux_devs; struct typec_mux_dev **mux_devs; };
 #define IS_ERR_OR_NULL(p) (!(p))''')
    prefix=prefix.replace('#define guard(kind) (void)',r'''
struct lock_guard { int *lock; };
static struct lock_guard lock_take(int *lock) { assert(!*lock); *lock=1; return (struct lock_guard){lock}; }
static void lock_release(struct lock_guard *g) { assert(*g->lock==1); *g->lock=0; }
#define guard(kind) struct lock_guard __attribute__((cleanup(lock_release))) _guard = lock_take
''')
    # Basic USB/device APIs are modeled, but get/put/wrapper and all parking/
    # restoration bodies are extracted verbatim from the chosen public source.
    pmfixture=r'''
#define CONFIG_USB 1
#define READ_ONCE(x) (x)
#define IS_REACHABLE(x) USB_REACHABLE
struct device_node { int id; };
struct device { int refs; };
struct platform_device { struct device dev; };
struct usb_device;
struct usb_hcd;
struct usb_bus { struct device *sysdev, *controller; struct usb_hcd *hcd; struct usb_device *root_hub; };
struct usb_hcd { unsigned int rh_registered:1;
 struct usb_bus self; unsigned long flags;
 struct usb_hcd *shared_hcd,*primary_hcd; struct {bool pending;} died_work; };
enum { HCD_FLAG_RH_RUNNING,HCD_FLAG_DEAD,HCD_FLAG_POLL_RH,HCD_FLAG_HW_ACCESSIBLE };
static int __attribute__((unused)) hcd_root_hub_lock;
#define spin_lock_irqsave(lock, flags) do { assert(!*(lock));*(lock)=1;(flags)=0; } while(0)
#define spin_unlock_irqrestore(lock, flags) do { assert(*(lock));*(lock)=0;(void)(flags); } while(0)
#define clear_bit(bit, p) (*(p)&=~(1UL<<(bit)))
#define set_bit(bit, p) (*(p)|=1UL<<(bit))
#define usb_hcd_is_primary_hcd(hcd) (!(hcd)->primary_hcd||(hcd)->primary_hcd==(hcd))
#define usb_set_device_state(u, s) ((u)->state=(s))
#define usb_kick_hub_wq(u) ((void)(u))
#define schedule_work(w) ((w)->pending=true)
#define bus_to_hcd(bus) ((bus)->hcd)
#define HCD_DEAD(hcd) (!!((hcd)->flags&(1UL<<HCD_FLAG_DEAD)))
#define HCD_HW_ACCESSIBLE(hcd) (!!((hcd)->flags&(1UL<<HCD_FLAG_HW_ACCESSIBLE)))
struct usb_device { struct usb_device *parent; int speed,state,refs; struct usb_bus *bus; struct device dev; };
enum { USB_SPEED_SUPER=4, USB_STATE_NOTATTACHED=0 };
static struct device_node ep_node,ctrl_node;
static struct platform_device controller;
static struct device wrong_controller;
static struct usb_bus bus,wrong_bus;
static struct usb_hcd hcd;
static struct usb_device roots[4];
static struct apple_atcphy *pm_phy;
static bool root_locked, graph_missing, controller_missing, root_missing;
static int node_refs, pm_refs, resumes, resume_error, detach_on_lock, detach_on_resume;
static int inaccessible_on_resume, dead_on_resume;
static void node_put(struct device_node **n) { if(*n) node_refs--; }
#define __free(x) __attribute__((cleanup(node_put)))
static struct device_node *of_graph_get_endpoint_by_regs(struct device_node *n,int p,int e) { (void)n; assert(p==1&&e==-1); if(graph_missing)return NULL;node_refs++;return &ep_node; }
static struct device_node *of_graph_get_remote_port_parent(struct device_node *n) { assert(n==&ep_node);node_refs++;return &ctrl_node; }
static struct platform_device *of_find_device_by_node(struct device_node *n) { assert(n==&ctrl_node);if(controller_missing)return NULL;controller.dev.refs++;return &controller; }
static void put_device(struct device *d) { assert(d->refs>0);d->refs--; }
static struct usb_device *usb_get_dev(struct usb_device *u) { u->refs++;return u; }
static void usb_put_dev(struct usb_device *u) { assert(u->refs>0);u->refs--; }
static int usb_for_each_dev(void *data,int (*fn)(struct usb_device *,void *)) { if(root_missing)return 0;for(unsigned int i=0;i<4;i++){int r=fn(&roots[i],data);if(r)return r;}return 0; }
static void usb_lock_device(struct usb_device *u) { assert(u==&roots[3]&&!root_locked);root_locked=true;if(detach_on_lock)u->state=0; }
static void usb_unlock_device(struct usb_device *u) { assert(u==&roots[3]&&root_locked);root_locked=false; }
static int pm_runtime_resume_and_get(struct device *d) {
 assert(d==&roots[3].dev&&root_locked&&!pm_phy->lock);resumes++;
 /* A re-entrant PHY lock acquisition during resume must remain possible. */
 struct lock_guard g=lock_take(&pm_phy->lock);lock_release(&g);
 if(resume_error)return resume_error;
 pm_refs++;stuck_ack=false;if(detach_on_resume)roots[3].state=0;
 if(inaccessible_on_resume)clear_bit(HCD_FLAG_HW_ACCESSIBLE,&hcd.flags);
 if(dead_on_resume)set_bit(HCD_FLAG_DEAD,&hcd.flags);
 return 0;
}
static void pm_runtime_put_noidle(struct device *d) { assert(d==&roots[3].dev&&pm_refs==1);pm_refs--; }
static void usb_mark_last_busy(struct usb_device *u) { assert(u==&roots[3]&&root_locked&&!pm_phy->lock); }
static void pm_runtime_put_autosuspend(struct device *d) { assert(d==&roots[3].dev&&root_locked&&!pm_phy->lock&&pm_refs==1);pm_refs--; }
'''
    suffix=ns['suffix'].replace('int main(', 'int __attribute__((unused)) fixture_main(',1)
    controls=r'''
static void prepare(struct apple_atcphy *p,bool fixed,bool t8140,bool swap)
{
    hw.has_t8140_dp=t8140;initial(p,fixed,false,swap);
    p->np=&ctrl_node;pm_phy=p;
    hcd=(struct usb_hcd){.rh_registered=true,.flags=1UL<<HCD_FLAG_HW_ACCESSIBLE};
    bus.sysdev=&controller.dev;bus.hcd=&hcd;wrong_bus.sysdev=&wrong_controller;
    roots[0]=(struct usb_device){.speed=3,.state=1,.bus=&bus};
    roots[1]=(struct usb_device){.parent=&roots[0],.speed=4,.state=1,.bus=&bus};
    roots[2]=(struct usb_device){.speed=4,.state=1,.bus=&wrong_bus};
    roots[3]=(struct usb_device){.speed=5,.state=1,.bus=&bus};
    hcd.self.root_hub=&roots[3];hcd.self.controller=&controller.dev;
    root_locked=graph_missing=controller_missing=root_missing=false;
    node_refs=pm_refs=resumes=resume_error=detach_on_lock=detach_on_resume=0;controller.dev.refs=0;
    inaccessible_on_resume=dead_on_resume=0;
}
static int safe(struct apple_atcphy *p)
{
 struct typec_mux_dev mux={p};struct typec_mux_state state={.mode=TYPEC_STATE_SAFE};
 return atcphy_mux_set(&mux,&state);
}
static void balanced(struct apple_atcphy *p)
{
 assert(!p->lock&&!root_locked&&!pm_refs&&!node_refs&&!controller.dev.refs);
 for(unsigned int i=0;i<4;i++)assert(!roots[i].refs);
}
int main(void)
{
 struct apple_atcphy p;
#if !USB_REACHABLE
 { prepare(&p,true,true,false);unsigned int before=writes;
 struct typec_mux_dev provider={.data=&p,.set=atcphy_mux_set};
 struct typec_mux_dev *providers[]={&provider};struct typec_mux mux={1,providers};
 struct typec_mux_state state={.mode=TYPEC_STATE_SAFE};
 assert(typec_mux_set(&mux,&state)==-ENODEV&&writes==before);balanced(&p);
 puts("PASS USB-unreachable fixed route blocks before MMIO");return 0; }
#endif
 /* HC death before rh_registered is set need not mark the root NOTATTACHED. */
 prepare(&p,true,true,false);hcd.rh_registered=false;usb_hc_died(&hcd);
 assert(roots[3].state!=USB_STATE_NOTATTACHED&&HCD_DEAD(&hcd));unsigned int before_hcd=writes;
 int hcd_ret=safe(&p);fprintf(stderr,"dead HCD admission returned %d, PM resumes %d\n",hcd_ret,resumes);
 assert(hcd_ret==-ENODEV&&writes==before_hcd&&resumes==0);balanced(&p);
 if(HCD_DEAD_ONLY){puts("PASS dead HCD rejected before PM/MMIO");return 0;}
 for(unsigned int gate=0;gate<2;gate++){
  prepare(&p,true,true,false);before_hcd=writes;
  if(gate==0)inaccessible_on_resume=1;
  if(gate==1)dead_on_resume=1;
  assert(safe(&p)==-ENODEV&&writes==before_hcd);balanced(&p);
 }
 for(unsigned int swap=0;swap<2;swap++){
   prepare(&p,true,true,swap);stuck_ack=true;
   assert(safe(&p)==0);assert(resumes==1);balanced(&p);
 }
 /* Resume failure must return before ANY lane/core programming. */
 prepare(&p,true,true,false);unsigned int before=writes;resume_error=-EIO;
 unsigned int valid=p.dp_usb_context_valid,dirty=p.dp_usb_context_dirty,owned=p.dp_owned_pmas;
 assert(safe(&p)==-EIO&&writes==before&&p.dp_usb_context_valid==valid&&p.dp_usb_context_dirty==dirty&&p.dp_owned_pmas==owned);balanced(&p);
 resume_error=0;assert(safe(&p)==0);balanced(&p);
 /* PM resume is NOT proof of ACK. Real parking timeout stays fatal. */
 prepare(&p,true,true,false);fail_caller="atcphy_pipehandler_lock";before=writes;
 assert(safe(&p)==-ETIMEDOUT&&writes==before&&p.pipe_state==ATCPHY_PIPEHANDLER_STATE_USB3&&p.mode_dirty);balanced(&p);
 fail_caller=NULL;assert(safe(&p)==0);balanced(&p);
 for(unsigned int missing=0;missing<5;missing++){
  prepare(&p,true,true,false);before=writes;
  if(missing==0)graph_missing=true;
  if(missing==1)controller_missing=true;
  if(missing==2)root_missing=true;
  if(missing==3)detach_on_lock=1;
  if(missing==4)detach_on_resume=1;
  assert(safe(&p)==(missing==0||missing>=3?-ENODEV:-EAGAIN)&&writes==before);balanced(&p);
 }
 /* Root appears after independent fixed-host probe: missing root is retryable. */
 prepare(&p,true,true,false);root_missing=true;before=writes;
 assert(safe(&p)==-EAGAIN&&writes==before);balanced(&p);
 root_missing=false;assert(safe(&p)==0);balanced(&p);
 /* BIST faults are independent of the runtime clock wake; never force readiness. */
 for(unsigned int swap=0;swap<2;swap++){
   prepare(&p,true,true,swap);assert(safe(&p)==0);balanced(&p);
   orient(&p,!swap);bist_status_stuck=true;
   assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==-ETIMEDOUT&&p.mode_dirty&&p.pipe_pending);balanced(&p);
   bist_status_stuck=false;recover();assert(select_mode(&p,APPLE_ATCPHY_MODE_USB3_DP)==0);balanced(&p);
 }
 /* Scope and clean same-mode preserve MMIO guards; fixed no-op briefly owns PM. */
 struct typec_altmode alt={.svid=USB_TYPEC_DP_SID};
 struct typec_mux_dev mux={&p};struct typec_mux_state same={.mode=TYPEC_DP_STATE_D,.alt=&alt};
 prepare(&p,false,true,false);before=writes;assert(atcphy_mux_set(&mux,&same)==0&&resumes==0&&writes==before);balanced(&p);
 prepare(&p,true,false,false);before=writes;assert(atcphy_mux_set(&mux,&same)==0&&resumes==0&&writes==before);balanced(&p);
 prepare(&p,true,true,false);before=writes;
 assert(atcphy_mux_set(&mux,&same)==0&&writes==before&&resumes==1);balanced(&p);
 puts("PASS actual PM wrapper + actual parking/restore: both PMAs, gated clock, PM error/retry, fatal ACK/retry, graph/controller/root/removal gates, legacy/ordinary and cached mode");
 return 0;
}
'''
    helpers=''
    if has_pm:
     helpers=ns['block'](s,'struct atcphy_usb3_pm {')+';\n'
     helpers+='#if IS_REACHABLE(CONFIG_USB)\n'+ns['function'](s,'atcphy_usb3_pm_find')+'\n#endif\n'
     helpers+=ns['function'](s,'atcphy_usb3_pm_get')+'\n'+ns['function'](s,'atcphy_usb3_pm_put')+'\n'
     # The fixture extractor adapts the locked body; extract the wrapper directly.
     marker=re.search(r'^static int atcphy_mux_set\(',s,re.M).group()
     helpers+=ns['block'](s,marker)+'\n'
    else:
     # Parent runs its ACTUAL mux callback from production, without a substituted wrapper.
     pmfixture=pmfixture.replace('static ', 'static __attribute__((unused)) ')
    if usb_unreachable:
     pmfixture=pmfixture.replace('static ', 'static __attribute__((unused)) ')
    typec_source=(root/'drivers/usb/typec/mux.c').read_text()
    typec_caller=ns['block'](typec_source,'int typec_mux_set(')+'\n'
    hcd_source=(root/'drivers/usb/core/hcd.c').read_text()
    hcd_death=ns['block'](hcd_source,'void usb_hc_died (')+'\n'
    text=prefix+prod+'\n#define USB_REACHABLE '+str(int(not usb_unreachable))+'\n#define HCD_DEAD_ONLY '+str(int(False))+'\n'+pmfixture+helpers+typec_caller+hcd_death+suffix+controls
    return text

def probe(root):
    def source(path):
        return (root/path).read_text()


    def function(text, name):
        match = re.search(r'^static (?:int|void)\s*\n?' + name + r'\(', text, re.M)
        assert match, name
        start = match.start()
        opening = text.index('{', start)
        depth = 1
        end = opening + 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        return text[start:end]


    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#define EPROBE_DEFER 517
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define guard(kind) (void)
#define TYPEC_STATE_SAFE 0
#define INIT_DELAYED_WORK(p,fn) ((p)->pending=false)
#define atomic_set(p,n) (*(p)=(n))
struct work { bool pending; };
struct fwnode_handle { int refs; };
struct typec_mux_state { void *alt,*data; unsigned long mode; };
struct tps6598x { int lock; void *port; };
struct notifier_block { int (*notifier_call)(struct notifier_block *,unsigned long,void *); };
'''
    pm = source('drivers/usb/typec/tipd/cd321x-pm.h').replace('#include <linux/types.h>', '')
    mocks = r'''
struct cd321x {
 struct tps6598x tps;
 struct work update_work,resume_work;
 int link_event;
 struct cd321x_pm_state pm;
 void *mux,*tbt_switch,*port_altmode_dp,*port_altmode_tbt;
 struct fwnode_handle *connector_fwnode;
 struct typec_mux_state state;
 struct notifier_block tbt_notifier;
 bool state_valid,tbt_notifier_registered;
};
static struct fwnode_handle connector;
static int mux_error, notifier_error, switch_error;
static int ports,altmodes,muxes,switches,notifiers,stop_calls;
static bool displayport=true;
static int tps6598x_register_port(struct tps6598x *t,struct fwnode_handle *f) {
 (void)f;t->port=t;ports++;return 0;
}
static int cd321x_register_port_altmodes(struct cd321x *c) {
 c->port_altmode_dp=c;c->port_altmode_tbt=c;altmodes+=2;return 0;
}
static void *fwnode_typec_mux_get(struct fwnode_handle *f) { (void)f;muxes++;return &muxes; }
static bool fwnode_property_present(struct fwnode_handle *f,const char *s) { (void)f;(void)s;return displayport; }
static struct fwnode_handle *fwnode_find_reference(struct fwnode_handle *f,const char *s,int n) {
 (void)f;(void)s;(void)n;connector.refs++;return &connector;
}
static void __attribute__((unused)) fwnode_handle_put(struct fwnode_handle *f) {
 if(f){assert(f->refs>0);f->refs--;}
}
static void *fwnode_typec_thunderbolt_switch_get(struct fwnode_handle *f) {
 (void)f;if(switch_error)return (void *)(intptr_t)switch_error;switches++;return &switches;
}
static int typec_set_mode(void *p,unsigned long m) { (void)p;assert(m==TYPEC_STATE_SAFE);return mux_error; }
static int cd321x_tbt_notify(struct notifier_block *n,unsigned long e,void *d) { (void)n;(void)e;(void)d;return 0; }
static int typec_thunderbolt_switch_register_notifier(void *s,struct notifier_block *n) {
 (void)s;(void)n;if(notifier_error)return notifier_error;notifiers++;return 0;
}
static void typec_thunderbolt_switch_put(void *s) { assert(s&&switches>0);switches--; }
static void typec_mux_put(void *m) { assert(m&&muxes>0);muxes--; }
static void typec_unregister_altmode(void *a) { if(a){assert(altmodes>0);altmodes--;} }
static void typec_unregister_port(void *p) { assert(p&&ports>0);ports--; }
static void cd321x_stop_work(struct cd321x *c) {
 stop_calls++;if(c->tbt_notifier_registered){notifiers--;c->tbt_notifier_registered=false;}
 cd321x_pm_remove(&c->pm);c->resume_work.pending=c->update_work.pending=false;
}
'''
    core = source('drivers/usb/typec/tipd/core.c')
    production = function(core, 'cd321x_register_port') + '\n' + function(core, 'cd321x_unregister_port')
    controls = r'''
static void reset(struct cd321x *c) {
 memset(c,0,sizeof(*c));connector.refs=0;
 mux_error=notifier_error=switch_error=0;stop_calls=0;
 assert(!ports&&!altmodes&&!muxes&&!switches&&!notifiers);
}
static void balanced(void) {
 assert(!ports&&!altmodes&&!muxes&&!switches&&!notifiers&&!connector.refs);
}
int main(void) {
 struct cd321x c;
 struct cd321x_pm_state budget;
 cd321x_pm_init(&budget);assert(cd321x_pm_ready(&budget));
 for(unsigned int i=0;i<CD321X_RESUME_ATTEMPTS;i++){
  assert(cd321x_pm_begin_read(&budget));cd321x_pm_snapshot_ready(&budget);
  assert(cd321x_pm_retry(&budget,false)==(i+1<CD321X_RESUME_ATTEMPTS));
 }
 assert(budget.phase==CD321X_PM_STALE&&!cd321x_pm_can_update(&budget));
 /* Root appearance does not itself deliver a Type-C event. */
 assert(!cd321x_pm_begin_read(&budget));
 assert(cd321x_pm_event(&budget)&&cd321x_pm_begin_read(&budget));
 cd321x_pm_snapshot_ready(&budget);cd321x_pm_complete(&budget);
 assert(cd321x_pm_can_update(&budget));cd321x_pm_prepare(&budget);
 assert(!cd321x_pm_can_update(&budget)&&cd321x_pm_resume(&budget));
 assert(cd321x_pm_link_event(&budget));
 puts("PASS actual retry budget becomes stale without a fresh event");
 if(TEST_DEFER){
  reset(&c);mux_error=-EAGAIN;
  int ret=cd321x_register_port(&c.tps,&connector);
  fprintf(stderr,"missing root probe returned %d (expected -EPROBE_DEFER)\n",ret);
  assert(ret==-EPROBE_DEFER&&!c.state_valid&&!notifiers&&!c.resume_work.pending);
  balanced();assert(!stop_calls);
  /* Supplier completion reprobes the consumer, with a fresh retry budget. */
  reset(&c);assert(cd321x_register_port(&c.tps,&connector)==0&&c.state_valid);
  assert(cd321x_pm_ready(&c.pm)&&cd321x_pm_begin_read(&c.pm));
  cd321x_pm_snapshot_ready(&c.pm);cd321x_pm_complete(&c.pm);
  assert(c.pm.phase==CD321X_PM_RUNNING);
  cd321x_unregister_port(&c.tps);balanced();
 }
 if(TEST_FWNODE){
  reset(&c);notifier_error=-ENOMEM;
  assert(cd321x_register_port(&c.tps,&connector)==-ENOMEM);balanced();
  reset(&c);switch_error=-ENODEV;
  assert(cd321x_register_port(&c.tps,&connector)==-ENODEV);balanced();
 }
 /* Unrelated permanent errors retain the existing charging-port probe behavior. */
 reset(&c);mux_error=-EIO;
 assert(cd321x_register_port(&c.tps,&connector)==0&&!c.state_valid);
 cd321x_unregister_port(&c.tps);balanced();
 reset(&c);displayport=false;
 assert(cd321x_register_port(&c.tps,&connector)==0&&c.state_valid);
 cd321x_unregister_port(&c.tps);balanced();
 puts("PASS actual cd321x port: mux defer/reprobe, rollback, fwnode balance and ordinary scope");
}
'''

    return prefix + pm + mocks + production + '\n#define TEST_DEFER 1\n#define TEST_FWNODE 1\n' + controls


def kconfig(root, output):
    """Run the actual optional USB dependency through a real Kconfig producer."""
    source = (root / 'drivers/phy/apple/Kconfig').read_text()
    dependency = next(line.strip() for line in source.splitlines()
                      if line.strip() == 'depends on USB || !USB')
    directory = output / 'usb-kconfig'
    directory.mkdir(parents=True, exist_ok=True)
    kconfig = directory / 'Kconfig'
    kconfig.write_text('config MODULES\n\tbool "modules"\n\tmodules\n\tdefault y\n'
                      'config USB\n\ttristate "USB"\n'
                      'config PHY_APPLE_ATC\n\ttristate "PHY"\n\t' + dependency + '\n')
    compiler = Path(os.environ.get('KCONFIG_CONF', root / 'scripts/kconfig/conf'))
    if not compiler.is_file():
        subprocess.run(['make', '-C', str(root), 'O=' + str(directory / 'producer'),
                        'ARCH=arm64', 'LLVM=1', 'scripts/kconfig/conf'], check=True)
        compiler = directory / 'producer/scripts/kconfig/conf'
    for usb, phy_setting in [('n', 'y'), ('y', 'y'), ('m', 'y'), ('m', 'm')]:
        case = directory / (usb + '-' + phy_setting)
        case.mkdir(exist_ok=True)
        config = case / 'config'
        config.write_text('CONFIG_MODULES=y\n' +
                          ('CONFIG_USB=' + usb + '\n' if usb != 'n' else
                           '# CONFIG_USB is not set\n') + 'CONFIG_PHY_APPLE_ATC=' + phy_setting + '\n')
        env = dict(os.environ, KCONFIG_CONFIG=str(config),
                   KCONFIG_AUTOCONFIG=str(case / 'auto.conf'),
                   KCONFIG_AUTOHEADER=str(case / 'autoconf.h'))
        result = subprocess.run([str(compiler.resolve()), '--olddefconfig', str(kconfig)],
                                cwd=case, env=env, capture_output=True, text=True)
        (case / 'run.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        expected = 'm' if usb == 'm' else 'y'
        assert 'CONFIG_PHY_APPLE_ATC=' + expected in config.read_text()


def probe_dispatch(root):
    """Connect real port probe -> typec_set_mode -> mux core -> PM wrapper."""
    unit = ownership(root)
    unit = unit.replace('int main(void)', 'static int __attribute__((unused)) ownership_fixture_main(void)', 1)
    unit = unit.replace('#define IS_ERR_OR_NULL(p) (!(p))', '#define IS_ERR(p) ((intptr_t)(p)<0)\n #define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))')
    port = probe(root)
    port = port[:port.index('static void reset(struct cd321x *c)')]
    # Type-C and mutex declarations already belong to the shared PM fixture.
    port = port.replace('struct typec_mux_state { void *alt,*data; unsigned long mode; };', '')
    port = port.replace('#define guard(kind) (void)', '')
    port = port.replace('static int typec_set_mode(void *p,unsigned long m) { (void)p;assert(m==TYPEC_STATE_SAFE);return mux_error; }', '')
    port = port.replace('(void)f;t->port=t;ports++;return 0;', '(void)f;t->port=&probe_port;ports++;return 0;')
    # Real class dispatch requires the port's mux; its allocator remains a mock.
    declarations = 'struct typec_port { struct typec_mux *mux; };\nstatic struct typec_port probe_port;\n'
    dispatch = block((root / 'drivers/usb/typec/class.c').read_text(), 'int typec_set_mode(')
    marker = port.index('static int tps6598x_register_port(')
    port = port[:marker] + declarations + dispatch + '\n' + port[marker:]
    import re
    # Only helpers used at this boundary belong in this translation unit;
    # the separate probe group exercises the complete retry state machine.
    used = {'cd321x_pm_new_connection', 'cd321x_pm_init', 'cd321x_pm_remove', 'cd321x_pm_ready',
            'cd321x_pm_begin_read', 'cd321x_pm_snapshot_ready',
            'cd321x_pm_complete', 'cd321x_pm_can_update'}
    for match in list(re.finditer(r'^static inline (?:void|bool) (cd321x_\w+)\(', port, re.M))[::-1]:
        if match.group(1) not in used:
            body = block(port, match.group())
            port = port[:match.start()] + port[match.start() + len(body):]
    port = port.replace('static int mux_error, notifier_error, switch_error;',
                        'static int notifier_error, switch_error;')
    existing = set(re.findall(r'^\s*#define (\w+)', unit, re.M))
    port = '\n'.join(line for line in port.splitlines()
                     if not (line.startswith('#define ') and line.split()[1].split('(')[0] in existing))
    controls = r"""
static void port_balance(void)
{
    assert(!ports&&!altmodes&&!muxes&&!switches&&!notifiers&&!connector.refs);
}
int main(void)
{
    struct apple_atcphy p;
    struct cd321x c={};
    struct typec_mux_dev provider={.data=&p,.set=atcphy_mux_set};
    struct typec_mux_dev *providers[]={&provider};
    struct typec_mux route={1,providers};probe_port.mux=&route;
    prepare(&p,true,true,false);root_missing=true;unsigned int before=writes;
    assert(cd321x_register_port(&c.tps,&connector)==-EPROBE_DEFER);
    assert(!c.state_valid&&!resumes&&writes==before);balanced(&p);port_balance();
    memset(&c,0,sizeof(c));root_missing=false;
    assert(cd321x_register_port(&c.tps,&connector)==0 && c.state_valid && resumes==1);
    assert(cd321x_pm_ready(&c.pm) && cd321x_pm_begin_read(&c.pm));
    cd321x_pm_snapshot_ready(&c.pm);cd321x_pm_complete(&c.pm);
    assert(cd321x_pm_can_update(&c.pm));
    balanced(&p);cd321x_unregister_port(&c.tps);port_balance();
    memset(&c,0,sizeof(c));prepare(&p,true,true,false);notifier_error=-ENOMEM;
    assert(cd321x_register_port(&c.tps,&connector)==-ENOMEM);
    balanced(&p);port_balance();notifier_error=0;
    memset(&c,0,sizeof(c));prepare(&p,true,true,false);resume_error=-EIO;before=writes;
    assert(cd321x_register_port(&c.tps,&connector)==0 && !c.state_valid && writes==before);
    balanced(&p);cd321x_unregister_port(&c.tps);port_balance();
    puts("PASS: actual CD321x probe -> typec_set_mode -> typec_mux_set -> bounded PM wrapper; delayed root reprobe and rollback references");
}
"""
    return unit + '\n' + port + '\n' + controls
