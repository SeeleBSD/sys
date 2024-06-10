// SPDX-License-Identifier: GPL-2.0

//! Platform devices and drivers.
//!
//! Also called `platdev`, `pdev`.
//!
//! C header: [`include/linux/platform_device.h`](../../../../include/linux/platform_device.h)

use crate::{
    bindings,
    device::{self, RawDevice},
    // driver,
    error::{code::*, from_result, to_result, Result},
    io_mem::{IoMem, IoResource, Resource},
    of,
    str::CStr,
    types::ForeignOwnable,
    // ThisModule,
};

/// A platform device.
///
/// # Invariants
///
/// The field `ptr` is non-null and valid for the lifetime of the object.
pub struct Device {
    ptr: *mut bindings::platform_device,
    used_resource: u64,
}

impl Device {
    /// Creates a new device from the given pointer.
    ///
    /// # Safety
    ///
    /// `ptr` must be non-null and valid. It must remain valid for the lifetime of the returned
    /// instance.
    unsafe fn from_ptr(ptr: *mut bindings::platform_device) -> Self {
        // INVARIANT: The safety requirements of the function ensure the lifetime invariant.
        Self {
            ptr,
            used_resource: 0,
        }
    }

    /*
    /// Returns id of the platform device.
    pub fn id(&self) -> i32 {
        // SAFETY: By the type invariants, we know that `self.ptr` is non-null and valid.
        unsafe { (*self.ptr).id }
    }
    */

    /// Sets the DMA masks (normal and coherent) for a platform device.
    pub fn set_dma_masks(&mut self, mask: u64) -> Result {
        // to_result(unsafe { bindings::dma_set_mask_and_coherent(&mut (*self.ptr).dev, mask) })
        to_result(0)
    }

    /// Gets a system resources of a platform device.
    pub fn get_resource(&mut self, rtype: IoResource, num: usize) -> Result<Resource> {
        // SAFETY: `self.ptr` is valid by the type invariant.
        let res = unsafe { bindings::platform_get_resource(self.ptr, rtype as _, num as _) };
        if res.is_null() {
            return Err(EINVAL);
        }

        // Get the position of the found resource in the array.
        // SAFETY:
        //   - `self.ptr` is valid by the type invariant.
        //   - `res` is a displaced pointer to one of the array's elements,
        //     and `resource` is its base pointer.
        let index = unsafe { res.offset_from((*self.ptr).resource) } as usize;

        // Make sure that the index does not exceed the 64-bit mask.
        assert!(index < 64);

        if self.used_resource >> index & 1 == 1 {
            return Err(EBUSY);
        }
        self.used_resource |= 1 << index;

        // SAFETY: The pointer `res` is returned from `bindings::platform_get_resource`
        // above and checked if it is not a NULL.
        unsafe {
            Resource::new((*res).start, (*res).end, /*(*res).flags*/ 0)
        }
        .ok_or(EINVAL)
    }

    /// Ioremaps resources of a platform device.
    ///
    /// # Safety
    ///
    /// Callers must ensure that either (a) the resulting interface cannot be used to initiate DMA
    /// operations, or (b) that DMA operations initiated via the returned interface use DMA handles
    /// allocated through the `dma` module.
    pub unsafe fn ioremap_resource<const SIZE: usize>(
        &mut self,
        index: usize,
    ) -> Result<IoMem<SIZE>> {
        let mask = self.used_resource;
        let res = self.get_resource(IoResource::Mem, index)?;

        // SAFETY: Valid by the safety contract.
        let iomem = unsafe { IoMem::<SIZE>::try_new(res, (*self.ptr).iot) };
        // If remapping fails, the given resource won't be used, so restore the old mask.
        if iomem.is_err() {
            self.used_resource = mask;
        }
        iomem
    }
}

// SAFETY: The device returned by `raw_device` is the raw platform device.
unsafe impl device::RawDevice for Device {
    fn raw_device(&self) -> *mut bindings::device {
        // SAFETY: By the type invariants, we know that `self.ptr` is non-null and valid.
        unsafe { &mut (*self.ptr).dev }
    }
}
