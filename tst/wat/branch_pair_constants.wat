(module
  (global $tag (mut i64) (i64.const 0))
  ;; BEGIN_FUNCTION
  (func $choose (param $x i32) (param $y i64) ;; PARAMETERS
                (result i64)
    ;; The second comparison materializes a full 64-bit constant after the
    ;; first jump, while every payload parameter remains live until the hash.
    (if (;; BOOLEAN
          (i32.lt_u (local.get $x) (i32.const 17))
          (i64.eq (local.get $y) (i64.const ;; CONSTANT
          )))
      (then (global.set $tag (i64.const 123)))
      (else (global.set $tag (i64.const 45))))
    ;; RESULT
  )
  ;; END_FUNCTION
)
