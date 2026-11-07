package main

// The frontend turns a function body into SSA in one pass. Locals are
// variables resolved with Braun et al.'s on-the-fly construction; operand
// stack values crossing a branch become block parameters.

const (
	ctrlBlock = iota
	ctrlLoop
	ctrlIf
	ctrlElse
	ctrlFunc
)

type control struct {
	kind    int
	params  []ValType
	results []ValType
	// height is the operand stack height below the frame's parameters.
	height int
	// target is the block a branch to this frame jumps to: the header of a
	// loop, the merge block otherwise.
	target int32
	// elseBlock is the block of an if's false arm until else is reached.
	elseBlock int32
	// elseArgs are the parameters an if passes to its else arm.
	elseArgs []int32
	// dead is set after an unconditional branch in the frame: the rest of
	// it is unreachable and nothing is generated for it.
	dead bool
	// outer is set for frames opened in unreachable code.
	outer bool
}

type predRef struct {
	block int32
	edge  int32
}

type pendingPhi struct {
	local int32
	phi   int32
}

type frontend struct {
	m      *Module
	f      *Function
	r      reader
	locals []ValType
	stack  []int32
	ctrls  []control
	cur    int32

	defs       [][]int32
	sealed     []bool
	incomplete [][]pendingPhi
	preds      [][]predRef
	alias      []int32
	consts     map[constKey]int32
	// phiOps holds a variable phi's operands, one per entry of its block's
	// preds; they become edge arguments when construction is done.
	phiOps map[int32][]int32
	// explicit counts each block's parameters that carry operand stack
	// values; variable phis follow them.
	explicit []int
}

type constKey struct {
	typ  Type
	bits int64
}

func buildFunction(m *Module, index int) *Function {
	fn := &m.funcs[index]
	f := &Function{module: m, index: index, typ: &m.types[fn.typ]}
	fe := &frontend{m: m, f: f, r: reader{data: fn.body}, consts: map[constKey]int32{}, phiOps: map[int32][]int32{}}
	fe.run()

	return f
}

func (fe *frontend) run() {
	f := fe.f
	r := &fe.r
	fe.locals = append(fe.locals, f.typ.params...)
	groups := r.u32()
	total := uint64(len(fe.locals))

	for i := uint32(0); i < groups; i++ {
		n := r.u32()
		t := r.valType()
		total += uint64(n)

		if total > 50000 {
			throwFmt("too many locals")
		}

		for j := uint32(0); j < n; j++ {
			fe.locals = append(fe.locals, t)
		}
	}

	entry := fe.newBlock()
	fe.seal(entry)
	fe.cur = entry

	for i, t := range f.typ.params {
		fe.write(int32(i), entry, fe.addExplicit(entry, t))
	}

	for i := len(f.typ.params); i < len(fe.locals); i++ {
		fe.write(int32(i), entry, fe.zero(fe.locals[i]))
	}

	ret := fe.newBlock()
	fe.ctrls = append(fe.ctrls, control{kind: ctrlFunc, results: f.typ.results, target: ret, elseBlock: -1})

	for _, t := range f.typ.results {
		fe.addExplicit(ret, t)
	}

	fe.body()
	fe.resolveAliases()
}

func (fe *frontend) newBlock() int32 {
	b := fe.f.newBlock()
	fe.defs = append(fe.defs, nil)
	fe.sealed = append(fe.sealed, false)
	fe.incomplete = append(fe.incomplete, nil)
	fe.preds = append(fe.preds, nil)
	fe.explicit = append(fe.explicit, 0)

	return b
}

func (fe *frontend) addParam(b int32, t Type) int32 {
	blk := &fe.f.blocks[b]
	p := fe.f.newValue(OpParam, t, b, int64(len(blk.params)))
	blk.params = append(blk.params, p)

	return p
}

// addExplicit adds a parameter for an operand stack value; edges supply
// its argument when they are created.
func (fe *frontend) addExplicit(b int32, t Type) int32 {
	if len(fe.f.blocks[b].params) != fe.explicit[b] {
		throwFmt("internal error: explicit parameter after a phi")
	}

	fe.explicit[b]++

	return fe.addParam(b, t)
}

func (fe *frontend) zero(t Type) int32 {
	if t == FuncRef || t == ExternRef {
		return fe.f.newValue(OpRefNull, t, -1, 0)
	}

	return fe.constant(t, 0)
}

