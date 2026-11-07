package main

import "math"

// Lowering turns SSA into machine instructions over virtual registers in
// two-address form. Constants are rematerialized at each use or folded into
// immediates; a comparison used only by a branch or select of its own block
// is emitted at that use so the flags feed it directly.

// call targets
const (
	calleeFunc = 0 // a function by index: an imported one is its adapter
	calleeStub = 1 // a stub of the module (stubs.go) by kind
)

// float constants in the module constant pool
const (
	constF32Sign = iota
	constF32Abs
	constF64Sign
	constF64Abs
	constCount
)

type lowerer struct {
	m        *Module
	f        *Function
	lay      *Layout
	ci       *cfgInfo
	mf       *MFunc
	vregs    []VReg
	uses     []int32
	deferred []bool
	mblk     []int32
	cur      int32
	// split maps an edge (block, edge index) to the block placed on it.
	split map[[2]int32]int32
}

func lowerFunction(m *Module, lay *Layout, f *Function) *MFunc {
	l := &lowerer{m: m, f: f, lay: lay, mf: &MFunc{}, split: map[[2]int32]int32{}}
	l.ci = f.analyze()
	l.vregs = make([]VReg, len(f.values))

	for i := range l.vregs {
		l.vregs[i] = noReg
	}

	l.countUses()
	l.markDeferred()
	l.mblk = make([]int32, len(f.blocks))

	for i := range l.mblk {
		l.mblk[i] = -1
	}

	for _, b := range l.ci.order {
		l.mblk[b] = int32(len(l.mf.blocks))
		l.mf.blocks = append(l.mf.blocks, MBlock{id: b, depth: int(l.ci.depth[b])})
	}

	// Parameters of every block are defined by their incoming edges.
	for _, b := range l.ci.order {
		for _, p := range f.blocks[b].params {
			l.def(p)
		}
	}

	for _, b := range l.ci.order {
		l.cur = l.mblk[b]

		if b == l.ci.order[0] {
			l.entry()
		}

		for _, v := range f.blocks[b].instrs {
			if !l.deferred[v] {
				l.lowerValue(v)
			}
		}

		l.terminator(b)
	}

	l.linkBlocks()

	return l.mf
}

func (l *lowerer) countUses() {
	f := l.f
	l.uses = make([]int32, len(f.values))

	for _, b := range l.ci.order {
		blk := &f.blocks[b]

		for _, v := range blk.instrs {
			for _, a := range f.args(v) {
				l.uses[a]++
			}
		}

		t := &blk.term

		if t.kind == TermIf || t.kind == TermSwitch {
			l.uses[t.value]++
		}

		for _, v := range t.values {
			l.uses[v]++
		}

		for _, e := range t.edges {
			for _, a := range e.args {
				l.uses[a]++
			}
		}
	}
}

func isCompare(op Op) bool {
	return (op >= OpEqz && op <= OpGeU) || (op >= OpFEq && op <= OpFGe)
}

// markDeferred finds comparisons whose only use is the branch or a select of
// their own block.
func (l *lowerer) markDeferred() {
	f := l.f
	l.deferred = make([]bool, len(f.values))

	for _, b := range l.ci.order {
		blk := &f.blocks[b]

		if blk.term.kind == TermIf {
			c := blk.term.value

			if l.uses[c] == 1 && f.values[c].block == b && isCompare(f.values[c].op) {
				l.deferred[c] = true
			}
		}

		for _, v := range blk.instrs {
			if f.values[v].op != OpSelect {
				continue
			}

			c := f.args(v)[0]
			op := f.values[c].op

			// A select takes one condition code: float equality needs the
			// parity flag besides, so it stays a value.
			if l.uses[c] == 1 && f.values[c].block == b && isCompare(op) && op != OpFEq && op != OpFNe && !isFloat(f.values[v].typ) {
				l.deferred[c] = true
			}
		}
	}
}

func (l *lowerer) emit(in MInst) *MInst {
	blk := &l.mf.blocks[l.cur]
	blk.insts = append(blk.insts, in)

	return &blk.insts[len(blk.insts)-1]
}

func inst(op MOp) MInst {
	return MInst{op: op, dst: noReg, src: noReg, src2: noReg, mem: Mem{base: noReg, index: noReg, label: -1}, target: -1}
}

func (l *lowerer) classOf(t Type) uint8 {
	if isFloat(t) || t == V128 {
		return classXMM
	}

	return classGPR
}

func (l *lowerer) def(v int32) VReg {
	if l.vregs[v] == noReg {
		l.vregs[v] = l.mf.newReg(l.classOf(l.f.values[v].typ))
	}

	return l.vregs[v]
}

func (l *lowerer) isConst(v int32) bool {
	op := l.f.values[v].op

	return op == OpConst || op == OpUndef || op == OpRefNull
}

func (l *lowerer) constBits(v int32) int64 {
	if l.f.values[v].op == OpConst {
		return l.f.values[v].aux
	}

	return 0
}

// imm reports whether v is a constant usable as an immediate of an
// instruction of the given width.
func (l *lowerer) imm(v int32, size uint8) (int64, bool) {
	if !l.isConst(v) || isFloat(l.f.values[v].typ) {
		return 0, false
	}

	x := l.constBits(v)

	if size == 4 {
		return int64(int32(x)), true
	}

	if x == int64(int32(x)) {
		return x, true
	}

	return 0, false
}

// use returns a register holding v, materializing a constant.
func (l *lowerer) use(v int32) VReg {
	val := &l.f.values[v]

	if l.isConst(v) {
		bits := l.constBits(v)

		if isFloat(val.typ) {
			r := l.mf.newReg(classXMM)
			in := inst(mFConst)
			in.dst = r
			in.imm = bits
			in.size = typeSize(val.typ)
			l.emit(in)

			return r
		}

		r := l.mf.newReg(classGPR)
		in := inst(mMovImm)
		in.dst = r
		in.imm = bits
		in.size = typeSize(val.typ)

		if val.typ == I32 {
			in.imm = int64(uint32(bits))
		}

		l.emit(in)

		return r
	}

	if l.deferred[v] {
		// A deferred comparison used as a value after all.
		l.deferred[v] = false
		l.lowerValue(v)
	}

	if l.vregs[v] == noReg {
		throwFmt("internal error: v%d used before definition in function %d", v, l.f.index)
	}

	return l.vregs[v]
}

