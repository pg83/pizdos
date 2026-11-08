package main

// A fast loop checks its address ranges once on entry. If a range does
// not fit, the original loop runs with its individual bounds checks, so
// stores before a later trapping access remain observable in the same order.
type LoopSpan struct {
	base                int32
	mul, factor, offset uint64
	base2               int32
	mul2, lo            uint64
	stride              int32 // SSA value plus one; zero is a constant factor
	valid               bool
}

type MemoryLoop struct {
	f               *Function
	ci              *cfgInfo
	header, latch   int32
	preheader, exit int32
	entry           Edge
	members         []bool
	order           []int32
	count           int32
	start, limit    int32
	signedLimit     bool
	induction       map[int32]LoopSpan
	cache           map[int32]LoopSpan
	unsigned        *UnsignedRanges
	allowLoads      bool
	invariantCache  map[int32]bool
	invariantNodes  []int32
	copies          []int32
}

func versionMemoryLoops(f *Function) {
	if len(f.module.mems) == 0 {
		return
	}

	done := map[int32]bool{}

	for round := 0; round < 64; round++ {
		ci := f.analyze()
		changed := false

		for _, h := range ci.order {
			if !ci.header[h] || done[h] {
				continue
			}

			done[h] = true
			loop := findMemoryLoop(f, ci, h)

			if loop == nil {
				continue
			}

			if fast := loop.version(); fast >= 0 {
				for _, h := range loop.copies {
					done[h] = true
				}

				changed = true

				break
			}
		}

		if !changed {
			break
		}
	}
}

func findMemoryLoop(f *Function, ci *cfgInfo, h int32) *MemoryLoop {
	l := &MemoryLoop{f: f, ci: ci, header: h, latch: -1, preheader: -1, exit: -1, start: -1, limit: -1,
		members: make([]bool, len(f.blocks)), induction: map[int32]LoopSpan{}, cache: map[int32]LoopSpan{}}

	for _, p := range f.blocks[h].preds {
		if ci.dominates(h, p) {
			if l.latch >= 0 {
				return nil
			}

			l.latch = p
		} else {
			if l.preheader >= 0 {
				return nil
			}

			l.preheader = p
		}
	}

	if l.latch < 0 || l.preheader < 0 {
		return nil
	}

	for _, e := range f.blocks[l.preheader].term.edges {
		if e.block == h {
			l.entry = e
		}
	}

	l.members[h] = true
	work := []int32{l.latch}

	for len(work) > 0 {
		b := work[len(work)-1]
		work = work[:len(work)-1]

		if l.members[b] {
			continue
		}

		if ci.header[b] || !ci.dominates(h, b) {
			return nil
		}

		l.members[b] = true
		work = append(work, f.blocks[b].preds...)
	}

	insts := 0

	for _, b := range ci.order {
		if !l.members[b] {
			continue
		}

		l.order = append(l.order, b)
		insts += len(f.blocks[b].instrs)

		for _, e := range f.blocks[b].term.edges {
			if !l.members[e.block] {
				if l.exit >= 0 && l.exit != e.block {
					return nil
				}

				l.exit = e.block
			}
		}
	}

	if l.exit < 0 || len(l.order) > 48 || insts > 400 {
		return nil
	}

	outsidePred := false

	for _, p := range f.blocks[l.exit].preds {
		outsidePred = outsidePred || !l.members[p]
	}

	if outsidePred && l.hasLiveOut() {
		return nil
	}

	t := &f.blocks[l.latch].term

	if t.kind != TermIf {
		return nil
	}

	condition := &f.values[t.value]
	args := f.args(t.value)
	var next int32

	if condition.op == OpEqz && t.edges[1].block == h {
		next = args[0]
	} else if condition.op == OpNe && t.edges[0].block == h && f.values[args[1]].op == OpConst && f.values[args[1]].aux == 0 {
		next = args[0]
	} else if (condition.op == OpGtS || condition.op == OpGtU) && t.edges[0].block == h && f.values[args[1]].op == OpConst && f.values[args[1]].aux == 0 {
		next = args[0]
	} else if t.edges[0].block == h && (condition.op == OpAdd || condition.op == OpSub) {
		next = t.value
	} else if t.edges[0].block == h && (condition.op == OpLtS || condition.op == OpLtU) {
		next = args[0]
		l.limit = args[1]
		l.signedLimit = condition.op == OpLtS
	} else {
		return nil
	}

	var back Edge

	for _, e := range t.edges {
		if e.block == h {
			back = e
		}
	}

	l.count = -1

	for j, p := range f.blocks[h].params {
		if back.args[j] != next {
			continue
		}

		if l.limit >= 0 && inductionStep(f, p, next) == 1 {
			l.start = l.entry.args[j]
		} else if l.limit < 0 && inductionStep(f, p, next) == -1 {
			l.count = l.entry.args[j]
			l.induction[p] = LoopSpan{base: -1, factor: 1, offset: 1, valid: true}
		}
	}

	if l.count < 0 && l.start < 0 {
		return nil
	}

	for j, p := range f.blocks[h].params {
		step := inductionStep(f, p, back.args[j])

		if step > 0 && step <= 65536 {
			l.induction[p] = LoopSpan{base: l.entry.args[j], mul: 1, factor: uint64(step), valid: true}
		}
	}

	l.unsigned = &UnsignedRanges{f: f, cache: make([]UnsignedRange, len(f.values)), seen: make([]bool, len(f.values))}

	return l
}

