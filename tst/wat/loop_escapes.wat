(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (global $wide_result (mut i64) (i64.const 0))

  (func $copy_many (param $dest i32) (param $source i32) (param $count i32) (param $pointer i32) (result i32)
    (local $index i32)
    (local $last i32)
    (local $sum i32)
    (local $wide i64)
    ;; LOOP-BEGIN
    (local.set $last (i32.const 0))
    (if (local.get $count)
      (then
        (local.set $index (i32.const 0))
        (loop $copy
          (local.set $last (i32.load8_u (local.get $pointer)))
          (i32.store8 (i32.add (local.get $dest) (local.get $index))
            (i32.load8_u
              (i32.add (i32.add (local.get $source) (local.get $index)) (local.get $last))))
          ;; WIDE-BEGIN
          (local.set $wide (i64.add (local.get $wide) (i64.extend_i32_u (local.get $last))))
          ;; WIDE-END
          (local.set $index (i32.add (local.get $index) (i32.const 1)))
          (br_if $copy (i32.lt_u (local.get $index) (local.get $count))))))
    (local.set $sum (i32.add (local.get $sum) (local.get $last)))
    (local.set $dest (i32.add (local.get $dest) (i32.const 256)))
    (local.set $source (i32.add (local.get $source) (i32.const 32)))
    ;; LOOP-END
    (global.set $wide_result (local.get $wide))
    (local.get $sum))
)
