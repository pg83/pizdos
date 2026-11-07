package main

import (
	"fmt"
	"math"
)

// The emitter turns allocated machine code into bytes of the module's text:
// prologue and epilogue, blocks in the allocator's order, out-of-line slow
// paths and trap stubs, jump tables.

type jumpFixup struct {
	at    int32
	block int32
}

type trapFixup struct {
	at   int32
	code int64
}

type tableFixup struct {
	lea   int32 // displacement of the lea that loads the table address
	end   int32
	table []int32
}

type funcEmitter struct {
	oc       *objCode
	a        *asm
	mf       *MFunc
	al       *allocator
	frame    int32
	pushes   []VReg
	blockAt  []int32
	jumps    []jumpFixup
	traps    []trapFixup
	tables   []tableFixup
	late     []func()
	incoming int32
}

func (oc *objCode) emitFunction(index int, mf *MFunc, al *allocator) {
	a := &oc.a

	for len(a.buf)%16 != 0 {
		a.b(0xcc)
	}

	oc.funcAt[index] = a.pos()
	e := &funcEmitter{oc: oc, a: a, mf: mf, al: al}
	e.pushes = al.calleeSavedUsed()
	frame := int32(mf.stackArgs+mf.retArea) + 8*al.nslots
	total := 8 + 8*int32(len(e.pushes)) + frame

	if total%16 != 0 {
		frame += 8
		total += 8
	}

	e.frame = frame
	e.incoming = frame + 8*int32(len(e.pushes)) + 8

	if mf.hasCalls || total > 4096 {
		if total > 4096 {
			a.lea(8, RAX, rspAt(-total))
			a.aluRM(aluCmp, 8, RAX, baseAt(ctxReg, offStackLimit))
		} else {
			a.aluRM(aluCmp, 8, RSP, baseAt(ctxReg, offStackLimit))
		}

		e.trapJcc(ccB, trapStack)
	}

	for _, r := range e.pushes {
		a.push(r)
	}

	if frame > 0 {
		a.aluRI(aluSub, 8, RSP, int64(frame))
	}

	e.blockAt = make([]int32, len(mf.blocks))

	for i := range e.blockAt {
		e.blockAt[i] = -1
	}

	for i, b := range al.order {
		next := int32(-1)

		if i+1 < len(al.order) {
			next = al.order[i+1]
		}

		e.blockAt[b] = a.pos()
		e.block(&mf.blocks[b], next)
	}

	for i := 0; i < len(e.late); i++ {
		e.late[i]()
	}

	stubs := map[int64]int32{}

	for _, t := range e.traps {
		at, ok := stubs[t.code]

		if !ok {
			at = a.pos()
			stubs[t.code] = at
			// The stack may be misaligned in the prologue; the handler does
			// not return.
			a.aluRI(aluAnd, 8, RSP, -16)
			a.movRR(8, RDI, ctxReg)
			a.movImm(4, RSI, t.code)
			oc.callHelper(helperTrap)
			a.ud2()
		}

		a.patch32(t.at, at)
	}

	for _, j := range e.jumps {
		a.patch32(j.at, e.blockAt[j.block])
	}

	for _, t := range e.tables {
		for len(a.buf)%4 != 0 {
			a.b(0xcc)
		}

		base := a.pos()
		a.patch32(t.lea, base)

		for _, b := range t.table {
			a.u32(uint32(e.blockAt[b] - base))
		}
	}
}

func (e *funcEmitter) trapJcc(cc uint8, code int64) {
	e.traps = append(e.traps, trapFixup{e.a.jccRel(cc), code})
}

func (e *funcEmitter) trapJmp(code int64) {
	e.traps = append(e.traps, trapFixup{e.a.jmpRel(), code})
}

func (e *funcEmitter) jumpTo(cc int, block int32) {
	var at int32

	if cc < 0 {
		at = e.a.jmpRel()
	} else {
		at = e.a.jccRel(uint8(cc))
	}

	e.jumps = append(e.jumps, jumpFixup{at, block})
}

func (e *funcEmitter) block(blk *MBlock, next int32) {
	insts := blk.insts
	n := len(insts)

	for i := 0; i < n; i++ {
		in := &insts[i]

		switch in.op {
		case mJmp:
			if in.target != next {
				e.jumpTo(-1, in.target)
			}
		case mJcc:
			// jcc A; jmp B with A next: invert into j!cc B
			if i+1 == n-1 && insts[i+1].op == mJmp && in.target == next {
				e.jumpTo(int(in.cc^1), insts[i+1].target)
				i++

				continue
			}

			e.jumpTo(int(in.cc), in.target)
		default:
			e.inst(in)
		}
	}
}

