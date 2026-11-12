(module
  (func $choose_true (param $value f64) (result i32)
    (select
      (i32.trunc_sat_f64_s (local.get $value))
      (i32.const 73)
      (i32.and
        (f64.ge (local.get $value) (f64.const 0))
        (f64.lt (local.get $value) (f64.const 2147483648)))))

  (func $choose_false (param $value f64) (result i32)
    (select
      (i32.const 73)
      (i32.trunc_sat_f64_s (local.get $value))
      (i32.or
        (f64.lt (local.get $value) (f64.const 0))
        (f64.ge (local.get $value) (f64.const 2147483648)))))

  (func $saturate (param $value f64) (result i32)
    (i32.trunc_sat_f64_s (local.get $value)))
)