func typeSize(t Type) uint8 {
	switch t {
	case I32, F32:
		return 4
	case V128:
		return 16
	}

	return 8
}

func (l *lowerer) mov(dst, src VReg, size uint8) {
	in := inst(mMov)

	if dst.isXMM() || (!dst.isPhys() && l.mf.regClass(dst) == classXMM) {
		in.op = mFMov
	}

	in.dst = dst
	in.src = src
	in.size = size
	l.emit(in)
}

func (l *lowerer) trapIf(cc uint8, code int64) {
	in := inst(mTrapIf)
	in.cc = cc
	in.imm = code
	l.emit(in)
}

func (l *lowerer) entry() {
	f := l.f
	params := f.blocks[l.ci.order[0]].params
	gi, xi := 0, 0
	stack := 0
	pm := inst(mParMove)

	for _, p := range params {
		t := f.values[p].typ
		r := l.vregs[p]

		if isFloat(t) {
			if xi < len(argXMMs) {
				pm.moves = append(pm.moves, Move{r, argXMMs[xi]})
				xi++

				continue
			}
		} else if gi < len(argGPRs) {
			pm.moves = append(pm.moves, Move{r, argGPRs[gi]})
			gi++

			continue
		}

		in := inst(mLoadArg)
		in.dst = r
		in.imm = int64(stack)
		in.size = typeSize(t)
		l.emit(in)
		stack++
	}

	if len(pm.moves) > 0 {
		l.emit(pm)
	}

	l.mf.incoming = stack
}

func (l *lowerer) lowerValue(v int32) {
	f := l.f
	val := &f.values[v]
	args := f.args(v)
	size := typeSize(val.typ)

	switch val.op {
	case OpAdd, OpSub, OpAnd, OpOr, OpXor, OpMul:
		l.alu(v, val.op, args[0], args[1], size)
	case OpShl, OpShrS, OpShrU:
		l.shift(v, val.op, args[0], args[1], size)
	case OpRotl, OpRotr:
		l.rotate(v, val.op, args[0], args[1], size)
	case OpDivS, OpDivU, OpRemS, OpRemU:
		l.divide(v, val.op, args[0], args[1], size)
	case OpClz, OpCtz, OpPopcnt:
		in := inst(mBitCount)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.size = size
		in.aux = uint8(val.op - OpClz)
		l.emit(in)
	case OpExtend8S, OpExtend16S, OpExtend32S:
		in := inst(mExt)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.size = [3]uint8{1, 2, 4}[val.op-OpExtend8S]
		in.dsize = size
		in.aux = 1
		l.emit(in)
	case OpEqz, OpEq, OpNe, OpLtS, OpLtU, OpGtS, OpGtU, OpLeS, OpLeU, OpGeS, OpGeU,
		OpFEq, OpFNe, OpFLt, OpFGt, OpFLe, OpFGe:
		l.setcc(v)
	case OpFAdd, OpFSub, OpFMul, OpFDiv:
		in := inst(mFAlu)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.src2 = l.use(args[1])
		in.size = size
		in.aux = uint8(val.op - OpFAdd)
		l.emit(in)
	case OpFMin, OpFMax:
		in := inst(mFMinMax)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.src2 = l.use(args[1])
		in.size = size

		if val.op == OpFMax {
			in.aux = 1
		}

		l.emit(in)
	case OpFCopysign:
		l.copysign(v, args[0], args[1], size)
	case OpFAbs, OpFNeg:
		in := inst(mFAluMem)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.size = size
		in.aux = uint8(fAnd)
		in.mem = Mem{base: noReg, index: noReg, label: constF32Abs}

		if val.op == OpFNeg {
			in.aux = uint8(fXor)
			in.mem.label = constF32Sign
		}

		if size == 8 {
			in.mem.label += 2
		}

		l.emit(in)
	case OpFSqrt, OpFCeil, OpFFloor, OpFTrunc, OpFNearest:
		in := inst(mFUnary)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.size = size

		if val.op != OpFSqrt {
			in.aux = fRound
			in.imm = map[Op]int64{OpFNearest: 0, OpFFloor: 1, OpFCeil: 2, OpFTrunc: 3}[val.op]
		}

		l.emit(in)
	case OpWrap:
		in := inst(mMov)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.size = 4
		in.aux = 1
		l.emit(in)
	case OpExtendU:
		// i32 values are kept zero-extended
		l.mov(l.def(v), l.use(args[0]), 8)
	case OpExtendS:
		in := inst(mExt)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.size = 4
		in.dsize = 8
		in.aux = 1
		l.emit(in)
	case OpTruncS, OpTruncU, OpTruncSatS, OpTruncSatU:
		in := inst(mTruncF)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.size = size
		in.dsize = typeSize(f.values[args[0]].typ)

		if val.op == OpTruncU || val.op == OpTruncSatU {
			in.aux |= truncUnsigned
		}

		if val.op == OpTruncSatS || val.op == OpTruncSatU {
			in.aux |= truncSat
		}

		in.temps = []VReg{l.mf.newReg(classXMM), l.mf.newReg(classGPR)}
		l.emit(in)
	case OpConvertS, OpConvertU:
		l.convert(v, val.op, args[0], size)
	case OpDemote, OpPromote:
		in := inst(mCvtFF)
		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.size = typeSize(f.values[args[0]].typ)
		in.dsize = size
		l.emit(in)
	case OpReinterpret:
		in := inst(mMovGX)

		if !isFloat(val.typ) {
			in.op = mMovXG
		}

		in.dst = l.def(v)
		in.src = l.use(args[0])
		in.size = size
		l.emit(in)
	case OpSelect:
		l.selectValue(v, args)
	case OpLoad:
		l.load(v, args[0])
	case OpStore:
		l.store(v, args[0], args[1])
	case OpMemSize:
		in := inst(mLoad)
		in.dst = l.def(v)
		in.mem = memAt(ctxReg, offMemSize)
		in.size = 8
		in.dsize = 8
		l.emit(in)
		sh := inst(mShiftImm)
		sh.dst = in.dst
		sh.imm = 16
		sh.size = 8
		sh.aux = shiftShr
		l.emit(sh)
	case OpMemGrow:
		l.stubCall(stubMemoryGrow, []VReg{l.use(args[0])}, []Type{I32}, v)
	case OpMemCopy:
		l.memCopy(args)
	case OpMemFill:
		l.memFill(args)
	case OpMemInit:
		l.memInit(int(val.aux), args)
	case OpDataDrop:
		in := inst(mStoreImm)
		in.mem = memAt(ctxReg, l.lay.dataDropped(int(val.aux)))
		in.size = 1
		in.imm = 1
		l.emit(in)
	case OpGlobalGet:
		l.globalGet(v, int(val.aux))
	case OpGlobalSet:
		l.globalSet(int(val.aux), args[0])
	case OpCall:
		l.call(v, int(val.aux), args)
	case OpCallIndirect:
		l.callIndirect(v, int(val.aux), int(val.aux2), args)
	case OpExtract:
		// defined by the call
	case OpRefFunc:
		in := inst(mLea)
		in.dst = l.def(v)
		in.mem = memAt(ctxReg, l.lay.desc(int(val.aux)))
		in.size = 8
		l.emit(in)
	case OpRefIsNull:
		in := inst(mTest)
		a := l.use(args[0])
		in.src = a
		in.src2 = a
		in.size = 8
		l.emit(in)
		set := inst(mSetcc)
		set.dst = l.def(v)
		set.cc = ccE
		l.emit(set)
	case OpTableGet:
		l.tableGet(v, int(val.aux), args[0])
	case OpTableSet:
		l.tableSet(int(val.aux), args[0], args[1])
	case OpTableSize:
		in := inst(mLoad)
		in.dst = l.def(v)
		in.mem = memAt(ctxReg, l.lay.tables[val.aux]+tableSize)
		in.size = 4
		in.dsize = 4
		l.emit(in)
	case OpTableGrow:
		table := l.constReg(val.aux, 4)
		l.stubCall(stubTableGrow, []VReg{table, l.use(args[1]), l.use(args[0])}, []Type{I32, I32, FuncRef}, v)
	default:
		throwFmt("unsupported operator %s in function %d", val.op, l.f.index)
	}
}

