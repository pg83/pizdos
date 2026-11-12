(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  (global $tag (mut i32) (i32.const 0))

  (func $copy (param $dest i32) (param $source i32) (param $count i32) (param $early i32) (result i32)
    (local $index i32)
    (local $last i32)
    (global.set $tag (i32.const 0))
    (loop $next
      (local.set $last (i32.load8_u (i32.add (local.get $source) (local.get $index))))
      (i32.store8 (i32.add (local.get $dest) (local.get $index)) (local.get $last))
      (if (i32.and (local.get $early) (i32.eq (local.get $index) (i32.const 5)))
        (then
          (global.set $tag (i32.const 99))
          (return (i32.const 123))))
      (local.set $index (i32.add (local.get $index) (i32.const 1)))
      (br_if $next (i32.lt_u (local.get $index) (local.get $count))))
    ;; AFTER
  )
)
