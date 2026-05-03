//! On panic, abort. ESP-IDF's panic handler will catch the trap and
//! print a backtrace from the C side.

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    extern "C" { fn abort() -> !; }
    unsafe { abort() }
}
