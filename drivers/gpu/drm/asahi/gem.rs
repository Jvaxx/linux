// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Asahi driver GEM object implementation
//!
//! Basic wrappers and adaptations between generic GEM shmem objects and this driver's
//! view of what a GPU buffer object is. It is in charge of keeping track of all mappings for
//! each GEM object so we can remove them when a client (File) or a Vm are destroyed, as well as
//! implementing RTKit buffers on top of GEM objects for firmware use.

use kernel::{
    drm,
    drm::gem::{
        shmem,
        shmem::VMap,
        BaseObject,
        DriverObject, //
    },
    error::Result,
    prelude::*,
    sync::aref::ARef,
    uapi, //
};

use core::ops::Range;
use core::sync::atomic::{
    AtomicU64,
    Ordering, //
};

use crate::{
    debug::*,
    driver::{
        AsahiDevice,
        AsahiDriver, //
    },
    file,
    mmu,
};

const DEBUG_CLASS: DebugFlags = DebugFlags::Gem;

/// Represents the inner data of a GEM object for this driver.
#[pin_data(PinnedDrop)]
pub(crate) struct AsahiObject {
    /// ID for debug
    id: u64,
    /// Object size retained for live-object accounting.
    size: usize,
    /// Object creation flags.
    flags: u32,
    /// Whether this object can be exported.
    exportable: bool,
    /// Whether this is a kernel-created object.
    kernel: bool,
    _allocation: Option<crate::agx_memory_stats::Allocation>,
    /// Driver aliases outside the user GPUVA tree; they pin the backing.
    kernel_mappings: AtomicU64,
}

/// Type alias for the shmem GEM object type for this driver.
pub(crate) type Object = shmem::Object<AsahiObject>;

/// VM-private BOs share their assigned VM's reservation object. Validate this
/// before acquiring mapping state; generic GPUVM accepts external shareable BOs.
pub(crate) fn validate_vm_binding(gem: &Object, vm: &mmu::Vm) -> Result {
    if gem.flags & uapi::drm_asahi_gem_flags_DRM_ASAHI_GEM_VM_PRIVATE != 0
        && vm.is_extobj(gem)
    {
        return Err(EINVAL);
    }
    Ok(())
}

unsafe impl Send for AsahiObject {}
unsafe impl Sync for AsahiObject {}

// /// Type alias for the SGTable type for this driver.
// pub(crate) type SGTable = shmem::SGTable<AsahiObject>;

/// A shared reference to a GEM object for this driver.
pub(crate) struct ObjectRef {
    /// The underlying GEM object reference
    pub(crate) gem: ARef<Object>,
    /// The kernel-side VMap of this object, if needed
    vmap: Option<VMap<AsahiObject, u8>>,
}

crate::no_debug!(ObjectRef);

static GEM_ID: AtomicU64 = AtomicU64::new(0);

impl ObjectRef {
    /// Create a new wrapper for a raw GEM object reference.
    pub(crate) fn new(gem: ARef<Object>) -> ObjectRef {
        ObjectRef { gem, vmap: None }
    }

