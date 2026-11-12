(module
  (type $tuple (func (param i32) (result ;; RESULTS
  )))
  (table 1 funcref)
  (elem (i32.const 0) $values)

  (func $values (type $tuple) (param $seed i32) (result ;; RESULTS
  )
    ;; VALUES
  )

  (func $hash (param $seed i32) (result i64)
    ;; LOCALS
    local.get $seed
    ;; CALL
    ;; CAPTURE
    i64.const 0
    ;; HASH
  )
)
