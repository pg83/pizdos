package main

import "math"

type UnsignedRange struct {
	lo, hi uint64
}

type UnsignedRanges struct {
	f     *Function
	cache []UnsignedRange
	seen  []bool
}

// Ranges are unsigned and inclusive. Arithmetic that could wrap stays
// unknown; a mask, a narrow load or a shift can bound it again. A cyclic
// dependency starts at the full range, so loop parameters never assume an
// unproved bound from their back edge.
func (r *UnsignedRanges) of(v int32, depth int) UnsignedRange {
	if r.seen[v] {
		return r.cache[v]
	}

	unknown := UnsignedRange{0, math.MaxUint32}
	r.seen[v] = true
	r.cache[v] = unknown
	val := &r.f.values[v]

	if val.typ != I32 || depth > 32 {
		return unknown
	}

	out := unknown
	args := r.f.args(v)

	switch val.op {
	case OpConst:
		k := uint64(uint32(val.aux))
		out = UnsignedRange{k, k}
	case OpLoad:
		kind := memKind(val.aux2)

		if !kind.signed() && kind.width() < 4 {
			out.hi = 1<<uint(kind.width()*8) - 1
		}
	case OpEqz, OpEq, OpNe, OpLtS, OpLtU, OpGtS, OpGtU, OpLeS, OpLeU, OpGeS, OpGeU,
		OpFEq, OpFNe, OpFLt, OpFGt, OpFLe, OpFGe, OpRefIsNull:
		out.hi = 1
	case OpAnd, OpAdd, OpSub, OpMul, OpShrU, OpShl:
		a, b := r.of(args[0], depth+1), r.of(args[1], depth+1)

		switch val.op {
		case OpAnd:
			out.hi = min(a.hi, b.hi)
		case OpAdd:
			if a.hi+b.hi <= math.MaxUint32 {
				out = UnsignedRange{a.lo + b.lo, a.hi + b.hi}
			}
		case OpSub:
			if a.lo >= b.hi {
				out = UnsignedRange{a.lo - b.hi, a.hi - b.lo}
			}
		case OpMul:
			if a.hi*b.hi <= math.MaxUint32 {
				out = UnsignedRange{a.lo * b.lo, a.hi * b.hi}
			}
		case OpShrU:
			if b.lo == b.hi {
				shift := b.lo & 31
				out = UnsignedRange{a.lo >> shift, a.hi >> shift}
			}
		case OpShl:
			if b.lo == b.hi {
				shift := b.lo & 31

				if a.hi <= math.MaxUint32>>shift {
					out = UnsignedRange{a.lo << shift, a.hi << shift}
				}
			}
		}
	case OpSelect:
		a, b := r.of(args[1], depth+1), r.of(args[2], depth+1)
		out = UnsignedRange{min(a.lo, b.lo), max(a.hi, b.hi)}
	case OpParam:
		first := true

		for _, p := range r.f.blocks[val.block].preds {
			for _, e := range r.f.blocks[p].term.edges {
				if e.block != val.block {
					continue
				}

				bound := r.of(e.args[val.aux], depth+1)

				if first {
					out = bound
					first = false
				} else {
					out = UnsignedRange{min(out.lo, bound.lo), max(out.hi, bound.hi)}
				}
			}
		}
	case OpWrap:
		if r.f.values[args[0]].op == OpExtendU {
			out = r.of(r.f.args(args[0])[0], depth+1)
		}
	}

	r.cache[v] = out

	return out
}