func (fe *frontend) constant(t Type, bits int64) int32 {
	key := constKey{t, bits}

	if v, ok := fe.consts[key]; ok {
		return v
	}

	v := fe.f.newValue(OpConst, t, -1, bits)
	fe.consts[key] = v

	return v
}

// emit appends an instruction to the current block.
func (fe *frontend) emit(op Op, t Type, aux int64, args ...int32) int32 {
	v := fe.f.newValue(op, t, fe.cur, aux, args...)
	blk := &fe.f.blocks[fe.cur]
	blk.instrs = append(blk.instrs, v)

	return v
}

func (fe *frontend) push(v int32) {
	fe.stack = append(fe.stack, v)
}

func (fe *frontend) pop() int32 {
	if len(fe.stack) <= fe.ctrls[len(fe.ctrls)-1].height {
		throwFmt("type mismatch: operand stack underflow")
	}

	v := fe.stack[len(fe.stack)-1]
	fe.stack = fe.stack[:len(fe.stack)-1]

	return v
}

// popN pops n values and returns them in stack order.
func (fe *frontend) popN(n int) []int32 {
	if len(fe.stack)-n < fe.ctrls[len(fe.ctrls)-1].height {
		throwFmt("type mismatch: operand stack underflow")
	}

	vals := append([]int32(nil), fe.stack[len(fe.stack)-n:]...)
	fe.stack = fe.stack[:len(fe.stack)-n]

	return vals
}

// top returns the n values on top of the stack without popping them.
func (fe *frontend) top(n int) []int32 {
	if len(fe.stack)-n < fe.ctrls[len(fe.ctrls)-1].height {
		throwFmt("type mismatch: operand stack underflow")
	}

	return append([]int32(nil), fe.stack[len(fe.stack)-n:]...)
}

func (fe *frontend) typeOf(v int32) Type {
	return fe.f.values[v].typ
}

// SSA construction

func (fe *frontend) write(local int32, b int32, v int32) {
	if fe.defs[b] == nil {
		fe.defs[b] = make([]int32, len(fe.locals))

		for i := range fe.defs[b] {
			fe.defs[b][i] = -1
		}
	}

	fe.defs[b][local] = v
}

func (fe *frontend) read(local int32, b int32) int32 {
	if d := fe.defs[b]; d != nil && d[local] >= 0 {
		return fe.resolve(d[local])
	}

	var v int32
	t := fe.locals[local]

	switch {
	case !fe.sealed[b]:
		v = fe.addParam(b, t)
		fe.incomplete[b] = append(fe.incomplete[b], pendingPhi{local, v})
	case len(fe.preds[b]) == 1:
		v = fe.read(local, fe.preds[b][0].block)
	case len(fe.preds[b]) == 0:
		v = fe.f.newValue(OpUndef, t, -1, 0)
	default:
		v = fe.addParam(b, t)
		fe.write(local, b, v)
		v = fe.addOperands(local, v)
	}

	fe.write(local, b, v)

	return v
}

func (fe *frontend) addOperands(local int32, phi int32) int32 {
	b := fe.f.values[phi].block
	ops := make([]int32, 0, len(fe.preds[b]))

	for _, p := range fe.preds[b] {
		ops = append(ops, fe.read(local, p.block))
	}

	fe.phiOps[phi] = ops

	return fe.tryRemoveTrivial(phi)
}

// tryRemoveTrivial replaces a parameter that receives a single value (or
// itself) on every edge by that value.
func (fe *frontend) tryRemoveTrivial(phi int32) int32 {
	same := int32(-1)

	for _, op := range fe.phiOps[phi] {
		arg := fe.resolve(op)

		if arg == same || arg == phi {
			continue
		}

		if same >= 0 {
			return phi
		}

		same = arg
	}

	if same < 0 {
		same = fe.f.newValue(OpUndef, fe.f.values[phi].typ, -1, 0)
	}

	fe.alias[fe.paramSlot(phi)] = same

	return same
}

// paramSlot maps a parameter value to its alias slot. Aliases are kept per
// value; values created after the parameter grow the table on demand.
func (fe *frontend) paramSlot(v int32) int32 {
	for int(v) >= len(fe.alias) {
		fe.alias = append(fe.alias, -1)
	}

	return v
}

