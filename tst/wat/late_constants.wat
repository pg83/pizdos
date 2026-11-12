(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (global $tag (mut i32) (i32.const 0))

  ;; BEGIN_FUNCTION
  (func $scan (param $base i32) (param $count i32) (param $bits i32) (result i32)
    ;; LOCALS
    (local $left i32)
    (local $sum i32)
    (local.set $left (local.get $count))
    (global.set $tag (i32.const 0))
    ;; INITIAL
    (loop $next ;; PARAMETERS
      ;; RESTORE
      ;; READS
      ;; Each pass replaces the final carried value with the same constant.
      ;; Earlier parameters become constants only after their successors do.
      (br_if $next ;; ROTATION
        (local.tee $left (i32.sub (local.get $left) (i32.const 1))))
      ;; DROP
    )
    (i32.add (local.get $sum) (global.get $tag)))
  ;; END_FUNCTION

  (data (i32.const 8229) "\13")
)