var aluOps = map[Op]aluOp{OpAdd: aluAdd, OpSub: aluSub, OpAnd: aluAnd, OpOr: aluOr, OpXor: aluXor, OpMul: aluImul}

func (l *lowerer) alu(v int32, op Op, a, b int32, size uint8) {
	if op != OpSub && l.isConst(a) && !l.isConst(b) {
		a, b = b, a
	}

	d := l.def(v)

	if op == OpAnd && size == 8 && l.isConst(b) && uint64(l.constBits(b)) == 0xffffffff {
		in := inst(mMov)
		in.dst = d
		in.src = l.use(a)
		in.size = 4
		in.aux = 1
		l.emit(in)

		return
	}

	if x, ok := l.imm(b, size); ok {
		switch op {
		case OpAdd, OpSub:
			if op == OpSub {
				x = -x
			}

			if x == int64(int32(x)) {
				in := inst(mLea)
				in.dst = d
				in.mem = memAt(l.use(a), int32(x))
				in.size = size
				l.emit(in)

				return
			}
		case OpMul:
			in := inst(mImul3)
			in.dst = d
			in.src = l.use(a)
			in.imm = x
			in.size = size
			l.emit(in)

			return
		}

		if op != OpAdd && op != OpSub {
			l.mov(d, l.use(a), size)
			in := inst(mAluImm)
			in.dst = d
			in.imm = x
			in.size = size
			in.aux = uint8(aluOps[op])
			l.emit(in)

			return
		}
	}

	ra := l.use(a)
	rb := l.use(b)
	l.mov(d, ra, size)
	in := inst(mAlu)
	in.dst = d
	in.src = rb
	in.size = size
	in.aux = uint8(aluOps[op])
	l.emit(in)
}

func (l *lowerer) shift(v int32, op Op, a, b int32, size uint8) {
	d := l.def(v)
	kind := map[Op]uint8{OpShl: shiftShl, OpShrS: shiftSar, OpShrU: shiftShr}[op]

	if l.isConst(b) {
		n := l.constBits(b) & int64(size*8-1)
		l.mov(d, l.use(a), size)

		if n != 0 {
			in := inst(mShiftImm)
			in.dst = d
			in.imm = n
			in.size = size
			in.aux = kind
			l.emit(in)
		}

		return
	}

	in := inst(mShiftX)
	in.dst = d
	in.src = l.use(a)
	in.src2 = l.use(b)
	in.size = size
	in.aux = kind
	l.emit(in)
}

func (l *lowerer) rotate(v int32, op Op, a, b int32, size uint8) {
	d := l.def(v)
	kind := uint8(shiftRol)

	if op == OpRotr {
		kind = shiftRor
	}

	if l.isConst(b) {
		n := l.constBits(b) & int64(size*8-1)
		l.mov(d, l.use(a), size)

		if n != 0 {
			in := inst(mShiftImm)
			in.dst = d
			in.imm = n
			in.size = size
			in.aux = kind
			l.emit(in)
		}

		return
	}

	ra := l.use(a)
	rb := l.use(b)
	l.mov(d, ra, size)
	l.mov(RCX, rb, size)
	in := inst(mRotCL)
	in.dst = d
	in.src = RCX
	in.size = size
	in.aux = kind
	l.emit(in)
}

