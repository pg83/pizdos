package main

// Generated code runs on a goroutine's stack and keeps Go's rules there:
// R14 is g and RBP the frame pointer, a frame saves RBP right below its
// return address, every instruction's stack pointer is known (asm.sps), and
// a prologue compares the stack pointer with g.stackguard0, calling the
// morestack stub when it is low. Go may then copy the stack: no frame holds
// a stack address but the saved RBP, which Go adjusts.
//
// Go is entered only through the code here. Exports and the init function
// are thunks Go calls with ABIInternal; imports, memory.grow and table.grow
// are adapters that call a Go function value whose address is in the GOT.
// A Go call clobbers everything, so an adapter saves what the internal
// convention preserves; traps call Go and never return.

// stubs: the module's code besides its functions and their boundaries
const (
	stubMorestack = iota
	stubMemmove
	stubMemset
	stubMemoryGrow
	stubTableGrow
	stubCount
)

// The bulk memory stubs clobber only what their string instructions use.
var memmoveClobbers = []VReg{RAX, RCX, RSI, RDI}
var memsetClobbers = []VReg{RCX, RDI}

// g's fields read by generated code (Go 1.25, runtime/runtime2.go).
const (
	gStackHi     = 8
	gStackguard0 = 16
)

// stackSmall is Go's StackSmall: a frame this small compares the stack
// pointer itself with the guard. leafFrame is the most a function without
// calls takes without a check, as Go's nosplit functions do.
const (
	stackSmall = 128
	leafFrame  = 512
)

var goIntRegs = []VReg{RAX, RBX, RCX, RDI, RSI, R8, R9, R10, R11}
var goFloatRegs = xmmRange(15)

// goLoc is where ABIInternal passes a value: a register, or an offset in
// the call's stack area.
type goLoc struct {
	reg VReg
	off int32
}

// goFrame lays out a call of func(ctx uintptr, params...) (results...):
// size is the whole stack area, stack arguments, stack results and the spill
// space of the register arguments.
type goFrame struct {
	params  []goLoc
	results []goLoc
	size    int32
}

func goSize(t ValType) int32 {
	if t == I32 || t == F32 {
		return 4
	}

	return 8
}

func alignTo(x, n int32) int32 {
	return (x + n - 1) &^ (n - 1)
}

func goAssign(params, results []ValType) goFrame {
	var f goFrame
	off := int32(0)

	assign := func(types []ValType, ints int) []goLoc {
		floats := 0
		locs := make([]goLoc, len(types))

		for i, t := range types {
			switch {
			case isFloat(t) && floats < len(goFloatRegs):
				locs[i] = goLoc{reg: goFloatRegs[floats]}
				floats++
			case !isFloat(t) && ints < len(goIntRegs):
				locs[i] = goLoc{reg: goIntRegs[ints]}
				ints++
			default:
				off = alignTo(off, goSize(t))
				locs[i] = goLoc{reg: noReg, off: off}
				off += goSize(t)
			}
		}

		off = alignTo(off, 8)

		return locs
	}

	// The instance takes RAX.
	f.params = assign(params, 1)
	f.results = assign(results, 0)
	off += 8

	for i, l := range f.params {
		if l.reg != noReg {
			off = alignTo(off, goSize(params[i])) + goSize(params[i])
		}
	}

	f.size = alignTo(off, 8)

	return f
}

// internalLocs gives the internal convention's places of values: a
// register, or noReg and the index of a stack slot, counted from first.
func internalLocs(types []ValType, gprs, xmms []VReg, first int) ([]VReg, []int, int) {
	gi, xi, slot := 0, 0, first
	regs := make([]VReg, len(types))
	slots := make([]int, len(types))

	for i, t := range types {
		regs[i] = noReg

		switch {
		case isFloat(t) && xi < len(xmms):
			regs[i] = xmms[xi]
			xi++
		case !isFloat(t) && gi < len(gprs):
			regs[i] = gprs[gi]
			gi++
		default:
			slots[i] = slot
			slot++
		}
	}

	return regs, slots, slot
}

