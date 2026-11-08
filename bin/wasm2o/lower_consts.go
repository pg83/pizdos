package main

// Nonzero float constants share a register at their common dominator,
// outside a loop when needed, including uses in returns and edge copies.
// Integer constants and zero remain cheap to rematerialize at each use.
func (l *lowerer) placeFloatConstants() {
	f := l.f
	homes := make([]int32, len(f.values))

	for v := range homes {
		homes[v] = -1
	}

	use := func(b, v int32) {
		val := &f.values[v]

		if val.op != OpConst || !isFloat(val.typ) || val.aux == 0 || l.uses[v] < 2 {
			return
		}

		if homes[v] < 0 {
			homes[v] = b
		} else {
			homes[v] = l.ci.intersect(homes[v], b)
		}
	}

	for _, b := range l.ci.order {
		blk := &f.blocks[b]

		for _, v := range blk.instrs {
			for _, a := range f.args(v) {
				use(b, a)
			}
		}

		for _, v := range blk.term.values {
			use(b, v)
		}

		for _, e := range blk.term.edges {
			for _, v := range e.args {
				use(b, v)
			}
		}
	}

	l.floatHomes = make([][]int32, len(f.blocks))

	for v, b := range homes {
		if b < 0 {
			continue
		}

		for l.ci.depth[b] > 0 && l.ci.idom[b] != b {
			b = l.ci.idom[b]
		}

		l.floatHomes[b] = append(l.floatHomes[b], int32(v))
	}
}