func (fe *frontend) resolve(v int32) int32 {
	for int(v) < len(fe.alias) && fe.alias[v] >= 0 {
		v = fe.alias[v]
	}

	return v
}

func (fe *frontend) seal(b int32) {
	if fe.sealed[b] {
		return
	}

	fe.sealed[b] = true
	pending := fe.incomplete[b]
	fe.incomplete[b] = nil

	for _, p := range pending {
		fe.addOperands(p.local, p.phi)
	}
}

// edge ends the current block's terminator edge list with an edge to b and
// records it as one of b's predecessors.
func (fe *frontend) addEdge(from int32, to int32, args []int32) {
	t := &fe.f.blocks[from].term
	t.edges = append(t.edges, Edge{block: to, args: args})
	fe.preds[to] = append(fe.preds[to], predRef{from, int32(len(t.edges) - 1)})

	if fe.sealed[to] {
		throwFmt("internal error: edge to sealed block b%d", to)
	}
}

func (fe *frontend) jump(to int32, args []int32) {
	fe.f.blocks[fe.cur].term.kind = TermJump
	fe.addEdge(fe.cur, to, args)
}

// branchArgs are the values a branch to frame c passes: a loop's parameters
// or a block's results.
func (fe *frontend) labelTypes(c *control) []ValType {
	if c.kind == ctrlLoop {
		return c.params
	}

	return c.results
}

func (fe *frontend) label(depth uint32) *control {
	if int(depth) >= len(fe.ctrls) {
		throwFmt("unknown label %d", depth)
	}

	return &fe.ctrls[len(fe.ctrls)-1-int(depth)]
}

// markDead makes the rest of the current frame unreachable.
func (fe *frontend) markDead() {
	c := &fe.ctrls[len(fe.ctrls)-1]
	c.dead = true
	fe.stack = fe.stack[:c.height]
	fe.cur = -1
}

func (fe *frontend) blockType() ([]ValType, []ValType) {
	r := &fe.r

	if r.pos < len(r.data) {
		switch b := r.data[r.pos]; b {
		case 0x40:
			r.pos++

			return nil, nil
		case 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x70, 0x6f:
			r.pos++

			return nil, []ValType{ValType(b)}
		}
	}

	index := r.sleb(33)

	if index < 0 || int(index) >= len(fe.m.types) {
		throwFmt("unknown type %d", index)
	}

	t := &fe.m.types[index]

	return t.params, t.results
}

func (fe *frontend) memarg() (int64, int32) {
	r := &fe.r
	align := r.u32()
	mem := uint32(0)

	if align&0x40 != 0 {
		align &^= 0x40
		mem = r.u32()
	}

	if mem != 0 || len(fe.m.mems) == 0 {
		throwFmt("unknown memory %d", mem)
	}

	if align >= 32 {
		throwFmt("malformed memop flags")
	}

	if fe.m.mems[0].limits.is64 {
		return int64(r.leb(64)), int32(align)
	}

	return int64(r.u32()), int32(align)
}

func (fe *frontend) load(t Type, width int, signed bool) {
	offset, _ := fe.memarg()

	if fe.cur < 0 {
		return
	}

	addr := fe.pop()
	v := fe.emit(OpLoad, t, offset, addr)
	fe.f.values[v].aux2 = int32(makeMemKind(width, signed))
	fe.push(v)
}

func (fe *frontend) store(width int) {
	offset, _ := fe.memarg()

	if fe.cur < 0 {
		return
	}

	val := fe.pop()
	addr := fe.pop()
	v := fe.emit(OpStore, TNone, offset, addr, val)
	fe.f.values[v].aux2 = int32(makeMemKind(width, false))
}

func (fe *frontend) unary(op Op, t Type) {
	if fe.cur < 0 {
		return
	}

	a := fe.pop()
	fe.push(fe.emit(op, t, 0, a))
}

func (fe *frontend) binary(op Op, t Type) {
	if fe.cur < 0 {
		return
	}

	b := fe.pop()
	a := fe.pop()
	fe.push(fe.emit(op, t, 0, a, b))
}

