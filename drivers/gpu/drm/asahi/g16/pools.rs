// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Two bounded retained M4 render allocators. Every unit has its own 32 MiB UMA pool,
//! parameter heap, firmware-mutated lists and PB manager. UMA slot 33 alternates between the
//! two owners; PB slots 0 and 1 remain owned by their respective managers. No pool or manager
//! is reinitialized after exposure, and aborted units are retained by the lifetime ledger.

#[path = "pools/lifetime.rs"]
pub(crate) mod lifetime;

use super::fw::buffer::{self, uma_manager};
use super::hw::{client, engine, pte};
use super::object::{Class, FwObject, Objects, Placement, Request};
use super::Firmware;
use crate::driver::AsahiDrmDevice;
use core::fmt;
use kernel::{
    device,
    prelude::*,
    sync::{aref::ARef, Arc},
};

#[derive(Copy, Clone, Default)]
pub(crate) struct AllocatorRefs {
    pub(crate) uma_mgr: [u64; 2],
    /// Firmware aliases of GPU-writable per-engine auxiliary state, separate from the manager.
    pub(crate) uma_aux: [u64; 2],
    pub(crate) uma_gpu: u64,
    pub(crate) pb_mgr: u64,
    pub(crate) scene_list: u64,
    pub(crate) mgr_misc: u64,
    pub(crate) init_blocks: u32,
    pub(crate) parameter_id: u32,
    pub(crate) submitted_fw: u64,
    pub(crate) counter_completed_fw: u64,
    pub(crate) uma_submitted: u64,
    pub(crate) init_buffer: bool,
}

struct Allocator {
    descriptor: Arc<FwObject>,
    auxiliary: Arc<FwObject>,
    pool: Arc<FwObject>,
    state: Arc<FwObject>,
    list: Arc<FwObject>,
    submitted: Arc<FwObject>,
    manager: Arc<FwObject>,
    heap: Arc<FwObject>,
    pb_pages: Arc<FwObject>,
    pb_blocks: Arc<FwObject>,
    pb_ring: Arc<FwObject>,
    pb_counter: Arc<FwObject>,
    scene_list: Arc<FwObject>,
    misc: Arc<FwObject>,
    published: bool,
    id: u32,
}

#[derive(Default)]
pub(crate) struct PbStats {
    pub(crate) requests: u64,
    pub(crate) grown: u64,
    pub(crate) refused: u64,
    pub(crate) bad: u64,
}

pub(crate) struct Pools {
    pub(crate) dev: ARef<device::Device>,
    units: KVec<Allocator>,
    pub(crate) stats: PbStats,
}

