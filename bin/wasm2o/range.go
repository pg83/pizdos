package main

import "math"

type FloatRange struct {
	lo, hi float64
	nan    bool
}

type FloatRanges struct {
	f     *Function
	facts map[int32]FloatRange
	cache map[int32]FloatRange
}

func unknownFloatRange() FloatRange {
	return FloatRange{math.Inf(-1), math.Inf(1), true}
}

func meetFloatRange(a, b FloatRange) FloatRange {
	return FloatRange{max(a.lo, b.lo), min(a.hi, b.hi), a.nan && b.nan}
}

func floatConstant(f *Function, v int32) (float64, bool) {
	val := &f.values[v]

	if val.op != OpConst || !isFloat(val.typ) {
		return 0, false
	}

	if val.typ == F32 {
		return float64(math.Float32frombits(uint32(val.aux))), true
	}

	return math.Float64frombits(uint64(val.aux)), true
}

// constrain records an ordered comparison on the path where a value is
// used. A false comparison admits NaN; another fact must exclude it before
// a conversion may lose its saturation checks.
func (r *FloatRanges) constrain(cond int32, truth bool) {
	f := r.f
	val := &f.values[cond]
	op := val.op

	if op < OpFEq || op > OpFGe {
		return
	}

	args := f.args(cond)
	v, c := args[0], args[1]
	k, ok := floatConstant(f, c)

	if !ok {
		k, ok = floatConstant(f, v)
		v = c

		switch op {
		case OpFLt:
			op = OpFGt
		case OpFLe:
			op = OpFGe
		case OpFGt:
			op = OpFLt
		case OpFGe:
			op = OpFLe
		}
	}

	if !ok || math.IsNaN(k) {
		return
	}

	prev, next := math.Nextafter(k, math.Inf(-1)), math.Nextafter(k, math.Inf(1))

	if f.values[v].typ == F32 {
		prev = float64(math.Nextafter32(float32(k), float32(math.Inf(-1))))
		next = float64(math.Nextafter32(float32(k), float32(math.Inf(1))))
	}

	bound := unknownFloatRange()
	bound.nan = !truth

	switch op {
	case OpFEq, OpFNe:
		if truth != (op == OpFEq) {
			return
		}

		bound.lo, bound.hi, bound.nan = k, k, false
	case OpFLt:
		if truth {
			bound.hi = prev
		} else {
			bound.lo = k
		}
	case OpFLe:
		if truth {
			bound.hi = k
		} else {
			bound.lo = next
		}
	case OpFGt:
		if truth {
			bound.lo = next
		} else {
			bound.hi = k
		}
	case OpFGe:
		if truth {
			bound.lo = k
		} else {
			bound.hi = prev
		}
	}

	r.add(v, bound)

	if f.values[v].op == OpFAbs && bound.hi >= 0 && !math.IsInf(bound.hi, 0) {
		r.add(f.args(v)[0], FloatRange{-bound.hi, bound.hi, bound.nan})
	}
}

func (r *FloatRanges) add(v int32, bound FloatRange) {
	if prior, ok := r.facts[v]; ok {
		bound = meetFloatRange(prior, bound)
	}

	r.facts[v] = bound
}

func (r *FloatRanges) of(v int32, depth int) FloatRange {
	if got, ok := r.cache[v]; ok {
		return got
	}

	val := &r.f.values[v]
	out := unknownFloatRange()

	if k, ok := floatConstant(r.f, v); ok {
		out = FloatRange{k, k, math.IsNaN(k)}
	} else if depth < 32 {
		args := r.f.args(v)

		switch val.op {
		case OpFAdd, OpFSub, OpFMul, OpFDiv:
			a, b := r.of(args[0], depth+1), r.of(args[1], depth+1)
			finiteB := !b.nan && !math.IsInf(b.lo, 0) && !math.IsInf(b.hi, 0)

			if finiteB && (val.op == OpFAdd || val.op == OpFSub || b.lo == b.hi && b.lo > 0) {
				out.nan = a.nan

				switch val.op {
				case OpFAdd:
					out.lo, out.hi = a.lo+b.lo, a.hi+b.hi
				case OpFSub:
					out.lo, out.hi = a.lo-b.hi, a.hi-b.lo
				case OpFMul:
					out.lo, out.hi = a.lo*b.lo, a.hi*b.hi
				case OpFDiv:
					out.lo, out.hi = a.lo/b.lo, a.hi/b.hi
				}
			}
		case OpFAbs:
			a := r.of(args[0], depth+1)
			out = FloatRange{0, max(math.Abs(a.lo), math.Abs(a.hi)), a.nan}
		case OpFNeg:
			a := r.of(args[0], depth+1)
			out = FloatRange{-a.hi, -a.lo, a.nan}
		case OpSelect:
			a, b := r.of(args[1], depth+1), r.of(args[2], depth+1)
			out = FloatRange{min(a.lo, b.lo), max(a.hi, b.hi), a.nan || b.nan}
		}
	}

	if val.typ == F32 {
		out.lo, out.hi = float64(float32(out.lo)), float64(float32(out.hi))
	}

	if bound, ok := r.facts[v]; ok {
		out = meetFloatRange(out, bound)
	}

	r.cache[v] = out

	return out
}

// Saturation is unnecessary when the input is in the signed destination's
// range on every path that observes the result. A select may discard an
// out-of-range conversion; raw x86 conversion is non-trapping too, so its
// unobserved result does not matter. Trapping wasm conversions stay intact.
func (l *lowerer) findRawTruncations() {
	f := l.f
	l.rawTrunc = make([]bool, len(f.values))
	user := make([]int32, len(f.values))

	for v := range user {
		user[v] = -1
	}

	for _, b := range l.ci.order {
		for _, v := range f.blocks[b].instrs {
			for _, a := range f.args(v) {
				user[a] = v
			}
		}
	}

	for v := range f.values {
		val := &f.values[v]

		if val.op != OpTruncSatS && val.op != OpTruncSatU || val.block < 0 || l.ci.rpoIndex[val.block] < 0 || l.uses[v] == 0 {
			continue
		}

		r := &FloatRanges{f: f, facts: map[int32]FloatRange{}, cache: map[int32]FloatRange{}}
		b := val.block

		for p := l.ci.idom[b]; p != b; {
			t := &f.blocks[p].term

			if t.kind == TermIf && len(t.edges) == 2 {
				a := l.ci.dominates(t.edges[0].block, b)
				z := l.ci.dominates(t.edges[1].block, b)

				if a != z {
					r.constrain(t.value, a)
				}
			}

			if l.ci.idom[p] == p {
				break
			}

			p = l.ci.idom[p]
		}

		if l.uses[v] == 1 && user[v] >= 0 {
			u := user[v]

			if f.values[u].op == OpSelect {
				args := f.args(u)

				if args[1] != args[2] {
					r.constrain(args[0], args[1] == int32(v))
				}
			}
		}

		bound := r.of(f.args(int32(v))[0], 0)
		hi := math.Ldexp(1, int(typeSize(val.typ))*8-1)
		lo := -hi

		if val.op == OpTruncSatU {
			lo = 0
		}

		l.rawTrunc[v] = !bound.nan && bound.lo <= bound.hi && bound.lo >= lo && bound.hi < hi
	}
}