func (fe *frontend) body() {
	r := &fe.r

	for len(fe.ctrls) > 0 {
		op := r.byte()

		if fe.cur < 0 {
			fe.skip(op)

			continue
		}

		switch op {
		case 0x00:
			fe.f.blocks[fe.cur].term = Term{kind: TermTrap, aux: trapUnreachable}
			fe.markDead()
		case 0x01:
		case 0x02, 0x03:
			params, results := fe.blockType()
			args := fe.popN(len(params))
			c := control{kind: ctrlBlock, params: params, results: results, height: len(fe.stack), elseBlock: -1}

			if op == 0x03 {
				c.kind = ctrlLoop
				header := fe.newBlock()
				c.target = header
				vals := make([]int32, len(params))

				for i, t := range params {
					vals[i] = fe.addExplicit(header, t)
				}

				fe.jump(header, args)
				fe.cur = header
				args = vals
			} else {
				c.target = fe.newBlock()

				for _, t := range results {
					fe.addExplicit(c.target, t)
				}
			}

			fe.ctrls = append(fe.ctrls, c)
			fe.stack = append(fe.stack, args...)
		case 0x04:
			params, results := fe.blockType()
			cond := fe.pop()
			args := fe.popN(len(params))
			c := control{kind: ctrlIf, params: params, results: results, height: len(fe.stack), elseArgs: args}
			c.target = fe.newBlock()

			for _, t := range results {
				fe.addExplicit(c.target, t)
			}

			then := fe.newBlock()
			c.elseBlock = fe.newBlock()
			blk := &fe.f.blocks[fe.cur]
			blk.term.kind = TermIf
			blk.term.value = cond
			fe.addEdge(fe.cur, then, nil)
			fe.addEdge(fe.cur, c.elseBlock, nil)
			fe.seal(then)
			fe.seal(c.elseBlock)
			fe.cur = then
			fe.ctrls = append(fe.ctrls, c)
			fe.stack = append(fe.stack, args...)
		case 0x05:
			fe.elseOp()
		case 0x0b:
			fe.end()
		case 0x0c:
			c := fe.label(r.u32())
			fe.jump(c.target, fe.top(len(fe.labelTypes(c))))
			fe.markDead()
		case 0x0d:
			c := fe.label(r.u32())
			cond := fe.pop()
			args := fe.top(len(fe.labelTypes(c)))
			next := fe.newBlock()
			blk := &fe.f.blocks[fe.cur]
			blk.term.kind = TermIf
			blk.term.value = cond
			fe.addEdge(fe.cur, c.target, args)
			fe.addEdge(fe.cur, next, nil)
			fe.seal(next)
			fe.cur = next
		case 0x0e:
			n := r.u32()

			if n > 1<<20 {
				throwFmt("br_table too large")
			}

			labels := make([]uint32, n+1)

			for i := range labels {
				labels[i] = r.u32()
			}

			index := fe.pop()
			arity := len(fe.labelTypes(fe.label(labels[n])))
			args := fe.top(arity)
			blk := &fe.f.blocks[fe.cur]
			blk.term.kind = TermSwitch
			blk.term.value = index

			for _, l := range labels {
				c := fe.label(l)

				if len(fe.labelTypes(c)) != arity {
					throwFmt("type mismatch: br_table arity")
				}

				fe.addEdge(fe.cur, c.target, append([]int32(nil), args...))
			}

			fe.markDead()
		case 0x0f:
			fe.f.blocks[fe.cur].term = Term{kind: TermReturn, values: fe.top(len(fe.f.typ.results))}
			fe.markDead()
		case 0x10:
			index := r.u32()

			if int(index) >= len(fe.m.funcs) {
				throwFmt("unknown function %d", index)
			}

			fe.call(OpCall, fe.m.funcType(index), int64(index), 0, nil)
		case 0x11:
			typeIndex := r.u32()
			table := r.u32()

			if int(typeIndex) >= len(fe.m.types) {
				throwFmt("unknown type %d", typeIndex)
			}

			if int(table) >= len(fe.m.tables) {
				throwFmt("unknown table %d", table)
			}

			elem := fe.pop()
			fe.call(OpCallIndirect, &fe.m.types[typeIndex], int64(typeIndex), int32(table), []int32{elem})
		case 0x1a:
			fe.pop()
		case 0x1b, 0x1c:
			if op == 0x1c {
				n := r.u32()

				for i := uint32(0); i < n; i++ {
					r.valType()
				}
			}

			cond := fe.pop()
			b := fe.pop()
			a := fe.pop()
			fe.push(fe.emit(OpSelect, fe.typeOf(a), 0, cond, a, b))
		case 0x20:
			local := fe.localIndex()
			fe.push(fe.read(local, fe.cur))
		case 0x21:
			local := fe.localIndex()
			fe.write(local, fe.cur, fe.pop())
		case 0x22:
			local := fe.localIndex()
			v := fe.pop()
			fe.write(local, fe.cur, v)
			fe.push(v)
		case 0x23:
			index := fe.globalIndex()
			fe.push(fe.emit(OpGlobalGet, fe.m.globals[index].typ, int64(index)))
		case 0x24:
			index := fe.globalIndex()
			fe.emit(OpGlobalSet, TNone, int64(index), fe.pop())
		case 0x25:
			table := fe.tableIndex()
			index := fe.pop()
			fe.push(fe.emit(OpTableGet, fe.m.tables[table].elem, int64(table), index))
		case 0x26:
			table := fe.tableIndex()
			val := fe.pop()
			index := fe.pop()
			fe.emit(OpTableSet, TNone, int64(table), index, val)
		case 0x28:
			fe.load(I32, 4, false)
		case 0x29:
			fe.load(I64, 8, false)
		case 0x2a:
			fe.load(F32, 4, false)
		case 0x2b:
			fe.load(F64, 8, false)
		case 0x2c:
			fe.load(I32, 1, true)
		case 0x2d:
			fe.load(I32, 1, false)
		case 0x2e:
			fe.load(I32, 2, true)
		case 0x2f:
			fe.load(I32, 2, false)
		case 0x30:
			fe.load(I64, 1, true)
		case 0x31:
			fe.load(I64, 1, false)
		case 0x32:
			fe.load(I64, 2, true)
		case 0x33:
			fe.load(I64, 2, false)
		case 0x34:
			fe.load(I64, 4, true)
		case 0x35:
			fe.load(I64, 4, false)
		case 0x36, 0x38:
			fe.store(4)
		case 0x37, 0x39:
			fe.store(8)
		case 0x3a, 0x3c:
			fe.store(1)
		case 0x3b, 0x3d:
			fe.store(2)
		case 0x3e:
			fe.store(4)
		case 0x3f:
			fe.memIndex()
			fe.push(fe.emit(OpMemSize, fe.addrType(), 0))
		case 0x40:
			fe.memIndex()
			delta := fe.pop()
			fe.push(fe.emit(OpMemGrow, fe.addrType(), 0, delta))
		case 0x41:
			fe.push(fe.constant(I32, int64(r.s32())))
		case 0x42:
			fe.push(fe.constant(I64, r.s64()))
		case 0x43:
			fe.push(fe.constant(F32, int64(r.f32())))
		case 0x44:
			fe.push(fe.constant(F64, int64(r.f64())))
		case 0x45:
			fe.unary(OpEqz, I32)
		case 0x50:
			fe.unary(OpEqz, I32)
		case 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f:
			fe.binary(OpEq+Op(op-0x46), I32)
		case 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a:
			fe.binary(OpEq+Op(op-0x51), I32)
		case 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60:
			fe.binary(OpFEq+Op(op-0x5b), I32)
		case 0x61, 0x62, 0x63, 0x64, 0x65, 0x66:
			fe.binary(OpFEq+Op(op-0x61), I32)
		case 0x67, 0x68, 0x69:
			fe.unary(OpClz+Op(op-0x67), I32)
		case 0x79, 0x7a, 0x7b:
			fe.unary(OpClz+Op(op-0x79), I64)
		case 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78:
			fe.binary(OpAdd+Op(op-0x6a), I32)
		case 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a:
			fe.binary(OpAdd+Op(op-0x7c), I64)
		case 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91:
			fe.unary(floatUnary[op-0x8b], F32)
		case 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f:
			fe.unary(floatUnary[op-0x99], F64)
		case 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98:
			fe.binary(OpFAdd+Op(op-0x92), F32)
		case 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6:
			fe.binary(OpFAdd+Op(op-0xa0), F64)
		case 0xa7:
			fe.unary(OpWrap, I32)
		case 0xa8, 0xaa:
			fe.unary(OpTruncS, I32)
		case 0xa9, 0xab:
			fe.unary(OpTruncU, I32)
		case 0xac:
			fe.unary(OpExtendS, I64)
		case 0xad:
			fe.unary(OpExtendU, I64)
		case 0xae, 0xb0:
			fe.unary(OpTruncS, I64)
		case 0xaf, 0xb1:
			fe.unary(OpTruncU, I64)
		case 0xb2, 0xb4:
			fe.unary(OpConvertS, F32)
		case 0xb3, 0xb5:
			fe.unary(OpConvertU, F32)
		case 0xb6:
			fe.unary(OpDemote, F32)
		case 0xb7, 0xb9:
			fe.unary(OpConvertS, F64)
		case 0xb8, 0xba:
			fe.unary(OpConvertU, F64)
		case 0xbb:
			fe.unary(OpPromote, F64)
		case 0xbc:
			fe.unary(OpReinterpret, I32)
		case 0xbd:
			fe.unary(OpReinterpret, I64)
		case 0xbe:
			fe.unary(OpReinterpret, F32)
		case 0xbf:
			fe.unary(OpReinterpret, F64)
		case 0xc0:
			fe.unary(OpExtend8S, I32)
		case 0xc1:
			fe.unary(OpExtend16S, I32)
		case 0xc2:
			fe.unary(OpExtend8S, I64)
		case 0xc3:
			fe.unary(OpExtend16S, I64)
		case 0xc4:
			fe.unary(OpExtend32S, I64)
		case 0xd0:
			t := r.refType()
			fe.push(fe.f.newValue(OpRefNull, t, -1, 0))
		case 0xd1:
			fe.unary(OpRefIsNull, I32)
		case 0xd2:
			index := r.u32()

			if int(index) >= len(fe.m.funcs) {
				throwFmt("unknown function %d", index)
			}

			fe.push(fe.emit(OpRefFunc, FuncRef, int64(index)))
		case 0xfc:
			fe.prefixFC()
		default:
			throwFmt("illegal opcode 0x%02x at offset %d", op, r.pos-1)
		}
	}

	if !r.eof() {
		throwFmt("operators remaining after end of function")
	}
}

