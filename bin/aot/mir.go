package main

import (
	"fmt"
	"strings"
)

// VReg names a register operand: 0-15 are the general purpose registers,
// 16-31 the xmm registers, the rest virtual registers.
type VReg int32

const (
	RAX VReg = iota
	RCX
	RDX
	RBX
	RSP
	RBP
	RSI
	RDI
	R8
	R9
	R10
	R11
	R12
	R13
	R14
	R15
	XMM0
)

const (
	noReg        VReg = -1
	firstVirtual VReg = 32
	// ctxReg holds the instance, memReg the base of linear memory; neither
	// is ever allocated.
	ctxReg = R14
	memReg = R15
)

func (r VReg) isXMM() bool {
	return r >= XMM0 && r < firstVirtual
}

func (r VReg) isPhys() bool {
	return r >= 0 && r < firstVirtual
}

var gprNames = [16]string{"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"}

func (r VReg) String() string {
	switch {
	case r < noReg:
		return fmt.Sprintf("slot%d", -2-r)
	case r == noReg:
		return "_"
	case r < XMM0:
		return gprNames[r]
	case r < firstVirtual:
		return fmt.Sprintf("xmm%d", r-XMM0)
	}

	return fmt.Sprintf("v%d", r)
}

// register classes
const (
	classGPR = 0
	classXMM = 1
)

// The internal calling convention is System V's without the first integer
// argument register, which carries the instance in System V and is free
// here: a call to the host needs no shuffling.
var argGPRs = []VReg{RSI, RDX, RCX, R8, R9}
var argXMMs = []VReg{XMM0, XMM0 + 1, XMM0 + 2, XMM0 + 3, XMM0 + 4, XMM0 + 5, XMM0 + 6, XMM0 + 7}
var resultGPRs = []VReg{RAX, RDX}
var resultXMMs = []VReg{XMM0, XMM0 + 1}

// callerSaved are clobbered by every call.
var callerSaved = []VReg{RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11,
	XMM0, XMM0 + 1, XMM0 + 2, XMM0 + 3, XMM0 + 4, XMM0 + 5, XMM0 + 6, XMM0 + 7,
	XMM0 + 8, XMM0 + 9, XMM0 + 10, XMM0 + 11, XMM0 + 12, XMM0 + 13, XMM0 + 14, XMM0 + 15}

var calleeSaved = []VReg{RBX, RBP, R12, R13}

// allocation order: caller-saved first, so short values do not cost a save
var allocGPRs = []VReg{RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, RBX, RBP, R12, R13}
var allocXMMs = []VReg{XMM0, XMM0 + 1, XMM0 + 2, XMM0 + 3, XMM0 + 4, XMM0 + 5, XMM0 + 6, XMM0 + 7,
	XMM0 + 8, XMM0 + 9, XMM0 + 10, XMM0 + 11, XMM0 + 12, XMM0 + 13, XMM0 + 14, XMM0 + 15}

type MOp uint8

