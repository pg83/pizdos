package main

type SpillDef struct {
	count int
	block int32
	pos   int32
}

// A spill slot belongs to one virtual register. If that register has one
// definition outside a loop, saving it there makes every later save of the
// same value redundant, including saves repeated on each loop iteration.
// Coalesced loop parameters have several definitions and are left alone.
func (a *allocator) saveSpillsBeforeLoops(inner, atEnd, atStart map[int32][]Move) {
	defs := make([]SpillDef, a.mf.nregs)

	for _, b := range a.order {
		for i := range a.mf.blocks[b].insts {
			in := &a.mf.blocks[b].insts[i]
			in.forEachOperand(func(r *VReg, role int) {
				if *r == noReg || r.isPhys() || role&roleDef == 0 || role == roleTemp {
					return
				}

				d := &defs[a.vindex(*r)]
				d.count++
				d.block = b
				d.pos = a.instPos[b][i]
			})
		}
	}

	owners := make(map[VReg]int)

	for x, slot := range a.slots {
		if slot >= 0 && defs[x].count == 1 {
			owners[slotLoc(slot)] = x
		}
	}

	save := make([]bool, len(defs))
	consider := func(moves []Move, b int32) {
		for _, m := range moves {
			if x, ok := owners[m.dst]; ok && a.mf.blocks[b].depth > a.mf.blocks[defs[x].block].depth {
				save[x] = true
			}
		}
	}

	for key, moves := range inner {
		consider(moves, a.posBlock[key/4])
	}

	for b, moves := range atEnd {
		consider(moves, b)
	}

	for b, moves := range atStart {
		consider(moves, b)
	}

	filter := func(moves []Move) []Move {
		out := moves[:0]

		for _, m := range moves {
			if x, ok := owners[m.dst]; ok && save[x] {
				continue
			}

			out = append(out, m)
		}

		return out
	}

	for key, moves := range inner {
		inner[key] = filter(moves)
	}

	for b, moves := range atEnd {
		atEnd[b] = filter(moves)
	}

	for b, moves := range atStart {
		atStart[b] = filter(moves)
	}

	for x, yes := range save {
		if !yes {
			continue
		}

		d := defs[x]
		r := VReg(x) + firstVirtual
		src := a.location(r, d.pos+1)
		dst := slotLoc(a.slots[x])

		if src != dst {
			key := 2 * (d.pos + 2)
			inner[key] = append(inner[key], Move{dst, src})
		}
	}
}
