(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))

  (func $add32 (param $address i32) (param $right f32) (result f32)
    (f32.add (f32.load (local.get $address)) (local.get $right)))
  (func $mul32 (param $address i32) (param $right f32) (result f32)
    (f32.mul (f32.load (local.get $address)) (local.get $right)))
  (func $add64 (param $address i32) (param $right f64) (result f64)
    (f64.add (f64.load (local.get $address)) (local.get $right)))
  (func $mul64 (param $address i32) (param $right f64) (result f64)
    (f64.mul (f64.load (local.get $address)) (local.get $right)))

  ;; COMPARISONS

  (func $divide_between (param $address i32) (param $numerator i32) (param $denominator i32) (result i32)
    (local $before i32)
    (local $quotient i32)
    (local.set $before (i32.load (local.get $address)))
    (local.set $quotient (i32.div_s (local.get $numerator) (local.get $denominator)))
    (i32.store (local.get $address) (i32.add (local.get $before) (local.get $quotient)))
    (local.get $quotient))
)
