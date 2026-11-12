(module
  (func $mix (param $value i64) (param $salt i64) (result i64)
    (i64.xor (i64.mul (local.get $value) (i64.const 33)) (local.get $salt)))
  (func $bump (param $value f64) (param $salt i32) (result f64)
    (f64.add (local.get $value) (f64.convert_i32_u (local.get $salt))))

  (func $dispatch (param $key i32) (param $rounds i32) (param $seed i64) (result i64)
    ;; LOCALS
    ;; INITIALIZE
    (block $done
      (br_if $done (i32.eqz (local.get $rounds)))
      (loop $again
        ;; DISPATCH
      ))
    ;; RESULT
  )
)