// beginRegion starts a code region at a 16-byte boundary; endRegion records
// it with its stack pointer table.
func (oc *objCode) beginRegion(name string) int32 {
	oc.align16()
	oc.a.setSP(0)
	oc.regionName = name

	return oc.a.pos()
}

func (oc *objCode) endRegion(start int32) {
	end := oc.a.pos()
	oc.regions = append(oc.regions, Region{name: oc.regionName, start: uint32(start), end: uint32(end), sp: oc.a.spTable(start, end)})
}

// extern returns the index of an external symbol.
func (oc *objCode) extern(sym string) int32 {
	if i, ok := oc.externIx[sym]; ok {
		return i
	}

	i := int32(len(oc.externs))
	oc.externs = append(oc.externs, sym)
	oc.externIx[sym] = i

	return i
}

// gotMem is the GOT entry of a symbol: it holds the symbol's address.
func (oc *objCode) gotMem(sym string) amem {
	return amem{base: -1, index: -1, label: oc.extern(sym), labelKind: labelGot}
}

// callGo calls the Go function value sym names: the closure context goes in
// RDX, the code is its first word.
func (oc *objCode) callGo(sym string) {
	oc.a.load(8, 8, false, RDX, oc.gotMem(sym))
	oc.a.callMem(baseAt(RDX, 0))
}

// trapCall calls Go's trap with the instance and code; it does not return.
// Like any Go call it leaves the callee room to spill its arguments. Code
// after it is reached by jumps, with the stack pointer of before.
func (oc *objCode) trapCall(code int64) {
	a := &oc.a
	sp := a.sp
	a.aluRI(aluSub, 8, RSP, int64(goAssign([]ValType{I32}, nil).size))
	a.movRR(8, RAX, ctxReg)
	a.movImm(4, RBX, code)
	oc.callGo(symTrap)
	a.ud2()
	a.setSP(sp)
}

// stackCheck compares the stack pointer, less total-stackSmall for a large
// frame computed in temp, with g.stackguard0 and calls the morestack stub
// from out of line when it is not above. The caller emits the slow path at
// the delta it had here, with slowPath.
func (oc *objCode) stackCheck(total int32, temp VReg) int32 {
	a := &oc.a

	if total <= stackSmall {
		a.aluRM(aluCmp, 8, RSP, baseAt(R14, gStackguard0))
	} else {
		a.lea(8, temp, rspAt(-(total - stackSmall)))
		a.aluRM(aluCmp, 8, temp, baseAt(R14, gStackguard0))
	}

	return a.jccRel(ccBE)
}

// slowPath grows the stack and starts the region again at entry.
func (oc *objCode) slowPath(at, entry int32) {
	a := &oc.a
	a.patch32(at, a.pos())
	oc.callStub(stubMorestack)
	a.patch32(a.jmpRel(), entry)
}

func (oc *objCode) callStub(kind int) {
	oc.stubCalls = append(oc.stubCalls, callFixup{oc.a.callRel(), int32(kind)})
}

