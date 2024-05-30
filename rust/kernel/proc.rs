// SPDX-License-Identifier: GPL-2.0

//! Procs (threads and processes).
//!
//! C header: [`include/linux/sched.h`](../../../../include/linux/sched.h).

use crate::{bindings, types::Opaque};
use core::{marker::PhantomData, ops::Deref, ptr};

/// Returns the currently running proc.
#[macro_export]
macro_rules! current {
    () => {
        // SAFETY: Deref + addr-of below create a temporary `ProcRef` that cannot outlive the
        // caller.
        unsafe { &*$crate::proc::Proc::current() }
    };
}

/// Wraps the kernel's `struct proc`.
///
/// # Invariants
///
/// All instances are valid procs created by the C portion of the kernel.
///
/// Instances of this type are always ref-counted, that is, a call to `get_proc_struct` ensures
/// that the allocation remains valid at least until the matching call to `put_proc_struct`.
///
/// # Examples
///
/// The following is an example of getting the PID of the current thread with zero additional cost
/// when compared to the C version:
///
/// ```
/// let pid = current!().pid();
/// ```
///
/// Getting the PID of the current process, also zero additional cost:
///
/// ```
/// let pid = current!().group_leader().pid();
/// ```
///
/// Getting the current proc and storing it in some struct. The reference count is automatically
/// incremented when creating `State` and decremented when it is dropped:
///
/// ```
/// use kernel::{proc::Proc, types::ARef};
///
/// struct State {
///     creator: ARef<Proc>,
///     index: u32,
/// }
///
/// impl State {
///     fn new() -> Self {
///         Self {
///             creator: current!().into(),
///             index: 0,
///         }
///     }
/// }
/// ```
#[repr(transparent)]
pub struct Proc(pub(crate) Opaque<bindings::proc_>);

// SAFETY: By design, the only way to access a `Proc` is via the `current` function or via an
// `ARef<Proc>` obtained through the `AlwaysRefCounted` impl. This means that the only situation in
// which a `Proc` can be accessed mutably is when the refcount drops to zero and the destructor
// runs. It is safe for that to happen on any thread, so it is ok for this type to be `Send`.
unsafe impl Send for Proc {}

// SAFETY: It's OK to access `Proc` through shared references from other threads because we're
// either accessing properties that don't change (e.g., `pid`, `group_leader`) or that are properly
// synchronised by C code (e.g., `signal_pending`).
unsafe impl Sync for Proc {}

/// The type of process identifiers (PIDs).
type Pid = bindings::pid_t;

impl Proc {
    /// Returns a proc reference for the currently executing proc/thread.
    ///
    /// The recommended way to get the current proc/thread is to use the
    /// [`current`](crate::current) macro because it is safe.
    ///
    /// # Safety
    ///
    /// Callers must ensure that the returned object doesn't outlive the current proc/thread.
    pub unsafe fn current() -> impl Deref<Target = Proc> {
        struct ProcRef<'a> {
            proc: &'a Proc,
            _not_send: PhantomData<*mut ()>,
        }

        impl Deref for ProcRef<'_> {
            type Target = Proc;

            fn deref(&self) -> &Self::Target {
                self.proc
            }
        }

        // SAFETY: Just an FFI call with no additional safety requirements.
        let ptr = unsafe { bindings::BINDING_curproc() };

        ProcRef {
            // SAFETY: If the current thread is still running, the current proc is valid. Given
            // that `ProcRef` is not `Send`, we know it cannot be transferred to another thread
            // (where it could potentially outlive the caller).
            proc: unsafe { &*ptr.cast() },
            _not_send: PhantomData,
        }
    }

    /// Returns the group leader of the given proc.
    pub fn group_leader(&self) -> &Proc {
        // SAFETY: By the type invariant, we know that `self.0` is a valid proc. Valid procs always
        // have a valid group_leader.
        let ptr = unsafe { bindings::BINDING_curproc() };

        // SAFETY: The lifetime of the returned proc reference is tied to the lifetime of `self`,
        // and given that a proc has a reference to its group leader, we know it must be valid for
        // the lifetime of the returned proc reference.
        unsafe { &*ptr.cast() }
    }

    /// Returns the PID of the given proc.
    pub fn pid(&self) -> Pid {
        // SAFETY: By the type invariant, we know that `self.0` is a valid proc. Valid procs always
        // have a valid pid.
        unsafe { *ptr::addr_of!((*(*self.0.get()).p_p).ps_pid) }
    }

    /// Determines whether the given proc has pending signals.
    pub fn signal_pending(&self) -> bool {
        // SAFETY: By the type invariant, we know that `self.0` is valid.
        unsafe { bindings::BINDING_signal_pending(self.0.get()) != 0 }
    }

    /// Wakes up the proc.
    pub fn wake_up(&self) {
        // SAFETY: By the type invariant, we know that `self.0.get()` is non-null and valid.
        // And `wake_up_process` is safe to be called for any valid proc, even if the proc is
        // running.
        unsafe { bindings::wake_up_process(self.0.get()) };
    }
}

// SAFETY: The type invariants guarantee that `Proc` is always ref-counted.
unsafe impl crate::types::AlwaysRefCounted for Proc {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference means that the refcount is nonzero.
        //unsafe { bindings::get_proc_struct(self.0.get()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        //unsafe { bindings::put_proc_struct(obj.cast().as_ptr()) }
    }
}