// With an entry that bypasses the loop, existing exit parameters already
// merge the two paths. A new parameter for a collapsed live-out value
// would need an undefined argument on that bypass, so leave that case alone.
func (l *MemoryLoop) hasLiveOut() bool {
	f := l.f
	inside := func(v int32) bool {
		b := f.values[v].block

		return b >= 0 && l.members[b]
	}

	for b := range f.blocks {
		if l.members[b] || f.blocks[b].dead {
			continue
		}

		blk := &f.blocks[b]

		for _, v := range blk.instrs {
			for _, a := range f.args(v) {
				if inside(a) {
					return true
				}
			}
		}

		if (blk.term.kind == TermIf || blk.term.kind == TermSwitch) && inside(blk.term.value) {
			return true
		}

		for _, v := range blk.term.values {
			if inside(v) {
				return true
			}
		}

		for _, e := range blk.term.edges {
			for _, v := range e.args {
				if inside(v) {
					return true
				}
			}
		}
	}

	return false
}

func inductionStep(f *Function, phi, next int32) int64 {
	val := &f.values[next]
	args := f.args(next)

	if val.typ != I32 || val.op != OpAdd && val.op != OpSub {
		return 0
	}

	a, b := args[0], args[1]

	if val.op == OpAdd && b == phi {
		a, b = b, a
	}

	if a != phi || f.values[b].op != OpConst {
		return 0
	}

	step := int64(int32(f.values[b].aux))

	if val.op == OpSub {
		step = -step
	}

	return step
}

// Unrolled loops may advance a pointer by adding the same stride several
// times. Keep those increments as a factored stride, without changing the
// wrapping i32 arithmetic of the original loop.
func inductionTerms(f *Function, v, phi int32, depth int) ([]int32, bool) {
	if v == phi {
		return nil, true
	}

	if depth > 16 || f.values[v].op != OpAdd || f.values[v].typ != I32 {
		return nil, false
	}

	args := f.args(v)

	for j := 0; j < 2; j++ {
		if terms, ok := inductionTerms(f, args[j], phi, depth+1); ok {
			return append(terms, args[1-j]), true
		}
	}

	return nil, false
}