const (
	mNop       MOp = iota
	mMov           // dst = src; aux 1: zero-extend from 32 bits even if the registers match
	mMovImm        // dst = imm
	mLoad          // dst = [mem]; size: width; aux: 1 sign-extends; dsize: register width
	mStore         // [mem] = src; size: width
	mStoreImm      // [mem] = imm
	mLea           // dst = &mem
	mAlu           // dst = dst alu src; aux: aluOp
	mAluImm        // dst = dst alu imm
	mAluMem        // dst = dst alu [mem]
	mImul3         // dst = src * imm
	mCmp           // flags = src - src2
	mCmpImm        // flags = src - imm
	mCmpMem        // flags = src - [mem]
	mCmpMemImm     // flags = [mem] - imm
	mTest          // flags = src & src2
	mTestImm       // flags = src & imm
	mSetcc         // dst = cc ? 1 : 0
	mCmov          // if cc: dst = src
	mShiftImm      // dst = dst shift imm; aux: shift kind
	mShiftX        // dst = src shift src2 (BMI2)
	mRotCL         // dst = dst rotate rcx; aux: 0 rol, 1 ror
	mUnary         // dst = op dst; aux: 0 neg, 1 not
	mBitCount      // dst = op src; aux: 0 lzcnt, 1 tzcnt, 2 popcnt
	mExt           // dst = extend src; size: source width; aux 1: signed
	mDiv           // rax, rdx = rdx:rax / src; aux 1: signed
	mSignExtAx     // rdx = sign of rax (cdq/cqo)
	mZeroDx        // rdx = 0 (keeps flags? no: xor)
	mFMov          // dst = src (xmm)
	mFLoad         // dst = [mem]
	mFStore        // [mem] = src
	mFConst        // dst = constant imm (bits)
	mFAlu          // dst = src op src2; aux: fAluOp
	mFUnary        // dst = op src; aux: fUnaryOp, imm: rounding mode
	mFCmp          // flags = ucomis(src, src2)
	mCvtIF         // dst(xmm) = convert signed int src; size: int width, dsize: float width
	mCvtFI         // dst(gpr) = truncate float src; size: int width, dsize: float width
	mCvtFF         // dst = convert float src to dsize
	mMovGX         // dst(xmm) = bits of src(gpr)
	mMovXG         // dst(gpr) = bits of src(xmm)
	mJmp           // target block
	mJcc           // if cc: target block
	mSwitch        // jump table on src; table: blocks, last is the default
	mCall          // direct call; aux: callee kind, imm: callee index; uses/defs listed
	mCallInd       // call src
	mRet
	mTrap     // jump to the trap stub for code imm
	mTrapIf   // if cc: trap imm
	mCheck    // bounds check of src (i32 address) for an access ending at imm
	mParMove  // parallel moves
	mStackArg // store src to the outgoing argument slot imm
	mLoadArg  // dst = incoming stack argument imm
	mGrowMem  // memory.grow: src pages -> dst
	mMemCopy  // memory.copy dst=src, src2... see lowering
	mMemFill
	mSpillStore
	mSpillLoad
	mDivOvf  // trap if src is the minimum value and src2 is -1
	mTruncF  // dst(gpr) = truncate src(xmm); aux: truncFlags; trapping unless saturating
	mFMinMax // dst = min/max(src, src2) with wasm semantics; aux 1: max
	mCvtU64F // dst(xmm) = convert unsigned 64-bit src; temps: two gprs
	mFAluMem // dst = src op [mem]; aux: fAluOp
	mFSelect // dst = src2 if src (an i32 condition) is zero else temps[0]... see lowering
	mMemOp   // memory.copy/fill/init: see emitter
)

// truncFlags for mTruncF
const (
	truncUnsigned = 1
	truncSat      = 2
)

type aluOp uint8

const (
	aluAdd aluOp = iota
	aluOr
	aluAdc
	aluSbb
	aluAnd
	aluSub
	aluXor
	aluCmp
	aluImul
)

const (
	shiftRol = 0
	shiftRor = 1
	shiftShl = 4
	shiftShr = 5
	shiftSar = 7
)

type fAluOp uint8

const (
	fAdd fAluOp = iota
	fSub
	fMul
	fDiv
	fMin
	fMax
	fAnd
	fAndn
	fOr
	fXor
)

const (
	fSqrt  = 0
	fRound = 1
)

// condition codes in their x86 encoding
const (
	ccO  = 0
	ccNO = 1
	ccB  = 2
	ccAE = 3
	ccE  = 4
	ccNE = 5
	ccBE = 6
	ccA  = 7
	ccS  = 8
	ccNS = 9
	ccP  = 10
	ccNP = 11
	ccL  = 12
	ccGE = 13
	ccLE = 14
	ccG  = 15
)

var ccNames = [16]string{"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"}

type Mem struct {
	base  VReg
	index VReg
	scale uint8
	disp  int32
	// label, when not negative, makes the operand RIP-relative to a
	// constant of the function or module.
	label int32
	// frame makes disp relative to the return value area of the frame,
	// above the outgoing arguments: the base is rsp.
	frame bool
}

func memAt(base VReg, disp int32) Mem {
	return Mem{base: base, index: noReg, scale: 1, disp: disp, label: -1}
}

func memIndexed(base VReg, index VReg, disp int32) Mem {
	return Mem{base: base, index: index, scale: 1, disp: disp, label: -1}
}

type Move struct {
	dst VReg
	src VReg
}

type MInst struct {
	op    MOp
	size  uint8
	dsize uint8
	cc    uint8
	aux   uint8
	dst   VReg
	src   VReg
	src2  VReg
	imm   int64
	mem   Mem
	// target is a block for branches, a callee for calls
	target int32
	// uses and defs list the fixed registers a call or return reads and
	// writes; moves holds a parallel move.
	uses  []VReg
	defs  []VReg
	temps []VReg
	moves []Move
	table []int32
}

type MBlock struct {
	insts []MInst
	succs []int32
	preds []int32
	// depth is the loop nesting depth, for spill costs.
	depth int
	// id is the SSA block this was lowered from, or -1 for a split edge.
	id int32
}

