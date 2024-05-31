// SPDX-License-Identifier: ISC

use core::marker::PhantomData;
use core::time::Duration;

pub mod clock;

pub trait Clock: Sized {}

pub trait Now: Clock {
    fn now() -> Instance<Self>;
}

pub trait Monotonic {}

pub trait WallTime {}

#[derive(Debug)]
pub struct Instance<T: Clock> {
    nanosecs: u64,
    _type: PhantomData<T>,
}

impl<T: Clock> Clone for Instance<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T: Clock> Copy for Instance<T> {}

impl<T: Clock> Instance<T> {
    fn new(nanosecs: u64) -> Self {
        Instance {
            nanosecs,
            _type: PhantomData,
        }
    }

    pub fn since(&self, earlier: Instance<T>) -> Option<Duration> {
        if earlier.nanosecs > self.nanosecs {
            None
        } else {
            Some(Duration::from_nanos(self.nanosecs - earlier.nanosecs))
        }
    }
}

impl<T: Clock + Now + Monotonic> Instance<T> {
    pub fn elapsed(&self) -> Duration {
        T::now().since(*self).unwrap_or(Duration::ZERO)
    }
}