func (e *funcEmitter) slotMem(slot int32) amem {
	return rspAt(int32(e.mf.stackArgs+e.mf.retArea) + 8*slot)
}

func (e *funcEmitter) mem(m Mem) amem {
	am := memOf(m)

	if m.frame {
		am.disp += int32(e.mf.stackArgs)
	}

	if m.label >= 0 {
		am.label = m.label
		am.labelKind = labelModule
	}

	return am
}

func (e *funcEmitter) inst(in *MInst) {
	a := e.a

	switch in.op {
	case mNop:
	case mMov:
		if in.dst == in.src && in.aux == 0 {
			return
		}

		size := in.size

		if in.aux == 1 {
			size = 4
		}

		a.movRR(size, in.dst, in.src)
	case mFMov:
		if in.dst != in.src {
			a.fmov(in.dst, in.src)
		}
	case mMovImm:
		a.movImm(in.size, in.dst, in.imm)
	case mLoad:
		a.load(in.size, in.dsize, in.aux == 1, in.dst, e.mem(in.mem))
	case mStore:
		a.store(in.size, in.src, e.mem(in.mem))
	case mStoreImm:
		a.storeImm(in.size, e.mem(in.mem), in.imm)
	case mLea:
		a.lea(in.size, in.dst, e.mem(in.mem))
	case mAlu:
		a.aluRR(aluOp(in.aux), in.size, in.dst, in.src)
	case mAluImm:
		a.aluRI(aluOp(in.aux), in.size, in.dst, in.imm)
	case mAluMem:
		a.aluRM(aluOp(in.aux), in.size, in.dst, e.mem(in.mem))
	case mImul3:
		a.imul3(in.size, in.dst, in.src, in.imm)
	case mCmp:
		a.aluRR(aluCmp, in.size, in.src, in.src2)
	case mCmpImm:
		a.aluRI(aluCmp, in.size, in.src, in.imm)
	case mCmpMem:
		a.aluRM(aluCmp, in.size, in.src, e.mem(in.mem))
	case mCmpMemImm:
		a.aluMI(aluCmp, in.size, e.mem(in.mem), in.imm)
	case mTest:
		a.test(in.size, in.src, in.src2)
	case mTestImm:
		a.testImm(in.size, in.src, in.imm)
	case mSetcc:
		a.setcc(in.cc, in.dst)
	case mCmov:
		a.cmov(in.cc, in.size, in.dst, in.src)
	case mShiftImm:
		a.shiftImm(in.aux, in.size, in.dst, in.imm)
	case mShiftX:
		a.shiftX(in.aux, in.size, in.dst, in.src, in.src2)
	case mRotCL:
		a.shiftCL(in.aux, in.size, in.dst)
	case mUnary:
		a.unary(in.aux, in.size, in.dst)
	case mBitCount:
		if in.dst != in.src {
			// break the false dependency on the destination
			a.aluRR(aluXor, 4, in.dst, in.dst)
		}

		a.bitCount(in.aux, in.size, in.dst, in.src)
	case mExt:
		a.ext(in.size, in.dsize, in.aux == 1, in.dst, in.src)
	case mDiv:
		a.div(in.aux == 1, in.size, in.src)
	case mSignExtAx:
		if in.size == 8 {
			a.b(0x48)
		}

		a.b(0x99)
	case mZeroDx:
		a.aluRR(aluXor, 4, RDX, RDX)
	case mFLoad:
		a.fload(in.size, in.dst, e.mem(in.mem))
	case mFStore:
		a.fstore(in.size, in.src, e.mem(in.mem))
	case mFConst:
		if in.imm == 0 {
			a.fxor(in.dst)
		} else {
			a.fload(in.size, in.dst, e.oc.floatConst(uint64(in.imm), in.size))
		}
	case mFAlu:
		a.falu(fAluOp(in.aux), in.size, in.dst, in.src, in.src2)
	case mFAluMem:
		a.faluM(fAluOp(in.aux), in.size, in.dst, in.src, e.mem(in.mem))
	case mFUnary:
		if in.aux == fSqrt {
			a.fsqrt(in.size, in.dst, in.src)
		} else {
			a.fround(in.size, in.dst, in.src, in.imm)
		}
	case mFCmp:
		a.fcmp(in.size, in.src, in.src2)
	case mCvtIF:
		a.fxor(in.dst)
		a.cvtIF(in.size, in.dsize, in.dst, in.src)
	case mCvtFF:
		if in.dst != in.src {
			a.fxor(in.dst)
		}

		a.cvtFF(in.size, in.dst, in.src)
	case mMovGX:
		a.movGX(in.size, in.dst, in.src)
	case mMovXG:
		a.movXG(in.size, in.dst, in.src)
	case mSwitch:
		e.switchTable(in)
	case mCall:
		switch in.aux {
		case calleeFunc:
			e.oc.callFunc(int(in.imm))
		case calleeImport:
			e.oc.callImport(int(in.imm))
		default:
			e.oc.callHelper(int(in.imm))
		}
	case mCallInd:
		a.callReg(in.src)
	case mRet:
		if e.frame > 0 {
			a.aluRI(aluAdd, 8, RSP, int64(e.frame))
		}

		for i := len(e.pushes) - 1; i >= 0; i-- {
			a.pop(e.pushes[i])
		}

		a.ret()
	case mTrap:
		e.trapJmp(in.imm)
	case mTrapIf:
		e.trapJcc(in.cc, in.imm)
	case mCheck:
		e.check(in)
	case mParMove:
		e.parMove(in.moves)
	case mStackArg:
		if in.src.isXMM() {
			a.fstore(8, in.src, rspAt(int32(8*in.imm)))
		} else {
			a.store(8, in.src, rspAt(int32(8*in.imm)))
		}
	case mLoadArg:
		m := rspAt(e.incoming + int32(8*in.imm))

		if in.dst.isXMM() {
			a.fload(8, in.dst, m)
		} else {
			a.load(8, 8, false, in.dst, m)
		}
	case mDivOvf:
		a.aluRI(aluCmp, in.size, in.src2, -1)
		j := a.jcc8(ccNE)
		a.aluRI(aluCmp, in.size, in.src, 1)
		e.trapJcc(ccO, trapIntOverflow)
		a.here8(j)
	case mTruncF:
		e.truncF(in)
	case mFMinMax:
		e.minMax(in)
	case mCvtU64F:
		e.cvtU64F(in)
	default:
		throwFmt("internal error: cannot emit %s", in.String())
	}
}

