(module
  (memory 1)
  (data (i32.const 2048) "\03\00\a5\a5\20\00\a5\a5\3d\00\a5\a5\5a\00\a5\a5\77\00\a5\a5\94\00\a5\a5\b1\00\a5\a5\ce\00\a5\a5\eb\00\a5\a5\08\00\a5\a5\25\00\a5\a5\42\00\a5\a5\5f\00\a5\a5\7c\00\a5\a5\99\00\a5\a5\b6\00\a5\a5")
  ;; Every byte selects one of sixteen handlers. Each handler loads a word
  ;; whose high bits are unused, then returns through a one-target table.
  (func $route (param $seed i32) (param $mode i32) (param $rounds i32) (result i32)
    (local $x i32)
    (local.get $seed)
    (loop $dispatch (param i32)
      (local.set $x)
      (block $default
        (block $case15
          (block $case14
            (block $case13
              (block $case12
                (block $case11
                  (block $case10
                    (block $case9
                      (block $case8
                        (block $case7
                          (block $case6
                            (block $case5
                              (block $case4
                                (block $case3
                                  (block $case2
                                    (block $case1
                                      (block $case0
                                        (br_table
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $case9 $case10 $case11 $case12 $case13 $case14 $case15
                                          $default (i32.and (local.get $x) (i32.const 255)))
                                      )
                                      (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 0))))
                                      (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                                      (i32.load (i32.const 2048))
                                      (br_table $dispatch (local.get $mode))
                                    )
                                    (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 1))))
                                    (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                                    (i32.load (i32.const 2052))
                                    (br_table $dispatch (local.get $mode))
                                  )
                                  (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 2))))
                                  (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                                  (i32.load (i32.const 2056))
                                  (br_table $dispatch (local.get $mode))
                                )
                                (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 3))))
                                (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                                (i32.load (i32.const 2060))
                                (br_table $dispatch (local.get $mode))
                              )
                              (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 4))))
                              (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                              (i32.load (i32.const 2064))
                              (br_table $dispatch (local.get $mode))
                            )
                            (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 5))))
                            (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                            (i32.load (i32.const 2068))
                            (br_table $dispatch (local.get $mode))
                          )
                          (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 6))))
                          (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                          (i32.load (i32.const 2072))
                          (br_table $dispatch (local.get $mode))
                        )
                        (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 7))))
                        (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                        (i32.load (i32.const 2076))
                        (br_table $dispatch (local.get $mode))
                      )
                      (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 8))))
                      (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                      (i32.load (i32.const 2080))
                      (br_table $dispatch (local.get $mode))
                    )
                    (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 9))))
                    (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                    (i32.load (i32.const 2084))
                    (br_table $dispatch (local.get $mode))
                  )
                  (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 10))))
                  (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                  (i32.load (i32.const 2088))
                  (br_table $dispatch (local.get $mode))
                )
                (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 11))))
                (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
                (i32.load (i32.const 2092))
                (br_table $dispatch (local.get $mode))
              )
              (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 12))))
              (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
              (i32.load (i32.const 2096))
              (br_table $dispatch (local.get $mode))
            )
            (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 13))))
            (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
            (i32.load (i32.const 2100))
            (br_table $dispatch (local.get $mode))
          )
          (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 14))))
          (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
          (i32.load (i32.const 2104))
          (br_table $dispatch (local.get $mode))
        )
        (if (i32.le_u (local.get $rounds) (i32.const 1)) (then (return (i32.const 15))))
        (local.set $rounds (i32.sub (local.get $rounds) (i32.const 1)))
        (i32.load (i32.const 2108))
        (br_table $dispatch (local.get $mode))
      )
      (return (i32.const 99))
    )
    unreachable
  )
)
