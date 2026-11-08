package main

import "math/bits"

// A checked, contiguous byte-copy loop can use a string instruction.
// Forward order preserves dependencies between overlapping loads and stores;
// memory.copy's memmove semantics would change those dependencies.
func (l *MemoryLoop) copyLoop(fast, count, size int32, proven map[int32]bool, invariants map[int32]int32) int32 {
	f := l.f

	if len(l.order) != 1 {
		return -1
	}

	var loads, stores []int32
	pending := int32(-1)

	for _, v := range f.blocks[l.header].instrs {
		val := &f.values[v]

		switch val.op {
		case OpLoad:
			if pending >= 0 || !proven[v] || memKind(val.aux2).width() != 1 {
				return -1
			}

			loads = append(loads, v)
			pending = v
		case OpStore:
			if pending < 0 || !proven[v] || memKind(val.aux2).width() != 1 || f.args(v)[1] != pending {
				return -1
			}

			stores = append(stores, v)
			pending = -1
		default:
			if readsState(val.op) {
				return -1
			}
		}
	}

	if pending >= 0 || len(loads) == 0 {
		return -1
	}

	var back, exit Edge

	for _, e := range f.blocks[l.header].term.edges {
		if e.block == l.header {
			back = e
		} else {
			exit = e
		}
	}

	steps := make([]int64, len(f.blocks[l.header].params))

	for j, p := range f.blocks[l.header].params {
		if f.values[p].typ != I32 {
			return -1
		}

		steps[j] = inductionStep(f, p, back.args[j])

		if steps[j] == 0 && back.args[j] != p {
			return -1
		}
	}

	width := uint64(len(loads))
	addresses := map[int32]CopyAddress{}
	contiguous := func(values []int32) bool {
		var first CopyAddress

		for j, v := range values {
			addr := l.copyAddress(f.args(v)[0], steps, invariants, addresses, 0)
			addr.offset += uint32(f.values[v].aux)

			if !addr.valid || addr.step != uint32(width) {
				return false
			}

			if j == 0 {
				first = addr
			} else if addr.offset != first.offset+uint32(j) || len(addr.terms) != len(first.terms) {
				return false
			}

			for v, mul := range first.terms {
				if addr.terms[v] != mul {
					return false
				}
			}
		}

		return true
	}

	if !contiguous(loads) || !contiguous(stores) {
		return -1
	}

	reverse := map[int32]int32{}

	for j, p := range f.blocks[l.header].params {
		reverse[f.blocks[fast].params[j]] = p
	}

	index := 0

	for _, v := range f.blocks[l.header].instrs {
		if _, hoisted := invariants[v]; !hoisted {
			reverse[f.blocks[fast].instrs[index]] = v
			index++
		}
	}

	var can func(int32, int) bool
	can = func(v int32, depth int) bool {
		val := &f.values[v]

		if _, hoisted := invariants[v]; hoisted || val.block != l.header || val.op == OpParam {
			return true
		}

		if depth > 24 || readsState(val.op) {
			return false
		}

		for _, a := range f.args(v) {
			if !can(a, depth+1) {
				return false
			}
		}

		return true
	}
	source, dest := f.args(loads[0])[0], f.args(stores[0])[0]

	if !can(source, 0) || !can(dest, 0) {
		return -1
	}

	// clone has already restored any exit parameters for values escaping
	// the original loop. Their fast-edge arguments now need the last
	// iteration's values, rather than the first iteration's values.
	fastExit := f.blocks[fast].term.edges[0]

	if fastExit.block == fast {
		fastExit = f.blocks[fast].term.edges[1]
	}

	original := func(v int32) int32 {
		if old, ok := reverse[v]; ok {
			return old
		}

		return v
	}

	for _, v := range fastExit.args {
		if !can(original(v), 0) {
			return -1
		}
	}

	bulk := f.newBlock()
	current := bulk

	for j, p := range f.blocks[fast].params {
		v := f.newValue(OpParam, f.values[p].typ, bulk, int64(j))
		f.blocks[bulk].params = append(f.blocks[bulk].params, v)
	}

	emit := func(op Op, typ Type, aux int64, args ...int32) int32 {
		v := f.newValue(op, typ, current, aux, args...)
		f.blocks[current].instrs = append(f.blocks[current].instrs, v)

		return v
	}
	constant := func(typ Type, x int64) int32 {
		return f.newValue(OpConst, typ, -1, x)
	}
	iterations := emit(OpExtendU, I64, 0, count)

	if l.limit < 0 && (l.countStep != 1 || l.countStop != 0) {
		iterations = emit(OpSub, I64, 0, iterations, constant(I64, int64(l.countStop+1)))

		if l.countStep&(l.countStep-1) == 0 {
			iterations = emit(OpShrU, I64, 0, iterations, constant(I64, int64(bits.TrailingZeros64(l.countStep))))
		} else {
			iterations = emit(OpDivU, I64, 0, iterations, constant(I64, int64(l.countStep)))
		}

		iterations = emit(OpAdd, I64, 0, iterations, constant(I64, 1))
	}

	last := emit(OpWrap, I32, 0, emit(OpSub, I64, 0, iterations, constant(I64, 1)))
	var evaluate func(int32, bool, map[int32]int32) int32
	evaluate = func(v int32, final bool, cache map[int32]int32) int32 {
		if mapped, ok := invariants[v]; ok {
			return mapped
		}

		if mapped, ok := cache[v]; ok {
			return mapped
		}

		val := f.values[v]

		if val.block != l.header {
			return v
		}

		var mapped int32

		if val.op == OpParam {
			mapped = f.blocks[bulk].params[val.aux]

			if final && steps[val.aux] != 0 {
				delta := emit(OpMul, I32, 0, last, constant(I32, steps[val.aux]))
				mapped = emit(OpAdd, I32, 0, mapped, delta)
			}
		} else {
			args := append([]int32(nil), f.args(v)...)

			for j, a := range args {
				args[j] = evaluate(a, final, cache)
			}

			mapped = emit(val.op, val.typ, val.aux, args...)
			f.values[mapped].aux2 = val.aux2
		}

		cache[v] = mapped

		return mapped
	}
	initial := map[int32]int32{}
	src, dst := evaluate(source, false, initial), evaluate(dest, false, initial)

	if off := f.values[loads[0]].aux; off != 0 {
		src = emit(OpAdd, I32, 0, src, constant(I32, off))
	}

	if off := f.values[stores[0]].aux; off != 0 {
		dst = emit(OpAdd, I32, 0, dst, constant(I32, off))
	}

	length := emit(OpMul, I64, 0, iterations, constant(I64, int64(width)))
	condition := emit(OpGeU, I32, 0, length, constant(I64, 16))

	if l.countSigned {
		positive := emit(OpLeU, I32, 0, count, constant(I32, 1<<31-1))
		condition = emit(OpAnd, I32, 0, condition, positive)
	}

	// Symbolic addresses describe i32 arithmetic exactly, including wrap.
	// The string instruction needs one nonwrapping span, so guard that
	// span separately and retain the byte loop when it crosses 4 GiB.
	for _, addr := range []int32{src, dst} {
		end := emit(OpAdd, I64, 0, emit(OpExtendU, I64, 0, addr), length)
		condition = emit(OpAnd, I32, 0, condition, emit(OpLeU, I32, 0, end, size))
	}

	current = f.newBlock()
	f.blocks[bulk].term = Term{kind: TermIf, value: condition, edges: []Edge{{block: current}, {block: fast, args: append([]int32(nil), f.blocks[bulk].params...)}}}
	emit(OpCopyForward, TNone, 0, dst, src, length)
	final := map[int32]int32{}
	args := make([]int32, len(fastExit.args))

	for j, v := range fastExit.args {
		args[j] = evaluate(original(v), true, final)
	}

	f.blocks[current].term = Term{kind: TermJump, edges: []Edge{{block: exit.block, args: args}}}

	return bulk
}

