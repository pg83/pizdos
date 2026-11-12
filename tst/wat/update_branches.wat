(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (global $branch (mut i32) (i32.const 0))

  (func $update32 (param $address i32) (param $operand i32)
    (local $value32 i32)
    (i32.store (local.get $address)
      (local.tee $value32
        (i32.sub (i32.load (local.get $address)) (local.get $operand))))
    (if (local.get $value32)
      (then (global.set $branch (i32.const 10)))
      (else (global.set $branch (i32.const 13)))))

  (func $update64 (param $address i32) (param $operand i64)
    (local $value64 i64)
    (i64.store (local.get $address)
      (local.tee $value64
        (i64.sub (i64.load (local.get $address)) (local.get $operand))))
    (if (i64.eqz (local.get $value64))
      (then (global.set $branch (i32.const 10)))
      (else (global.set $branch (i32.const 13)))))
)
