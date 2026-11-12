(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (global $tag (mut i32) (i32.const 0))
  ;; BEGIN_FUNCTION
  (func $choose (param $x i32) (param $base i32) (param $wanted i32) (result i32)
    ;; The second comparison needs an address whose displacement is larger
    ;; than the accessible tail. Its load must still follow the first jump.
    (if (;; BOOLEAN
          (i32.lt_u (local.get $x) (i32.const 17))
          (i32.eq (i32.load (i32.add (local.get $base) (i32.const ;; OFFSET
              ))) (local.get $wanted)))
      (then (global.set $tag (i32.const 123)))
      (else (global.set $tag (i32.const 45))))
    (i32.xor (global.get $tag) (i32.add (local.get $x) (local.get $base))))
  ;; END_FUNCTION
)
