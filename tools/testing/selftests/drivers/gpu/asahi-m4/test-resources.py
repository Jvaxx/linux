#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the actual M4 resource allocators against a CPU-only allocation recorder."""
from common import kernel_root, output_dir, rustc
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
root = kernel_root()
out = args.output or output_dir()
out.mkdir(parents=True, exist_ok=True)
def item(text, declaration):
    start = text.index(declaration)
    at = text.index('{', start)
    depth = 1
    end = at + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def module(text, name):
    return item(text, 'pub(crate) mod ' + name + ' {')

hw = (root/'drivers/gpu/drm/asahi/g16/hw.rs').read_text()
production = root/'drivers/gpu/drm/asahi/g16'
graph_source = (production/'graph.rs').read_text()
# Qualified J713 noncached read/write leaf attributes, pinned independently
# of the production allocator and page-table implementation.
private_rw = 0x00c0000000000c8b
source = r'''
#![allow(dead_code, unused_imports, private_interfaces)]
extern crate self as kernel;
#[derive(Copy,Clone,Debug,Eq,PartialEq)] pub struct Error(i32);
pub const EINVAL:Error=Error(22); pub const EAGAIN:Error=Error(11); pub const E2BIG:Error=Error(7);
pub const EBUSY:Error=Error(16); pub const EOVERFLOW:Error=Error(75); pub const EIO:Error=Error(5);
pub const GFP_KERNEL:u32=0;
pub type Result<T=()> = std::result::Result<T,Error>;
pub mod prelude {pub use crate::{Result,KVec,GFP_KERNEL,EINVAL,EAGAIN,E2BIG,EBUSY,EOVERFLOW,EIO};}
pub struct KVec<T>(Vec<T>);
impl<T>KVec<T>{pub fn new()->Self{Self(Vec::new())} pub fn with_capacity(n:usize,_:u32)->Result<Self>{Ok(Self(Vec::with_capacity(n)))} pub fn push(&mut self,x:T,_:u32)->Result{self.0.push(x);Ok(())}}
impl<T>std::ops::Deref for KVec<T>{type Target=Vec<T>;fn deref(&self)->&Vec<T>{&self.0}}
impl<T>std::ops::DerefMut for KVec<T>{fn deref_mut(&mut self)->&mut Vec<T>{&mut self.0}}
pub mod sync {
    pub struct Arc<T>(std::sync::Arc<T>);
    impl<T>Arc<T>{pub fn new(v:T,_:u32)->crate::Result<Self>{Ok(Self(std::sync::Arc::new(v)))}}
    impl<T>Clone for Arc<T>{fn clone(&self)->Self{Self(self.0.clone())}}
    impl<T>std::ops::Deref for Arc<T>{type Target=T;fn deref(&self)->&T{&self.0}}
    pub mod aref {pub struct ARef<T>(T);impl From<&crate::device::Device>for ARef<crate::device::Device>{fn from(v:&crate::device::Device)->Self{Self(v.clone())}}}
}
pub mod device {#[derive(Clone)]pub struct Device;impl AsRef<Device> for Device {fn as_ref(&self)->&Device{self}}}
pub mod driver {pub struct AsahiDrmDevice(pub crate::device::Device);impl AsRef<crate::device::Device>for AsahiDrmDevice{fn as_ref(&self)->&crate::device::Device{&self.0}}}
pub mod g16 {
    pub mod hw {__HW__}
    pub mod fw {
        #[path="__ROOT__/fw/buffer.rs"]pub mod buffer;
        #[path="__ROOT__/fw/graph.rs"]pub mod graph;
        #[path="__ROOT__/fw/work.rs"]pub mod work;
    }
    pub mod object {
        use crate::*;use crate::sync::Arc;
        #[derive(Copy,Clone,Debug,Eq,PartialEq)]pub enum Class{Shared,Fw,FwCached,SharedCached,Mmio}
        pub enum Placement{Anywhere}
        pub struct Request{pub size:usize,pub class:Class,pub placement:Placement,pub offset:usize,pub client:Option<(u64,u64)>,pub lower:Option<u64>,pub cpu:bool}
        pub struct FwObject{pub class:Class,pub client_leaf:Option<u64>,pub va:u64,pub gva:Option<u64>,pub lower_gva:Option<u64>,pub lower_leaf:Option<u64>,pub size:usize,pub mapped:usize,pub offset:usize,pub data:std::sync::Mutex<Vec<u8>>,pub fills:std::sync::Mutex<Vec<(usize,usize)>>,pub fail_fill:std::sync::atomic::AtomicBool}
        impl FwObject{
            pub fn write(&self,o:usize,b:&[u8])->Result{let mut d=self.data.lock().unwrap();d[self.offset+o..self.offset+o+b.len()].copy_from_slice(b);Ok(())}
            pub fn fill(&self,o:usize,v:u8,n:usize)->Result{if self.fail_fill.swap(false,std::sync::atomic::Ordering::Relaxed){return Err(EIO);}self.fills.lock().unwrap().push((o,n));let mut d=self.data.lock().unwrap();d[self.offset+o..self.offset+o+n].fill(v);Ok(())}
            pub fn w32(&self,o:usize,v:u32)->Result{self.write(o,&v.to_le_bytes())}
            pub fn w64(&self,o:usize,v:u64)->Result{self.write(o,&v.to_le_bytes())}
            pub fn r32(&self,o:usize)->Result<u32>{let d=self.data.lock().unwrap();Ok(u32::from_le_bytes(d[self.offset+o..self.offset+o+4].try_into().unwrap()))}
            pub fn snapshot(&self)->Vec<u8>{self.data.lock().unwrap().clone()}
        }
        pub struct Objects{pub objects:Vec<Arc<FwObject>>,next:u64}
        impl Objects{
            pub fn new()->Self{Self{objects:vec![],next:0xfffffc2001000000}}
            pub fn alloc(&mut self,r:Request)->Result<Arc<FwObject>>{let bytes=(r.size+r.offset+0x3fff)&!0x3fff;let va=self.next;self.next+=bytes as u64+0x4000;let x=Arc::new(FwObject{class:r.class,client_leaf:r.client.map(|c|c.1),va:va+r.offset as u64,gva:r.client.map(|c|c.0+r.offset as u64),lower_gva:r.lower.map(|g|g+r.offset as u64),lower_leaf:r.lower.map(|_|super::hw::pte::LOWER),size:r.size,mapped:bytes,offset:r.offset,data:std::sync::Mutex::new(if r.cpu{vec![0;bytes]}else{vec![]}),fills:std::sync::Mutex::new(Vec::new()),fail_fill:std::sync::atomic::AtomicBool::new(false)},0)?;self.objects.push(x.clone());Ok(x)}
        }
    }
    pub mod graph {
        use crate::*;use crate::sync::Arc;use super::object::*;
        use super::hw::pte;
        __CLIENT__
        pub fn cmd_fw()->Class{Class::FwCached}pub fn cmd_shared()->Class{Class::SharedCached}
        __ALLOC__
        __ALLOC_WORK__
    }
    pub mod encode {
        pub struct Geometry{pub tpc_bytes:u64,pub tilemap_bytes:u64}
        pub struct PassRefs{pub ta_work:u64,pub ta_work_gva:u64,pub fs_work:u64,pub fs_work_gva:u64,pub ta_seq:u64,pub fs_seq:u64,pub scene:u64,pub tpc_gva:u64,pub tilemap_gva:u64,pub heap_gva:u64,pub scene_user_gva:u64,pub pre_gva:[u64;3],pub fs_scratch_gva:u64,pub aux_gva:u64,pub its:u64,pub ta_scratch:u64,pub tpc_fw:u64,pub scratch_fw:u64}
    }
    pub mod vm {pub struct VmState{pub id:u64,pub slot:u32,pub generation:u8}}
    pub mod boot {pub fn eproto()->crate::Error{crate::Error(71)}}
    pub struct Events{pub grow:crate::KVec<[u32;18]>}
    pub struct Firmware{pub pools:pools::Pools,pub events:Events}
    #[path="__ROOT__/slots.rs"]pub mod slots;
    #[path="__ROOT__/pools.rs"]pub mod pools;
}
#[cfg(test)]mod resources {
    use super::*;use g16::{object::Objects,slots::{Slots,Claims},pools::Pools};
    fn retire_render(s:&mut Slots,k:usize){let mut c=Claims::new();c.pass[0]=k as u32;c.pass_mask=1<<k;s.publish(&c).unwrap();s.retire(&mut c).unwrap();}
    #[test]fn render_reuse_clears_only_previous_write_spans(){
        let mut o=Objects::new();let mut s=Slots::new(&mut o).unwrap();
        s.claim_pass(0).unwrap();
        let p=s.scrub.passes[0].tpc.clone();let m=s.scrub.passes[0].tilemap.clone();
        assert_eq!(*p.fills.lock().unwrap(),[(0,0)]);assert_eq!(*m.fills.lock().unwrap(),[(0,0)]);
        s.set_pass_dirty(0,0x8000,0x5000).unwrap();p.fill(0,0xa5,0x8000).unwrap();m.fill(0,0x5a,0x5000).unwrap();
        p.write(0x8000,&[0xf1]).unwrap();m.write(0x5000,&[0xf2]).unwrap();
        retire_render(&mut s,0);s.claim_pass(1).unwrap();retire_render(&mut s,1);s.claim_pass(0).unwrap();
        assert!(p.snapshot()[..0x8000].iter().all(|&b|b==0));assert!(m.snapshot()[..0x5000].iter().all(|&b|b==0));
        assert_eq!(p.snapshot()[0x8000],0xf1);assert_eq!(m.snapshot()[0x5000],0xf2);
        assert_eq!(p.fills.lock().unwrap().last(),Some(&(0,0x8000)));assert_eq!(m.fills.lock().unwrap().last(),Some(&(0,0x5000)));
        s.set_pass_dirty(0,0x2000,0x1000).unwrap();p.fill(0,0x45,0x2000).unwrap();m.fill(0,0x67,0x1000).unwrap();
        retire_render(&mut s,0);s.claim_pass(1).unwrap();retire_render(&mut s,1);s.claim_pass(0).unwrap();
        assert!(p.snapshot()[..0x8000].iter().all(|&b|b==0));assert!(m.snapshot()[..0x5000].iter().all(|&b|b==0));
        assert_eq!(p.fills.lock().unwrap().last(),Some(&(0,0x2000)));assert_eq!(m.fills.lock().unwrap().last(),Some(&(0,0x1000)));
        assert_eq!((s.stats.tpc_clear_bytes,s.stats.tilemap_clear_bytes,s.stats.pass_scrubs),(0xa000,0x6000,5));
    }
    #[test]fn cancelled_render_preparation_keeps_dirty_union(){
        let mut o=Objects::new();let mut s=Slots::new(&mut o).unwrap();s.claim_pass(0).unwrap();
        let p=s.scrub.passes[0].tpc.clone();let m=s.scrub.passes[0].tilemap.clone();
        s.set_pass_dirty(0,0x1000,0x3000).unwrap();s.set_pass_dirty(0,0x2000,0x1000).unwrap();
        p.fill(0,0x91,0x2000).unwrap();m.fill(0,0x92,0x3000).unwrap();
        let mut c=Claims::new();c.pass_mask=1;s.release(&mut c);s.claim_pass(0).unwrap();
        assert!(p.snapshot()[..0x2000].iter().all(|&b|b==0));assert!(m.snapshot()[..0x3000].iter().all(|&b|b==0));
        assert_eq!(p.fills.lock().unwrap().last(),Some(&(0,0x2000)));assert_eq!(m.fills.lock().unwrap().last(),Some(&(0,0x3000)));
    }
    #[test]fn failed_scrub_preserves_dirty_bounds_for_retry(){
        let mut o=Objects::new();let mut s=Slots::new(&mut o).unwrap();s.claim_pass(0).unwrap();
        let p=s.scrub.passes[0].tpc.clone();let m=s.scrub.passes[0].tilemap.clone();
        s.set_pass_dirty(0,0x1000,0x2000).unwrap();p.fill(0,0x31,0x1000).unwrap();m.fill(0,0x32,0x2000).unwrap();
        retire_render(&mut s,0);s.claim_pass(1).unwrap();retire_render(&mut s,1);
        m.fail_fill.store(true,std::sync::atomic::Ordering::Relaxed);assert_eq!(s.claim_pass(0),Err(EIO));
        s.claim_pass(0).unwrap();assert!(p.snapshot()[..0x1000].iter().all(|&b|b==0));assert!(m.snapshot()[..0x2000].iter().all(|&b|b==0));
        assert_eq!(p.fills.lock().unwrap().last(),Some(&(0,0x1000)));assert_eq!(m.fills.lock().unwrap().last(),Some(&(0,0x2000)));
        assert_eq!((s.stats.tpc_clear_bytes,s.stats.tilemap_clear_bytes,s.stats.pass_scrubs),(0x2000,0x2000,3));
    }
    #[test]fn dirty_bounds_reject_overflow_without_poisoning_next_claim(){
        let mut o=Objects::new();let mut s=Slots::new(&mut o).unwrap();s.claim_pass(0).unwrap();
        assert_eq!(s.set_pass_dirty(0,u64::MAX,0),Err(E2BIG));assert_eq!(s.set_pass_dirty(0,0,u64::MAX),Err(E2BIG));assert_eq!(s.set_pass_dirty(2,0,0),Err(EINVAL));
        let mut c=Claims::new();c.pass_mask=1;s.release(&mut c);s.claim_pass(0).unwrap();
        assert_eq!(s.scrub.passes[0].tpc.fills.lock().unwrap().last(),Some(&(0,0)));assert_eq!(s.scrub.passes[0].tilemap.fills.lock().unwrap().last(),Some(&(0,0)));
    }
    #[test]fn production_allocations_are_aligned_disjoint_and_in_mesa_hole(){
        let mut o=Objects::new();let slots=Slots::new(&mut o).unwrap();
        let pools=Pools::new(&driver::AsahiDrmDevice(device::Device),&mut o).unwrap();
        let mut aliases=Vec::new();
        for a in &o.objects{if let Some(g)=a.gva{assert_eq!(a.client_leaf,Some(__PRIVATE_RW__),"private GPU alias must match qualified J713 NC RW PTE");let base=g-a.offset as u64;assert_eq!(base&0x3fff,0);assert_eq!(a.mapped&0x3fff,0);assert!(base>=0x7000000000);assert!(base+a.mapped as u64<=0x7100000000);aliases.push((base,base+a.mapped as u64));}}
        aliases.push((0x7000000000,0x7000004000)); // Boot UMA alias.
        aliases.push((0x7000008000,0x700000c000)); // Boot parameter-descriptor alias.
        aliases.push((0x1000000000,0x1000010000)); // Per-VM context store.
        aliases.sort();for a in aliases.windows(2){assert!(a[0].1<=a[1].0,"overlap {:x?} {:x?}",a[0],a[1]);}
        for (k,s) in slots.scrub.compute.iter().enumerate(){assert_eq!(s.work.lower_gva.unwrap()&0x3fff,0);assert_eq!(s.work.size,0x2000);assert_eq!(s.pool.size,0x2000000);assert_eq!(s.pool_state.size,0x20000);assert_eq!(s.page_list.size,0x4000);assert_eq!(s.context_store.size,0x40000);assert_eq!(s.pool_state.r32(0).unwrap()as u64 |((s.pool_state.r32(4).unwrap()as u64)<<32),s.pool.gva.unwrap());assert_eq!(pools.allocator_refs_for(k).unwrap().parameter_id,k as u32);}
        assert_eq!(aliases.len(),47);
    }
    #[test]fn work_has_kernel_lower_alias_without_application_alias(){
        let mut o=Objects::new();let slots=Slots::new(&mut o).unwrap();
        let work=slots.scrub.compute.iter().map(|s|&s.work)
            .chain(slots.scrub.passes.iter().flat_map(|s|[&s.ta_work,&s.fs_work]));
        for object in work {
            assert_eq!(object.gva,None,"work must not enter the application alias inventory");
            assert_eq!(object.client_leaf,None);
            assert!(object.lower_gva.is_some());
            assert_eq!(object.lower_leaf,Some(0x00c0000000000c8b));
            assert_eq!(object.class,g16::object::Class::SharedCached);
            assert!(object.va>=0xfffffc2000000000);
            let lower=object.lower_gva.unwrap();
            for client in &o.objects {
                if let Some(gva)=client.gva {
                    assert!(lower<gva-client.offset as u64 || lower>=gva-client.offset as u64+client.mapped as u64,
                        "a second application alias exposes retained work backing");
                }
            }
        }
        for s in slots.scrub.compute.iter() {
            for object in [&s.preempt,&s.pool,&s.pool_state,&s.page_list,&s.context_store,&s.aux] {
                assert_eq!(object.client_leaf,Some(0x00c0000000000c8b));
                assert!(object.gva.is_some());
            }
        }
        for s in slots.scrub.passes.iter() {
            for object in [&s.tpc,&s.tilemap,&s.heap,&s.scene_user,&s.fs_scratch,&s.aux] {
                assert_eq!(object.client_leaf,Some(0x00c0000000000c8b));
                assert!(object.gva.is_some());
            }
            let refs=s.refs();
            assert_eq!(refs.ta_work_gva,s.ta_work.lower_gva.unwrap());
            assert_eq!(refs.fs_work_gva,s.fs_work.lower_gva.unwrap());
        }
    }
    #[test]fn allocator_metadata_is_firmware_only_and_shared_state_keeps_real_backing(){
        let mut o=Objects::new();let pools=Pools::new(&driver::AsahiDrmDevice(device::Device),&mut o).unwrap();
        let read64=|obj:&g16::object::FwObject,at:usize|->u64{obj.r32(at).unwrap()as u64|((obj.r32(at+4).unwrap()as u64)<<32)};
        for k in 0..2 {
            let refs=pools.allocator_refs_for(k).unwrap();
            let descriptor=o.objects.iter().find(|obj|obj.va==refs.uma_mgr[0]).unwrap();
            let auxiliary=o.objects.iter().find(|obj|obj.va+0x100==refs.uma_aux[0]).unwrap();
            assert_eq!(descriptor.class,g16::object::Class::Fw);
            assert_eq!(descriptor.gva,None);assert_eq!(descriptor.client_leaf,None);
            assert_eq!(descriptor.size,g16::fw::graph::manager::SIZE);
            assert_eq!(auxiliary.class,g16::object::Class::Shared);
            assert_eq!(auxiliary.client_leaf,Some(__PRIVATE_RW__));
            assert_eq!(auxiliary.gva,Some(refs.uma_gpu));
            assert_eq!(refs.uma_aux,[auxiliary.va+0x100,auxiliary.va+0x200]);
            assert!(!std::ptr::eq(&descriptor.data,&auxiliary.data));
            // Shared auxiliary updates cannot change allocator pointers/counters.
            let metadata=descriptor.snapshot();
            {
                // The recorder allocates CPU-visible bytes only for cpu=true.
                // Materialize this GPU-only backing to model writes through its
                // writable client alias without requesting a CPU map in production.
                let mut gpu_bytes=auxiliary.data.lock().unwrap();
                gpu_bytes.resize(auxiliary.mapped,0);
                gpu_bytes[0x100..0x108].copy_from_slice(&0xabcdef0123456789u64.to_le_bytes());
                gpu_bytes[0x200..0x208].copy_from_slice(&0x76543210fedcba98u64.to_le_bytes());
            }
            assert_eq!(metadata,descriptor.snapshot());
            // Freelist and pool-table pointers still select writable client mappings.
            let state_va=read64(descriptor,g16::fw::graph::manager::FREELIST);
            let list_va=read64(descriptor,g16::fw::graph::manager::POOL_TABLE);
            for va in [list_va,state_va] {
                let shared=o.objects.iter().find(|obj|obj.gva==Some(va)).unwrap();
                assert_eq!(shared.class,g16::object::Class::Shared);
                assert_eq!(shared.client_leaf,Some(__PRIVATE_RW__));
            }
            let state=o.objects.iter().find(|obj|obj.gva==Some(state_va)).unwrap();
            let pool_va=read64(state,0);
            let pool=o.objects.iter().find(|obj|obj.gva==Some(pool_va)).unwrap();
            assert_eq!(pool.class,g16::object::Class::Shared);
            assert_eq!(pool.client_leaf,Some(__PRIVATE_RW__));
        }
    }
    #[test]fn reuse_preserves_pool_lists_counter_and_requires_full_displacement(){
        let mut o=Objects::new();let mut s=Slots::new(&mut o).unwrap();
        s.scrub.compute[0].pool_state.w64(8,0xdeadbeef).unwrap();
        s.scrub.compute[0].page_list.w64(0x1000,0xaabbccdd).unwrap();
        let a=s.scrub.compute[0].pool_state.snapshot();let b=s.scrub.compute[0].page_list.snapshot();
        let mut c=Claims::new();assert_eq!(s.claim_compute(&mut c,0).unwrap(),Some(0));s.publish(&c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.free_compute(),1);assert_eq!(s.claim_compute(&mut c,0).unwrap(),Some(1));s.publish(&c).unwrap();s.retire(&mut c).unwrap();
        s.scrub.compute[0].counter.w64(0,91).unwrap();
        assert_eq!(s.claim_compute(&mut c,0).unwrap(),Some(0));
        assert_eq!(a,s.scrub.compute[0].pool_state.snapshot());assert_eq!(b,s.scrub.compute[0].page_list.snapshot());assert_eq!(s.scrub.compute[0].counter.r32(0).unwrap(),91);
        s.publish(&c).unwrap();s.quarantine(&mut c);assert!(s.compute_exhausted());
    }
    #[test]fn production_render_descriptors_preserve_firmware_bytes_and_exact_counter(){
        let mut o=Objects::new();let mut pools=Pools::new(&driver::AsahiDrmDevice(device::Device),&mut o).unwrap();
        let a=pools.prepare(0,1).unwrap();assert!(a.init_buffer);assert_eq!(a.uma_submitted,2);
        assert!(!pools.retired(0).unwrap());pools.publish(0,2).unwrap();
        assert!(pools.publish(0,2).is_err());assert!(pools.prepare(0,2).is_err());
        let d=o.objects.iter().find(|o|o.va==a.uma_mgr[0]).unwrap();
        d.w64(0x54,2).unwrap();d.w64(0x60,0x5a5aa5a5).unwrap();
        let original=d.snapshot();assert!(pools.retired(0).unwrap());
        let next=pools.prepare(0,2).unwrap();assert!(!next.init_buffer);assert_eq!(next.uma_submitted,4);
        assert_eq!(original,d.snapshot());assert_eq!(pools.prepare(1,3).unwrap().parameter_id,1);
    }

    #[test]fn cross_vm_evidence_counts_only_noted_publication(){
        let mut o=Objects::new();let mut s=Slots::new(&mut o).unwrap();
        let a=sync::Arc::new(g16::vm::VmState{id:100,slot:1,generation:1},0).unwrap();
        let b=sync::Arc::new(g16::vm::VmState{id:101,slot:2,generation:1},0).unwrap();
        let c=sync::Arc::new(g16::vm::VmState{id:102,slot:3,generation:9},0).unwrap();
        let mut claims=Claims::new();
        for vm in [&a,&b]{s.claim_compute(&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,vm).unwrap();s.publish(&claims).unwrap();s.note_published(&claims);s.retire(&mut claims).unwrap();}
        assert_eq!(s.stats.cross_vm_retargets,[0,0]);
        assert_eq!(s.claim_compute(&mut claims,0).unwrap(),Some(0));
        s.hold_vm(&claims,&c).unwrap();assert_eq!(s.stats.cross_vm_retargets,[0,0]);
        s.publish(&claims).unwrap();assert_eq!(s.stats.cross_vm_retargets,[0,0]);
        s.note_published(&claims);assert_eq!(s.stats.cross_vm_retargets,[1,0]);
        let proof=s.stats.last_retarget[0];
        assert_eq!((proof.unit,proof.old_vm,proof.new_vm),(0,100,102));
        assert_eq!((proof.old_asid,proof.new_asid,proof.old_generation,proof.new_generation),(1,3,1,9));
        s.note_published(&claims);assert_eq!(s.stats.cross_vm_retargets,[1,0]);
        s.retire(&mut claims).unwrap();
        s.claim_compute(&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,&a).unwrap();
        s.release(&mut claims);s.note_published(&claims);assert_eq!(s.stats.cross_vm_retargets,[1,0]);
    }

}
'''
source = source.replace('__ALLOC__',item(graph_source,'pub(crate) fn alloc(')).replace('__ALLOC_WORK__',item(graph_source,'pub(crate) fn alloc_work('))
source = source.replace('__HW__','\n'.join(module(hw,n) for n in ('client','sizes','engine','tuning','pte'))).replace('__ROOT__',str(production)).replace('__PRIVATE_RW__',hex(private_rw)+'u64').replace('__CLIENT__', '#[derive(Copy,Clone)]\n'+item(graph_source, 'pub(crate) struct Client {')+'\n'+item(graph_source, 'impl Client {'))
(out/'check-resources.rs').write_text(source)
log = ''
for command in [
    rustc() + [ '--edition=2021', '--test', str(out/'check-resources.rs'), '-o', str(out/'check-resources')],
    [str(out/'check-resources'), '--nocapture'],
]:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log += result.stdout
    print(result.stdout, end='')
    (out/'resource-host.log').write_text(log)
    if result.returncode:
        raise SystemExit(result.returncode)
files = ['graph.rs', 'slots.rs', 'pools.rs', 'pools/lifetime.rs', 'fw/buffer.rs', 'fw/graph.rs', 'fw/work.rs', 'hw.rs']
(out/'resource-host.json').write_text(json.dumps({
    'result': 'pass', 'tests': 18,
    'scope': 'Actual production allocation, initialization and lifetime code against a CPU-only backing recorder; no hardware qualification.',
    'private_rw_pte': hex(private_rw),
    'private_rw_reference': 'Qualified J713 NC read/write leaf attributes',
    'source': {f: hashlib.sha256((production/f).read_bytes()).hexdigest() for f in files},
}, indent=2) + '\n')
