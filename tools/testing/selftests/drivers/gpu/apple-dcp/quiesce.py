# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Actual mux prepare/set and TPS removal ordering; firmware/PHY leaves are mocks."""
from common import block

def make_unit(source, parent=False, reference=None):
 reference=source if reference is None else reference
 core=(source/'drivers/usb/typec/tipd/core.c').read_text();mux=(source/'drivers/usb/typec/mux.c').read_text()
 dcp=(reference/'drivers/gpu/drm/apple/dcp.c').read_text()
 prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
typedef unsigned u32;
#define IS_ERR(p) false
#define IS_ERR_OR_NULL(p) (!(p))
#define EXPORT_SYMBOL_GPL(x)
#define guard(k) (void)
#define scoped_guard(k,l) for(int once=((void)(l),1);once;once=0)
#define READ_ONCE(x) (x)
#define WARN_ON_ONCE(x) (x)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_platform_device(p) (p)
#define typec_mux_get_drvdata(m) ((m)->data)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_name(p) "device"
#define list_for_each_entry(p,h,m) for((void)(h),(p)=sole_port();(p);(p)=NULL)
#define lockdep_assert_held(p) ((void)(p))
#define dev_err(dev,fmt,ret) do { (void)(dev);last_error=(ret); } while(0)
enum {TYPEC_STATE_SAFE,TYPEC_STATE_USB,TYPEC_ORIENTATION_NONE,TYPEC_PWR_MODE_USB,TYPEC_THUNDERBOLT_SWITCH_OFF};
enum usb_role {USB_ROLE_NONE,USB_ROLE_HOST,USB_ROLE_DEVICE};
enum typec_data_role {TYPEC_HOST,TYPEC_DEVICE};
#define TPS_STATUS_TO_TYPEC_PORTROLE(s) (s)
#define TPS_STATUS_TO_TYPEC_VCONN(s) (s)
#define TPS_STATUS_TO_TYPEC_DATAROLE(s) (s)
struct typec_mux_state { void *alt; unsigned long mode; void *data; };
struct typec_mux_dev { bool removed; void (*commit)(struct typec_mux_dev *,struct typec_mux_state *); void (*abort)(struct typec_mux_dev *,struct typec_mux_state *); int (*finish)(struct typec_mux_dev *,struct typec_mux_state *); void *data; int (*set)(struct typec_mux_dev *,struct typec_mux_state *); int (*prepare)(struct typec_mux_dev *,struct typec_mux_state *); };
struct task_struct { int unused; };
static _Thread_local struct task_struct this_task;
#define current (&this_task)
struct mutex { pthread_mutex_t value; };
struct typec_mux_context { struct mutex lock; };
static struct typec_mux_context shared_context={.lock={.value=PTHREAD_MUTEX_INITIALIZER}};
static int typec_mux_srcu;
static int srcu_read_lock(int *s) { (void)s;return 0; }
static void srcu_read_unlock(int *s,int i) { (void)s;(void)i; }
static int mutex_trylock(struct mutex *m) { return pthread_mutex_trylock(&m->value)==0; }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->value)); }
struct typec_mux { struct typec_mux_context *context; struct typec_mux_state transaction; struct task_struct *owner; int srcu_index; unsigned prepared; bool applied; unsigned num_mux_devs; struct typec_mux_dev *mux_devs[3]; };
struct connector { bool connected; };
struct apple_dcp { int external_lock,external_error; bool external_cable,external_stopped; struct { bool connected; } dptxport[1]; void *hdmi_hpd; bool active; bool external,typec_cable_connected; void *dev; struct connector *typec_connector; };
struct apple_dcp_typec_route;
struct apple_dcp_typec_port { int link; void *connector_np; struct apple_dcp_typec_route *owner,*target,*secondary_target,*preferred_route; void *applied_alt; unsigned long applied_mode; unsigned applied_status,applied_conf; bool applied_valid,safe_pending,safe_finish_ready,dp_wanted,dp_hpd,hpd; };
struct apple_dcp_typec_route { struct apple_dcp_typec_port *port; struct apple_dcp *dcp; void *xbar; bool deactivate_xbar_done; int deactivate_error; bool selected,tunnel; };
static int dcp_typec_fabric_lock,dcp_typec_ports,deactivate_calls;
struct drm_device { int unused; };
static struct apple_dcp display;
static struct connector connector;
static struct apple_dcp_typec_port port;
static struct apple_dcp_typec_route route;
static struct apple_dcp_typec_port *sole_port(void) { return &port; }
static char events[100];static unsigned count;
static int power_error,phy_error,tbt_error,role_error,last_error,phy_calls,reset_calls,gpio_calls,role_calls,stops,unregisters;
static void event(char c) { assert(count+1<sizeof(events));events[count++]=c;events[count]=0; }
static bool dcp_is_external_h17p(struct apple_dcp *d) { return d->external; }
static int dcp_dptx_disconnect_oob(void *dev,u32 index) { (void)dev;assert(index==0);event('Q');stops++;if(power_error){display.external_error=power_error;return power_error;}display.external_cable=false;display.external_stopped=true;display.dptxport[0].connected=false;display.typec_cable_connected=false;connector.connected=false;return 0; }
static struct drm_device *dcp_typec_drm(void) { return NULL; }
static void dcp_typec_plan(struct drm_device *d,struct apple_dcp_typec_port *p,unsigned n) { (void)d;(void)p;(void)n; }
static bool dcp_typec_tunnel_held(struct apple_dcp *d) { (void)d;return false; }
static int dcp_typec_route_deactivate(struct apple_dcp_typec_route *r) { deactivate_calls++;r->selected=false;return 0; }
static void dcp_typec_port_attach(struct apple_dcp_typec_port *p) { (void)p; }
static int gpiod_get_value_cansleep(void *p) { (void)p;return 0; }
static int dcp_dptx_connect(struct apple_dcp *d,unsigned n) { (void)d;(void)n;return 0; }
static bool dcp_typec_route_available(struct apple_dcp_typec_route *r) { return !r->selected; }
static int dcp_typec_route_activate(struct apple_dcp_typec_route *r,void *x) { (void)x;r->selected=true;return 0; }
struct tps6598x;
struct tipd_data { REMOVE_RETURN (*remove)(struct tps6598x *); void (*unregister_port)(struct tps6598x *); int (*reset)(struct tps6598x *); };
struct typec_port { struct typec_mux *mux; };
struct tps6598x { bool remove_safe_prepared; struct typec_mux *mux; void *partner,*dev,*psy,*role_sw,*reset; int irq,wq_poll; struct typec_port *port; struct tipd_data *data; };
struct cd321x { struct tps6598x tps; struct typec_mux *mux; void *tbt_switch; };
struct typec_thunderbolt_switch_data { int state; };
static void cd321x_stop_work(struct cd321x *c) { (void)c;event('W'); }
static int typec_thunderbolt_switch_set(void *p,void *s) { (void)p;(void)s;event('B');return tbt_error; }
static int usb_role_switch_set_role(void *p,int r) { (void)p;assert(r==USB_ROLE_NONE);event('R');role_calls++;return role_error; }
static void typec_unregister_partner(void *p) { (void)p;event('P'); }
#define typec_set_pwr_opmode(...) ((void)0)
#define typec_set_pwr_role(...) ((void)0)
#define typec_set_vconn_role(...) ((void)0)
static int typec_set_orientation(void *p,int o) { (void)p;assert(o==TYPEC_ORIENTATION_NONE);return 0; }
static void typec_set_data_role(struct typec_port *p,enum typec_data_role r) { (void)p;(void)r;event('D'); }
static void dcp_typec_pipeline_freed(void) { event('F'); }
static void power_supply_changed(void *p) { (void)p; }
static void tipd_debugfs_unregister(struct tps6598x *t) { (void)t; }
static void cancel_delayed_work_sync(void *w) { (void)w; }
static void devm_free_irq(void *d,int i,void *t) { (void)d;(void)i;(void)t; }
static void typec_mux_put(struct typec_mux *m) { (void)m; }
static void usb_role_switch_put(void *p) { (void)p;event('V'); }
static void gpiod_set_value_cansleep(void *p,int v) { (void)p;assert(v==1);gpio_calls++;event('G'); }

