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

use core::time::Duration;

use kernel::{
    delay::coarse_sleep,
    device::{Device, RawDevice},
    drm, of, platform,
    prelude::*,
    str::CStr,
    sync::Arc,
};

use crate::driver::{AsahiData, AsahiDriver, DeviceData};
use crate::hw::HwConfig;

const __LOG_PREFIX: &'static str = "asahidrm";
static mut INFO: Option<&'static HwConfig> = None;
static mut PMAP: bindings::pmap_t = core::ptr::null_mut();
static mut DMAT: Option<bindings::bus_dma_tag_t> = None;

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
        (*sc).sc_pm = bindings::pmap_create();
        (*sc).sc_dev.faa = faa;
        bindings::platform_device_register(&mut (*sc).sc_dev as *mut _);
        bindings::drm_attach_platform(
            &drm::drv::Registration::<AsahiDriver>::VTABLE as *const _ as *mut _,
            (*sc).sc_iot,
            (*sc).sc_dmat,
            _self,
            &mut (*sc).sc_ddev as *mut _,
        );
        bindings::config_defer(_self, Some(asahidrm_attachhook));
    }
}

#[no_mangle]
pub extern "C" fn asahidrm_attachhook(_self: *mut bindings::device) {
    let sc = _self as *mut bindings::asahidrm_softc;
    if let Some(node) = of::Node::from_handle(unsafe { (*sc).sc_node }) {
        unsafe {
            INFO = compatible_info!(node, ASAHI_ID_TABLE);
        }
    }
    unsafe {
        DMAT = Some((*sc).sc_dmat);
        PMAP = (*sc).sc_pm;
        bindings::drm_sched_fence_slab_init();
    }
    let cfg = unsafe { INFO.expect("No GPU information!") };

    let dev = unsafe { Device::new(_self) };
    let reg =
        drm::drv::Registration::<AsahiDriver>::new(&dev, unsafe { &mut (*sc).sc_ddev as *mut _ })
            .expect("Failed to create reg");

    let mut pdev =
        unsafe { platform::Device::from_ptr(&mut (*sc).sc_dev as *mut bindings::platform_device) };
    let res = regs::Resources::new(&mut pdev).expect("Failed to create res");

    res.init_mmio().unwrap();
    res.start_cpu().unwrap();

    let node = of::Node::from_handle(unsafe { (*sc).sc_node }).unwrap();
    let compat: Vec<u32> = node
        .get_property(c_str!("apple,firmware-compat"))
        .expect("Failed to get compat");

    let gpu = unsafe {
        match (cfg.gpu_gen, cfg.gpu_variant, compat.as_slice()) {
            (hw::GpuGen::G13, _, &[12, 3, 0]) => {
                gpu::GpuManagerG13V12_3::new(reg.device(), &res, cfg, sc).unwrap()
                    as Arc<dyn gpu::GpuManager>
            }
            (hw::GpuGen::G14, hw::GpuVariant::G, &[12, 4, 0]) => {
                gpu::GpuManagerG14V12_4::new(reg.device(), &res, cfg, sc).unwrap()
                    as Arc<dyn gpu::GpuManager>
            }
            (hw::GpuGen::G13, _, &[13, 5, 0]) => {
                gpu::GpuManagerG13V13_5::new(reg.device(), &res, cfg, sc).unwrap()
                    as Arc<dyn gpu::GpuManager>
            }
            (hw::GpuGen::G14, hw::GpuVariant::G, &[13, 5, 0]) => {
                gpu::GpuManagerG14V13_5::new(reg.device(), &res, cfg, sc).unwrap()
                    as Arc<dyn gpu::GpuManager>
            }
            (hw::GpuGen::G14, _, &[13, 5, 0]) => {
                gpu::GpuManagerG14XV13_5::new(reg.device(), &res, cfg, sc).unwrap()
                    as Arc<dyn gpu::GpuManager>
            }
            _ => {
                dev_info!(
                    dev,
                    "Unsupported GPU/firmware combination ({:?}, {:?}, {:?})\n",
                    cfg.gpu_gen,
                    cfg.gpu_variant,
                    compat
                );
                return;
            }
        }
    };

    let data =
        kernel::new_device_data!(reg, res, AsahiData { dev, gpu }, "Asahi::Registrations").unwrap();
    let data: Arc<DeviceData> = data.into();

    data.gpu.init().unwrap();

    kernel::drm_device_register!(
        unsafe { Pin::new_unchecked(&mut *data.registrations().unwrap()) },
        data.clone(),
        0
    )
    .unwrap();
}

#[no_mangle]
pub extern "C" fn asahidrm_activate(_self: *mut bindings::device, act: i32) -> i32 {
    unsafe { bindings::config_activate_children(_self, act) }
}
