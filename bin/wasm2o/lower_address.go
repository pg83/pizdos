package main

import "math"

func (l *lowerer) scaledIndex(v, block int32) bool {
	f := l.f
	val := &f.values[v]

	if val.op != OpShl || val.block != block || l.uses[v] != 1 {
		return false
	}

	count := f.args(v)[1]

	return l.isConst(count) && l.constBits(count)&int64(typeSize(val.typ)*8-1) <= 3
}

// LEA computes a sum in one instruction with an independent destination.
// Its 32-bit form preserves wasm's wrapping arithmetic even when the full
// effective address overflows, and can absorb a single-use scaled index.
func (l *lowerer) leaAdd(v, a, b int32, size uint8) bool {
	f := l.f
	scale := uint8(1)
	folded := false

	if l.deferred[a] && f.values[a].op == OpShl {
		a, b = b, a
	}

	if l.deferred[b] && f.values[b].op == OpShl {
		args := f.args(b)
		scale = 1 << uint(l.constBits(args[1])&int64(size*8-1))
		b = args[0]
		folded = true
	}

	mem := Mem{base: noReg, index: noReg, scale: scale, label: -1}

	if x, ok := l.imm(a, size); ok && x >= math.MinInt32 && x <= math.MaxInt32 && folded {
		mem.disp = int32(x)
		mem.index = l.use(b)
	} else if !l.isConst(a) && !l.isConst(b) {
		mem.base = l.use(a)
		mem.index = l.use(b)
	} else {
		return false
	}

	in := inst(mLea)
	in.dst = l.def(v)
	in.size = size
	in.mem = mem
	l.emit(in)

	return true
}