// stubs emits the morestack and bulk memory stubs and the Go calls of
// memory.grow and table.grow.
func (oc *objCode) stubs() {
	a := &oc.a

	start := oc.beginRegion("w2o.morestack")
	oc.stubAt[stubMorestack] = start
	saved := []VReg{RAX, RBX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, R12, R13, R15}
	a.push(RBP)
	a.movRR(8, RBP, RSP)

	for _, r := range saved {
		a.push(r)
	}

	xmms := int32(8 * len(goFloatRegs))
	a.aluRI(aluSub, 8, RSP, int64(xmms))

	for i, x := range goFloatRegs {
		a.fstore(8, x, rspAt(int32(8*i)))
	}

	// The goroutine may not use more stack than the instance allows.
	a.load(8, 8, false, RAX, baseAt(R14, gStackHi))
	a.aluRR(aluSub, 8, RAX, RSP)
	a.aluRM(aluCmp, 8, RAX, baseAt(ctxReg, offStackLimit))
	overflow := a.jccRel(ccA)
	a.callMem(oc.gotMem(symMorestack))

	for i, x := range goFloatRegs {
		a.fload(8, x, rspAt(int32(8*i)))
	}

	a.aluRI(aluAdd, 8, RSP, int64(xmms))

	for i := len(saved) - 1; i >= 0; i-- {
		a.pop(saved[i])
	}

	a.pop(RBP)
	a.ret()
	a.setSP(8 + 8*int32(len(saved)) + xmms)
	a.patch32(overflow, a.pos())
	oc.trapCall(trapStack)
	oc.endRegion(start)

	// memmove(rdi dst, rsi src, rcx n): forward unless the destination
	// starts inside the source, then backward by words and bytes. Never std:
	// Go runs with the direction flag clear, preempted here or not.
	start = oc.beginRegion("w2o.memmove")
	oc.stubAt[stubMemmove] = start
	a.movRR(8, RAX, RDI)
	a.aluRR(aluSub, 8, RAX, RSI)
	a.aluRR(aluCmp, 8, RAX, RCX)
	forward := a.jcc8(ccAE)
	a.test(8, RAX, RAX)
	same := a.jcc8(ccE)
	a.aluRR(aluAdd, 8, RSI, RCX)
	a.aluRR(aluAdd, 8, RDI, RCX)
	words := a.pos()
	a.aluRI(aluCmp, 8, RCX, 8)
	bytes := a.jcc8(ccB)
	a.aluRI(aluSub, 8, RSI, 8)
	a.aluRI(aluSub, 8, RDI, 8)
	a.load(8, 8, false, RAX, baseAt(RSI, 0))
	a.store(8, RAX, baseAt(RDI, 0))
	a.aluRI(aluSub, 8, RCX, 8)
	a.patch8(a.jmp8(), words)
	a.here8(bytes)
	tail := a.pos()
	a.test(8, RCX, RCX)
	done := a.jcc8(ccE)
	a.aluRI(aluSub, 8, RSI, 1)
	a.aluRI(aluSub, 8, RDI, 1)
	a.load(1, 4, false, RAX, baseAt(RSI, 0))
	a.store(1, RAX, baseAt(RDI, 0))
	a.aluRI(aluSub, 8, RCX, 1)
	a.patch8(a.jmp8(), tail)
	a.here8(forward)
	a.repMovsb()
	a.here8(same)
	a.here8(done)
	a.ret()
	oc.endRegion(start)

	// memset(rdi dst, al byte, rcx n)
	start = oc.beginRegion("w2o.memset")
	oc.stubAt[stubMemset] = start
	a.repStosb()
	a.ret()
	oc.endRegion(start)

	oc.stubAt[stubMemoryGrow] = oc.goCall("w2o.memory_grow_call", []ValType{I32}, []ValType{I32}, symMemoryGrow)
	oc.stubAt[stubTableGrow] = oc.goCall("w2o.table_grow_call", []ValType{I32, I32, FuncRef}, []ValType{I32}, symTableGrow)
}