    /// Return the `VMap` for this object, creating it if necessary.
    pub(crate) fn vmap(&mut self) -> Result<shmem::VMapRef<'_, AsahiObject, u8>> {
        if self.vmap.is_none() {
            self.vmap = Some(self.gem.owned_vmap()?);
        }
        self.gem.vmap()
    }

    /// Borrow the mapping already retained by this object wrapper.
    ///
    /// Unlike a fresh GEM `VMapRef`, this borrow does not take the reservation
    /// lock or change the shmem vmap reference count on every access. The owned
    /// mapping pins the backing for the entire borrow, including GPU updates
    /// to shared command state. Callers still provide device memory ordering.
    pub(crate) fn cached_vmap(&mut self) -> Result<kernel::iosys_map::IoSysMapRef<'_, u8>> {
        if self.vmap.is_none() {
            self.vmap = Some(self.gem.owned_vmap()?);
        }
        Ok(self.vmap.as_ref().ok_or(EIO)?.get())
    }

    /// Returns the size of an object in bytes
    pub(crate) fn size(&self) -> usize {
        self.gem.size()
    }

    /// Maps an object into a given `Vm` at any free address within a given range.
    pub(crate) fn map_into_range(
        &mut self,
        vm: &crate::mmu::Vm,
        range: Range<u64>,
        alignment: u64,
        prot: mmu::Prot,
        guard: bool,
    ) -> Result<crate::mmu::KernelMapping> {
        // Only used for kernel objects now
        if !self.gem.kernel {
            return Err(EINVAL);
        }
        vm.map_in_range(&self.gem, 0..self.gem.size(), alignment, range, prot, guard)
    }

    /// Maps a range within an object into a given `Vm` at any free address within a given range.
    pub(crate) fn map_range_into_range(
        &mut self,
        vm: &crate::mmu::Vm,
        obj_range: Range<usize>,
        range: Range<u64>,
        alignment: u64,
        prot: mmu::Prot,
        guard: bool,
    ) -> Result<crate::mmu::KernelMapping> {
        if obj_range.end > self.gem.size() {
            return Err(EINVAL);
        }
        if self.gem.flags & uapi::drm_asahi_gem_flags_DRM_ASAHI_GEM_VM_PRIVATE != 0
            && vm.is_extobj(&*self.gem)
        {
            return Err(EINVAL);
        }
        vm.map_in_range(&self.gem, obj_range, alignment, range, prot, guard)
    }

    /// Maps an object into a given `Vm` at a specific address.
    ///
    /// Returns Err(ENOSPC) if the requested address is already busy.
    pub(crate) fn map_at(
        &mut self,
        vm: &crate::mmu::Vm,
        addr: u64,
        prot: mmu::Prot,
        guard: bool,
    ) -> Result<crate::mmu::KernelMapping> {
        if self.gem.flags & uapi::drm_asahi_gem_flags_DRM_ASAHI_GEM_VM_PRIVATE != 0
            && vm.is_extobj(&*self.gem)
        {
            return Err(EINVAL);
        }

        vm.map_at(addr, self.gem.size(), self.gem.clone(), prot, guard)
    }
}

#[pinned_drop]
impl PinnedDrop for AsahiObject {
    fn drop(self: Pin<&mut Self>) {
        crate::gem_stats::note_free(self.kernel, self.size);
    }
}

pub(crate) struct AsahiObjConfig {
    flags: u32,
    exportable: bool,
    kernel: bool,
}

/// Validate before rounding: wrapping a user size can create a much smaller
/// backing than requested (or panic with overflow checks). Kernel callers use
/// the same boundary so malformed calculated sizes cannot bypass this check.
fn checked_object_size(size: usize) -> Result<usize> {
    if size == 0 { return Err(EINVAL); }
    size.checked_add(mmu::UAT_PGMSK)
        .map(|rounded| rounded & !mmu::UAT_PGMSK)
        .ok_or(EOVERFLOW)
}

