(module
  (global $tag (mut i32) (i32.const 0))
  (func $choose (param $selector i32) (param $value i32) (result i32)
    (local $key i32)
    (local.set $key
      (block $join (result i32)
        (br_if $join (i32.const -1) (i32.eq (local.get $selector) (i32.const 1)))
        drop
        (br_if $join (i32.const 7) (i32.eq (local.get $selector) (i32.const 2)))
        drop
        (br_if $join (i32.const 1) (i32.eq (local.get $selector) (i32.const 3)))
        drop
        (i32.const 0)))
    (block $default
      (block $one
        (block $zero
          (br_table $zero $one $default (local.get $key)))
        (global.set $tag (i32.const 10))
        (return (i32.xor (local.get $value) (i32.const 0xa5a5a5a5))))
      (global.set $tag (i32.const 20))
      (return (i32.mul (local.get $value) (i32.const 33))))
    (global.set $tag (i32.const 30))
    (i32.sub (local.get $value) (i32.const 17)))
)
