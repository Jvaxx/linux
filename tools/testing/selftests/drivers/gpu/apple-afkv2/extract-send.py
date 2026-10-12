#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the real command sender against synthetic ring and DMA operations."""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
match = re.search(r"static int afk_send_command_timeout_len\([^;]*?\)\n\{", source)
if not match:
    raise RuntimeError("production command sender not found")
start, end, depth = match.start(), match.end(), 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
prefix = r'''
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "afkv2-codec.h"
#define SZ_1M 1048576U
#define GFP_KERNEL 0
#define MAX_PENDING_CMDS 16
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#define kmalloc(n,f) malloc(n)
#define kfree(p) free(p)
#define READ_ONCE(x) (x)
#define spin_lock_irqsave(l,f) ((void)(l),(f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l),(void)(f))
#define dma_rmb() do {} while(0)
#define dev_warn(...) do {} while(0)
#define EPIC_TYPE_COMMAND 3
#define EPIC_CAT_COMMAND 0x30
#define DECLARE_COMPLETION_ONSTACK(n) int n=0
#define init_completion(p) (*(p)=0)
#define msecs_to_jiffies(x) (x)
#define wait_for_completion_timeout(p,t) ((void)(t),*(p))
typedef u64 dma_addr_t;
struct epic_cmd { u32 retcode; u64 rxbuf,txbuf; u32 rxlen,txlen; u8 rxcookie,txcookie; };
struct cmd_info {
 u16 tag; u8 type; size_t reply_length; void *rxbuf,*txbuf;
 dma_addr_t rxbuf_dma,txbuf_dma; size_t rxlen,txlen; u32 retcode;
 bool done,free_on_ack; int *completion;
};
struct dcp { void *dev; bool quiescing; };
struct apple_dcp_afkep { struct dcp *dcp; bool wire_v2,inline_v2; int endpoint; };
struct ops { const char *name; };
struct apple_epic_service {
 struct apple_dcp_afkep *ep; struct ops *ops; unsigned int channel; unsigned long cmd_map[1];
 int lock; bool enabled,torndown; u8 cmd_tag; struct cmd_info cmds[16];
};
static struct apple_epic_service *active;
static unsigned int frees, sends;
static int wire_fail, timeout, reply_status;
static struct apple_epic_service *afk_service_get(struct apple_epic_service *s) { return s->enabled?s:NULL; }
static void afk_service_put(struct apple_epic_service *s) { (void)s; }
static void *dma_alloc_coherent(void *dev,size_t n,dma_addr_t *address,int flags)
{ void *p=calloc(1,n);(void)dev;(void)flags;*address=(uintptr_t)p;return p; }
static void dma_free_coherent(void *dev,size_t n,void *p,dma_addr_t a)
{ (void)dev;(void)n;assert((uintptr_t)p==a);free(p);frees++; }
static int bitmap_find_free_region(unsigned long *map,unsigned int n,int order)
{ (void)n;(void)order;assert(!*map);*map=1;return 0; }
static void bitmap_release_region(unsigned long *map,int i,int order)
{ (void)i;(void)order;*map=0; }
static int afk_send_epic(struct apple_dcp_afkep *ep,u32 channel,u16 tag,int type,int category,u8 subtype,const void *payload,size_t length)
{
 struct cmd_info *c=&active->cmds[0];struct afkv2_command parsed;
 assert(channel==4 && type==3 && category==0x30 && subtype==0xc0);
 sends++;
 /* The payload-envelope mock bounds the command separately from its response. */
 assert(length<=128);
 if(ep->inline_v2) {
  assert(length==120);
  assert(!afkv2_decode_command(payload,length,&parsed));
  assert(!parsed.oob && parsed.id==tag && parsed.value==32888 && parsed.inline_size==112);
  assert(parsed.inline_data[0]==0x5a);
 } else {
  const struct epic_cmd *legacy=payload;
  assert(length==sizeof(*legacy) && legacy->txlen==112 && legacy->rxlen==32888);
 }
 if(wire_fail)return -ENOSPC;
 if(timeout)return 0;
 c->reply_length=ep->inline_v2?4:32888;c->retcode=reply_status;
 memset(c->rxbuf,0x77,c->reply_length);c->done=true;*c->completion=1;
 return 0;
}
'''
suffix = r'''
int main(void)
{
 struct dcp dcp={0};struct apple_dcp_afkep ep={.dcp=&dcp,.wire_v2=true,.inline_v2=true};
 struct ops ops={.name="test"};struct apple_epic_service s={.ep=&ep,.ops=&ops,.channel=4,.enabled=true};
 u8 request[112]={0x5a},*output=calloc(1,32888);size_t received;u32 status=99;
 active=&s;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(received==4 && status==0 && output[0]==0x77 && frees==2 && sends==1 && !s.cmd_map[0]);
 /* Existing strict callers still reject a short inline result. */
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,NULL)==-EMSGSIZE);
 reply_status=5;received=99;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(status==5 && received==0);reply_status=0;
 wire_fail=1;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ENOSPC && !s.cmd_map[0]);
 wire_fail=0;ep.inline_v2=false;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(received==32888 && status==0);
 ep.inline_v2=true;timeout=1;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ETIMEDOUT);
 assert(s.cmd_map[0] && s.cmds[0].free_on_ack && !s.cmds[0].completion);
 /* Mock the late-reply owner's release, avoiding leaks in this host test. */
 free(s.cmds[0].rxbuf);free(s.cmds[0].txbuf);free(output);
 puts("PASS: extracted production sender, 128-byte command envelope / 32888-byte reply, strict OOB and timeout ownership");
}
'''
pathlib.Path(sys.argv[2]).write_text(prefix + source[start:end] + suffix)
