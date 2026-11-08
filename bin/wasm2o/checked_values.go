package main

type CheckedValue struct {
	value int32
	end   int64
}

type CheckedEdge struct {
	pred int32
	edge *Edge
}

// Carry successful memory checks through SSA edges and parameters. A
// parameter is checked only as far as every incoming argument has already
// been checked. Starting with no facts keeps cyclic proofs conservative.
func checkedValues(f *Function, ci *cfgInfo) [][]CheckedValue {
	// Keep the fact table below 8 MiB, including on the wasm compiler.
	limit := min(1024, (1<<20)/len(f.blocks))

	if limit == 0 {
		return nil
	}

	incoming := make([][]CheckedEdge, len(f.blocks))

	for _, b := range ci.order {
		for j := range f.blocks[b].term.edges {
			e := &f.blocks[b].term.edges[j]
			incoming[e.block] = append(incoming[e.block], CheckedEdge{b, e})
		}
	}

	tracked := make([]int32, len(f.values))

	for j := range tracked {
		tracked[j] = -1
	}

	var values []int32
	var add func(int32)
	add = func(v int32) {
		if tracked[v] >= 0 || len(values) > limit {
			return
		}

		tracked[v] = int32(len(values))
		values = append(values, v)
		val := &f.values[v]

		if val.op == OpParam {
			for _, e := range incoming[val.block] {
				add(e.edge.args[val.aux])
			}
		}
	}

	for _, b := range ci.order {
		for _, v := range f.blocks[b].instrs {
			val := &f.values[v]

			if val.op == OpLoad || val.op == OpStore {
				addr := f.args(v)[0]

				if f.values[addr].op == OpParam {
					add(addr)
				}
			}
		}
	}

	n := len(values)

	if n == 0 || n > limit {
		return nil
	}

	defs := make([][]int, len(f.blocks))

	for j, v := range values {
		if b := f.values[v].block; b >= 0 {
			defs[b] = append(defs[b], j)
		}
	}

	out := make([]int64, n*len(f.blocks))
	cur := make([]int64, n)
	entry := func(b int32) {
		first := true

		if b != 0 {
			for _, p := range f.blocks[b].preds {
				if ci.rpoIndex[p] < 0 {
					continue
				}

				row := out[int(p)*n : (int(p)+1)*n]

				for j := range cur {
					if first {
						cur[j] = row[j]
					} else {
						cur[j] = min(cur[j], row[j])
					}
				}

				first = false
			}
		}

		if first {
			clear(cur)
		}

		// Loop re-entry defines fresh values, even for an unchanged SSA
		// instruction number. Only edge arguments can prove new parameters.
		for _, j := range defs[b] {
			cur[j] = 0
		}

		for _, p := range f.blocks[b].params {
			j := tracked[p]

			if j < 0 || b == 0 || len(incoming[b]) == 0 {
				continue
			}

			end := int64(1 << 62)

			for _, e := range incoming[b] {
				arg := tracked[e.edge.args[f.values[p].aux]]
				end = min(end, out[int(e.pred)*n+int(arg)])
			}

			cur[j] = end
		}
	}

	for round := 0; round < 16; round++ {
		changed := false

		for _, b := range ci.order {
			entry(b)

			for _, v := range f.blocks[b].instrs {
				val := &f.values[v]

				if val.op == OpLoad || val.op == OpStore {
					if j := tracked[f.args(v)[0]]; j >= 0 {
						cur[j] = max(cur[j], val.aux+int64(memKind(val.aux2).width()))
					}
				}
			}

			row := out[int(b)*n : (int(b)+1)*n]

			for j, end := range cur {
				changed = changed || row[j] != end
				row[j] = end
			}
		}

		if !changed {
			break
		}
	}

	proofs := make([][]CheckedValue, len(f.blocks))

	for _, b := range ci.order {
		entry(b)

		for j, end := range cur {
			v := values[j]
			owner := f.values[v].block

			if end > 0 && (owner < 0 || ci.dominates(owner, b)) {
				proofs[b] = append(proofs[b], CheckedValue{v, end})
			}
		}
	}

	return proofs
}
