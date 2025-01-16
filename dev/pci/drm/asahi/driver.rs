// SPDX-License-Identifier: ISC

use kernel::{device, drm, drm::drv, drm::ioctl, prelude::*, sync::Arc, types::ARef};

use crate::{file, gem, gpu, regs};

const INFO: drv::DriverInfo = drv::DriverInfo {
    major: 0,
    minor: 1,
    patchlevel: 0,
    name: c_str!("asahi"),
    desc: c_str!("Apple AGX Graphics"),
    date: c_str!("20231005"),
};

pub(crate) struct AsahiData {
    pub dev: device::Device,
    pub gpu: Arc<dyn gpu::GpuManager>,
}

pub(crate) type DeviceData =
    device::Data<drv::Registration<AsahiDriver>, regs::Resources, AsahiData>;

pub(crate) struct AsahiDriver;
pub(crate) type AsahiDevice = drm::device::Device<AsahiDriver>;
pub(crate) type AsahiDevRef = ARef<AsahiDevice>;

#[vtable]
impl drv::Driver for AsahiDriver {
    type Data = Arc<DeviceData>;
    type File = crate::file::File;
    type Object = gem::Object;

    const INFO: drv::DriverInfo = INFO;
    const FEATURES: u32 =
        drv::FEAT_GEM | drv::FEAT_RENDER | drv::FEAT_SYNCOBJ | drv::FEAT_SYNCOBJ_TIMELINE;

    kernel::declare_drm_ioctls! {
                (ASAHI_GET_PARAMS,      drm_asahi_get_params,
                          ioctl::RENDER_ALLOW, crate::file::File::get_params),
        (ASAHI_VM_CREATE,       drm_asahi_vm_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::vm_create),
        (ASAHI_VM_DESTROY,      drm_asahi_vm_destroy,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::vm_destroy),
        (ASAHI_GEM_CREATE,      drm_asahi_gem_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_create),
        (ASAHI_GEM_MMAP_OFFSET, drm_asahi_gem_mmap_offset,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_mmap_offset),
        (ASAHI_GEM_BIND,        drm_asahi_gem_bind,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_bind),
        (ASAHI_QUEUE_CREATE,    drm_asahi_queue_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::queue_create),
        (ASAHI_QUEUE_DESTROY,   drm_asahi_queue_destroy,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::queue_destroy),
        (ASAHI_SUBMIT,          drm_asahi_submit,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::submit),
    }
}