var floatUnary = [7]Op{OpFAbs, OpFNeg, OpFCeil, OpFFloor, OpFTrunc, OpFNearest, OpFSqrt}

func (fe *frontend) addrType() Type {
	if fe.m.mems[0].limits.is64 {
		return I64
	}

	return I32
}

func (fe *frontend) memIndex() {
	if mem := fe.r.u32(); mem != 0 || len(fe.m.mems) == 0 {
		throwFmt("unknown memory %d", mem)
	}
}

func (fe *frontend) localIndex() int32 {
	index := fe.r.u32()

	if int(index) >= len(fe.locals) {
		throwFmt("unknown local %d", index)
	}

	return int32(index)
}

func (fe *frontend) globalIndex() uint32 {
	index := fe.r.u32()

	if int(index) >= len(fe.m.globals) {
		throwFmt("unknown global %d", index)
	}

	return index
}

func (fe *frontend) tableIndex() uint32 {
	index := fe.r.u32()

	if int(index) >= len(fe.m.tables) {
		throwFmt("unknown table %d", index)
	}

	return index
}

func (fe *frontend) call(op Op, t *FuncType, aux int64, aux2 int32, extra []int32) {
	args := fe.popN(len(t.params))
	args = append(args, extra...)
	typ := TNone

	switch len(t.results) {
	case 0:
	case 1:
		typ = t.results[0]
	default:
		typ = TTuple
	}

	v := fe.emit(op, typ, aux, args...)
	fe.f.values[v].aux2 = aux2

	if len(t.results) == 1 {
		fe.push(v)
	} else {
		for i, rt := range t.results {
			fe.push(fe.emit(OpExtract, rt, int64(i), v))
		}
	}
}

