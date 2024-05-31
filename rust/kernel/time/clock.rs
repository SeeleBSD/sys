use super::*;
use crate::bindings;

pub struct KernelTime;

impl Clock for KernelTime {

}

impl Monotonic for KernelTime {

}

impl Now for KernelTime {
    fn now() -> Instance<Self> {
        Instance::<Self>::new(unsafe { bindings::gettime() as u64 })
    }
}

pub struct UpTime;

impl Clock for UpTime {

}

impl Monotonic for UpTime {

}

impl Now for Uptime {
    fn now() -> Instance<Self> {
        Instance::<Self>>::new(unsafe { bindings::getuptime() as u64 })
    }
}