func (l *MemoryLoop) invariant(v int32, depth int) bool {
	val := &l.f.values[v]

	if val.block < 0 {
		return true
	}

	if !l.members[val.block] {
		return l.ci.dominates(val.block, l.header)
	}

	if known, ok := l.invariantCache[v]; ok {
		return known
	}

	l.invariantCache[v] = false

	if depth > 32 || val.typ != I32 && val.typ != I64 && val.typ != F32 && val.typ != F64 {
		return false
	}

	if val.op == OpLoad {
		if !l.allowLoads || len(l.invariantNodes) >= 128 || memKind(val.aux2).width() == 0 {
			return false
		}
	} else if readsState(val.op) {
		return false
	}

	for _, a := range l.f.args(v) {
		if !l.invariant(a, depth+1) {
			return false
		}
	}

	l.invariantCache[v] = true
	l.invariantNodes = append(l.invariantNodes, v)

	return true
}

func addLoopSpans(a, b LoopSpan) LoopSpan {
	if !a.valid || !b.valid || a.factor != 0 && b.factor != 0 && a.stride != b.stride {
		return LoopSpan{}
	}

	out := LoopSpan{base: -1, factor: a.factor + b.factor, offset: a.offset + b.offset, lo: a.lo + b.lo, valid: true, stride: a.stride}

	if a.factor == 0 {
		out.stride = b.stride
	}

	for _, term := range [][2]int64{{int64(a.base), int64(a.mul)}, {int64(a.base2), int64(a.mul2)}, {int64(b.base), int64(b.mul)}, {int64(b.base2), int64(b.mul2)}} {
		v, mul := int32(term[0]), uint64(term[1])

		if mul == 0 {
			continue
		}

		switch {
		case out.mul == 0 || out.base == v:
			out.base = v
			out.mul += mul
		case out.mul2 == 0 || out.base2 == v:
			out.base2 = v
			out.mul2 += mul
		default:
			return LoopSpan{}
		}
	}

	if out.mul2 != 0 && out.base2 < out.base {
		out.base, out.base2 = out.base2, out.base
		out.mul, out.mul2 = out.mul2, out.mul
	}

	return out
}

func (l *MemoryLoop) span(v int32, depth int) LoopSpan {
	if bound, ok := l.induction[v]; ok {
		return bound
	}

	if bound, ok := l.cache[v]; ok {
		return bound
	}

	bad := LoopSpan{}
	l.cache[v] = bad
	val := &l.f.values[v]

	if val.typ != I32 || depth > 32 {
		return bad
	}

	out := bad

	if val.op == OpConst {
		out = LoopSpan{base: -1, offset: uint64(uint32(val.aux)), lo: uint64(uint32(val.aux)), valid: true}
	} else if l.invariant(v, 0) {
		out = LoopSpan{base: v, mul: 1, valid: true}
	} else if bound := l.unsigned.of(v, 0); bound.hi < 1<<32-1 {
		out = LoopSpan{base: -1, offset: bound.hi, lo: bound.lo, valid: true}
	} else {
		args := l.f.args(v)

		switch val.op {
		case OpAdd:
			a, b := l.span(args[0], depth+1), l.span(args[1], depth+1)

			out = addLoopSpans(a, b)
		case OpShl:
			if l.f.values[args[1]].op == OpConst {
				n := uint32(l.f.values[args[1]].aux) & 31

				if n <= 16 {
					out = l.span(args[0], depth+1)
					out.mul <<= n
					out.mul2 <<= n
					out.factor <<= n
					out.offset <<= n
					out.lo <<= n
				}
			}
		}
	}

	if out.mul > 65536 || out.mul2 > 65536 || out.factor > 65536 || out.offset > 1<<40 {
		out.valid = false
	}

	l.cache[v] = out

	return out
}

func (l *MemoryLoop) version() int32 {
	for attempt := 0; attempt < 2; attempt++ {
		for p, bound := range l.induction {
			if bound.stride != 0 {
				delete(l.induction, p)
			}
		}

		// Kernel and WASI imports may expose memory to host callbacks.
		l.allowLoads = attempt == 0 && l.f.module.importedFuncs == 0 && !l.f.module.mems[0].imported

		for _, b := range l.order {
			for _, v := range l.f.blocks[b].instrs {
				switch l.f.values[v].op {
				case OpCall, OpCallIndirect, OpMemCopy, OpMemFill, OpMemInit:
					l.allowLoads = false
				}
			}
		}

		l.invariantCache = map[int32]bool{}
		l.invariantNodes = nil
		l.cache = map[int32]LoopSpan{}

		if fast := l.versionOne(); fast >= 0 {
			return fast
		}
	}

	return -1
}