func (fe *frontend) prefixFC() {
	r := &fe.r
	sub := r.u32()

	switch sub {
	case 0, 1, 2, 3:
		op := OpTruncSatS

		if sub&1 != 0 {
			op = OpTruncSatU
		}

		fe.unary(op, I32)
	case 4, 5, 6, 7:
		op := OpTruncSatS

		if sub&1 != 0 {
			op = OpTruncSatU
		}

		fe.unary(op, I64)
	case 8:
		seg := r.u32()
		fe.memIndex()
		fe.checkData(seg)
		n := fe.pop()
		s := fe.pop()
		d := fe.pop()
		fe.emit(OpMemInit, TNone, int64(seg), d, s, n)
	case 9:
		seg := r.u32()
		fe.checkData(seg)
		fe.emit(OpDataDrop, TNone, int64(seg))
	case 10:
		fe.memIndex()
		fe.memIndex()
		n := fe.pop()
		s := fe.pop()
		d := fe.pop()
		fe.emit(OpMemCopy, TNone, 0, d, s, n)
	case 11:
		fe.memIndex()
		n := fe.pop()
		val := fe.pop()
		d := fe.pop()
		fe.emit(OpMemFill, TNone, 0, d, val, n)
	case 12:
		seg := r.u32()
		table := fe.tableIndex()

		if int(seg) >= len(fe.m.elems) {
			throwFmt("unknown elem segment %d", seg)
		}

		n := fe.pop()
		s := fe.pop()
		d := fe.pop()
		v := fe.emit(OpTableInit, TNone, int64(seg), d, s, n)
		fe.f.values[v].aux2 = int32(table)
	case 13:
		seg := r.u32()

		if int(seg) >= len(fe.m.elems) {
			throwFmt("unknown elem segment %d", seg)
		}

		fe.emit(OpElemDrop, TNone, int64(seg))
	case 14:
		dst := fe.tableIndex()
		src := fe.tableIndex()
		n := fe.pop()
		s := fe.pop()
		d := fe.pop()
		v := fe.emit(OpTableCopy, TNone, int64(dst), d, s, n)
		fe.f.values[v].aux2 = int32(src)
	case 15:
		table := fe.tableIndex()
		n := fe.pop()
		val := fe.pop()
		fe.push(fe.emit(OpTableGrow, I32, int64(table), val, n))
	case 16:
		table := fe.tableIndex()
		fe.push(fe.emit(OpTableSize, I32, int64(table)))
	case 17:
		table := fe.tableIndex()
		n := fe.pop()
		val := fe.pop()
		d := fe.pop()
		fe.emit(OpTableFill, TNone, int64(table), d, val, n)
	default:
		throwFmt("illegal opcode 0xfc %d", sub)
	}
}