func (l *lowerer) divide(v int32, op Op, a, b int32, size uint8) {
	ra := l.use(a)
	rb := l.use(b)
	t := inst(mTest)
	t.src = rb
	t.src2 = rb
	t.size = size
	l.emit(t)
	l.trapIf(ccE, trapDivByZero)
	signed := op == OpDivS || op == OpRemS

	if op == OpDivS {
		in := inst(mDivOvf)
		in.src = ra
		in.src2 = rb
		in.size = size
		l.emit(in)
	}

	if op == OpRemS {
		// x % -1 is 0, and idiv faults on the minimum value: divide by 1.
		one := l.mf.newReg(classGPR)
		mi := inst(mMovImm)
		mi.dst = one
		mi.imm = 1
		mi.size = 4
		l.emit(mi)
		divisor := l.mf.newReg(classGPR)
		l.mov(divisor, rb, size)
		c := inst(mCmpImm)
		c.src = rb
		c.imm = -1
		c.size = size
		l.emit(c)
		cm := inst(mCmov)
		cm.dst = divisor
		cm.src = one
		cm.cc = ccE
		cm.size = size
		l.emit(cm)
		rb = divisor
	}

	l.mov(RAX, ra, size)

	if signed {
		in := inst(mSignExtAx)
		in.size = size
		in.uses = []VReg{RAX}
		in.defs = []VReg{RDX}
		l.emit(in)
	} else {
		in := inst(mZeroDx)
		in.defs = []VReg{RDX}
		l.emit(in)
	}

	in := inst(mDiv)
	in.src = rb
	in.size = size
	in.uses = []VReg{RAX, RDX}
	in.defs = []VReg{RAX, RDX}

	if signed {
		in.aux = 1
	}

	l.emit(in)

	if op == OpDivS || op == OpDivU {
		l.mov(l.def(v), RAX, size)
	} else {
		l.mov(l.def(v), RDX, size)
	}
}

// compare emits the flags for comparison c and returns the condition that
// holds when it is true. A float equality needs the parity flag as well:
// parity is then reported through the second result, 1 when an unordered
// result must make the comparison false (eq) and 2 when it must make it
// true (ne).
func (l *lowerer) compare(c int32) (uint8, int) {
	f := l.f
	val := &f.values[c]
	args := f.args(c)

	if val.op == OpEqz {
		a := l.use(args[0])
		in := inst(mTest)
		in.src = a
		in.src2 = a
		in.size = typeSize(f.values[args[0]].typ)
		l.emit(in)

		return ccE, 0
	}

	a, b := args[0], args[1]
	size := typeSize(f.values[a].typ)

	if val.op >= OpFEq {
		op := val.op

		// a < b is b > a, a <= b is b >= a: "above" conditions are false
		// for unordered operands.
		if op == OpFLt || op == OpFLe {
			a, b = b, a
		}

		in := inst(mFCmp)
		in.src = l.use(a)
		in.src2 = l.use(b)
		in.size = size
		l.emit(in)

		switch op {
		case OpFEq:
			return ccE, 1
		case OpFNe:
			return ccNE, 2
		case OpFLt, OpFGt:
			return ccA, 0
		}

		return ccAE, 0
	}

	cc := intCC[val.op-OpEq]

	if l.isConst(a) && !l.isConst(b) {
		a, b = b, a
		cc = swapCC(cc)
	}

	if x, ok := l.imm(b, size); ok {
		in := inst(mCmpImm)
		in.src = l.use(a)
		in.imm = x
		in.size = size

		if x == 0 && (cc == ccE || cc == ccNE) {
			in.op = mTest
			in.src2 = in.src
		}

		l.emit(in)

		return cc, 0
	}

	in := inst(mCmp)
	in.src = l.use(a)
	in.src2 = l.use(b)
	in.size = size
	l.emit(in)

	return cc, 0
}

// condition codes of OpEq..OpGeU
var intCC = [10]uint8{ccE, ccNE, ccL, ccB, ccG, ccA, ccLE, ccBE, ccGE, ccAE}

func swapCC(cc uint8) uint8 {
	switch cc {
	case ccL:
		return ccG
	case ccG:
		return ccL
	case ccLE:
		return ccGE
	case ccGE:
		return ccLE
	case ccB:
		return ccA
	case ccA:
		return ccB
	case ccBE:
		return ccAE
	case ccAE:
		return ccBE
	}

	return cc
}

func (l *lowerer) setcc(v int32) {
	cc, parity := l.compare(v)
	d := l.def(v)
	in := inst(mSetcc)
	in.dst = d
	in.cc = cc
	l.emit(in)

	if parity == 0 {
		return
	}

	p := l.mf.newReg(classGPR)
	pi := inst(mSetcc)
	pi.dst = p
	pi.cc = ccNP
	op := aluAnd

	if parity == 2 {
		pi.cc = ccP
		op = aluOr
	}

	l.emit(pi)
	a := inst(mAlu)
	a.dst = d
	a.src = p
	a.size = 4
	a.aux = uint8(op)
	l.emit(a)
}

func (l *lowerer) selectValue(v int32, args []int32) {
	f := l.f
	cond, x, y := args[0], args[1], args[2]
	t := f.values[v].typ
	size := typeSize(t)

	if isFloat(t) {
		// Through general registers: cmov has no xmm form.
		gx := l.mf.newReg(classGPR)
		gy := l.mf.newReg(classGPR)
		in := inst(mMovXG)
		in.dst = gx
		in.src = l.use(x)
		in.size = size
		l.emit(in)
		in = inst(mMovXG)
		in.dst = gy
		in.src = l.use(y)
		in.size = size
		l.emit(in)
		c := l.use(cond)
		tst := inst(mTest)
		tst.src = c
		tst.src2 = c
		tst.size = 4
		l.emit(tst)
		cm := inst(mCmov)
		cm.dst = gy
		cm.src = gx
		cm.cc = ccNE
		cm.size = size
		l.emit(cm)
		back := inst(mMovGX)
		back.dst = l.def(v)
		back.src = gy
		back.size = size
		l.emit(back)

		return
	}

	rx := l.use(x)
	ry := l.use(y)
	d := l.def(v)
	l.mov(d, ry, size)
	cc := uint8(ccNE)

	if l.deferred[cond] {
		var parity int
		cc, parity = l.compare(cond)

		if parity != 0 {
			throwFmt("internal error: float comparison deferred to select")
		}
	} else {
		c := l.use(cond)
		tst := inst(mTest)
		tst.src = c
		tst.src2 = c
		tst.size = 4
		l.emit(tst)
	}

	cm := inst(mCmov)
	cm.dst = d
	cm.src = rx
	cm.cc = cc
	cm.size = size

	if size < 4 {
		cm.size = 4
	}

	l.emit(cm)
}

