(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (data (i32.const 8192) "\13")
  (data (i32.const 12288) "\e7")
  ;; BEGIN_FUNCTION
  (func $rotate (param $a i32) (param $b i32) (param $count i32) (result i32)
    ;; LOCALS
    (local $left i32)
    ;; Only a is checked before the loop. Rotating b must never make its
    ;; address inherit that successful check when it finally reaches p0.
    (drop (i32.load8_u (local.get $a)))
    (local.set $left (local.get $count))
    ;; INITIAL
    (loop $next ;; PARAMETERS
      ;; RESTORE
      (br_if $next ;; ROTATION
        (local.tee $left (i32.sub (local.get $left) (i32.const 1)))))
    ;; RESTORE
    (i32.load8_u (local.get $p0)))
  ;; END_FUNCTION
)
