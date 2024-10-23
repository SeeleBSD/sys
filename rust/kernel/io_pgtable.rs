// SPDX-License-Identifier: ISC

use crate::{
    bindings, device,
    error::{code::*, to_result, Result},
    types::{ForeignOwnable, ScopeGuard},
};

use kernel::println;

use core::marker::PhantomData;
use core::mem;
use core::num::NonZeroU64;

pub mod prot {
    pub const READ: u32 = bindings::BINDINGS_IOMMU_READ;
    pub const WRITE: u32 = bindings::BINDINGS_IOMMU_WRITE;
}

pub struct Config {
    pub quirks: usize,
    pub pgsize_bitmap: usize,
    pub ias: usize,
    pub oas: usize,
    pub coherent_walk: bool,

    pub dmat: bindings::bus_dma_tag_t,
    pub dmamap: bindings::bus_dmamap_t,
}

pub trait FlushOps {
    type Data: ForeignOwnable + Send + Sync;

    fn tlb_flush_all(data: <Self::Data as ForeignOwnable>::Borrowed<'_>);

    fn tlb_flush_walk(
        data: <Self::Data as ForeignOwnable>::Borrowed<'_>,
        iova: usize,
        size: usize,
        granule: usize,
    );

    fn tlb_add_page(
        data: <Self::Data as ForeignOwnable>::Borrowed<'_>,
        iova: usize,
        granule: usize,
    );
}

pub struct IoPageTableInner {
    ops: *mut bindings::io_pgtable_ops,
    cfg: bindings::io_pgtable_cfg,
    data: *mut core::ffi::c_void,
}

pub trait GetConfig {
    fn cfg(iopt: &impl IoPageTable) -> &Self
    where
        Self: Sized;
}

pub trait IoPageTable: crate::private::Sealed {
    const FLUSH_OPS: bindings::iommu_flush_ops;

    fn new_fmt<T: FlushOps>(
        dev: &dyn device::RawDevice,
        format: u32,
        config: Config,
        data: T::Data,
    ) -> Result<IoPageTableInner> {
        let ptr = data.into_foreign() as *mut _;
        let guard = ScopeGuard::new(|| {
            // SAFETY: `ptr` came from a previous call to `into_foreign`.
            unsafe { T::Data::from_foreign(ptr) };
        });

        let mut raw_cfg = bindings::io_pgtable_cfg {
            quirks: config.quirks.try_into()?,
            pgsize_bitmap: config.pgsize_bitmap.try_into()?,
            ias: config.ias.try_into()?,
            oas: config.oas.try_into()?,
            coherent_walk: config.coherent_walk,
            dmat: config.dmat,
            dmamap: config.dmamap,
            tlb: &Self::FLUSH_OPS,
            iommu_dev: dev.raw_device(),
            alloc: None,
            free: None,
            __bindgen_anon_1: unsafe { mem::zeroed() },
        };

        let ops = unsafe {
            bindings::alloc_io_pgtable_ops(format as bindings::io_pgtable_fmt, &mut raw_cfg, ptr)
        };

        if ops.is_null() {
            println!("EINVAL due to ops.is_null()");
            return Err(EINVAL);
        }

        guard.dismiss();
        Ok(IoPageTableInner {
            ops,
            cfg: raw_cfg,
            data: ptr,
        })
    }

    fn map_pages(
        &mut self,
        iova: usize,
        paddr: usize,
        pgsize: usize,
        pgcount: usize,
        prot: u32,
    ) -> Result<usize> {
        let mut mapped: usize = 0;

        to_result(unsafe {
            (*self.inner_mut().ops).map_pages.unwrap()(
                self.inner_mut().ops,
                iova as u64,
                paddr as u64,
                pgsize,
                pgcount,
                prot as i32,
                bindings::GFP_KERNEL,
                &mut mapped,
            )
        })?;

        Ok(mapped)
    }

    fn unmap_pages(
        &mut self,
        iova: usize,
        pgsize: usize,
        pgcount: usize,
        // TODO: gather: *mut iommu_iotlb_gather,
    ) -> usize {
        unsafe {
            (*self.inner_mut().ops).unmap_pages.unwrap()(
                self.inner_mut().ops,
                iova as u64,
                pgsize,
                pgcount,
                core::ptr::null_mut(),
            )
        }
    }