// check emits the bounds check of an access ending at most imm bytes past
// the address: the common case compares with size - boundSlack, the slow
// path the exact limit.
func (e *funcEmitter) check(in *MInst) {
	a := e.a
	src, end := in.src, in.imm
	a.aluRM(aluCmp, 8, src, baseAt(ctxReg, offMemBound))
	at := a.jccRel(ccG)
	resume := a.pos()
	e.late = append(e.late, func() {
		a.patch32(at, a.pos())
		s := RAX

		if src == RAX {
			s = RCX
		}

		a.push(s)
		a.load(8, 8, false, s, baseAt(ctxReg, offMemSize))
		a.aluRI(aluSub, 8, s, end)
		a.aluRR(aluCmp, 8, src, s)
		a.pop(s)
		e.trapJcc(ccG, trapMemoryOOB)
		a.patch32(a.jmpRel(), resume)
	})
}

func (e *funcEmitter) switchTable(in *MInst) {
	a := e.a
	n := len(in.table) - 1
	def := in.table[n]

	if n == 0 {
		e.jumpTo(-1, def)

		return
	}

	t0, t1 := in.temps[0], in.temps[1]
	a.aluRI(aluCmp, 4, in.src, int64(n))
	e.jumpTo(ccAE, def)
	// lea t0, [rip+table]: the displacement is patched when the table is
	// placed
	a.rex(true, int(t0), -1, -1, false)
	a.b(0x8d, lo3(int(t0))<<3|5)
	leaAt := a.pos()
	a.u32(0)
	a.opRM(true, []byte{0x63}, int(t1), amem{base: int(t0), index: int(in.src), scale: 4, label: -1}, 0, false)
	a.aluRR(aluAdd, 8, t1, t0)
	a.jmpReg(t1)
	e.tables = append(e.tables, tableFixup{lea: leaAt, end: leaAt + 4, table: in.table})
}

