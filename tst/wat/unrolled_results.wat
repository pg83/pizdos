(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  ;; BEGIN_FUNCTION
  (func $channels (param $dest i32) (param $seed i32) (param $selector i32) (result i32 i32)
    (local $i i32)
    (local $sum i32)
    (local.set $sum (local.get $seed))
    (loop $again
      (i32.store8 (i32.add (local.get $dest) (local.get $i))
        (i32.add (local.get $seed) (i32.mul (local.get $i) (i32.const 17))))
      (local.set $sum
        (i32.xor (i32.mul (local.get $sum) (i32.const 33))
          (i32.load8_u (i32.add (local.get $dest) (local.get $i)))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (i32.const ;; TRIPS
        ))))
    ;; Both payload slots hold the same loop result. Only the second is
    ;; transformed, and repeated default labels must preserve both slots.
    (block $two (result i32 i32)
      (block $one (result i32 i32)
        (block $zero (result i32 i32)
          (br_table $zero $one $two $two
            (local.get $sum) (local.get $sum) (local.get $selector)))
        (i32.const 165)
        i32.xor
        (br $two))
      (i32.const 33)
      i32.mul)
    (i32.const 17)
    i32.sub
    return)
  ;; END_FUNCTION
)