    fn iova_to_phys(&mut self, iova: usize) -> Option<NonZeroU64> {
        NonZeroU64::new(unsafe {
            (*self.inner_mut().ops).iova_to_phys.unwrap()(self.inner_mut().ops, iova as u64)
        })
    }

    #[doc(hidden)]
    fn inner_mut(&mut self) -> &mut IoPageTableInner;

    #[doc(hidden)]
    fn inner(&self) -> &IoPageTableInner;

    #[doc(hidden)]
    fn raw_cfg(&self) -> &bindings::io_pgtable_cfg {
        &self.inner().cfg
    }
}

unsafe impl Send for IoPageTableInner {}
unsafe impl Sync for IoPageTableInner {}

unsafe extern "C" fn tlb_flush_all_callback<T: FlushOps>(cookie: *mut core::ffi::c_void) {
    T::tlb_flush_all(unsafe { T::Data::borrow(cookie) });
}

unsafe extern "C" fn tlb_flush_walk_callback<T: FlushOps>(
    iova: core::ffi::c_ulong,
    size: usize,
    granule: usize,
    cookie: *mut core::ffi::c_void,
) {
    T::tlb_flush_walk(
        unsafe { T::Data::borrow(cookie) },
        iova as usize,
        size,
        granule,
    );
}

unsafe extern "C" fn tlb_add_page_callback<T: FlushOps>(
    _gather: *mut bindings::iommu_iotlb_gather,
    iova: core::ffi::c_ulong,
    granule: usize,
    cookie: *mut core::ffi::c_void,
) {
    T::tlb_add_page(unsafe { T::Data::borrow(cookie) }, iova as usize, granule);
}

macro_rules! iopt_cfg {
    ($name:ident, $field:ident, $type:ident) => {
        /// An IOMMU page table configuration for a specific kind of pagetable.
        pub type $name = bindings::$type;

        impl GetConfig for $name {
            fn cfg(iopt: &impl IoPageTable) -> &$name {
                unsafe { &iopt.raw_cfg().__bindgen_anon_1.$field }
            }
        }
    };
}

impl GetConfig for () {
    fn cfg(_iopt: &impl IoPageTable) -> &() {
        &()
    }
}

macro_rules! iopt_type {
    ($type:ident, $cfg:ty, $fmt:ident) => {
        /// Represents an IOPagetable of this type.
        pub struct $type<T: FlushOps>(IoPageTableInner, PhantomData<T>);

        impl<T: FlushOps> $type<T> {
            /// Creates a new IOPagetable implementation of this type.
            pub fn new(dev: &dyn device::RawDevice, config: Config, data: T::Data) -> Result<Self> {
                Ok(Self(
                    <Self as IoPageTable>::new_fmt::<T>(dev, bindings::$fmt, config, data)?,
                    PhantomData,
                ))
            }

            /// Get the configuration for this IOPagetable.
            pub fn cfg(&self) -> &$cfg {
                <$cfg as GetConfig>::cfg(self)
            }
        }

        impl<T: FlushOps> crate::private::Sealed for $type<T> {}

        impl<T: FlushOps> IoPageTable for $type<T> {
            const FLUSH_OPS: bindings::iommu_flush_ops = bindings::iommu_flush_ops {
                tlb_flush_all: Some(tlb_flush_all_callback::<T>),
                tlb_flush_walk: Some(tlb_flush_walk_callback::<T>),
                tlb_add_page: Some(tlb_add_page_callback::<T>),
            };

            fn inner(&self) -> &IoPageTableInner {
                &self.0
            }

            fn inner_mut(&mut self) -> &mut IoPageTableInner {
                &mut self.0
            }
        }

        impl<T: FlushOps> Drop for $type<T> {
            fn drop(&mut self) {
                // SAFETY: The pointer is valid by the type invariant.
                unsafe { bindings::free_io_pgtable_ops(self.0.ops) };

                // Free context data.
                //
                // SAFETY: This matches the call to `into_foreign` from `new` in the success case.
                unsafe { T::Data::from_foreign(self.0.data) };
            }
        }
    };
}

iopt_cfg!(
    AppleUATCfg,
    apple_uat_cfg,
    io_pgtable_cfg__bindgen_ty_1__bindgen_ty_2
);

iopt_type!(AppleUAT, AppleUATCfg, io_pgtable_fmt_APPLE_UAT);