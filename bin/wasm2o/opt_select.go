package main

// Replace short, non-trapping conditional computations by selects. Bit
// tests often have unpredictable outcomes; float equality is also used by
// scalar rounding code to apply a small correction on ties.
func (o *optimizer) selectTriangles() {
	f := o.f

	for round := 0; round < 8; round++ {
		changed := false

		for _, b := range f.rpo() {
			blk := &f.blocks[b]
			t := blk.term

			if blk.dead || t.kind != TermIf {
				continue
			}

			budget := 0

			switch f.values[t.value].op {
			case OpAnd:
				budget = 8
			case OpFEq, OpFNe:
				budget = 4
			}

			if budget == 0 {
				continue
			}

			for side := 0; side < 2; side++ {
				arm, merge := t.edges[side], t.edges[1-side]
				body := &f.blocks[arm.block]

				if arm.block == b || body.dead || len(body.params) != 0 || len(body.preds) != 1 || body.preds[0] != b || body.term.kind != TermJump || body.term.edges[0].block != merge.block {
					continue
				}

				if len(body.instrs) > budget {
					continue
				}

				pure := true

				for _, v := range body.instrs {
					op := f.values[v].op
					pure = pure && !readsState(op)

					// Avoid making division, saturation checks or other long
					// scalar sequences unconditional on the discarded path.
					switch op {
					case OpFDiv, OpFSqrt, OpFMin, OpFMax, OpTruncSatS, OpTruncSatU, OpConvertU:
						pure = false
					}
				}

				for _, p := range f.blocks[merge.block].params {
					typ := f.values[p].typ
					pure = pure && (typ == I32 || typ == I64 || typ == F32 || typ == F64)
				}

				if !pure {
					continue
				}

				yes, no := body.term.edges[0].args, merge.args

				if side == 1 {
					yes, no = no, yes
				}

				for _, v := range body.instrs {
					f.values[v].block = b
					blk.instrs = append(blk.instrs, v)
				}

				args := make([]int32, len(yes))

				for j, x := range yes {
					args[j] = x

					if x != no[j] {
						v := f.newValue(OpSelect, f.values[x].typ, b, 0, t.value, x, no[j])
						blk.instrs = append(blk.instrs, v)
						args[j] = v
					}
				}

				blk.term = Term{kind: TermJump, edges: []Edge{{block: merge.block, args: args}}}
				*body = Block{dead: true}
				f.computePreds()
				changed = true

				break
			}
		}

		if !changed {
			break
		}
	}
}
