(module
  (global $tag (mut i32) (i32.const 0))
  ;; BEGIN_FUNCTION
  (func $choose (param $selector i32) (param $seed i32) (result i32)
    (local $key i32)
    ;; LOCALS
    (block $join (result ;; TYPES
      )
      ;; ENTRIES
    )
    (local.set $key)
    ;; TAKE_VALUES
    (block $default
      (block $one
        (block $zero
          (br_table $zero $one $default $default (local.get $key)))
        (global.set $tag (i32.const 10))
        (return (i32.xor ;; HASH
          (i32.const 0xa5a5a5a5))))
      (global.set $tag (i32.const 20))
      (return (i32.mul ;; HASH
        (i32.const 33))))
    (global.set $tag (i32.const 30))
    (i32.sub ;; HASH
      (i32.const 17)))
  ;; END_FUNCTION
)
