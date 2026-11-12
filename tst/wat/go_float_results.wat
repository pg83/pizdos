(module
  ;; DIFFERENCE

  (func $relay (param $first f64) (param $later i64) (param $earlier i64) (result f64 f64)
    (local.get $first)
    (call $difference (local.get $later) (local.get $earlier)))

  (func $choose (param $condition i32) (param $first f64) (param $other f64)
                (param $later i64) (param $earlier i64) (result f64 f64)
    (local.get $first)
    (if (result f64) (local.get $condition)
      (then (local.get $other))
      (else (call $difference (local.get $later) (local.get $earlier)))))
)
