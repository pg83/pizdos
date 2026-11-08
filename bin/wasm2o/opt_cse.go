package main

// CseKey describes an expression with no memory reads, writes or traps.
// Its arguments are SSA values, so a dominating expression stays valid
// across calls and stores. Loads, globals and memory.size are excluded.
type CseKey struct {
	op    Op
	typ   Type
	nargs uint16
	aux   int64
	aux2  int32
	args  [3]int32
}

type CseVisit struct {
	block int32
	leave bool
	keys  []CseKey
}

func (o *optimizer) commonExpressions() {
	f := o.f
	o.apply()
	ci := f.analyze()
	children := ci.children()
	available := map[CseKey]int32{}
	stack := []CseVisit{{block: ci.order[0]}}

	for len(stack) > 0 {
		visit := stack[len(stack)-1]
		stack = stack[:len(stack)-1]

		if visit.leave {
			for _, key := range visit.keys {
				delete(available, key)
			}

			continue
		}

		var added []CseKey

		for _, v := range f.blocks[visit.block].instrs {
			val := &f.values[v]

			if readsState(val.op) || val.nargs > 3 || (val.typ != I32 && val.typ != I64 && val.typ != F32 && val.typ != F64) {
				continue
			}

			key := CseKey{op: val.op, typ: val.typ, nargs: val.nargs, aux: val.aux, aux2: val.aux2}

			for i, a := range f.args(v) {
				key.args[i] = o.resolve(a)
			}

			if !isFloat(val.typ) {
				switch val.op {
				case OpAdd, OpMul, OpAnd, OpOr, OpXor, OpEq, OpNe:
					if key.args[0] > key.args[1] {
						key.args[0], key.args[1] = key.args[1], key.args[0]
					}
				}
			}

			if prior, ok := available[key]; ok {
				o.replace(v, prior)
			} else {
				available[key] = v
				added = append(added, key)
			}
		}

		stack = append(stack, CseVisit{block: visit.block, leave: true, keys: added})
		kids := children[visit.block]

		for i := len(kids) - 1; i >= 0; i-- {
			stack = append(stack, CseVisit{block: kids[i]})
		}
	}

	o.apply()
}