type MFunc struct {
	blocks []MBlock
	nregs  int
	class  []uint8
	// stackArgs is the size of the outgoing argument area.
	stackArgs int
	// retArea is the size of the area callees store multiple results in.
	retArea int
	// incoming is the number of stack-passed parameters.
	incoming int
	// hasCalls reports a non-leaf function.
	hasCalls bool
	// consts are 16-byte aligned float constants referenced by label.
	consts []uint64
}

func (mf *MFunc) newReg(class uint8) VReg {
	r := VReg(int(firstVirtual) + mf.nregs)
	mf.nregs++
	mf.class = append(mf.class, class)

	return r
}

func (mf *MFunc) regClass(r VReg) uint8 {
	if r.isPhys() {
		if r.isXMM() {
			return classXMM
		}

		return classGPR
	}

	return mf.class[r-firstVirtual]
}

// Operand roles reported by forEachOperand.
const (
	roleUse = 1
	roleDef = 2
	// roleUseDef is read and written by the same instruction: the result
	// must take the register of the operand.
	roleUseDef = 3
	// roleTemp is a scratch register the instruction may write at any point:
	// it conflicts with every other operand.
	roleTemp = 4
)

// forEachOperand calls cb for every register operand of an instruction with
// its role, uses before defs.
func (in *MInst) forEachOperand(cb func(r *VReg, role int)) {
	mem := func() {
		if in.mem.base != noReg {
			cb(&in.mem.base, roleUse)
		}

		if in.mem.index != noReg {
			cb(&in.mem.index, roleUse)
		}
	}

	switch in.op {
	case mMov, mFMov, mExt, mBitCount, mCvtIF, mCvtFI, mCvtFF, mMovGX, mMovXG, mFUnary:
		cb(&in.src, roleUse)
		cb(&in.dst, roleDef)
	case mMovImm, mSetcc, mFConst:
		cb(&in.dst, roleDef)
	case mLoad, mFLoad, mLea:
		mem()
		cb(&in.dst, roleDef)
	case mStore, mFStore:
		mem()
		cb(&in.src, roleUse)
	case mStoreImm, mCmpMemImm:
		mem()
	case mAlu:
		cb(&in.src, roleUse)
		cb(&in.dst, roleUseDef)
	case mAluImm, mShiftImm, mUnary:
		cb(&in.dst, roleUseDef)
	case mAluMem:
		mem()
		cb(&in.dst, roleUseDef)
	case mImul3:
		cb(&in.src, roleUse)
		cb(&in.dst, roleDef)
	case mCmp, mTest, mFCmp:
		cb(&in.src, roleUse)
		cb(&in.src2, roleUse)
	case mCmpImm, mTestImm, mCheck, mSwitch, mCallInd, mStackArg:
		cb(&in.src, roleUse)
	case mCmpMem:
		mem()
		cb(&in.src, roleUse)
	case mCmov:
		cb(&in.src, roleUse)
		cb(&in.dst, roleUseDef)
	case mShiftX, mFAlu:
		cb(&in.src, roleUse)
		cb(&in.src2, roleUse)
		cb(&in.dst, roleDef)
	case mRotCL:
		cb(&in.src, roleUse)
		cb(&in.dst, roleUseDef)
	case mParMove:
		for i := range in.moves {
			cb(&in.moves[i].src, roleUse)
		}

		for i := range in.moves {
			cb(&in.moves[i].dst, roleDef)
		}
	case mLoadArg:
		cb(&in.dst, roleDef)
	case mDivOvf:
		cb(&in.src, roleUse)
		cb(&in.src2, roleUse)
	case mTruncF, mCvtU64F:
		cb(&in.src, roleUse)
		cb(&in.dst, roleDef)
	case mFMinMax:
		cb(&in.src, roleUse)
		cb(&in.src2, roleUse)
		cb(&in.dst, roleDef)
	case mFAluMem:
		mem()
		cb(&in.src, roleUse)
		cb(&in.dst, roleDef)
	}

	for i := range in.temps {
		cb(&in.temps[i], roleTemp)
	}

	if in.op == mDiv || in.op == mCall || in.op == mCallInd || in.op == mRet || in.op == mSignExtAx || in.op == mZeroDx || in.op == mGrowMem {
		if in.op == mDiv || in.op == mGrowMem {
			cb(&in.src, roleUse)
		}

		for i := range in.uses {
			cb(&in.uses[i], roleUse)
		}

		for i := range in.defs {
			cb(&in.defs[i], roleDef)
		}
	}
}

// clobbers reports whether an instruction destroys every caller-saved
// register.
func (in *MInst) clobbers() bool {
	return in.op == mCall || in.op == mCallInd || in.op == mGrowMem
}

