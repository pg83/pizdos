//! aot as a wasm module, for the kernel to compile modules with. It imports
//! nothing and exports its memory and four functions:
//!
//!     aot_input(size) -> address    a buffer for the module's bytes
//!     aot_compile() -> status       0: the output is the artifact, 1: an error message
//!     aot_output() -> address
//!     aot_output_size() -> size
//!
//! aot_input drops the previous input and output.

#![no_std]

extern crate alloc;

use alloc::vec;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::cell::UnsafeCell;
use core::ptr::null_mut;

const PAGE: usize = 65536;

/// The module runs on one thread; this is its only mutable state.
struct Single<T>(UnsafeCell<T>);

unsafe impl<T> Sync for Single<T> {}

impl<T> Single<T> {
    #[allow(clippy::mut_from_ref)]
    fn get(&self) -> &mut T {
        unsafe { &mut *self.0.get() }
    }
}

/// Memory comes from memory.grow and is never given back. A block's size
/// class is its size and alignment rounded up to a power of two; freed
/// blocks wait in a list per class. Classes below a page are cut from
/// page-sized chunks at multiples of their size, so a block is aligned to its
/// class; larger classes are runs of whole pages.
struct Heap {
    free: [usize; usize::BITS as usize],
    cursor: usize,
    end: usize,
}

fn class(size: usize, align: usize) -> usize {
    size.max(align)
        .max(size_of::<usize>())
        .next_power_of_two()
        .trailing_zeros() as usize
}

fn grow(pages: usize) -> usize {
    match core::arch::wasm32::memory_grow(0, pages) {
        usize::MAX => 0,
        old => old * PAGE,
    }
}

static HEAP: Single<Heap> = Single(UnsafeCell::new(Heap {
    free: [0; usize::BITS as usize],
    cursor: 0,
    end: 0,
}));

struct Allocator;

#[global_allocator]
static ALLOCATOR: Allocator = Allocator;

unsafe impl GlobalAlloc for Allocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let heap = HEAP.get();
        let class = class(layout.size(), layout.align());
        let head = heap.free[class];

        if head != 0 {
            heap.free[class] = unsafe { *(head as *const usize) };

            return head as *mut u8;
        }

        let size = 1 << class;

        if size >= PAGE {
            return grow(size / PAGE) as *mut u8;
        }

        let mut block = heap.cursor.next_multiple_of(size);

        if block + size > heap.end {
            block = grow(1);

            if block == 0 {
                return null_mut();
            }

            heap.end = block + PAGE;
        }

        heap.cursor = block + size;

        block as *mut u8
    }

    unsafe fn dealloc(&self, block: *mut u8, layout: Layout) {
        let heap = HEAP.get();
        let class = class(layout.size(), layout.align());
        unsafe { *(block as *mut usize) = heap.free[class] };
        heap.free[class] = block as usize;
    }

    unsafe fn realloc(&self, block: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        if class(size, layout.align()) == class(layout.size(), layout.align()) {
            return block;
        }

        let moved = unsafe { self.alloc(Layout::from_size_align_unchecked(size, layout.align())) };

        if !moved.is_null() {
            unsafe {
                core::ptr::copy_nonoverlapping(block, moved, layout.size().min(size));
                self.dealloc(block, layout);
            }
        }

        moved
    }
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

struct Io {
    input: Vec<u8>,
    output: Vec<u8>,
}

static IO: Single<Io> = Single(UnsafeCell::new(Io {
    input: Vec::new(),
    output: Vec::new(),
}));

#[unsafe(no_mangle)]
pub extern "C" fn aot_input(size: usize) -> *mut u8 {
    let io = IO.get();
    io.output = Vec::new();
    io.input = vec![0; size];

    io.input.as_mut_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn aot_compile() -> u32 {
    let io = IO.get();

    match aot::compile(&io.input) {
        Ok(artifact) => {
            io.output = artifact;
            0
        }
        Err(error) => {
            io.output = error.into_bytes();
            1
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn aot_output() -> *const u8 {
    IO.get().output.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn aot_output_size() -> usize {
    IO.get().output.len()
}
