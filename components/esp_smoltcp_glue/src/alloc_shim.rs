//! Plug Rust's global allocator into ESP-IDF's heap.
//!
//! smoltcp uses `alloc::vec::Vec` internally for buffer storage. Rather
//! than ship a separate allocator, we forward `alloc::alloc::*` to
//! `heap_caps_malloc` so allocations land in normal IDF heap (which is
//! already PSRAM-aware via SPIRAM_USE_MALLOC).

use core::alloc::{GlobalAlloc, Layout};

extern "C" {
    fn heap_caps_aligned_alloc(alignment: usize, size: usize, caps: u32) -> *mut u8;
    fn heap_caps_free(ptr: *mut u8);
}

const MALLOC_CAP_8BIT:     u32 = 1 << 2;
const MALLOC_CAP_INTERNAL: u32 = 1 << 11;

/*
 * Two-tier allocation strategy:
 *   1. Try internal SRAM first — it's ~10× faster than cached PSRAM and
 *      the smoltcp hot path (TX/RX ring buffers, per-segment Vecs) is
 *      sensitive to allocator latency.
 *   2. Fall back to any 8-bit heap (PSRAM included) only if internal is
 *      exhausted, so we never panic on capacity overflow under load.
 *
 * Without the internal-first preference, large allocs (32 KiB+ socket
 * buffers) tend to land in PSRAM where each per-packet TX zero-fill
 * holds the smoltcp lock for hundreds of microseconds and starves
 * concurrent socket calls.
 */
struct EspAlloc;

unsafe impl GlobalAlloc for EspAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align().max(4);
        let p = heap_caps_aligned_alloc(align, layout.size(),
                                        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if !p.is_null() { return p; }
        heap_caps_aligned_alloc(align, layout.size(), MALLOC_CAP_8BIT)
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        heap_caps_free(ptr);
    }
}

#[global_allocator]
static GLOBAL: EspAlloc = EspAlloc;
