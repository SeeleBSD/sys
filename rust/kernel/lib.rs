// SPDX-License-Identifier: ISC

#![no_std]
#![feature(coerce_unsized)]
#![feature(dispatch_from_dyn)]
#![feature(new_uninit)]
#![feature(receiver_trait)]
#![feature(unsize)]
#![feature(allocator_api)]
#![feature(alloc_error_handler)]
#![feature(strict_provenance)]

extern crate self as kernel;

#[macro_use]
extern crate alloc;
#[macro_use]
extern crate macros;

pub mod allocator;
pub mod build_assert;
pub mod device;
pub mod dma_fence;
pub mod tools;
pub mod user_ptr;
pub(crate) mod private;
pub mod proc;
pub mod soc;
pub mod static_assert;
pub mod str;
pub mod sync;
pub mod time;
pub mod tools;
pub mod types;

#[doc(hidden)]
pub use bindings;
pub use build_error::build_error;
pub use macros;
pub use uapi;

const __LOG_PREFIX: &'static str = "rust_kernel";

#[no_mangle]
pub extern "C" fn __muloti4() -> ! {
    todo!()
}

#[no_mangle]
pub extern "C" fn __udivti3() -> ! {
    todo!()
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    print!("{}\n", info);
    unreachable!()
}

#[no_mangle]
pub extern "C" fn _rust_kernel_main() {
    info!("hello");
}

/// Print kernel debug messages without a trailing newline
#[macro_export]
macro_rules! print {
	// Static (zero-allocation) implementation that uses compile-time `concat!()` only
	($fmt:expr) => ({
		let msg = $crate::c_str!($fmt);
		let ptr = msg.as_ptr() as *const core::ffi::c_char;
		unsafe {
			$crate::bindings::printf(ptr);
		}
	});

	// Dynamic implementation that processes format arguments
	($fmt:expr, $($arg:tt)*) => ({
		use ::core::fmt::Write;
		use $crate::io::KernelDebugWriter;
		let mut writer = KernelDebugWriter {};
        writer.write_fmt(format_args!($fmt, $($arg)*)).unwrap();
	});
}

#[macro_export]
macro_rules! info {
	($fmt:expr)              => ($crate::println!("{}:info: {}", crate::__LOG_PREFIX, alloc::format!($fmt)));
	($fmt:expr, $($arg:tt)+) => ($crate::println!("{}:info: {}", crate::__LOG_PREFIX, alloc::format!($fmt, $($arg)*)));
}

#[macro_export]
macro_rules! warn {
	($fmt:expr)              => ($crate::println!("{}:warn: {}", crate::__LOG_PREFIX, alloc::format!($fmt)));
	($fmt:expr, $($arg:tt)+) => ($crate::println!("{}:warn: {}", crate::__LOG_PREFIX, alloc::format!($fmt, $($arg)*)));
}

#[macro_export]
macro_rules! err {
	($fmt:expr)              => ($crate::println!("{}:err: {}", crate::__LOG_PREFIX, alloc::format!($fmt)));
	($fmt:expr, $($arg:tt)+) => ($crate::println!("{}:err: {}", crate::__LOG_PREFIX, alloc::format!($fmt, $($arg)*)));
}

/// Print kernel debug messages with a trailing newline
#[macro_export]
macro_rules! println {
	($fmt:expr)              => ($crate::print!(concat!($fmt, "\n")));
	($fmt:expr, $($arg:tt)+) => ($crate::print!(concat!($fmt, "\n"), $($arg)*));
}

#[macro_export]
macro_rules! count_items {
	(($($t:tt)*), $($rem:tt)*) => { 1 + $crate::count_items!($($rem)*) };
	(($($t:tt)*)) => { 1 };
	() => { 0 };
}