func (fe *frontend) checkData(seg uint32) {
	if fe.m.dataCount < 0 {
		throwFmt("data count section required")
	}

	if int64(seg) >= fe.m.dataCount {
		throwFmt("unknown data segment %d", seg)
	}
}

func (fe *frontend) elseOp() {
	c := &fe.ctrls[len(fe.ctrls)-1]

	if c.kind != ctrlIf {
		throwFmt("else without if")
	}

	if c.outer {
		c.kind = ctrlElse

		return
	}

	if fe.cur >= 0 {
		fe.jump(c.target, fe.popN(len(c.results)))
	}

	fe.stack = append(fe.stack[:c.height], c.elseArgs...)
	fe.cur = c.elseBlock
	c.elseBlock = -1
	c.kind = ctrlElse
	c.dead = false
}

func (fe *frontend) end() {
	c := &fe.ctrls[len(fe.ctrls)-1]

	if c.outer {
		fe.ctrls = fe.ctrls[:len(fe.ctrls)-1]

		return
	}

	live := fe.cur >= 0
	var results []int32

	if live {
		results = fe.popN(len(c.results))

		if len(fe.stack) != c.height {
			throwFmt("type mismatch: values remaining on stack at end of block")
		}
	}

	frame := *c
	fe.ctrls = fe.ctrls[:len(fe.ctrls)-1]
	fe.stack = fe.stack[:frame.height]

	if frame.kind == ctrlLoop {
		fe.seal(frame.target)

		if live {
			fe.stack = append(fe.stack, results...)
		} else {
			fe.deadAfter()
		}

		return
	}

	if frame.kind == ctrlIf {
		// The missing false arm passes the parameters through.
		cur := fe.cur
		fe.cur = frame.elseBlock
		fe.jump(frame.target, frame.elseArgs)
		fe.cur = cur
	}

	if live && len(fe.preds[frame.target]) == 0 {
		// Nothing else reaches the merge block: keep appending to the
		// current one.
		fe.f.blocks[frame.target].dead = true
		fe.seal(frame.target)

		if frame.kind == ctrlFunc {
			fe.f.blocks[fe.cur].term = Term{kind: TermReturn, values: results}
			fe.cur = -1

			return
		}

		fe.stack = append(fe.stack, results...)

		return
	}

	if live {
		fe.jump(frame.target, results)
	}

	fe.seal(frame.target)

	if len(fe.preds[frame.target]) == 0 {
		fe.f.blocks[frame.target].dead = true
		fe.deadAfter()

		return
	}

	fe.cur = frame.target
	params := append([]int32(nil), fe.f.blocks[frame.target].params[:len(frame.results)]...)

	if frame.kind == ctrlFunc {
		fe.f.blocks[fe.cur].term = Term{kind: TermReturn, values: params}
		fe.cur = -1

		return
	}

	fe.stack = append(fe.stack, params...)
}

