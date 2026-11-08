package main

import "math"

type FloatIntegerNarrow struct {
	o      *optimizer
	valid  []uint8
	values map[int32]int32
}

// Every u32 has an exact f64 representation. Comparisons after a merge of
// such conversions and integral constants can stay entirely in integers;
// the original float values remain available to any other users.
func (o *optimizer) narrowFloatComparisons() bool {
	f := o.f
	n := &FloatIntegerNarrow{o: o, valid: make([]uint8, len(f.values)), values: map[int32]int32{}}
	changed := false

	for v := range f.values {
		val := f.values[v]

		if val.op < OpFEq || val.op > OpFGe || val.block < 0 || f.blocks[val.block].dead {
			continue
		}

		args := f.args(int32(v))

		if !n.can(args[0], 0) || !n.can(args[1], 0) {
			continue
		}

		a, b := n.integer(args[0]), n.integer(args[1])
		ops := [6]Op{OpEq, OpNe, OpLtU, OpGtU, OpLeU, OpGeU}
		f.values[v].op = ops[val.op-OpFEq]
		o.setArgs(int32(v), a, b)
		changed = true
	}

	return changed
}

func (n *FloatIntegerNarrow) can(v int32, depth int) bool {
	f := n.o.f
	val := &f.values[v]

	if val.typ != F64 || depth > 24 {
		return false
	}

	if n.valid[v] != 0 {
		return n.valid[v] == 2
	}

	n.valid[v] = 1
	ok := false

	switch val.op {
	case OpConst:
		x := math.Float64frombits(uint64(val.aux))
		ok = x >= 0 && x <= math.MaxUint32 && math.Trunc(x) == x
	case OpConvertU:
		ok = f.values[f.args(v)[0]].typ == I32
	case OpParam:
		incoming := n.o.incoming(val.block)
		ok = val.block != 0 && len(incoming) > 0

		for _, e := range incoming {
			ok = ok && n.can(e.args[val.aux], depth+1)
		}
	}

	if ok {
		n.valid[v] = 2
	}

	return ok
}

func (n *FloatIntegerNarrow) integer(v int32) int32 {
	if mapped, ok := n.values[v]; ok {
		return mapped
	}

	f := n.o.f
	val := f.values[v]

	switch val.op {
	case OpConst:
		return n.o.constant(I32, int64(math.Float64frombits(uint64(val.aux))))
	case OpConvertU:
		return f.args(v)[0]
	}

	b := val.block
	index := len(f.blocks[b].params)
	p := f.newValue(OpParam, I32, b, int64(index))
	f.blocks[b].params = append(f.blocks[b].params, p)
	n.values[v] = p
	incoming := n.o.incoming(b)

	// Reserve every argument slot before recursively adding another
	// parameter to the same block.
	for _, e := range incoming {
		e.args = append(e.args, -1)
	}

	for _, e := range incoming {
		arg := e.args[val.aux]
		e.args[index] = n.integer(arg)
	}

	return p
}