// parMove performs moves between registers and stack slots as if all at
// once.
func (e *funcEmitter) parMove(moves []Move) {
	pending := make([]Move, 0, len(moves))

	for _, m := range moves {
		if m.dst != m.src {
			pending = append(pending, m)
		}
	}

	for len(pending) > 0 {
		progress := false

		for i := 0; i < len(pending); i++ {
			m := pending[i]
			blocked := false

			for j, o := range pending {
				if j != i && o.src == m.dst {
					blocked = true

					break
				}
			}

			if blocked {
				continue
			}

			e.move(m.dst, m.src)
			pending = append(pending[:i], pending[i+1:]...)
			i--
			progress = true
		}

		if progress {
			continue
		}

		// Only cycles remain: swap one pair.
		m := pending[0]
		e.swap(m.dst, m.src)
		pending = pending[1:]

		for k := range pending {
			switch pending[k].src {
			case m.dst:
				pending[k].src = m.src
			case m.src:
				pending[k].src = m.dst
			}
		}
	}
}

func (e *funcEmitter) move(dst, src VReg) {
	a := e.a

	switch {
	case isSlot(dst) && isSlot(src):
		a.pushMem(e.slotMem(slotOf(src)))
		a.popMem(e.slotMem(slotOf(dst)))
	case isSlot(dst):
		if src.isXMM() {
			a.fstore(8, src, e.slotMem(slotOf(dst)))
		} else {
			a.store(8, src, e.slotMem(slotOf(dst)))
		}
	case isSlot(src):
		if dst.isXMM() {
			a.fload(8, dst, e.slotMem(slotOf(src)))
		} else {
			a.load(8, 8, false, dst, e.slotMem(slotOf(src)))
		}
	case dst.isXMM():
		a.fmov(dst, src)
	default:
		a.movRR(8, dst, src)
	}
}

func (e *funcEmitter) swap(x, y VReg) {
	a := e.a

	if isSlot(x) {
		x, y = y, x
	}

	switch {
	case isSlot(x):
		throwFmt("internal error: swap of two stack slots")
	case isSlot(y):
		m := e.slotMem(slotOf(y))
		a.pushMem(m)
		shifted := m
		shifted.disp += 8

		if x.isXMM() {
			a.fstore(8, x, shifted)
			a.fload(8, x, rspAt(0))
			a.lea(8, RSP, rspAt(8))
		} else {
			a.store(8, x, shifted)
			a.pop(x)
		}
	case x.isXMM():
		a.falu(fXor, 8, x, x, y)
		a.falu(fXor, 8, y, y, x)
		a.falu(fXor, 8, x, x, y)
	default:
		a.xchg(x, y)
	}
}

func maxSigned(size uint8) int64 {
	if size == 8 {
		return math.MaxInt64
	}

	return math.MaxInt32
}

// truncBounds returns the float bit patterns bounding a truncation that
// produced the minimum integer: the input is in range when it is above low
// (at or above, unless strict) and below high.
func truncBounds(isize, fsize uint8) (low, high uint64, strict bool) {
	if fsize == 8 {
		if isize == 4 {
			return math.Float64bits(-2147483649.0), math.Float64bits(2147483648.0), true
		}

		return math.Float64bits(-9223372036854775808.0), math.Float64bits(9223372036854775808.0), false
	}

	if isize == 4 {
		return uint64(math.Float32bits(-2147483648.0)), uint64(math.Float32bits(2147483648.0)), false
	}

	return uint64(math.Float32bits(-9223372036854775808.0)), uint64(math.Float32bits(9223372036854775808.0)), false
}

