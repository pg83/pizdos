package main

import "sort"

// Coalescing joins the virtual registers a copy connects when their
// lifetimes do not overlap: a block parameter and the argument an edge
// passes it, the two sides of a mov. One register, or one stack slot, then
// holds both, and the copy disappears. Threading turns the locals of a Go
// function into parameters of every case, which makes the most of them.
//
// Copies in deeper loops go first. The joined lifetime is the union of the
// two, so a later copy is checked against everything joined before it.

// coalesce renames the operands of the copies whose registers it joins
// and reports whether it joined any: the allocator then starts again on
// the renamed code.
func (a *allocator) coalesce() bool {
	mf := a.mf
	parent := make([]int32, mf.nregs)
	ranges := make([][]lrange, mf.nregs)

	for x := range parent {
		parent[x] = int32(x)

		if it := a.intervals[x]; it != nil {
			ranges[x] = it.ranges
		}
	}

	var find func(x int32) int32

	find = func(x int32) int32 {
		for parent[x] != x {
			parent[x] = parent[parent[x]]
			x = parent[x]
		}

		return x
	}

	type copyPair struct {
		dst, src int32
		depth    int
	}

	var copies []copyPair
	add := func(dst, src VReg, depth int) {
		if dst.isPhys() || src.isPhys() || dst == noReg || src == noReg || dst == src {
			return
		}

		if mf.class[a.vindex(dst)] == mf.class[a.vindex(src)] {
			copies = append(copies, copyPair{int32(a.vindex(dst)), int32(a.vindex(src)), depth})
		}
	}

	for b := range mf.blocks {
		depth := mf.blocks[b].depth

		for i := range mf.blocks[b].insts {
			in := &mf.blocks[b].insts[i]

			switch in.op {
			case mParMove:
				for _, m := range in.moves {
					add(m.dst, m.src, depth)
				}
			case mMov, mFMov:
				add(in.dst, in.src, depth)
			}
		}
	}

	sort.SliceStable(copies, func(i, j int) bool { return copies[i].depth > copies[j].depth })
	joined := false

	for _, c := range copies {
		x, y := find(c.dst), find(c.src)

		if x == y || ranges[x] == nil || ranges[y] == nil || overlap(ranges[x], ranges[y]) {
			continue
		}

		parent[y] = x
		ranges[x] = mergeRanges(ranges[x], ranges[y])
		joined = true
	}

	if !joined {
		return false
	}

	for b := range mf.blocks {
		for i := range mf.blocks[b].insts {
			in := &mf.blocks[b].insts[i]

			in.forEachOperand(func(r *VReg, role int) {
				if *r != noReg && !r.isPhys() {
					*r = VReg(find(int32(a.vindex(*r)))) + firstVirtual
				}
			})

			switch {
			case in.op == mParMove:
				in.moves = dropSelfMoves(in.moves)
			case (in.op == mMov || in.op == mFMov) && in.dst == in.src && in.aux == 0:
				*in = inst(mNop)
			}
		}
	}

	return true
}

// overlap tells whether two sorted lists of ranges share a position.
func overlap(x, y []lrange) bool {
	i, j := 0, 0

	for i < len(x) && j < len(y) {
		switch {
		case x[i].to <= y[j].from:
			i++
		case y[j].to <= x[i].from:
			j++
		default:
			return true
		}
	}

	return false
}

// mergeRanges is the sorted union of two lists of ranges that do not
// overlap.
func mergeRanges(x, y []lrange) []lrange {
	out := make([]lrange, 0, len(x)+len(y))
	i, j := 0, 0

	for i < len(x) || j < len(y) {
		var r lrange

		if j == len(y) || i < len(x) && x[i].from < y[j].from {
			r = x[i]
			i++
		} else {
			r = y[j]
			j++
		}

		if n := len(out); n > 0 && out[n-1].to == r.from {
			out[n-1].to = r.to
		} else {
			out = append(out, r)
		}
	}

	return out
}