// Address equality must use exact affine expressions, not coinciding
// range bounds. Each header parameter has its own identity and step;
// decreasing or unrelated indices cannot masquerade as a forward copy.
type CopyAddress struct {
	terms        map[int32]uint32
	offset, step uint32
	valid        bool
}

func (l *MemoryLoop) copyAddress(v int32, steps []int64, invariants map[int32]int32, cache map[int32]CopyAddress, depth int) CopyAddress {
	if known, ok := cache[v]; ok {
		return known
	}

	f := l.f
	val := f.values[v]
	bad := CopyAddress{}

	if depth > 24 || val.typ != I32 {
		return bad
	}

	out := CopyAddress{terms: map[int32]uint32{}, valid: true}
	mapped, hoisted := invariants[v]

	if val.op == OpConst {
		out.offset = uint32(val.aux)
	} else if hoisted || val.block != l.header || val.op == OpParam {
		key := v

		if hoisted {
			key = mapped
		} else if val.block == l.header {
			out.step = uint32(steps[val.aux])
		}

		out.terms[key] = 1
	} else {
		args := f.args(v)

		switch val.op {
		case OpAdd, OpSub:
			a := l.copyAddress(args[0], steps, invariants, cache, depth+1)
			b := l.copyAddress(args[1], steps, invariants, cache, depth+1)

			if !a.valid || !b.valid {
				return bad
			}

			mul := uint32(1)

			if val.op == OpSub {
				mul = 0xffffffff
			}

			out.offset, out.step = a.offset+mul*b.offset, a.step+mul*b.step

			for v, k := range a.terms {
				out.terms[v] = k
			}

			for v, k := range b.terms {
				out.terms[v] += mul * k
			}
		case OpShl, OpMul:
			a, k := args[0], f.values[args[1]]

			if val.op == OpMul && k.op != OpConst {
				a, k = args[1], f.values[args[0]]
			}

			if k.op != OpConst {
				return bad
			}

			base := l.copyAddress(a, steps, invariants, cache, depth+1)

			if !base.valid {
				return bad
			}

			mul := uint32(k.aux)

			if val.op == OpShl {
				mul = 1 << (mul & 31)
			}

			out.offset, out.step = base.offset*mul, base.step*mul

			for v, k := range base.terms {
				out.terms[v] = k * mul
			}
		default:
			return bad
		}
	}

	for v, k := range out.terms {
		if k == 0 {
			delete(out.terms, v)
		}
	}

	cache[v] = out

	return out
}