'''.replace('REMOVE_RETURN','void' if parent else 'int')
 functions='\n'.join(block(mux, marker) for marker in ('static void typec_mux_unlock(', 'void typec_mux_abort(', 'int typec_mux_begin(', 'int typec_mux_apply(', 'int typec_mux_end(', 'int typec_mux_prepare(')) if not parent else block((reference/'drivers/usb/typec/mux.c').read_text(),'int typec_mux_prepare(')+'\n'
 functions+=block(mux,'int typec_mux_set(')+'\n'+block(dcp,'static int dcp_typec_route_prepare(')+'\n'+block(dcp,'static int dcp_typec_route_finish(')+'\n'+block(dcp,'static void dcp_typec_route_commit(')+'\n'
 functions+=block(dcp,'static struct apple_dcp_typec_route *\ndcp_typec_rebalance_locked(')+'\n'
 if not parent:functions+=block(core,'static void tps6598x_drop_partner(')+'\n'+block(core,'static int tps6598x_set_data_role(')+'\n'
 functions+=block(core,'static '+('void' if parent else 'int')+' tps6598x_disconnect(')+'\n'+block(core,'static '+('void' if parent else 'int')+' cd321x_remove(')+'\n'+block(core,'void tipd_remove(')+'\n'
 controls=r'''
static struct typec_mux_dev phy_provider,display_provider;
static struct typec_mux mux;
static struct typec_port class_port;
static struct tipd_data data;