fn new_kernel_object_with_cpu_mapping(
    dev: &AsahiDevice,
    size: usize,
    map_wc: bool,
) -> Result<ObjectRef> {
/// Create a new kernel-owned GEM object.
pub(crate) fn new_kernel_object(dev: &AsahiDevice, size: usize) -> Result<ObjectRef> {
    new_kernel_object_mapped(dev, size, false)
}

/// Create a new kernel-owned GEM object whose kernel mapping is write-combined.
///
/// Used for memory that a non-coherent agent reads as soon as the host has stored to it and
/// issued a barrier, without cache maintenance.
pub(crate) fn new_kernel_object_wc(dev: &AsahiDevice, size: usize) -> Result<ObjectRef> {
    new_kernel_object_mapped(dev, size, true)
}

fn new_kernel_object_mapped(dev: &AsahiDevice, size: usize, map_wc: bool) -> Result<ObjectRef> {
    let gem = shmem::Object::<AsahiObject>::new(
        dev,
        checked_object_size(size)?,
        shmem::ObjectConfig::<AsahiObject> {
            reclaimable_cpu_mappings: false,
            map_wc,
            parent_resv_obj: None,
            synchronous_reclaim: dev.is_m3,
        },
        AsahiObjConfig {
            flags: 0,
            exportable: false,
            kernel: true,
        },
    )?;

    mod_pr_debug!("AsahiObject new kernel object id={}\n", gem.id);
    Ok(ObjectRef::new(gem))
}

/// Create a new kernel-owned GEM object with a write-back CPU mapping.
pub(crate) fn new_kernel_object(dev: &AsahiDevice, size: usize) -> Result<ObjectRef> {
    new_kernel_object_with_cpu_mapping(dev, size, false)
}

pub(crate) fn new_kernel_object_wc(dev: &AsahiDevice, size: usize) -> Result<ObjectRef> {
    new_kernel_object_with_cpu_mapping(dev, size, true)
}

/// Create a new user-owned GEM object with the given flags.
pub(crate) fn new_object(
    dev: &AsahiDevice,
    size: usize,
    flags: u32,
    parent_object: Option<&shmem::Object<AsahiObject>>,
    reclaim_cpu: bool,
) -> Result<ARef<Object>> {
    if (flags & uapi::drm_asahi_gem_flags_DRM_ASAHI_GEM_VM_PRIVATE != 0) != parent_object.is_some()
    {
        return Err(EINVAL);
    }

    let gem = shmem::Object::<AsahiObject>::new(
        dev,
        checked_object_size(size)?,
        shmem::ObjectConfig::<AsahiObject> {
            reclaimable_cpu_mappings: parent_object.is_some() && reclaim_cpu,
            map_wc: flags & uapi::drm_asahi_gem_flags_DRM_ASAHI_GEM_WRITEBACK == 0,
            parent_resv_obj: parent_object,
            synchronous_reclaim: dev.is_m3,
        },
        AsahiObjConfig {
            flags,
            exportable: parent_object.is_none(),
            kernel: false,
        },
    )?;

    mod_pr_debug!("AsahiObject new user object: id={}\n", gem.id);
    Ok(gem)
}

#[vtable]
impl DriverObject for AsahiObject {
    type Driver = AsahiDriver;
    type Args = AsahiObjConfig;

    const HAS_EXPORT: bool = true;

    /// Callback to create the inner data of a GEM object
    fn new(dev: &AsahiDevice, size: usize, args: Self::Args) -> impl PinInit<Self, Error> {
        let id = GEM_ID.fetch_add(1, Ordering::Relaxed);
        mod_pr_debug!("AsahiObject::new id={}\n", id);
        try_pin_init!(AsahiObject {
            id,
            // All remaining fields are infallible. PinnedDrop balances the
            // accounting once this initializer completes.
            size: {
                crate::gem_stats::note_new(args.kernel, size);
                size
            },
            flags: args.flags,
            exportable: args.exportable,
            kernel: args.kernel,
            _allocation: if dev.is_m3 {
                Some(crate::agx_memory_stats::Allocation::gem(size, args.kernel)?)
            } else { None },
            kernel_mappings: AtomicU64::new(0),
        })
    }

    /// Callback to drop all mappings for a GEM object owned by a given `File`
    fn close(obj: &<Self::Driver as drm::Driver>::Object, file: &drm::gem::DriverFile<Self>) {
        // fn close(obj: &Object, file: &DrmFile) {
        mod_pr_debug!("AsahiObject::close id={}\n", obj.id);
        if file::File::unbind_gem_object(file, obj).is_err() {
            pr_err!("AsahiObject::close: Failed to unbind GEM object\n");
        }
    }

    /// Optional handle for exporting a gem object.
    fn export(
        obj: &<Self::Driver as drm::Driver>::Object,
        flags: u32,
    ) -> Result<drm::gem::DmaBuf<Object>> {
        if !obj.exportable {
            return Err(EINVAL);
        }

        obj.prime_export(flags)
    }
}

/// A driver-created alias of a GEM object, possibly in a root the user GPUVA
/// tree does not describe. It keeps the object out of idle reclaim.
pub(crate) struct KernelMappingPin {
    object: ARef<Object>,
}

impl KernelMappingPin {
    pub(crate) fn new(object: &Object) -> Self {
        object.kernel_mappings.fetch_add(1, Ordering::AcqRel);
        Self {
            object: object.into(),
        }
    }
}

impl core::ops::Deref for KernelMappingPin {
    type Target = Object;
    fn deref(&self) -> &Object {
        &self.object
    }
}

impl Drop for KernelMappingPin {
    fn drop(&mut self) {
        self.object.kernel_mappings.fetch_sub(1, Ordering::Release);
    }
}

impl AsahiObject {
    pub(crate) fn idle_reclaim_candidate(&self) -> bool {
        !self.kernel && !self.exportable && self.kernel_mappings.load(Ordering::Acquire) == 0
    }
}

/// Purge advice for a VM-private, non-exportable object. Returns whether the
/// contents are retained (0 once purged).
pub(crate) fn madvise(gem: &Object, dontneed: bool) -> Result<u32> {
    if gem.flags & uapi::drm_asahi_gem_flags_DRM_ASAHI_GEM_VM_PRIVATE == 0 || gem.exportable {
        return Err(EINVAL);
    }
    Ok(u32::from(gem.madvise(i32::from(dontneed))))
}
