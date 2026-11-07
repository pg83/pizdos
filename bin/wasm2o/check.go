package main

import (
	"fmt"
	"os"
	"sort"
)

// The allocation checker replays the allocated code symbolically: every
// register and stack slot holds a set of virtual registers whose current
// value it contains. Each use must find its virtual register in the
// location the allocator gave it. It is a debugging aid, enabled by
// W2X_CHECK.

var checkAlloc = os.Getenv("W2X_CHECK") != ""

type origOperand struct {
	vreg VReg
	loc  VReg
	role int
}

// origInst records an instruction of the allocated code: its operands
// before rewriting, nil for moves the allocator inserted.
type origInst struct {
	ops []origOperand
}

type locSet map[VReg]bool

type checkState map[VReg]locSet

func (s checkState) clone() checkState {
	out := checkState{}

	for l, set := range s {
		c := locSet{}

		for v := range set {
			c[v] = true
		}

		out[l] = c
	}

	return out
}

// meet keeps what both states agree on and reports whether s changed.
func (s checkState) meet(o checkState) bool {
	changed := false

	for l, set := range s {
		other := o[l]

		for v := range set {
			if !other[v] {
				delete(set, v)
				changed = true
			}
		}
	}

	return changed
}

func (s checkState) define(v VReg, loc VReg, from locSet) {
	for _, set := range s {
		delete(set, v)
	}

	set := locSet{v: true}

	for w := range from {
		if w != v {
			set[w] = true
		}
	}

	s[loc] = set
}

func (a *allocator) check(orig [][]origInst) {
	mf := a.mf
	in := make([]checkState, len(mf.blocks))
	work := []int32{a.order[0]}
	in[a.order[0]] = checkState{}
	queued := make([]bool, len(mf.blocks))
	queued[a.order[0]] = true

	for len(work) > 0 {
		b := work[0]
		work = work[1:]
		queued[b] = false
		s := in[b].clone()
		blk := &mf.blocks[b]

		for i := range blk.insts {
			a.checkInst(s, &blk.insts[i], orig[b][i], b, i)
		}

		for _, succ := range blk.succs {
			changed := false

			if in[succ] == nil {
				in[succ] = s.clone()
				changed = true
			} else {
				changed = in[succ].meet(s)
			}

			if changed && !queued[succ] {
				queued[succ] = true
				work = append(work, succ)
			}
		}
	}
}

func (a *allocator) checkInst(s checkState, in *MInst, orig origInst, b int32, i int) {
	fail := func(format string, args ...any) {
		throwFmt("allocation check: B%d inst %d %s: %s", b, i, in.String(), fmt.Sprintf(format, args...))
	}

	if orig.ops == nil {
		// a move between locations the allocator inserted: in parallel
		next := map[VReg]locSet{}

		for _, m := range in.moves {
			c := locSet{}

			for v := range s[m.src] {
				c[v] = true
			}

			next[m.dst] = c
		}

		for l, set := range next {
			s[l] = set
		}

		return
	}

	for _, op := range orig.ops {
		if op.role&roleUse != 0 && op.role != roleTemp && !op.vreg.isPhys() {
			if !s[op.loc][op.vreg] {
				fail("%s expected in %s, which holds %v", op.vreg, locName(op.loc), sortedSet(s[op.loc]))
			}
		}
	}

	// copies keep the value: the destination holds what the source held
	switch {
	case (in.op == mMov && in.aux == 0) || in.op == mFMov:
		var src, dst origOperand

		for _, op := range orig.ops {
			if op.role == roleUse {
				src = op
			} else {
				dst = op
			}
		}

		from := s[src.loc]

		if src.vreg.isPhys() {
			from = locSet{}
		}

		if dst.vreg.isPhys() {
			s[dst.loc] = locSet{}

			for v := range from {
				s[dst.loc][v] = true
			}
		} else {
			s.define(dst.vreg, dst.loc, from)
		}

		return
	case in.op == mParMove:
		type def struct {
			op   origOperand
			from locSet
		}

		var defs []def
		k := 0

		for _, op := range orig.ops {
			if op.role == roleUse {
				continue
			}

			src := orig.ops[k]
			k++
			from := locSet{}

			if !src.vreg.isPhys() {
				for v := range s[src.loc] {
					from[v] = true
				}
			}

			defs = append(defs, def{op, from})
		}

		for _, d := range defs {
			if d.op.vreg.isPhys() {
				s[d.op.loc] = d.from
			} else {
				s.define(d.op.vreg, d.op.loc, d.from)
			}
		}

		return
	}

	for _, r := range in.clobberSet() {
		s[r] = locSet{}
	}

	for _, op := range orig.ops {
		if op.role == roleTemp {
			s[op.loc] = locSet{}
		}
	}

	for _, op := range orig.ops {
		if op.role&roleDef == 0 || op.role == roleTemp {
			continue
		}

		if op.vreg.isPhys() {
			s[op.loc] = locSet{}
		} else {
			s.define(op.vreg, op.loc, nil)
		}
	}
}

func locName(l VReg) string {
	if isSlot(l) {
		return fmt.Sprintf("slot%d", slotOf(l))
	}

	return l.String()
}

func sortedSet(s locSet) []VReg {
	var out []VReg

	for v := range s {
		out = append(out, v)
	}

	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })

	return out
}

// checkSSA verifies a function after the SSA optimizer, with W2X_CHECK:
// every edge passes a value for each parameter of its block, a parameter
// is its block's, and every use is dominated by its definition, an edge's
// arguments being used at the end of the block it leaves.
func checkSSA(f *Function) {
	ci := f.analyze()

	type site struct {
		block int32
		at    int
	}

	defs := map[int32]site{}

	for _, b := range ci.order {
		blk := &f.blocks[b]

		for i, p := range blk.params {
			if v := &f.values[p]; v.op != OpParam || v.block != b || v.aux != int64(i) {
				throwFmt("internal error: SSA: v%d is not parameter %d of block %d", p, i, b)
			}

			defs[p] = site{b, -1}
		}

		for i, v := range blk.instrs {
			defs[v] = site{b, i}
		}
	}

	use := func(v, b int32, at int) {
		if f.values[v].block < 0 {
			return
		}

		d, ok := defs[v]

		switch {
		case !ok:
			throwFmt("internal error: SSA: v%d used in block %d is defined in no reachable block", v, b)
		case d.block == b && d.at >= at, d.block != b && !ci.dominates(d.block, b):
			throwFmt("internal error: SSA: v%d of block %d does not dominate its use in block %d", v, d.block, b)
		}
	}

	for _, b := range ci.order {
		blk := &f.blocks[b]

		for i, v := range blk.instrs {
			for _, a := range f.args(v) {
				use(a, b, i)
			}
		}

		t := &blk.term
		end := len(blk.instrs)

		if t.kind == TermIf || t.kind == TermSwitch {
			use(t.value, b, end)
		}

		for _, v := range t.values {
			use(v, b, end)
		}

		for _, e := range t.edges {
			if f.blocks[e.block].dead || len(e.args) != len(f.blocks[e.block].params) {
				throwFmt("internal error: SSA: block %d passes %d arguments to block %d of %d parameters", b, len(e.args), e.block, len(f.blocks[e.block].params))
			}

			for _, a := range e.args {
				use(a, b, end)
			}
		}
	}
}