static int phy_set(struct typec_mux_dev *m,struct typec_mux_state *s) { (void)m;assert(s->mode==TYPEC_STATE_SAFE);event('L');phy_calls++;return phy_error; }
static int display_set(struct typec_mux_dev *m,struct typec_mux_state *s) { (void)m;(void)s;event('S');if(port.owner&&display.external&&connector.connected) {int r=dcp_dptx_disconnect_oob(display.dev,0);if(r)return r;}if(port.safe_pending)return 0;port.owner=NULL;return 0; }
static void unregister_port(struct tps6598x *t) { (void)t;unregisters++;event('U'); }
static int reset(struct tps6598x *t) { (void)t;reset_calls++;event('X');return 0; }
static void initial(struct cd321x *c) {
 count=0;events[0]=0;power_error=phy_error=tbt_error=role_error=last_error=phy_calls=reset_calls=gpio_calls=role_calls=stops=unregisters=deactivate_calls=0;
 connector=(struct connector){.connected=true};display=(struct apple_dcp){.external=true,.external_cable=true,.dptxport={{.connected=true}},.typec_cable_connected=true,.typec_connector=&connector};
 port=(struct apple_dcp_typec_port){.owner=&route,.applied_valid=true,.dp_wanted=true,.dp_hpd=true,.hpd=true};route=(struct apple_dcp_typec_route){.port=&port,.dcp=&display,.selected=true};
 phy_provider=(struct typec_mux_dev){.set=phy_set};display_provider=(struct typec_mux_dev){.data=&route,.set=display_set,.prepare=dcp_typec_route_prepare,.finish=dcp_typec_route_finish,.commit=dcp_typec_route_commit};
 mux=(struct typec_mux){.context=&shared_context,.num_mux_devs=2,.mux_devs={&phy_provider,&display_provider}};class_port.mux=&mux;
 data=(struct tipd_data){.remove=cd321x_remove,.unregister_port=unregister_port,.reset=reset};
 *c=(struct cd321x){.tps={.mux=&mux,.partner=(void *)1,.port=&class_port,.data=&data,.reset=(void *)1},.mux=&mux};
}
static int position(char c) { for(unsigned i=0;i<count;i++)if(events[i]==c)return i;return -1; }
int main(int argc,char **argv) {
 (void)argc;struct cd321x c;struct typec_mux_state safe={.mode=TYPEC_STATE_SAFE};
 initial(&c);
 if(argv[1][0]=='o') {tipd_remove(&c.tps);fprintf(stderr,"ordered remove: %s\n",events);assert(position('Q')>=0&&position('Q')<position('R')&&position('Q')<position('L'));assert(!last_error&&reset_calls==1&&gpio_calls==1&&unregisters==1);}
 if(argv[1][0]=='p') {power_error=-EIO;tipd_remove(&c.tps);assert(port.safe_pending&&last_error==-EIO&&phy_calls==0&&role_calls==0&&reset_calls==0&&gpio_calls==0&&port.owner==&route&&unregisters==1);}
 if(argv[1][0]=='a') {phy_error=-ETIMEDOUT;tipd_remove(&c.tps);fprintf(stderr,"failed SAFE: error=%d reset=%d events=%s\n",last_error,reset_calls,events);assert(port.safe_pending&&last_error==-ETIMEDOUT&&phy_calls==1&&reset_calls==0&&gpio_calls==0&&port.owner==&route&&unregisters==1);}
 if(argv[1][0]=='r') {role_error=-EBUSY;tipd_remove(&c.tps);assert(last_error==-EBUSY&&phy_calls==1&&reset_calls==0&&gpio_calls==0&&port.safe_pending&&port.owner==&route);}
 if(argv[1][0]=='b') {tbt_error=-EIO;tipd_remove(&c.tps);assert(last_error==-EIO&&stops==0&&phy_calls==0&&role_calls==0&&reset_calls==0);}
 if(argv[1][0]=='m') {power_error=-EIO;assert(typec_mux_set(&mux,&safe)==-EIO&&phy_calls==0&&port.owner==&route);}
 if(argv[1][0]=='d') {assert(typec_mux_prepare(&mux,&safe)==0&&stops==1&&phy_calls==0&&port.owner==&route);assert(typec_mux_prepare(&mux,&safe)==0&&stops==1&&port.owner==&route);}
 if(argv[1][0]=='e') {display.external_error=-EIO;port.hpd=false;display.typec_cable_connected=false;connector.connected=false;assert(typec_mux_set(&mux,&safe)==-EIO&&phy_calls==0&&port.owner==&route);}
 if(argv[1][0]=='l') {display.external=false;assert(typec_mux_prepare(&mux,&safe)==0&&stops==1&&port.safe_pending&&port.owner==&route);}
 if(argv[1][0]=='f') {assert(typec_mux_prepare(&mux,&safe)==0&&port.safe_pending);dcp_typec_rebalance_locked(NULL,0);assert(port.owner==&route&&route.selected&&deactivate_calls==0);port.safe_pending=false;dcp_typec_rebalance_locked(NULL,0);assert(port.owner==NULL&&deactivate_calls==1);}
 if(argv[1][0]=='t') {route.tunnel=true;assert(typec_mux_prepare(&mux,&safe)==0&&stops==0&&port.dp_wanted);}
 puts("PASS actual ordered prepare/set/remove control");return 0;
}
'''
 return prefix+functions+controls
