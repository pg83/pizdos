(module
  ;; Retain unsigned i32 values across branches with a wide quotient and
  ;; an exact f64 sum that may saturate back to i32.
  (func $conversions (param $seed i32) (param $selector i32) (param $rounds i32) (result i32)
    (local $x0 i32)
    (local $x1 i32)
    (local $x2 i32)
    (local $x3 i32)
    (local $x4 i32)
    (local $x5 i32)
    (local $x6 i32)
    (local $x7 i32)
    (local $x8 i32)
    (local $x9 i32)
    (local $hash i32)
    (local.set $x0 (i32.xor (local.get $seed) (i32.const 2654435761)))
    (local.set $x1 (i32.xor (local.get $seed) (i32.const 1013904226)))
    (local.set $x2 (i32.xor (local.get $seed) (i32.const 3668339987)))
    (local.set $x3 (i32.xor (local.get $seed) (i32.const 2027808452)))
    (local.set $x4 (i32.xor (local.get $seed) (i32.const 387276917)))
    (local.set $x5 (i32.xor (local.get $seed) (i32.const 3041712678)))
    (local.set $x6 (i32.xor (local.get $seed) (i32.const 1401181143)))
    (local.set $x7 (i32.xor (local.get $seed) (i32.const 4055616904)))
    (local.set $x8 (i32.xor (local.get $seed) (i32.const 2415085369)))
    (local.set $x9 (i32.xor (local.get $seed) (i32.const 774553834)))
    (block $done
      (br_if $done (i32.eqz (local.get $rounds)))
      (loop $again
        (if (i32.and (i32.xor (local.get $selector) (local.get $x5)) (i32.const 16777216))
          (then
            (local.set $x8 (i32.mul (local.get $x5) (local.get $x7)))
            (local.set $x5
              (i32.wrap_i64
                (i64.div_u
                  (i64.or (i64.shl (i64.extend_i32_u (local.get $x8)) (i64.const 32))
                    (i64.extend_i32_u (local.get $x7)))
                  (i64.or (i64.extend_i32_u (local.get $x9)) (i64.const 1)))))
          )
          (else
            (local.set $x8 (i32.clz (local.get $x5)))
          ))
        (if (i32.and (i32.xor (local.get $selector) (local.get $x7)) (i32.const 32768))
          (then)
          (else
            (local.set $x7
              (i32.trunc_sat_f64_u
                (f64.add (f64.convert_i32_u (local.get $x2))
                  (f64.convert_i32_u (local.get $x0)))))
          ))
        (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
        (br_if $again (local.get $rounds))))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x0)))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x1)))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x2)))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x3)))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x4)))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x5)))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x6)))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x7)))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x8)))
    (local.set $hash (i32.xor (i32.rotl (local.get $hash) (i32.const 3)) (local.get $x9)))
    (local.get $hash))
)