fn alloc(
    o: &mut Objects,
    size: usize,
    class: Class,
    client: Option<(u64, u64)>,
    cpu: bool,
) -> Result<Arc<FwObject>> {
    o.alloc(Request {
        size,
        class,
        placement: Placement::Anywhere,
        offset: 0,
        client,
        lower: None,
        cpu,
    })
}
fn gva(o: &FwObject) -> Result<u64> {
    o.gva.ok_or(EINVAL)
}
fn read64(o: &FwObject, at: usize) -> Result<u64> {
    // Stable high/low/high observation of an unaligned firmware-owned u64.
    for _ in 0..4 {
        let high = o.r32(at + 4)?;
        let low = o.r32(at)?;
        if high == o.r32(at + 4)? {
            return Ok((high as u64) << 32 | low as u64);
        }
    }
    Err(EAGAIN)
}
fn write_records<const N: usize>(
    o: &FwObject,
    count: usize,
    record: impl Fn(usize) -> [u8; N],
) -> Result {
    let mut chunk = [0u8; 512];
    let mut first = 0;
    while first < count {
        let n = (count - first).min(512 / N);
        for j in 0..n {
            chunk[j * N..(j + 1) * N].copy_from_slice(&record(first + j));
        }
        o.write(first * N, &chunk[..n * N])?;
        first += n;
    }
    Ok(())
}
impl Allocator {
    fn new(o: &mut Objects, k: u64) -> Result<Self> {
        let base = client::render_base(k);
        let nc = |va| Some((va, pte::LEAF_NC));
        // Native UMA software metadata is ASC-owned and never needs a client GPU alias.
        // In particular its firmware pointers, +0x54 completion and +0x5c exclusion flag
        // must not share a writable GPU page with the engine auxiliary records.
        let descriptor = alloc(o, uma_manager::SIZE, Class::Fw, None, true)?;
        let auxiliary = alloc(
            o,
            0x4000,
            Class::Shared,
            nc(client::render_descriptor(k)),
            false,
        )?;
        let pool = alloc(
            o,
            0x2000000,
            Class::Shared,
            nc(client::render_pool(k)),
            false,
        )?;
        let state = alloc(o, 0x20000, Class::Shared, nc(client::render_state(k)), true)?;
        let list = alloc(o, 0x4000, Class::Shared, nc(client::render_list(k)), true)?;
        let submitted = alloc(o, 0x40, Class::Fw, None, true)?;
        let manager = alloc(o, 0xc0, Class::Fw, None, true)?;
        let heap = alloc(
            o,
            client::PB_HEAP_SIZE as usize,
            Class::Shared,
            nc(client::pb_heap(k)),
            false,
        )?;
        let pb_pages = alloc(
            o,
            buffer::PAGE_LIST_SIZE,
            Class::Shared,
            nc(client::render_pb_pages(k)),
            true,
        )?;
        let pb_blocks = alloc(o, buffer::BLOCK_LIST_SIZE, Class::Fw, None, true)?;
        let pb_ring = alloc(o, 8, Class::Fw, None, true)?;
        let pb_counter = alloc(o, 0x40, Class::Fw, None, true)?;
        let scene_list = alloc(
            o,
            buffer::SCENE_LIST_SIZE,
            Class::Shared,
            nc(base + 0x2070000),
            true,
        )?;
        let misc = alloc(o, 0x40, Class::Fw, None, true)?;
        super::slots::init_uma_pool(&state, &list, gva(&pool)?)?;
        descriptor.write(
            0,
            &buffer::uma_manager_image(
                descriptor.va,
                engine::RENDER_UMA_SLOT,
                gva(&state)?,
                gva(&list)?,
                submitted.va,
            ),
        )?;
        let heap_va = gva(&heap)?;
        write_records(&pb_pages, buffer::MAX_PAGES as usize, |i| {
            buffer::page_record(heap_va, i as u32).to_le_bytes()
        })?;
        write_records(&pb_blocks, buffer::MAX_BLOCKS as usize, |i| {
            (buffer::block_record(heap_va, i as u32) as u64).to_le_bytes()
        })?;
        pb_ring.w32(0, buffer::MAX_BLOCKS)?;
        pb_ring.w32(4, buffer::MAX_BLOCKS)?;
        pb_counter.w64(0, 1)?;
        manager.write(
            0,
            &buffer::pb_manager_image(
                k as u32,
                pb_pages.va,
                gva(&pb_pages)?,
                pb_blocks.va,
                pb_ring.va,
                pb_counter.va,
            ),
        )?;
        Ok(Self {
            descriptor,
            auxiliary,
            pool,
            state,
            list,
            submitted,
            manager,
            heap,
            pb_pages,
            pb_blocks,
            pb_ring,
            pb_counter,
            scene_list,
            misc,
            published: false,
            id: k as u32,
        })
    }
    fn refs(&self) -> Result<AllocatorRefs> {
        let prior = read64(&self.submitted, 0)?;
        if prior & 1 != 0 {
            return Err(EINVAL);
        }
        let next = prior.checked_add(2).ok_or(EOVERFLOW)?;
        Ok(AllocatorRefs {
            uma_mgr: [self.descriptor.va; 2],
            uma_aux: [self.auxiliary.va + 0x100, self.auxiliary.va + 0x200],
            uma_gpu: gva(&self.auxiliary)?,
            pb_mgr: self.manager.va,
            scene_list: gva(&self.scene_list)?,
            mgr_misc: self.misc.va,
            init_blocks: buffer::MAX_BLOCKS,
            parameter_id: self.id,
            submitted_fw: self.submitted.va,
            counter_completed_fw: self.descriptor.va + 0x54,
            uma_submitted: next,
            init_buffer: !self.published,
        })
    }
    fn retired(&self) -> Result<bool> {
        let submitted = read64(&self.submitted, 0)?;
        let completed = read64(&self.descriptor, uma_manager::COMPLETED)?;
        Ok(submitted != 0 && submitted == completed)
    }
}
impl Pools {
    pub(crate) fn new(dev: &AsahiDrmDevice, o: &mut Objects) -> Result<Self> {
        let mut units = KVec::with_capacity(2, GFP_KERNEL)?;
        for k in 0..2 {
            units.push(Allocator::new(o, k)?, GFP_KERNEL)?;
        }
        Ok(Self {
            dev: dev.as_ref().as_ref().into(),
            units,
            stats: PbStats::default(),
        })
    }
    pub(crate) fn allocator_refs_for(&self, k: usize) -> Result<AllocatorRefs> {
        self.units.get(k).ok_or(EINVAL)?.refs()
    }
    pub(crate) fn prepare(&self, k: usize, _draw: u64) -> Result<AllocatorRefs> {
        let u = self.units.get(k).ok_or(EINVAL)?;
        if u.published && !u.retired()? {
            return Err(EBUSY);
        }
        // Descriptor pointer/counter and slot must still identify this exact retained unit.
        if read64(&u.descriptor, 0)? != u.descriptor.va
            || u.descriptor.r32(8)? != engine::RENDER_UMA_SLOT
            || read64(&u.descriptor, 0x4c)? != u.submitted.va
        {
            return Err(EINVAL);
        }
        u.refs()
    }
    pub(crate) fn publish(&mut self, k: usize, submitted: u64) -> Result {
        let u = self.units.get_mut(k).ok_or(EINVAL)?;
        if submitted != read64(&u.submitted, 0)?.checked_add(2).ok_or(EOVERFLOW)? {
            return Err(EINVAL);
        }
        u.submitted.w64(0, submitted)?;
        u.published = true;
        Ok(())
    }
    pub(crate) fn retired(&self, k: usize) -> Result<bool> {
        self.units.get(k).ok_or(EINVAL)?.retired()
    }
    pub(crate) fn write_stats(&self, f: &mut impl fmt::Write) -> fmt::Result {
        writeln!(
            f,
            "pb: M4 retained allocators 2 blocks {} requests {} bad {}",
            buffer::MAX_BLOCKS,
            self.stats.requests,
            self.stats.bad
        )
    }
}
impl Firmware {
    pub(crate) fn init_render_pools(&mut self, rebuild: bool) -> Result {
        // An abort must quarantine its allocation set. M4 has no qualified allocator reset.
        if rebuild {
            return Err(EIO);
        }
        Ok(())
    }
    pub(crate) fn answer_growth(&mut self) -> Result {
        // M4 publishes its entire bounded heap up front; G15's event-7/device-control
        // protocol is not an M4 ABI. Never synthesize an unqualified firmware reply.
        if !self.events.grow.is_empty() {
            self.pools.stats.requests += self.events.grow.len() as u64;
            self.pools.stats.bad += self.events.grow.len() as u64;
            return Err(super::boot::eproto());
        }
        Ok(())
    }
}