// deadAfter makes the code following a construct unreachable: nothing
// falls out of it.
func (fe *frontend) deadAfter() {
	fe.cur = -1

	if len(fe.ctrls) > 0 {
		c := &fe.ctrls[len(fe.ctrls)-1]
		c.dead = true
		fe.stack = fe.stack[:c.height]
	}
}

// skip consumes an instruction in unreachable code, tracking only the
// structure needed to find where reachable code resumes.
func (fe *frontend) skip(op byte) {
	r := &fe.r

	switch op {
	case 0x02, 0x03, 0x04:
		fe.blockType()
		fe.ctrls = append(fe.ctrls, control{kind: ctrlBlock, outer: true, height: len(fe.stack), elseBlock: -1})

		if op == 0x04 {
			fe.ctrls[len(fe.ctrls)-1].kind = ctrlIf
		}
	case 0x05:
		fe.elseOp()
	case 0x0b:
		fe.end()
	case 0x0c, 0x0d:
		r.u32()
	case 0x0e:
		n := r.u32()

		for i := uint32(0); i <= n; i++ {
			r.u32()
		}
	case 0x10, 0x12, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0xd2:
		r.u32()
	case 0x11:
		r.u32()
		r.u32()
	case 0x1c:
		n := r.u32()

		for i := uint32(0); i < n; i++ {
			r.valType()
		}
	case 0x3f, 0x40:
		r.u32()
	case 0x41:
		r.s32()
	case 0x42:
		r.s64()
	case 0x43:
		r.bytes(4)
	case 0x44:
		r.bytes(8)
	case 0xd0:
		r.refType()
	case 0xfc:
		switch sub := r.u32(); sub {
		case 8, 12, 14:
			r.u32()
			r.u32()
		case 10:
			r.u32()
			r.u32()
		case 9, 11, 13, 15, 16, 17:
			r.u32()
		default:
			if sub > 7 {
				throwFmt("illegal opcode 0xfc %d", sub)
			}
		}
	default:
		if op >= 0x28 && op <= 0x3e {
			fe.memarg()
		} else if op > 0xc4 && op != 0xd1 {
			throwFmt("illegal opcode 0x%02x", op)
		}
	}
}

// resolveAliases rewrites every use of a removed phi to its replacement,
// keeps the remaining phis after the explicit parameters and appends their
// operands to the incoming edges.
func (fe *frontend) resolveAliases() {
	f := fe.f

	for b := range f.blocks {
		blk := &f.blocks[b]

		if blk.dead {
			continue
		}

		params := blk.params[:fe.explicit[b]]

		for _, p := range blk.params[fe.explicit[b]:] {
			if fe.resolve(p) != p {
				continue
			}

			f.values[p].aux = int64(len(params))
			params = append(params, p)

			for i, pr := range fe.preds[b] {
				e := &f.blocks[pr.block].term.edges[pr.edge]
				e.args = append(e.args, fe.phiOps[p][i])
			}
		}

		blk.params = params
	}

	for v := range f.values {
		val := &f.values[v]

		for i := int32(0); i < int32(val.nargs); i++ {
			f.pool[val.args+i] = fe.resolve(f.pool[val.args+i])
		}
	}

	for b := range f.blocks {
		t := &f.blocks[b].term

		if t.kind == TermIf || t.kind == TermSwitch {
			t.value = fe.resolve(t.value)
		}

		for i := range t.values {
			t.values[i] = fe.resolve(t.values[i])
		}

		for i := range t.edges {
			for j := range t.edges[i].args {
				t.edges[i].args[j] = fe.resolve(t.edges[i].args[j])
			}
		}
	}

	f.computePreds()
}
