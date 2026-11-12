(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (global $checksum (mut i64) (i64.const 0))
  (func $copy (param $dest i32) (param $source i32) (param $other i32)
              (param $count i32) (param $delta i32) (result i64)
    (local $index i32)
    (local $sum i64)
    (local $state i32)
    (local.set $state (i32.const 7))
    (if (local.get $count)
      (then
        (loop $next
          ;; COPY
          ;; CARRY
          (local.set $index (i32.add (local.get $index) (i32.const STRIDE)))
          (br_if $next (i32.lt_u (local.get $index) (local.get $count))))))
    (global.set $checksum (i64.add (local.get $sum) (i64.extend_i32_u (local.get $state))))
    (global.get $checksum))
)