func (mf *MFunc) dump() string {
	var sb strings.Builder

	for b := range mf.blocks {
		blk := &mf.blocks[b]
		fmt.Fprintf(&sb, "B%d (ssa %d, depth %d) preds=%v succs=%v\n", b, blk.id, blk.depth, blk.preds, blk.succs)

		for i := range blk.insts {
			fmt.Fprintf(&sb, "  %s\n", blk.insts[i].String())
		}
	}

	return sb.String()
}

var mopNames = map[MOp]string{
	mNop: "nop", mMov: "mov", mMovImm: "movi", mLoad: "load", mStore: "store", mStoreImm: "storei", mLea: "lea",
	mAlu: "alu", mAluImm: "alui", mAluMem: "alum", mImul3: "imul3", mCmp: "cmp", mCmpImm: "cmpi", mCmpMem: "cmpm", mCmpMemImm: "cmpmi",
	mTest: "test", mTestImm: "testi", mSetcc: "setcc", mCmov: "cmov", mShiftImm: "shifti", mShiftX: "shiftx",
	mRotCL: "rotcl", mUnary: "unary", mBitCount: "bitcount", mExt: "ext", mDiv: "div", mSignExtAx: "cqo",
	mZeroDx: "zerodx", mFMov: "fmov", mFLoad: "fload", mFStore: "fstore", mFConst: "fconst", mFAlu: "falu",
	mFUnary: "funary", mFCmp: "fcmp", mCvtIF: "cvtif", mCvtFI: "cvtfi", mCvtFF: "cvtff", mMovGX: "movgx",
	mMovXG: "movxg", mJmp: "jmp", mJcc: "jcc", mSwitch: "switch", mCall: "call", mCallInd: "callind",
	mRet: "ret", mTrap: "trap", mTrapIf: "trapif", mCheck: "check", mParMove: "pmov", mStackArg: "stackarg",
	mLoadArg: "loadarg", mGrowMem: "growmem", mSpillStore: "spill", mSpillLoad: "reload",
	mDivOvf: "divovf", mTruncF: "truncf", mFMinMax: "fminmax", mCvtU64F: "cvtu64f", mFAluMem: "falum",
	mFSelect: "fselect", mMemOp: "memop",
}

func (m Mem) String() string {
	var sb strings.Builder
	sb.WriteString("[")

	if m.label >= 0 {
		fmt.Fprintf(&sb, "L%d", m.label)
	}

	if m.base != noReg {
		sb.WriteString(m.base.String())
	}

	if m.index != noReg {
		fmt.Fprintf(&sb, "+%s*%d", m.index, m.scale)
	}

	fmt.Fprintf(&sb, "%+d]", m.disp)

	return sb.String()
}

func (in *MInst) String() string {
	var sb strings.Builder
	fmt.Fprintf(&sb, "%s", mopNames[in.op])

	if in.size != 0 {
		fmt.Fprintf(&sb, ".%d", in.size)
	}

	if in.op == mJcc || in.op == mSetcc || in.op == mCmov || in.op == mTrapIf {
		fmt.Fprintf(&sb, ".%s", ccNames[in.cc])
	}

	if in.aux != 0 {
		fmt.Fprintf(&sb, " aux=%d", in.aux)
	}

	if in.dst != noReg {
		fmt.Fprintf(&sb, " %s =", in.dst)
	}

	if in.src != noReg {
		fmt.Fprintf(&sb, " %s", in.src)
	}

	if in.src2 != noReg {
		fmt.Fprintf(&sb, " %s", in.src2)
	}

	switch in.op {
	case mLoad, mStore, mStoreImm, mLea, mAluMem, mCmpMem, mCmpMemImm, mFLoad, mFStore, mFAluMem:
		fmt.Fprintf(&sb, " %s", in.mem)
	}

	if in.imm != 0 {
		fmt.Fprintf(&sb, " #%d", in.imm)
	}

	if in.op == mJmp || in.op == mJcc {
		fmt.Fprintf(&sb, " B%d", in.target)
	}

	if len(in.moves) > 0 {
		for _, m := range in.moves {
			fmt.Fprintf(&sb, " %s<-%s", m.dst, m.src)
		}
	}

	if len(in.uses) > 0 {
		fmt.Fprintf(&sb, " uses%v", in.uses)
	}

	if len(in.defs) > 0 {
		fmt.Fprintf(&sb, " defs%v", in.defs)
	}

	if len(in.temps) > 0 {
		fmt.Fprintf(&sb, " temps%v", in.temps)
	}

	if len(in.table) > 0 {
		fmt.Fprintf(&sb, " table%v", in.table)
	}

	return sb.String()
}
