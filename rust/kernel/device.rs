// SPDX-License-Identifier: ISC

use crate::{
    bindings,
    error::Result,
    macros::pin_data,
    of,
    of::Node,
    pin_init,
    str::CStr,
    sync::{lock::mutex, lock::Guard, LockClassKey, Mutex, UniqueArc},
};

use crate::init::InPlaceInit;

use core::{
    ops::{Deref, DerefMut},
    pin::Pin,
};

pub unsafe trait RawDevice {
    fn raw_device(&self) -> *mut bindings::device;

    fn of_node(&self) -> Option<Node>;
    /*fn of_node(&self) -> Option<Node> {
        let rdev = self.raw_device();
        let rnode = unsafe { bindings::__of_devnode((*rdev).dv_cfdata as *mut core::ffi::c_void) };
        unsafe { Node::from_raw(rnode) }
    }*/
}

pub struct Device {
    pub ptr: *mut bindings::device,
}

impl Device {
    pub unsafe fn new(ptr: *mut bindings::device) -> Self {
        Self { ptr }
    }

    pub fn from_dev(dev: &dyn RawDevice) -> Self {
        unsafe { Self::new(dev.raw_device()) }
    }
}

unsafe impl Send for Device {}
unsafe impl Sync for Device {}

unsafe impl RawDevice for Device {
    fn raw_device(&self) -> *mut bindings::device {
        self.ptr
    }

    fn of_node(&self) -> Option<Node> {
        // let rdev = self.raw_device();
        // let rnode = unsafe { bindings::__of_devnode((*rdev).dv_cfdata as *mut core::ffi::c_void) };
        // unsafe { Node::from_raw(rnode) }
        None
    }
}

#[macro_export]
macro_rules! new_device_data {
    ($reg:expr, $res:expr, $gen:expr, $name:literal) => {{
       static CLASS1: $crate::sync::LockClassKey = $crate::static_lock_class!();
       let regs = $reg;
       let res = $res;
       let gen = $gen;
       let name = $crate::c_str!($name);
       $crate::device::Data::try_new(regs, res, gen, name, CLASS1) 
    }};
}

#[pin_data]
pub struct Data<T, U, V> {
    #[pin]
    regs: Mutex<T>,
    res: U,
    general: V,
}

impl<T, U, V> Data<T, U, V> {
    pub fn try_new(
        regs: T,
        res: U,
        general: V,
        name: &'static CStr,
        key: LockClassKey,
    ) -> Result<Pin<UniqueArc<Self>>> {
        let ret = UniqueArc::pin_init(pin_init!(Self {
            regs <- Mutex::new_with_key(regs, name, key),
            res,
            general,
        }))?;
        Ok(ret)
    }

    pub fn registrations(&self) -> Option<Guard<'_, T, mutex::MutexBackend>> {
        Some(self.regs.lock())
    }

    pub fn res(&self) -> Option<&U> {
        Some(&self.res)
    }
}

impl<T, U, V> Deref for Data<T, U, V> {
    type Target = V;

    fn deref(&self) -> &V {
        &self.general
    }
}

impl<T, U, V> DerefMut for Data<T, U, V> {
    fn deref_mut(&mut self) -> &mut V {
        &mut self.general
    }
}
