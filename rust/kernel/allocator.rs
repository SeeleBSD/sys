use core::alloc::{GlobalAlloc, Layout};

pub struct KernelAllocator;
use crate::c_str;

unsafe impl GlobalAlloc for KernelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe {
            bindings::malloc(
                layout.size(),
                bindings::M_DRM as i32,
                bindings::M_WAITOK as i32,
            ) as *mut u8
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe {
            bindings::free(ptr as *mut core::ffi::c_void, bindings::M_DRM as i32, 0);
        }
    }
}

#[global_allocator]
static ALLOCATOR: KernelAllocator = KernelAllocator;

#[no_mangle]
static __rust_no_alloc_shim_is_unstable: u8 = 0;

#[no_mangle]
static __rust_alloc_error_handler_should_panic: u8 = 0;

#[no_mangle]
fn __rust_alloc_error_handler(size: usize, align: usize) -> ! {
    panic!("Out of memory!");
}
