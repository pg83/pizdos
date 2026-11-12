(module
  (memory 1)
  (global (export "__stack_pointer") (mut i32) (i32.const 32768))
  ;; BEGIN_FUNCTION
  (func $copy (param $dest i32) (param $source i32) (param $count i32) (result i32)
    ;; LOCALS
    (local $index i32)
    (if (local.get $count)
      (then
        ;; INITIAL
        (loop $next ;; PARAMETERS
          ;; RESTORE
          ;; Each expression reads its predecessor twice. With p0 == 0,
          ;; all of them still address the current byte, including the
          ;; copies whose p0 becomes a constant only late in optimization.
          (local.set $g0 (local.get $index))
          ;; ADDRESS
          (i32.store8 (i32.add (local.get $dest) (local.get $g16))
            (i32.load8_u (i32.add (local.get $source) (local.get $g16))))
          (local.set $index (i32.add (local.get $index) (i32.const 1)))
          (br_if $next ;; ROTATION
            (i32.lt_u (local.get $index) (local.get $count)))
          ;; DROP
        )))
    (local.get $index))
  ;; END_FUNCTION
  ;; DATA
)