func (l *lowerer) copysign(v int32, a, b int32, size uint8) {
	sign, abs := int32(constF32Sign), int32(constF32Abs)

	if size == 8 {
		sign, abs = constF64Sign, constF64Abs
	}

	t1 := l.mf.newReg(classXMM)
	in := inst(mFAluMem)
	in.dst = t1
	in.src = l.use(b)
	in.mem = Mem{base: noReg, index: noReg, label: sign}
	in.aux = uint8(fAnd)
	in.size = size
	l.emit(in)
	t2 := l.mf.newReg(classXMM)
	in = inst(mFAluMem)
	in.dst = t2
	in.src = l.use(a)
	in.mem = Mem{base: noReg, index: noReg, label: abs}
	in.aux = uint8(fAnd)
	in.size = size
	l.emit(in)
	or := inst(mFAlu)
	or.dst = l.def(v)
	or.src = t1
	or.src2 = t2
	or.aux = uint8(fOr)
	or.size = size
	l.emit(or)
}

func (l *lowerer) convert(v int32, op Op, a int32, size uint8) {
	srcType := l.f.values[a].typ
	src := l.use(a)
	d := l.def(v)

	if op == OpConvertU && srcType == I64 {
		in := inst(mCvtU64F)
		in.dst = d
		in.src = src
		in.dsize = size
		in.size = 8
		in.temps = []VReg{l.mf.newReg(classGPR), l.mf.newReg(classGPR)}
		l.emit(in)

		return
	}

	in := inst(mCvtIF)
	in.dst = d
	in.src = src
	in.dsize = size
	in.size = typeSize(srcType)

	if op == OpConvertU {
		// a zero-extended i32 converts as a non-negative i64
		in.size = 8
	}

	l.emit(in)
}

// address returns the memory operand of an access of width bytes at addr
// plus the static offset, emitting its bounds check.
func (l *lowerer) address(addr int32, offset int64, width int) Mem {
	ra := l.use(addr)
	end := offset + int64(width)

	if end <= boundSlack {
		in := inst(mCheck)
		in.src = ra
		in.imm = end
		l.emit(in)

		return memIndexed(memReg, ra, int32(offset))
	}

	// A large offset: check the full 64-bit sum.
	t := l.mf.newReg(classGPR)
	mi := inst(mMovImm)
	mi.dst = t
	mi.imm = end
	mi.size = 8
	l.emit(mi)
	add := inst(mAlu)
	add.dst = t
	add.src = ra
	add.size = 8
	add.aux = uint8(aluAdd)
	l.emit(add)
	c := inst(mCmpMem)
	c.src = t
	c.mem = memAt(ctxReg, offMemSize)
	c.size = 8
	l.emit(c)
	l.trapIf(ccA, trapMemoryOOB)

	if offset < math.MaxInt32 {
		return memIndexed(memReg, ra, int32(offset))
	}

	p := l.mf.newReg(classGPR)
	mi = inst(mMovImm)
	mi.dst = p
	mi.imm = offset
	mi.size = 8
	l.emit(mi)
	add = inst(mAlu)
	add.dst = p
	add.src = ra
	add.size = 8
	add.aux = uint8(aluAdd)
	l.emit(add)

	return memIndexed(memReg, p, 0)
}

func (l *lowerer) load(v int32, addr int32) {
	val := &l.f.values[v]
	kind := memKind(val.aux2)
	mem := l.address(addr, val.aux, kind.width())
	in := inst(mLoad)

	if isFloat(val.typ) {
		in.op = mFLoad
	}

	in.dst = l.def(v)
	in.mem = mem
	in.size = uint8(kind.width())
	in.dsize = typeSize(val.typ)

	if kind.signed() {
		in.aux = 1
	}

	l.emit(in)
}

func (l *lowerer) store(v int32, addr, value int32) {
	val := &l.f.values[v]
	kind := memKind(val.aux2)
	mem := l.address(addr, val.aux, kind.width())
	width := uint8(kind.width())
	t := l.f.values[value].typ

	if !isFloat(t) {
		if x, ok := l.imm(value, width); ok && width <= 8 {
			in := inst(mStoreImm)
			in.mem = mem
			in.imm = x
			in.size = width
			l.emit(in)

			return
		}
	}

	in := inst(mStore)

	if isFloat(t) {
		in.op = mFStore
	}

	in.mem = mem
	in.src = l.use(value)
	in.size = width
	l.emit(in)
}

func (l *lowerer) globalAddr(index int) Mem {
	if l.m.globals[index].imported {
		p := l.mf.newReg(classGPR)
		in := inst(mLoad)
		in.dst = p
		in.mem = memAt(ctxReg, l.lay.globals[index])
		in.size = 8
		in.dsize = 8
		l.emit(in)

		return memAt(p, 0)
	}

	return memAt(ctxReg, l.lay.globals[index])
}

func (l *lowerer) globalGet(v int32, index int) {
	t := l.m.globals[index].typ
	mem := l.globalAddr(index)
	in := inst(mLoad)

	if isFloat(t) {
		in.op = mFLoad
	}

	in.dst = l.def(v)
	in.mem = mem
	in.size = typeSize(t)
	in.dsize = in.size
	l.emit(in)
}

func (l *lowerer) globalSet(index int, value int32) {
	t := l.m.globals[index].typ
	mem := l.globalAddr(index)

	if x, ok := l.imm(value, typeSize(t)); ok {
		in := inst(mStoreImm)
		in.mem = mem
		in.imm = x
		in.size = typeSize(t)
		l.emit(in)

		return
	}

	in := inst(mStore)

	if isFloat(t) {
		in.op = mFStore
	}

	in.mem = mem
	in.src = l.use(value)
	in.size = typeSize(t)
	l.emit(in)
}

// placeArgs moves call arguments to their registers and stack slots and
// returns the registers the call reads and the number of stack slots.
func (l *lowerer) placeArgs(args []int32) ([]VReg, int) {
	regs := make([]VReg, len(args))
	types := make([]Type, len(args))

	for i, a := range args {
		regs[i] = l.use(a)
		types[i] = l.f.values[a].typ
	}

	return l.placeRegs(regs, types)
}

