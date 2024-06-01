// SPDX-License-Identifier: ISC

use kernel::{types::ARef, drm::drv, drm::ioctl, device};

const INFO: drv::DriverInfo = drv::DriverInfo {
    major: 0,
    minor: 0,
    patchlevel: 0,
    name: c_str!("bronya"),
    desc: c_str!("Apple AGX Graphics"),
    date: c_str!("20240602"),
};

pub struct BronyaData {
    pub dev: device::Device,
    pub gpu: Arc<dyn gpu::GpuManager>,
}

type DeviceData = device::Data<drv::Registration<BronyaDriver>, regs::Resources, BronyaData>;

pub struct BronyaDriver;
pub type BronyaDevice = drm::device::Device<BronyaDriver>;
pub type BronyaDevRef = ARef<BronyaDevice>;

/// DRM Driver implementation for `AsahiDriver`.
#[vtable]
impl drv::Driver for AsahiDriver {
    /// Our `DeviceData` type, reference-counted
    type Data = Arc<DeviceData>;
    /// Our `File` type.
    type File = file::File;
    /// Our `Object` type.
    type Object = gem::Object;

    const INFO: drv::DriverInfo = INFO;
    const FEATURES: u32 =
        drv::FEAT_GEM | drv::FEAT_RENDER | drv::FEAT_SYNCOBJ | drv::FEAT_SYNCOBJ_TIMELINE;

    kernel::declare_drm_ioctls! {
        (ASAHI_GET_PARAMS,      drm_bronya_get_params,
                          ioctl::RENDER_ALLOW, file::File::get_params),
        (ASAHI_VM_CREATE,       drm_bronya_vm_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, file::File::vm_create),
        (ASAHI_VM_DESTROY,      drm_bronya_vm_destroy,
            ioctl::AUTH | ioctl::RENDER_ALLOW, file::File::vm_destroy),
        (ASAHI_GEM_CREATE,      drm_bronya_gem_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, file::File::gem_create),
        (ASAHI_GEM_MMAP_OFFSET, drm_bronya_gem_mmap_offset,
            ioctl::AUTH | ioctl::RENDER_ALLOW, file::File::gem_mmap_offset),
        (ASAHI_GEM_BIND,        drm_bronya_gem_bind,
            ioctl::AUTH | ioctl::RENDER_ALLOW, file::File::gem_bind),
        (ASAHI_QUEUE_CREATE,    drm_bronya_queue_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, file::File::queue_create),
        (ASAHI_QUEUE_DESTROY,   drm_bronya_queue_destroy,
            ioctl::AUTH | ioctl::RENDER_ALLOW, file::File::queue_destroy),
        (ASAHI_SUBMIT,          drm_bronya_submit,
            ioctl::AUTH | ioctl::RENDER_ALLOW, file::File::submit),
    }
