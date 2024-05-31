// SPDX-License-Identifier: ISC

#![no_std]

#[macro_use]
extern crate kernel;

pub(crate) mod debug;
pub(crate) mod float;

use kernel::{of, prelude::*};

const __LOG_PREFIX: &'static str = "bronyadrm";
//static mut INFO: Option<&'static HwConfig> = None;
static mut INFO: Option<()> = None;

/*
id_table! { BRONYA_ID_TABLE, &'static hw::HwConfig, [
    ("apple,agx-t8103", Some(&hw::t8103::HWCONFIG)),
    ("apple,agx-t8112", Some(&hw::t8112::HWCONFIG)),
    ("apple,agx-t6000", Some(&hw::t600x::HWCONFIG_T6000)),
    ("apple,agx-t6001", Some(&hw::t600x::HWCONFIG_T6001)),
    ("apple,agx-t6002", Some(&hw::t600x::HWCONFIG_T6002)),
    ("apple,agx-t6020", Some(&hw::t602x::HWCONFIG_T6020)),
    ("apple,agx-t6021", Some(&hw::t602x::HWCONFIG_T6021)),
    ("apple,agx-t6022", Some(&hw::t602x::HWCONFIG_T6022))
] }
*/

id_table! { BRONYA_ID_TABLE, (), [
    (c_str!("apple,agx-t8103"), Some(())),
    (c_str!("apple,agx-t8112"), Some(())),
    (c_str!("apple,agx-t6000"), Some(())),
    (c_str!("apple,agx-t6001"), Some(())),
    (c_str!("apple,agx-t6002"), Some(())),
    (c_str!("apple,agx-t6020"), Some(())),
    (c_str!("apple,agx-t6021"), Some(())),
    (c_str!("apple,agx-t6022"), Some(()))
] }

#[no_mangle]
pub extern "C" fn bronyadrm_match(
    parent: *mut bindings::device,
    _match: *mut core::ffi::c_void,
    aux: *mut core::ffi::c_void,
) -> i32 {
    let faa = unsafe { *(aux as *mut bindings::fdt_attach_args) };
    let handle = faa.fa_node;
    if let Some(node) = of::Node::from_handle(handle) {
        unsafe {
            INFO = compatible_info!(node, BRONYA_ID_TABLE);
        }
        compatible!(node, BRONYA_ID_TABLE)
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn bronyadrm_attach(
    parent: *mut bindings::device,
    _self: *mut bindings::device,
    aux: *mut core::ffi::c_void,
) {
    let sc = _self as *mut bindings::bronyadrm_softc;
    let faa = aux as *mut bindings::fdt_attach_args;
    unsafe {
        (*sc).sc_node = (*faa).fa_node;
    }

    print!("\n");

    unsafe {
        (*sc).sc_dev.faa = faa;
        bindings::platform_device_register(&mut (*sc).sc_dev as *mut bindings::platform_device);
    }
    info!("attached!");
}

#[no_mangle]
pub extern "C" fn bronyadrm_activate(_self: *mut bindings::device, act: i32) -> i32 {
    0
}
