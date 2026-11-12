(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (func $load ;; PARAMETERS
    (result i32)
    ;; A dropped expression precedes the live sum across the storage-page edge.
    local.get 0
    i32.const 90
    i32.xor
    drop
    ;; ADDRESS
    i32.load8_u)
)