func (l *MemoryLoop) versionOne() int32 {
	f := l.f

	if l.limit >= 0 && !l.invariant(l.limit, 0) {
		return -1
	}

	// Cache repeated format dispatches together with the address metadata.
	// The alias guards below also protect the loads used by these switches.
	var switches []int32

	for _, b := range l.order {
		t := &f.blocks[b].term

		if t.kind == TermSwitch && len(t.edges) >= 5 && t.edges[0].block != t.edges[len(t.edges)-1].block && l.invariant(t.value, 0) {
			switches = append(switches, b)
		}
	}

	type rangeKey struct {
		base, base2, stride int32
		mul, mul2, factor   uint64
	}
	var writes []LoopSpan
	writeIndex := map[rangeKey]int{}
	keyOf := func(bound LoopSpan) rangeKey {
		return rangeKey{bound.base, bound.base2, bound.stride, bound.mul, bound.mul2, bound.factor}
	}

	// A dynamic increment is invariant too, but its unsigned value must
	// be small enough before the 64-bit range product can justify a loop.
	for _, e := range f.blocks[l.latch].term.edges {
		if e.block != l.header {
			continue
		}

		for j, p := range f.blocks[l.header].params {
			if _, known := l.induction[p]; known {
				continue
			}

			terms, ok := inductionTerms(f, e.args[j], p, 0)

			if !ok {
				continue
			}

			bound := LoopSpan{base: l.entry.args[j], mul: 1, valid: true}

			for _, v := range terms {
				val := &f.values[v]

				if val.op == OpConst && bound.stride == 0 && val.aux >= 0 && val.aux <= 65536 {
					bound.factor += uint64(val.aux)
				} else if val.op != OpConst && (bound.factor == 0 || bound.stride == v+1) && l.invariant(v, 0) {
					bound.stride = v + 1
					bound.factor++
				} else {
					bound.valid = false

					break
				}
			}

			if bound.valid && bound.factor <= 65536 {
				l.induction[p] = bound
			}
		}
	}
	indices := map[rangeKey]int{}
	var guards []LoopSpan
	proven := map[int32]bool{}
	unboundedWrite := false

	for _, b := range l.order {
		for _, v := range f.blocks[b].instrs {
			val := &f.values[v]

			if val.op != OpLoad && val.op != OpStore || memKind(val.aux2)&memProven != 0 {
				continue
			}

			addr := f.args(v)[0]
			end := uint64(val.aux) + uint64(memKind(val.aux2).width())
			bound := l.span(addr, 0)

			if val.op == OpStore {
				if bound.valid {
					write := bound
					write.lo += uint64(val.aux)
					write.offset += end
					key := keyOf(write)

					if j, known := writeIndex[key]; known {
						writes[j].lo = min(writes[j].lo, write.lo)
						writes[j].offset = max(writes[j].offset, write.offset)
					} else {
						writeIndex[key] = len(writes)
						writes = append(writes, write)
					}
				} else {
					unboundedWrite = true
				}
			}

			if l.unsigned.of(addr, 0).hi+end <= f.module.mems[0].limits.min*65536 && !f.module.mems[0].imported {
				continue
			}

			if !bound.valid {
				continue
			}

			bound.offset += end
			key := keyOf(bound)

			if j, ok := indices[key]; ok {
				guards[j].offset = max(guards[j].offset, bound.offset)
			} else {
				indices[key] = len(guards)
				guards = append(guards, bound)
			}

			proven[v] = true
		}
	}

	hasLoad := false

	for _, v := range l.invariantNodes {
		hasLoad = hasLoad || f.values[v].op == OpLoad
	}

	if len(proven) < 4 || len(guards) > 24 || unboundedWrite && hasLoad {
		return -1
	}

	minimum := uint64(4)

	if hasLoad {
		minimum = 16
	}

	if l.count >= 0 && l.unsigned.of(l.count, 0).hi < minimum {
		return -1
	}

	guard := f.newBlock()
	current := guard
	emit := func(op Op, typ Type, args ...int32) int32 {
		v := f.newValue(op, typ, current, 0, args...)
		f.blocks[current].instrs = append(f.blocks[current].instrs, v)

		return v
	}
	constant := func(typ Type, x uint64) int32 {
		return f.newValue(OpConst, typ, -1, int64(x))
	}
	pages := emit(OpMemSize, I32)
	size := emit(OpShl, I64, emit(OpExtendU, I64, pages), constant(I64, 16))
	condition := constant(I32, 1)

	if l.count >= 0 {
		condition = emit(OpGeU, I32, l.count, constant(I32, minimum))
	}

	invariants := map[int32]int32{}
	mapValue := func(v int32) int32 {
		if mapped, ok := invariants[v]; ok {
			return mapped
		}

		return v
	}
	type loadRange struct{ begin, end int32 }
	var reads []loadRange
	available := map[CseKey]int32{}

	if len(l.invariantNodes) > 0 {
		next := f.newBlock()
		f.blocks[current].term = Term{kind: TermIf, value: condition, edges: []Edge{{block: next}, l.entry}}
		current = next
	}

	for _, v := range l.invariantNodes {
		val := f.values[v]
		args := append([]int32(nil), f.args(v)...)

		for j := range args {
			args[j] = mapValue(args[j])
		}

		key := CseKey{op: val.op, typ: val.typ, nargs: val.nargs, aux: val.aux, aux2: val.aux2}
		cacheable := len(args) <= len(key.args)

		if cacheable {
			copy(key.args[:], args)

			if n, known := available[key]; known {
				invariants[v] = n

				continue
			}
		}

		if val.op == OpLoad {
			addr := emit(OpExtendU, I64, args[0])
			begin := emit(OpAdd, I64, addr, constant(I64, uint64(val.aux)))
			end := emit(OpAdd, I64, begin, constant(I64, uint64(memKind(val.aux2).width())))
			ok := emit(OpLeU, I32, end, size)
			next := f.newBlock()
			f.blocks[current].term = Term{kind: TermIf, value: ok, edges: []Edge{{block: next}, l.entry}}
			current = next
			reads = append(reads, loadRange{begin, end})
			val.aux2 |= int32(memProven)
		}

		n := emit(val.op, val.typ, args...)
		f.values[n].aux, f.values[n].aux2 = val.aux, val.aux2
		invariants[v] = n

		if cacheable {
			available[key] = n
		}
	}

	countValue := l.count

	if l.limit >= 0 {
		limit := mapValue(l.limit)
		countValue = emit(OpSub, I32, limit, l.start)
		condition = emit(OpLtU, I32, l.start, limit)
		condition = emit(OpAnd, I32, condition, emit(OpGeU, I32, countValue, constant(I32, minimum)))

		// With signed termination, both endpoints must be nonnegative.
		// Then the final increment reaches the limit without wrapping.
		if l.signedLimit {
			condition = emit(OpAnd, I32, condition, emit(OpLeU, I32, limit, constant(I32, 1<<31-1)))
		}
	}

	count := emit(OpExtendU, I64, countValue)
	iterations := emit(OpSub, I64, count, constant(I64, 1))
	evaluate := func(bound LoopSpan) (int32, int32) {
		end := constant(I64, bound.offset)
		begin := constant(I64, bound.lo)

		for _, term := range [][2]int64{{int64(bound.base), int64(bound.mul)}, {int64(bound.base2), int64(bound.mul2)}} {
			if term[1] == 0 {
				continue
			}

			base := emit(OpExtendU, I64, mapValue(int32(term[0])))

			if term[1] != 1 {
				base = emit(OpMul, I64, base, constant(I64, uint64(term[1])))
			}

			end = emit(OpAdd, I64, end, base)
			begin = emit(OpAdd, I64, begin, base)
		}

		if bound.factor != 0 {
			distance := emit(OpMul, I64, iterations, constant(I64, bound.factor))

			if bound.stride != 0 {
				stride := mapValue(bound.stride - 1)
				condition = emit(OpAnd, I32, condition, emit(OpLeU, I32, stride, constant(I32, 65536/bound.factor)))
				distance = emit(OpMul, I64, distance, emit(OpExtendU, I64, stride))
			}

			end = emit(OpAdd, I64, end, distance)
		}

		return begin, end
	}

	for _, bound := range guards {
		_, end := evaluate(bound)
		condition = emit(OpAnd, I32, condition, emit(OpLeU, I32, end, size))
	}

	for _, write := range writes {
		begin, end := evaluate(write)
		condition = emit(OpAnd, I32, condition, emit(OpLeU, I32, end, size))

		for _, read := range reads {
			disjoint := emit(OpOr, I32, emit(OpLeU, I32, end, read.begin), emit(OpLeU, I32, read.end, begin))
			condition = emit(OpAnd, I32, condition, disjoint)
		}
	}

	fast := l.clone(proven, invariants, nil)
	l.copies = append(l.copies, fast)
	entry := Edge{block: fast, args: append([]int32(nil), l.entry.args...)}

	if len(switches) >= 2 {
		index := mapValue(f.blocks[switches[0]].term.value)
		cases := map[int32]int{}

		for _, b := range switches {
			if mapValue(f.blocks[b].term.value) == index {
				cases[b] = 0
			}
		}

		if len(cases) >= 2 {
			specialized := l.clone(proven, invariants, cases)
			l.copies = append(l.copies, specialized)
			choice := f.newBlock()
			eq := f.newValue(OpEqz, I32, choice, 0, index)
			f.blocks[choice].instrs = append(f.blocks[choice].instrs, eq)
			f.blocks[choice].term = Term{kind: TermIf, value: eq, edges: []Edge{{block: specialized, args: append([]int32(nil), l.entry.args...)}, entry}}
			entry = Edge{block: choice}
		}
	}

	f.blocks[current].term = Term{kind: TermIf, value: condition, edges: []Edge{entry, l.entry}}

	for j := range f.blocks[l.preheader].term.edges {
		if f.blocks[l.preheader].term.edges[j].block == l.header {
			f.blocks[l.preheader].term.edges[j] = Edge{block: guard}
		}
	}

	return fast
}

