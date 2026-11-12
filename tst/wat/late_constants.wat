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

  ;; BEGIN_COPY_FUNCTION
  (func $collapse (param $dest i32) (param $source i32) (param $count i32) (result i32)
    ;; LOCALS
    (local $index i32)
    (if (local.get $count)
      (then
        ;; INITIAL
        (loop $next ;; PARAMETERS
          ;; RESTORE
          ;; Both products become zero late. Every iteration writes the
          ;; same destination byte, so it must retain the final source byte.
          (i32.store8
            (i32.add (local.get $dest)
              (i32.sub
                (i32.mul (local.get $index) (local.get $p0))
                (i32.mul (i32.add (local.get $index) (i32.const 1)) (local.get $p0))))
            (i32.load8_u (i32.add (local.get $source) (local.get $index))))
          (local.set $index (i32.add (local.get $index) (i32.const 1)))
          (br_if $next ;; ROTATION
            (i32.lt_u (local.get $index) (local.get $count)))
          ;; DROP
        )))
    (local.get $index))
  ;; END_COPY_FUNCTION

  (data (i32.const 8229) "\13")
  ;; COPY_DATA
)
