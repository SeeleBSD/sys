// SPDX-License-Identifier: ISC

use crate::bindings;

#[inline(always)]
pub const fn _IOC(inout: u64, group: u64, num: u64, len: usize) -> u64 {
    (inout | (((len as u64) & bindings::BINDINGS_IOCPARM_MASK) << 16) | ((group) << 8) | (num))
}

#[inline(always)]
pub const fn _IO(group: u64, num: u64) -> u64 {
    _IOC(bindings::BINDINGS_IOC_VOID, group, num, 0)
}

#[inline(always)]
pub const fn _IOR<T>(group: u64, num: u64) -> u64 {
    _IOC(
        bindings::BINDINGS_IOC_OUT,
        group,
        num,
        core::mem::size_of::<T>(),
    )
}

#[inline(always)]
pub const fn _IOW<T>(group: u64, num: u64) -> u64 {
    _IOC(
        bindings::BINDINGS_IOC_IN,
        group,
        num,
        core::mem::size_of::<T>(),
    )
}

#[inline(always)]
pub const fn _IOWR<T>(group: u64, num: u64) -> u64 {
    _IOC(
        bindings::BINDINGS_IOC_INOUT,
        group,
        num,
        core::mem::size_of::<T>(),
    )
}