func (l *MemoryLoop) clone(proven map[int32]bool, invariants map[int32]int32, cases map[int32]int) int32 {
	f := l.f
	oldBlocks, oldValues := len(f.blocks), len(f.values)
	blocks := map[int32]int32{}
	values := map[int32]int32{}

	for v, mapped := range invariants {
		values[v] = mapped
	}

	for _, b := range l.order {
		blocks[b] = f.newBlock()
	}

	for v := 0; v < oldValues; v++ {
		val := f.values[v]

		if _, hoisted := invariants[int32(v)]; hoisted {
			continue
		}

		if val.block >= 0 && int(val.block) < len(l.members) && l.members[val.block] {
			n := f.newValue(val.op, val.typ, blocks[val.block], val.aux)
			f.values[n].aux2 = val.aux2

			if proven[int32(v)] {
				f.values[n].aux2 |= int32(memProven)
			}

			values[int32(v)] = n
		}
	}

	remap := func(v int32) int32 {
		if n, ok := values[v]; ok {
			return n
		}

		return v
	}

	for v := 0; v < oldValues; v++ {
		if n, ok := values[int32(v)]; ok {
			if _, hoisted := invariants[int32(v)]; hoisted {
				continue
			}

			args := append([]int32(nil), f.args(int32(v))...)
			f.values[n].args = int32(len(f.pool))
			f.values[n].nargs = uint16(len(args))

			for _, a := range args {
				f.pool = append(f.pool, remap(a))
			}
		}
	}

	for _, b := range l.order {
		src := &f.blocks[b]
		dst := &f.blocks[blocks[b]]

		for _, v := range src.params {
			dst.params = append(dst.params, remap(v))
		}

		for _, v := range src.instrs {
			if _, hoisted := invariants[v]; hoisted {
				continue
			}

			dst.instrs = append(dst.instrs, remap(v))
		}

		t := &src.term

		if j, selected := cases[b]; selected {
			t = &Term{kind: TermJump, edges: []Edge{t.edges[j]}}
		}

		dst.term = Term{kind: t.kind, value: t.value, aux: t.aux}

		if t.kind == TermIf || t.kind == TermSwitch {
			dst.term.value = remap(t.value)
		}

		for _, v := range t.values {
			dst.term.values = append(dst.term.values, remap(v))
		}

		for _, e := range t.edges {
			edge := Edge{block: e.block}

			if target, ok := blocks[e.block]; ok {
				edge.block = target
			}

			for _, a := range e.args {
				edge.args = append(edge.args, remap(a))
			}

			dst.term.edges = append(dst.term.edges, edge)
		}
	}

	// Reintroduce the exit parameters that trivial-parameter removal may
	// have collapsed when there was only one copy of the loop.
	escapes := make([]bool, oldValues)
	mark := func(v int32) {
		if _, ok := values[v]; ok {
			escapes[v] = true
		}
	}

	for b := 0; b < oldBlocks; b++ {
		if f.blocks[b].dead || b < len(l.members) && l.members[b] {
			continue
		}

		blk := &f.blocks[b]

		for _, v := range blk.instrs {
			for _, a := range f.args(v) {
				mark(a)
			}
		}

		if blk.term.kind == TermIf || blk.term.kind == TermSwitch {
			mark(blk.term.value)
		}

		for _, v := range blk.term.values {
			mark(v)
		}

		for _, e := range blk.term.edges {
			for _, v := range e.args {
				mark(v)
			}
		}
	}

	merged := map[int32]int32{}

	for v, escapes := range escapes {
		if !escapes {
			continue
		}

		p := f.newValue(OpParam, f.values[v].typ, l.exit, int64(len(f.blocks[l.exit].params)))
		f.blocks[l.exit].params = append(f.blocks[l.exit].params, p)
		merged[int32(v)] = p

		for _, b := range l.order {
			for j := range f.blocks[b].term.edges {
				if f.blocks[b].term.edges[j].block == l.exit {
					f.blocks[b].term.edges[j].args = append(f.blocks[b].term.edges[j].args, int32(v))
				}
			}

			for j := range f.blocks[blocks[b]].term.edges {
				if f.blocks[blocks[b]].term.edges[j].block == l.exit {
					f.blocks[blocks[b]].term.edges[j].args = append(f.blocks[blocks[b]].term.edges[j].args, remap(int32(v)))
				}
			}
		}
	}

	rewrite := func(v *int32) {
		if n, ok := merged[*v]; ok {
			*v = n
		}
	}

	for b := 0; b < oldBlocks; b++ {
		if f.blocks[b].dead || b < len(l.members) && l.members[b] {
			continue
		}

		blk := &f.blocks[b]

		for _, v := range blk.instrs {
			for j := range f.args(v) {
				rewrite(&f.pool[f.values[v].args+int32(j)])
			}
		}

		if blk.term.kind == TermIf || blk.term.kind == TermSwitch {
			rewrite(&blk.term.value)
		}

		for j := range blk.term.values {
			rewrite(&blk.term.values[j])
		}

		for j := range blk.term.edges {
			for k := range blk.term.edges[j].args {
				rewrite(&blk.term.edges[j].args[k])
			}
		}
	}

	return blocks[l.header]
}