// placeRegs moves argument registers to their argument registers and stack
// slots and returns the fixed registers the call reads and the number of
// stack slots.
func (l *lowerer) placeRegs(regs []VReg, types []Type) ([]VReg, int) {
	var uses []VReg
	gi, xi, stack := 0, 0, 0
	pm := inst(mParMove)

	for i, r := range regs {
		t := types[i]

		if isFloat(t) {
			if xi < len(argXMMs) {
				pm.moves = append(pm.moves, Move{argXMMs[xi], r})
				uses = append(uses, argXMMs[xi])
				xi++

				continue
			}
		} else if gi < len(argGPRs) {
			pm.moves = append(pm.moves, Move{argGPRs[gi], r})
			uses = append(uses, argGPRs[gi])
			gi++

			continue
		}

		in := inst(mStackArg)
		in.src = r
		in.imm = int64(stack)
		in.size = typeSize(t)
		l.emit(in)
		stack++
	}

	if len(pm.moves) > 0 {
		l.emit(pm)
	}

	return uses, stack
}

// resultRegs assigns results to the result registers in order; one that
// does not fit gets noReg and the next stack result slot, which follows the
// stack arguments in the caller's outgoing area.
func resultRegs(results []ValType) []VReg {
	gi, xi := 0, 0
	regs := make([]VReg, len(results))

	for i, t := range results {
		regs[i] = noReg

		switch {
		case isFloat(t) && xi < len(resultXMMs):
			regs[i] = resultXMMs[xi]
			xi++
		case !isFloat(t) && gi < len(resultGPRs):
			regs[i] = resultGPRs[gi]
			gi++
		}
	}

	return regs
}

func assigned(regs []VReg) []VReg {
	var out []VReg

	for _, r := range regs {
		if r != noReg {
			out = append(out, r)
		}
	}

	return out
}

// outgoing makes room for a call's stack arguments and results.
func (l *lowerer) outgoing(stackArgs int, regs []VReg) {
	n := stackArgs

	for _, r := range regs {
		if r == noReg {
			n++
		}
	}

	if 8*n > l.mf.stackArgs {
		l.mf.stackArgs = 8 * n
	}
}

// takeResults defines a call's results: from their registers, or from the
// stack result slots after stackArgs argument slots.
func (l *lowerer) takeResults(v int32, results []ValType, regs []VReg, stackArgs int) {
	if len(results) == 0 {
		return
	}

	var dsts []VReg

	if len(results) == 1 {
		dsts = []VReg{l.def(v)}
	} else {
		dsts = make([]VReg, len(results))

		for i := range dsts {
			dsts[i] = noReg
		}

		for _, user := range l.extracts(v) {
			dsts[l.f.values[user].aux] = l.def(user)
		}
	}

	pm := inst(mParMove)
	slot := stackArgs

	for i, d := range dsts {
		if regs[i] == noReg {
			if d != noReg {
				in := inst(mLoad)

				if isFloat(results[i]) {
					in.op = mFLoad
				}

				in.dst = d
				in.mem = memAt(RSP, int32(8*slot))
				in.size = typeSize(results[i])
				in.dsize = in.size
				l.emit(in)
			}

			slot++

			continue
		}

		if d != noReg {
			pm.moves = append(pm.moves, Move{d, regs[i]})
		}
	}

	if len(pm.moves) > 0 {
		l.emit(pm)
	}
}

// extracts lists the OpExtract values of a multi-value call; the frontend
// emits them right after it.
func (l *lowerer) extracts(call int32) []int32 {
	f := l.f
	blk := &f.blocks[f.values[call].block]
	var out []int32

	for i, v := range blk.instrs {
		if v != call {
			continue
		}

		for _, w := range blk.instrs[i+1:] {
			if f.values[w].op != OpExtract || f.args(w)[0] != call {
				break
			}

			out = append(out, w)
		}
	}

	return out
}

// call calls a function of the module; an imported one through its
// adapter, which keeps the internal convention, the memory register
// included.
func (l *lowerer) call(v int32, index int, args []int32) {
	t := l.m.funcType(uint32(index))
	uses, stack := l.placeArgs(args)
	regs := resultRegs(t.results)
	l.outgoing(stack, regs)
	in := inst(mCall)
	in.aux = calleeFunc
	in.imm = int64(index)
	in.uses = uses
	in.defs = assigned(regs)
	l.mf.hasCalls = true
	l.emit(in)
	l.takeResults(v, t.results, regs, stack)
}

func (l *lowerer) callIndirect(v int32, typeIndex int, table int, args []int32) {
	t := &l.m.types[typeIndex]
	elem := args[len(args)-1]
	args = args[:len(args)-1]
	tb := l.lay.tables[table]
	re := l.use(elem)
	c := inst(mCmpMem)
	c.src = re
	c.mem = memAt(ctxReg, tb+tableSize)
	c.size = 4
	l.emit(c)
	l.trapIf(ccAE, trapUndefinedElem)
	entries := l.load64(memAt(ctxReg, tb+tableEntries))
	desc := l.load64(Mem{base: entries, index: re, scale: 8, label: -1})
	tst := inst(mTest)
	tst.src = desc
	tst.src2 = desc
	tst.size = 8
	l.emit(tst)
	l.trapIf(ccE, trapUninitElem)
	id := int64(typeID(t))

	if id == int64(int32(id)) {
		ci := inst(mCmpMemImm)
		ci.mem = memAt(desc, 8)
		ci.imm = id
		ci.size = 8
		l.emit(ci)
	} else {
		cm := inst(mCmpMem)
		cm.src = l.constReg(id, 8)
		cm.mem = memAt(desc, 8)
		cm.size = 8
		l.emit(cm)
	}

	l.trapIf(ccNE, trapIndirectType)
	code := l.load64(memAt(desc, 0))
	uses, stack := l.placeArgs(args)
	regs := resultRegs(t.results)
	l.outgoing(stack, regs)
	in := inst(mCallInd)
	in.src = code
	in.uses = uses
	in.defs = assigned(regs)
	l.mf.hasCalls = true
	l.emit(in)
	l.takeResults(v, t.results, regs, stack)
}

