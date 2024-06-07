// SPDX-License-Identifier: ISC

use crate::bindings;

pub mod prot {
    pub const READ: u32 = bindings::BINDINGS_IOMMU_READ;
    pub const WRITE: u32 = bindings::BINDINGS_IOMMU_WRITE;
}