func (e *funcEmitter) truncF(in *MInst) {
	a := e.a
	isize, fsize := in.size, in.dsize
	dst, src := in.dst, in.src
	tx, tg := in.temps[0], in.temps[1]
	unsigned := in.aux&truncUnsigned != 0
	sat := in.aux&truncSat != 0
	_, high, _ := truncBounds(isize, fsize)

	if !unsigned {
		a.cvtFI(isize, fsize, dst, src)
		// out of range and NaN give the minimum value: x - 1 overflows
		a.aluRI(aluCmp, isize, dst, 1)
		at := a.jccRel(ccO)
		resume := a.pos()
		e.late = append(e.late, func() {
			a.patch32(at, a.pos())
			a.fcmp(fsize, src, src)

			if sat {
				nan := a.jcc8(ccP)
				a.fxor(tx)
				a.fcmp(fsize, src, tx)
				a.patch32(a.jccRel(ccBE), resume)
				a.movImm(isize, dst, maxSigned(isize))
				a.patch32(a.jmpRel(), resume)
				a.here8(nan)
				a.movImm(4, dst, 0)
				a.patch32(a.jmpRel(), resume)

				return
			}

			e.trapJcc(ccP, trapConversion)
			low, _, strict := truncBounds(isize, fsize)
			a.fcmpM(fsize, src, e.oc.floatConst(low, fsize))

			if strict {
				e.trapJcc(ccBE, trapIntOverflow)
			} else {
				e.trapJcc(ccB, trapIntOverflow)
			}

			a.fcmpM(fsize, src, e.oc.floatConst(high, fsize))
			e.trapJcc(ccAE, trapIntOverflow)
			a.patch32(a.jmpRel(), resume)
		})

		return
	}

	if isize == 4 {
		// through a 64-bit conversion: in range when the upper half is zero
		a.cvtFI(8, fsize, dst, src)
		a.movRR(8, tg, dst)
		a.shiftImm(shiftShr, 8, tg, 32)
		at := a.jccRel(ccNE)
		resume := a.pos()
		e.late = append(e.late, func() {
			a.patch32(at, a.pos())
			a.fcmp(fsize, src, src)

			if sat {
				zero := a.jcc8(ccP)
				a.fxor(tx)
				a.fcmp(fsize, src, tx)
				neg := a.jcc8(ccB)
				a.movImm(4, dst, 0xffffffff)
				a.patch32(a.jmpRel(), resume)
				a.here8(zero)
				a.here8(neg)
				a.movImm(4, dst, 0)
				a.patch32(a.jmpRel(), resume)

				return
			}

			e.trapJcc(ccP, trapConversion)
			e.trapJmp(trapIntOverflow)
		})

		return
	}

	// unsigned 64-bit: values from 2^63 up convert after subtracting 2^63
	a.fcmpM(fsize, src, e.oc.floatConst(high, fsize))
	big := a.jcc8(ccAE)
	a.cvtFI(8, fsize, dst, src)
	a.test(8, dst, dst)
	slowLow := a.jccRel(ccS)
	done := a.jmp8()
	a.here8(big)
	a.faluM(fSub, fsize, tx, src, e.oc.floatConst(high, fsize))
	a.cvtFI(8, fsize, dst, tx)
	a.test(8, dst, dst)
	slowHigh := a.jccRel(ccS)
	a.movImm(8, tg, math.MinInt64)
	a.aluRR(aluXor, 8, dst, tg)
	a.here8(done)
	resume := a.pos()
	e.late = append(e.late, func() {
		a.patch32(slowLow, a.pos())

		if sat {
			a.movImm(4, dst, 0)
		} else {
			a.fcmp(fsize, src, src)
			e.trapJcc(ccP, trapConversion)
			e.trapJmp(trapIntOverflow)
		}

		a.patch32(a.jmpRel(), resume)
		a.patch32(slowHigh, a.pos())

		if sat {
			a.movImm(8, dst, -1)
			a.patch32(a.jmpRel(), resume)
		} else {
			e.trapJmp(trapIntOverflow)
		}
	})
}

func (e *funcEmitter) minMax(in *MInst) {
	a := e.a
	size, d, x, y := in.size, in.dst, in.src, in.src2
	a.fcmp(size, x, y)
	nan := a.jcc8(ccP)
	normal := a.jcc8(ccNE)

	// equal: only the signs of zeros can differ
	if in.aux == 1 {
		a.falu(fAnd, size, d, x, y)
	} else {
		a.falu(fOr, size, d, x, y)
	}

	done1 := a.jmp8()
	a.here8(normal)

	if in.aux == 1 {
		a.falu(fMax, size, d, x, y)
	} else {
		a.falu(fMin, size, d, x, y)
	}

	done2 := a.jmp8()
	a.here8(nan)
	a.falu(fAdd, size, d, x, y)
	a.here8(done1)
	a.here8(done2)
}

func (e *funcEmitter) cvtU64F(in *MInst) {
	a := e.a
	d, s := in.dst, in.src
	t1, t2 := in.temps[0], in.temps[1]
	a.test(8, s, s)
	big := a.jcc8(ccS)
	a.fxor(d)
	a.cvtIF(8, in.dsize, d, s)
	done := a.jmp8()
	a.here8(big)
	a.movRR(8, t1, s)
	a.shiftImm(shiftShr, 8, t1, 1)
	a.movRR(4, t2, s)
	a.aluRI(aluAnd, 4, t2, 1)
	a.aluRR(aluOr, 8, t1, t2)
	a.fxor(d)
	a.cvtIF(8, in.dsize, d, t1)
	a.falu(fAdd, in.dsize, d, d, d)
	a.here8(done)
}

func sanitize(s string) string {
	b := make([]byte, 0, len(s))

	for i := 0; i < len(s); i++ {
		c := s[i]

		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9', c == '_':
			b = append(b, c)
		default:
			b = append(b, fmt.Sprintf("_%02X", c)...)
		}
	}

	return string(b)
}
