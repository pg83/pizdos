(module
  (import "kernel" "wcsdup" (func $duplicate (param i32) (result i32)))
  (import "kernel" "write" (func $write (param i32 i32 i32) (result i32)))
  (import "kernel" "signal" (func $signal (param i32 i32) (result i32)))
  (memory 32833 32833)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (global (export "__heap_base") i32 (i32.const 65536))
  (global $handled (mut i32) (i32.const 0))
  (data (i32.const 2048) "R")

  (func (export "__call_signal_handler") (param $handler i32) (param $number i32)
    (if (i32.ne (local.get $handler) (i32.const 1)) (then unreachable))
    (if (i32.ne (local.get $number) (i32.const 10)) (then unreachable))
    (global.set $handled (i32.const 1)))

  (func (export "_start")
    (if (i32.ne (call $signal (i32.const 10) (i32.const 1)) (i32.const 0)) (then unreachable))
    ;; Keep the stack, static results and signal info below the input.
    ;; 2^29 nonzero wchar_t words plus NUL require 2^31 + 4 bytes to copy.
    (memory.fill (i32.const 4194304) (i32.const 1) (i32.const 2147483648))
    (i32.store (i32.const 2151677952) (i32.const 0))
    (if (i32.ne (call $write (i32.const 1) (i32.const 2048) (i32.const 1)) (i32.const 1)) (then unreachable))
    (if (i32.ne (call $duplicate (i32.const 4194304)) (i32.const 0)) (then unreachable))
    (if (i32.ne (i32.load (i32.const 0)) (i32.const 12)) (then unreachable))
    (if (i32.ne (global.get $handled) (i32.const 1)) (then unreachable)))
)