// goCall emits a function of the internal convention that calls the Go
// function value of sym with the instance and its parameters, and returns
// its results.
func (oc *objCode) goCall(name string, params, results []ValType, sym string) int32 {
	a := &oc.a
	g := goAssign(params, results)
	pregs, pslots, nslots := internalLocs(params, argGPRs, argXMMs, 0)
	rregs, rslots, _ := internalLocs(results, resultGPRs, resultXMMs, nslots)
	saved := []VReg{R10, R11, R12, ctxReg}
	total := 8 + 8*int32(len(saved)) + g.size

	start := oc.beginRegion(name)
	slow := oc.stackCheck(total, RAX)
	a.push(RBP)
	a.movRR(8, RBP, RSP)

	for _, r := range saved {
		a.push(r)
	}

	if g.size > 0 {
		a.aluRI(aluSub, 8, RSP, int64(g.size))
	}

	incoming := total + 8

	for i, t := range params {
		dst := g.params[i]

		switch {
		case pregs[i] != noReg && dst.reg == pregs[i]:
		case pregs[i] == noReg && dst.reg != noReg:
			a.load(8, 8, false, dst.reg, rspAt(incoming+int32(8*pslots[i])))
		case pregs[i] == noReg:
			a.load(8, 8, false, RDX, rspAt(incoming+int32(8*pslots[i])))
			a.store(uint8(goSize(t)), RDX, rspAt(dst.off))
		default:
			throwFmt("internal error: parameter %d of %s moves between registers", i, name)
		}
	}

	a.movRR(8, RAX, ctxReg)
	oc.callGo(sym)

	for i, t := range results {
		src := g.results[i]

		switch {
		case rregs[i] != noReg && src.reg == rregs[i]:
			if t == I32 {
				a.movRR(4, src.reg, src.reg)
			}
		case rregs[i] == noReg && src.reg != noReg:
			a.store(8, src.reg, rspAt(incoming+int32(8*rslots[i])))
		case rregs[i] == noReg:
			a.load(uint8(goSize(t)), 8, false, RDX, rspAt(src.off))
			a.store(8, RDX, rspAt(incoming+int32(8*rslots[i])))
		default:
			throwFmt("internal error: result %d of %s moves between registers", i, name)
		}
	}

	if g.size > 0 {
		a.aluRI(aluAdd, 8, RSP, int64(g.size))
	}

	for i := len(saved) - 1; i >= 0; i-- {
		a.pop(saved[i])
	}

	a.load(8, 8, false, memReg, baseAt(ctxReg, offMem))
	a.pop(RBP)
	a.ret()
	a.setSP(0)
	oc.slowPath(slow, start)
	oc.endRegion(start)

	return start
}

// thunk emits a function Go calls with ABIInternal, func(ctx uintptr,
// params...) (results...), that calls function index in the internal
// convention; with index negative it calls body instead, which emits the
// work itself.
func (oc *objCode) thunk(name string, params, results []ValType, index int, body func()) int32 {
	a := &oc.a
	g := goAssign(params, results)
	pregs, pslots, nslots := internalLocs(params, argGPRs, argXMMs, 0)
	rregs, rslots, nall := internalLocs(results, resultGPRs, resultXMMs, nslots)
	frame := int32(8 * nall)
	total := 8 + frame

	start := oc.beginRegion(name)
	a.movRR(8, ctxReg, RAX)
	// R12 is scratch to Go; RAX carries the instance.
	slow := oc.stackCheck(total, R12)
	a.push(RBP)
	a.movRR(8, RBP, RSP)

	if frame > 0 {
		a.aluRI(aluSub, 8, RSP, int64(frame))
	}

	a.load(8, 8, false, memReg, baseAt(ctxReg, offMem))
	goArgs := total + 8

	for i, t := range params {
		src := g.params[i]

		switch {
		case pregs[i] != noReg && src.reg == pregs[i]:
			if t == I32 {
				a.movRR(4, src.reg, src.reg)
			}
		case pregs[i] == noReg && src.reg != noReg:
			if t == I32 {
				a.movRR(4, src.reg, src.reg)
			}

			a.store(8, src.reg, rspAt(int32(8*pslots[i])))
		case pregs[i] == noReg:
			a.load(uint8(goSize(t)), 8, false, RDX, rspAt(goArgs+src.off))
			a.store(8, RDX, rspAt(int32(8*pslots[i])))
		default:
			throwFmt("internal error: parameter %d of %s moves between registers", i, name)
		}
	}

	if index >= 0 {
		oc.callFunc(index)
	} else {
		body()
	}

	for i, t := range results {
		dst := g.results[i]

		switch {
		case rregs[i] != noReg && dst.reg == rregs[i]:
		case rregs[i] == noReg && dst.reg != noReg:
			a.load(8, 8, false, dst.reg, rspAt(int32(8*rslots[i])))
		case rregs[i] == noReg:
			a.load(8, 8, false, RDX, rspAt(int32(8*rslots[i])))
			a.store(uint8(goSize(t)), RDX, rspAt(goArgs+dst.off))
		default:
			throwFmt("internal error: result %d of %s moves between registers", i, name)
		}
	}

	if frame > 0 {
		a.aluRI(aluAdd, 8, RSP, int64(frame))
	}

	a.pop(RBP)
	a.ret()
	a.setSP(0)
	oc.slowPath(slow, start)
	oc.endRegion(start)

	return start
}
