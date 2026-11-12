(module
  (memory 1 2)
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

  ;; UNSIGNED_ROUNDTRIPS

  ;; The left operand has a constant range without being a literal. The
  ;; right operand's range crosses below zero before the bitwise mask.
  (func $left_mask (param $address i32) (param $value i32) (result i32)
    (i32.load8_u
      (i32.add (i32.const 4096)
        (i32.and
          (i32.add (i32.shr_u (i32.load8_u (local.get $address)) (i32.const 8)) (i32.const 255))
          (i32.sub (i32.and (local.get $value) (i32.const 255)) (i32.const 256))))))

  ;; Negating this signed immediate does not fit an x86 signed imm32.
  (func $subtract_min32 (param $value i32) (result i32)
    (i32.sub (local.get $value) (i32.const -2147483648)))
  (func $subtract_min64 (param $value i64) (result i64)
    (i64.sub (local.get $value) (i64.const -2147483648)))

  (func $merged_unsigned (param $address i32) (param $condition i32) (result i32)
    (local $first i64)
    (local.set $first (i64.extend_i32_u (local.get $address)))
    (i32.load8_u
      (i32.wrap_i64
        (if (result i64) (i32.lt_u (local.get $condition) (i32.const 7))
          (then (i64.extend_i32_u (local.get $address)))
          (else (local.get $first))))))

  (func $divide_between (param $address i32) (param $numerator i32) (param $denominator i32) (result i32)
    (local $before i32)
    (local $quotient i32)
    (local.set $before (i32.load (local.get $address)))
    (local.set $quotient (i32.div_s (local.get $numerator) (local.get $denominator)))
    (i32.store (local.get $address) (i32.add (local.get $before) (local.get $quotient)))
    (local.get $quotient))
)
