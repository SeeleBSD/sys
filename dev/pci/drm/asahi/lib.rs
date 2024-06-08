// SPDX-License-Identifier: ISC

#![no_std]
#![recursion_limit = "2048"]
#![feature(impl_trait_in_assoc_type)]

#[macro_use]
extern crate kernel;

pub(crate) mod alloc;
pub(crate) mod buffer;
pub(crate) mod channel;
pub(crate) mod debug;
pub(crate) mod driver;
pub(crate) mod event;
pub(crate) mod file;
pub(crate) mod float;
pub(crate) mod fw;
pub(crate) mod gem;
pub(crate) mod gpu;
pub(crate) mod hw;
pub(crate) mod initdata;
pub(crate) mod mem;
pub(crate) mod microseq;
pub(crate) mod mmu;
pub(crate) mod object;
pub(crate) mod queue;
pub(crate) mod regs;
pub(crate) mod slotalloc;
pub(crate) mod util;
pub(crate) mod workqueue;

use kernel::{of, prelude::*};

use crate::hw::HwConfig;

const __LOG_PREFIX: &'static str = "asahidrm";
static mut INFO: Option<&'static HwConfig> = None;

id_table! { ASAHI_ID_TABLE, &'static hw::HwConfig, [
    (c_str!("apple,agx-t8103"), Some(&hw::t8103::HWCONFIG)),
    (c_str!("apple,agx-t8112"), Some(&hw::t8112::HWCONFIG)),
    (c_str!("apple,agx-t6000"), Some(&hw::t600x::HWCONFIG_T6000)),
    (c_str!("apple,agx-t6001"), Some(&hw::t600x::HWCONFIG_T6001)),
    (c_str!("apple,agx-t6002"), Some(&hw::t600x::HWCONFIG_T6002)),
    (c_str!("apple,agx-t6020"), Some(&hw::t602x::HWCONFIG_T6020)),
    (c_str!("apple,agx-t6021"), Some(&hw::t602x::HWCONFIG_T6021)),
    (c_str!("apple,agx-t6022"), Some(&hw::t602x::HWCONFIG_T6022))
] }

#[no_mangle]
pub extern "C" fn asahidrm_match(
    parent: *mut bindings::device,
    _match: *mut core::ffi::c_void,
    aux: *mut core::ffi::c_void,
) -> i32 {
    let faa = unsafe { *(aux as *mut bindings::fdt_attach_args) };
    let handle = faa.fa_node;
    if let Some(node) = of::Node::from_handle(handle) {
        unsafe {
            INFO = compatible_info!(node, ASAHI_ID_TABLE);
        }
        compatible!(node, ASAHI_ID_TABLE)
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn asahidrm_attach(
    parent: *mut bindings::device,
    _self: *mut bindings::device,
    aux: *mut core::ffi::c_void,
) {
    let sc = _self as *mut bindings::asahidrm_softc;
    let faa = aux as *mut bindings::fdt_attach_args;
    unsafe {
        (*sc).sc_node = (*faa).fa_node;
        (*sc).sc_iot = (*faa).fa_iot;
        (*sc).sc_dmat = (*faa).fa_dmat;
    }

    print!("\n");

    unsafe {
        (*sc).sc_dev.faa = faa;
        bindings::platform_device_register(&mut (*sc).sc_dev as *mut bindings::platform_device);
    }
    info!("attached!");
}

#[no_mangle]
pub extern "C" fn asahidrm_activate(_self: *mut bindings::device, act: i32) -> i32 {
    0
}
