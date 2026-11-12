(module
  (import "kernel" "fork" (func $fork (result i32)))
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (global $checked (mut i32) (i32.const 0))

  ;; Initialization runs through the init thunk, outside the command's
  ;; _start frames. fork must reject that stack with ENOSYS, without a child.
  (func $initialize
    (if (i32.ne (call $fork) (i32.const -1)) (then unreachable))
    (if (i32.ne (i32.load (i32.const 0)) (i32.const 38)) (then unreachable))
    (global.set $checked (i32.const 1)))
  (start $initialize)

  (func (export "_start")
    (if (i32.ne (global.get $checked) (i32.const 1)) (then unreachable)))
)
