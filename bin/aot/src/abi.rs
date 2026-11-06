//! The contract between compiled code and the kernel.
//!
//! Every function, defined or imported, is called with the SystemV
//! convention and takes the process context (`vmctx`) as its first
//! argument, then its wasm parameters; it returns at most one value. The
//! context is the kernel's, laid out as below. Code never traps by itself: a
//! failed check calls the `trap` helper, which does not return.

/// Byte offsets into the process context.
pub const MEMORY_BASE: i32 = 0;
pub const MEMORY_SIZE: i32 = 8;
/// The code calls `stack_check` when its stack pointer is below this word.
/// The kernel keeps it at least `max_frame` (artifact header) above the
/// real end of the stack, and raises it above any stack pointer when it
/// wants the process to call in.
pub const STACK_LIMIT: i32 = 16;
/// The function table: `TABLE_SIZE` entries of `TABLE_ENTRY` bytes, each
/// the code address and the canonical type index of a function. A null
/// entry has type u64::MAX, which matches no signature.
pub const TABLE_BASE: i32 = 24;
pub const TABLE_SIZE: i32 = 32;
pub const TABLE_ENTRY: i64 = 16;
/// Globals, eight bytes each in global index order; a 32-bit value takes
/// the low half.
pub const GLOBALS: i32 = 40;

/// Functions of the kernel the code calls, by index in relocations.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Helper {
    /// trap(vmctx, code: i32), does not return
    Trap = 0,
    /// stack_check(vmctx)
    StackCheck = 1,
    /// memory_grow(vmctx, pages: i32) -> i32
    MemoryGrow = 2,
    /// memory_copy(vmctx, dst: i32, src: i32, len: i32)
    MemoryCopy = 3,
    /// memory_fill(vmctx, dst: i32, byte: i32, len: i32)
    MemoryFill = 4,
    /// memory_init(vmctx, segment: i32, dst: i32, src: i32, len: i32)
    MemoryInit = 5,
    /// data_drop(vmctx, segment: i32)
    DataDrop = 6,
}

/// The code argument of `trap`.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Trap {
    Unreachable = 0,
    MemoryOutOfBounds = 1,
    IntegerDivisionByZero = 2,
    IntegerOverflow = 3,
    BadConversionToInteger = 4,
    TableOutOfBounds = 5,
    BadSignature = 6,
}