func (l *lowerer) load64(mem Mem) VReg {
	r := l.mf.newReg(classGPR)
	in := inst(mLoad)
	in.dst = r
	in.mem = mem
	in.size = 8
	in.dsize = 8
	l.emit(in)

	return r
}

func (l *lowerer) constReg(x int64, size uint8) VReg {
	r := l.mf.newReg(classGPR)
	in := inst(mMovImm)
	in.dst = r
	in.imm = x
	in.size = size
	l.emit(in)

	return r
}

// stubCall calls a stub that goes into Go, in the internal convention;
// result, when not negative, receives the i32 result.
func (l *lowerer) stubCall(stub int, regs []VReg, types []Type, result int32) {
	uses, stack := l.placeRegs(regs, types)

	if stack != 0 {
		throwFmt("internal error: stub %d takes stack arguments", stub)
	}

	in := inst(mCall)
	in.aux = calleeStub
	in.imm = int64(stub)
	in.uses = uses

	if result >= 0 {
		in.defs = []VReg{RAX}
	}

	l.mf.hasCalls = true
	l.emit(in)

	if result >= 0 {
		mv := inst(mMov)
		mv.dst = l.def(result)
		mv.src = RAX
		mv.size = 4
		mv.aux = 1
		l.emit(mv)
	}
}

// checkRange traps unless a + n <= limit, all unsigned 32-bit values
// summed in 64 bits; limit is a memory operand.
func (l *lowerer) checkRange(a, n VReg, limit Mem, limitSize uint8, code int64) {
	t := l.mf.newReg(classGPR)
	l.mov(t, a, 8)
	add := inst(mAlu)
	add.dst = t
	add.src = n
	add.size = 8
	add.aux = uint8(aluAdd)
	l.emit(add)

	if limitSize == 8 {
		c := inst(mCmpMem)
		c.src = t
		c.mem = limit
		c.size = 8
		l.emit(c)
	} else {
		lim := l.mf.newReg(classGPR)
		ld := inst(mLoad)
		ld.dst = lim
		ld.mem = limit
		ld.size = limitSize
		ld.dsize = 8
		l.emit(ld)
		c := inst(mCmp)
		c.src = t
		c.src2 = lim
		c.size = 8
		l.emit(c)
	}

	l.trapIf(ccA, code)
}

func (l *lowerer) memAddr(dst VReg, a VReg) {
	in := inst(mLea)
	in.dst = dst
	in.mem = memIndexed(memReg, a, 0)
	in.size = 8
	l.emit(in)
}

func (l *lowerer) memCopy(args []int32) {
	d, s, n := l.use(args[0]), l.use(args[1]), l.use(args[2])
	l.checkRange(d, n, memAt(ctxReg, offMemSize), 8, trapMemoryOOB)
	l.checkRange(s, n, memAt(ctxReg, offMemSize), 8, trapMemoryOOB)
	dst := l.mf.newReg(classGPR)
	l.memAddr(dst, d)
	src := l.mf.newReg(classGPR)
	l.memAddr(src, s)
	l.memStub(stubMemmove, dst, src, n)
}

func (l *lowerer) memFill(args []int32) {
	d, val, n := l.use(args[0]), l.use(args[1]), l.use(args[2])
	l.checkRange(d, n, memAt(ctxReg, offMemSize), 8, trapMemoryOOB)
	dst := l.mf.newReg(classGPR)
	l.memAddr(dst, d)
	l.memStub(stubMemset, dst, val, n)
}

func (l *lowerer) memInit(seg int, args []int32) {
	d, s, n := l.use(args[0]), l.use(args[1]), l.use(args[2])
	// The segment's length is zero once dropped.
	length := l.constReg(int64(len(l.m.datas[seg].bytes)), 8)
	zero := l.constReg(0, 4)
	flag := l.mf.newReg(classGPR)
	ld := inst(mLoad)
	ld.dst = flag
	ld.mem = memAt(ctxReg, l.lay.dataDropped(seg))
	ld.size = 1
	ld.dsize = 4
	l.emit(ld)
	tst := inst(mTest)
	tst.src = flag
	tst.src2 = flag
	tst.size = 4
	l.emit(tst)
	cm := inst(mCmov)
	cm.dst = length
	cm.src = zero
	cm.cc = ccNE
	cm.size = 8
	l.emit(cm)
	t := l.mf.newReg(classGPR)
	l.mov(t, s, 8)
	add := inst(mAlu)
	add.dst = t
	add.src = n
	add.size = 8
	add.aux = uint8(aluAdd)
	l.emit(add)
	c := inst(mCmp)
	c.src = t
	c.src2 = length
	c.size = 8
	l.emit(c)
	l.trapIf(ccA, trapMemoryOOB)
	l.checkRange(d, n, memAt(ctxReg, offMemSize), 8, trapMemoryOOB)
	dst := l.mf.newReg(classGPR)
	l.memAddr(dst, d)
	src := l.mf.newReg(classGPR)
	lea := inst(mLea)
	lea.dst = src
	lea.mem = Mem{base: noReg, index: noReg, label: dataLabel(seg)}
	lea.size = 8
	l.emit(lea)
	addS := inst(mAlu)
	addS.dst = src
	addS.src = s
	addS.size = 8
	addS.aux = uint8(aluAdd)
	l.emit(addS)
	l.memStub(stubMemmove, dst, src, n)
}

// memStub calls the memmove stub with the destination, source and length,
// or the memset stub with the destination, byte and length, in the
// registers of their string instructions.
func (l *lowerer) memStub(stub int, a, b, c VReg) {
	second := RSI

	if stub == stubMemset {
		second = RAX
	}

	pm := inst(mParMove)
	pm.moves = []Move{{RDI, a}, {second, b}, {RCX, c}}
	l.emit(pm)
	in := inst(mCall)
	in.aux = calleeStub
	in.imm = int64(stub)
	in.uses = []VReg{RDI, second, RCX}
	l.mf.hasCalls = true
	l.emit(in)
}

func dataLabel(seg int) int32 {
	return constCount + int32(seg)
}

