(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  ;; BEGIN_FUNCTION
  (func $copy (param $dest i32) (param $source i32) (param $other_dest i32) (param $other_source i32)
              (param $first i32) (param $second i32) (param $tag i32) (result i32)
    (local $i i32)
    (local $j i32)
    (loop $one
      (if (local.get $tag)
        (then
          (i32.store8 (i32.add (local.get $dest) (local.get $i))
            (i32.load8_u (i32.add (local.get $source) (local.get $i)))))
        (else
          (i32.store8 (i32.add (local.get $dest) (local.get $i))
            (i32.xor (i32.load8_u (i32.add (local.get $source) (local.get $i))) (i32.const 1)))))
      ;; Keep an effect in the latch after the branch. Unrolling the first
      ;; loop can then place the next loop's preheader after its own latch.
      (i32.store8 offset=128 (i32.add (local.get $dest) (local.get $i))
        (i32.load8_u offset=128 (i32.add (local.get $source) (local.get $i))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $one (i32.lt_u (local.get $i) ;; FIRST_LIMIT
      )))
    (loop $two
      (i32.store8 (i32.add (local.get $other_dest) (local.get $j))
        (i32.load8_u (i32.add (local.get $other_source) (local.get $j))))
      (local.set $j (i32.add (local.get $j) (i32.const 1)))
      (br_if $two (i32.lt_u (local.get $j) (local.get $second))))
    (i32.add (local.get $i) (local.get $j)))
  ;; END_FUNCTION
  ;; DATA
)
