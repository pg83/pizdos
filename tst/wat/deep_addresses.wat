(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (func $load ;; PARAMETERS
    (result i32)
    ;; ADDRESS
    i32.load8_u)
)
