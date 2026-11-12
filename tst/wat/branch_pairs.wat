(module
  (global $tag (mut i32) (i32.const 0))
  ;; BEGIN_FUNCTION
  (func $choose (param $x i32) (param $y i32) (param $z i32) ;; PARAMETERS
                (result i32)
    (if (;; BOOLEAN
          (i32.lt_u (local.get $x) (i32.const 17))
          (i32.eq ;; OPERATION
            (local.get $z)))
      (then (global.set $tag (i32.const 123)))
      (else (global.set $tag (i32.const 45))))
    ;; RESULT
  )
  ;; END_FUNCTION
)
