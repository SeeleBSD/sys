// SPDX-License-Identifier: ISC

use crate::bindings;

pub unsafe trait RawDevice {
    fn raw_device(&self) -> *mut bindings::device;
}

pub struct Device {
    pub ptr: *mut bindings::device,
}

impl Device {
    pub unsafe fn new(ptr: *mut bindings::device) -> Self {
        Self { ptr }
    }
}

unsafe impl Send for Device {}
unsafe impl Sync for Device {}

unsafe impl RawDevice for Device {
    fn raw_device(&self) -> *mut bindings::device {
        self.ptr
    }
}