func (l *lowerer) tableGet(v int32, table int, index int32) {
	tb := l.lay.tables[table]
	ri := l.use(index)
	c := inst(mCmpMem)
	c.src = ri
	c.mem = memAt(ctxReg, tb+tableSize)
	c.size = 4
	l.emit(c)
	l.trapIf(ccAE, trapTableOOB)
	entries := l.mf.newReg(classGPR)
	ld := inst(mLoad)
	ld.dst = entries
	ld.mem = memAt(ctxReg, tb+tableEntries)
	ld.size = 8
	ld.dsize = 8
	l.emit(ld)
	ld = inst(mLoad)
	ld.dst = l.def(v)
	ld.mem = Mem{base: entries, index: ri, scale: 8, label: -1}
	ld.size = 8
	ld.dsize = 8
	l.emit(ld)
}

func (l *lowerer) tableSet(table int, index, value int32) {
	tb := l.lay.tables[table]
	ri := l.use(index)
	rv := l.use(value)
	c := inst(mCmpMem)
	c.src = ri
	c.mem = memAt(ctxReg, tb+tableSize)
	c.size = 4
	l.emit(c)
	l.trapIf(ccAE, trapTableOOB)
	entries := l.mf.newReg(classGPR)
	ld := inst(mLoad)
	ld.dst = entries
	ld.mem = memAt(ctxReg, tb+tableEntries)
	ld.size = 8
	ld.dsize = 8
	l.emit(ld)
	st := inst(mStore)
	st.src = rv
	st.mem = Mem{base: entries, index: ri, scale: 8, label: -1}
	st.size = 8
	l.emit(st)
}

// edgeBlock returns the MIR block a branch along edge i of SSA block b
// targets, placing the parameter moves on a block of their own when b has
// other successors.
func (l *lowerer) edgeBlock(b int32, i int) int32 {
	e := &l.f.blocks[b].term.edges[i]
	target := l.mblk[e.block]

	// Moves on the edge, or a critical edge the register allocator may need
	// to place moves on, get a block of their own.
	if len(e.args) == 0 && len(l.f.blocks[e.block].preds) <= 1 {
		return target
	}

	key := [2]int32{b, int32(i)}

	if s, ok := l.split[key]; ok {
		return s
	}

	s := int32(len(l.mf.blocks))
	l.mf.blocks = append(l.mf.blocks, MBlock{id: -1, depth: l.mf.blocks[l.cur].depth})
	l.split[key] = s
	saved := l.cur
	l.cur = s
	l.edgeMoves(e)
	j := inst(mJmp)
	j.target = target
	l.emit(j)
	l.cur = saved

	return s
}

func (l *lowerer) edgeMoves(e *Edge) {
	if len(e.args) == 0 {
		return
	}

	params := l.f.blocks[e.block].params
	in := inst(mParMove)

	for i, a := range e.args {
		src := l.use(a)
		dst := l.vregs[params[i]]

		if src != dst {
			in.moves = append(in.moves, Move{dst, src})
		}
	}

	if len(in.moves) > 0 {
		l.emit(in)
	}
}

func (l *lowerer) terminator(b int32) {
	t := &l.f.blocks[b].term

	switch t.kind {
	case TermJump:
		l.edgeMoves(&t.edges[0])
		j := inst(mJmp)
		j.target = l.mblk[t.edges[0].block]
		l.emit(j)
	case TermIf:
		then := l.edgeBlock(b, 0)
		els := l.edgeBlock(b, 1)
		var cc uint8
		parity := 0

		if l.deferred[t.value] {
			cc, parity = l.compare(t.value)
		} else {
			c := l.use(t.value)
			tst := inst(mTest)
			tst.src = c
			tst.src2 = c
			tst.size = 4
			l.emit(tst)
			cc = ccNE
		}

		switch parity {
		case 1:
			j := inst(mJcc)
			j.cc = ccP
			j.target = els
			l.emit(j)
		case 2:
			j := inst(mJcc)
			j.cc = ccP
			j.target = then
			l.emit(j)
		}

		j := inst(mJcc)
		j.cc = cc
		j.target = then
		l.emit(j)
		j = inst(mJmp)
		j.target = els
		l.emit(j)
	case TermSwitch:
		in := inst(mSwitch)
		in.src = l.use(t.value)
		in.temps = []VReg{l.mf.newReg(classGPR), l.mf.newReg(classGPR)}

		for i := range t.edges {
			in.table = append(in.table, l.edgeBlock(b, i))
		}

		l.emit(in)
	case TermReturn:
		results := l.f.typ.results
		regs := resultRegs(results)
		pm := inst(mParMove)
		slot := 0

		for i, v := range t.values {
			if regs[i] != noReg {
				pm.moves = append(pm.moves, Move{regs[i], l.use(v)})

				continue
			}

			in := inst(mStoreRet)
			in.src = l.use(v)
			in.imm = int64(slot)
			in.size = typeSize(results[i])
			l.emit(in)
			slot++
		}

		if len(pm.moves) > 0 {
			l.emit(pm)
		}

		in := inst(mRet)
		in.uses = assigned(regs)
		l.emit(in)
	case TermTrap:
		in := inst(mTrap)
		in.imm = t.aux
		l.emit(in)
	default:
		throwFmt("internal error: block %d of function %d has no terminator", b, l.f.index)
	}
}

// linkBlocks fills successor and predecessor lists from the branches.
func (l *lowerer) linkBlocks() {
	mf := l.mf

	for b := range mf.blocks {
		blk := &mf.blocks[b]

		for i := range blk.insts {
			in := &blk.insts[i]

			switch in.op {
			case mJmp, mJcc:
				blk.succs = appendUnique(blk.succs, in.target)
			case mSwitch:
				for _, t := range in.table {
					blk.succs = appendUnique(blk.succs, t)
				}
			}
		}
	}

	for b := range mf.blocks {
		for _, s := range mf.blocks[b].succs {
			mf.blocks[s].preds = append(mf.blocks[s].preds, int32(b))
		}
	}
}

func appendUnique(list []int32, x int32) []int32 {
	for _, y := range list {
		if y == x {
			return list
		}
	}

	return append(list, x)
}
