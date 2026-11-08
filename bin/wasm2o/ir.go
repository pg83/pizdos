package main

import (
	"fmt"
	"strings"
)

type Op uint16

const (
	OpInvalid Op = iota
	OpConst      // aux: the value's bits
	OpParam      // block parameter; aux: its index
	OpUndef      // a value on a path that never executes

	// integer arithmetic, same type in and out
	OpAdd
	OpSub
	OpMul
	OpDivS
	OpDivU
	OpRemS
	OpRemU
	OpAnd
	OpOr
	OpXor
	OpShl
	OpShrS
	OpShrU
	OpRotl
	OpRotr
	OpClz
	OpCtz
	OpPopcnt
	OpExtend8S
	OpExtend16S
	OpExtend32S

	// integer comparisons, i32 result; args may be i32 or i64
	OpEqz
	OpEq
	OpNe
	OpLtS
	OpLtU
	OpGtS
	OpGtU
	OpLeS
	OpLeU
	OpGeS
	OpGeU

	// floating point
	OpFAdd
	OpFSub
	OpFMul
	OpFDiv
	OpFMin
	OpFMax
	OpFCopysign
	OpFAbs
	OpFNeg
	OpFSqrt
	OpFCeil
	OpFFloor
	OpFTrunc
	OpFNearest
	OpFEq
	OpFNe
	OpFLt
	OpFGt
	OpFLe
	OpFGe

	// conversions; the result type is the value's type, the source type is
	// its argument's
	OpWrap
	OpExtendS
	OpExtendU
	OpTruncS
	OpTruncU
	OpTruncSatS
	OpTruncSatU
	OpConvertS
	OpConvertU
	OpDemote
	OpPromote
	OpReinterpret

	OpSelect // args: condition, if true, if false

	// memory; aux: the static offset, aux2: memKind
	OpLoad
	OpStore
	OpMemSize
	OpMemGrow
	OpMemCopy
	OpMemFill
	OpMemInit // aux: data segment
	OpDataDrop

	OpGlobalGet // aux: global index
	OpGlobalSet

	OpCall         // aux: function index
	OpCallIndirect // aux: type index, aux2: table; the last argument is the element index
	OpExtract      // aux: result index of a multi-value call

	OpRefNull
	OpRefIsNull
	OpRefFunc // aux: function index
	OpTableGet
	OpTableSet
	OpTableSize
	OpTableGrow
	OpTableFill
	OpTableCopy // aux: destination table, aux2: source table
	OpTableInit // aux: element segment, aux2: table
	OpElemDrop
	OpCopyForward // internal: positive, checked forward byte copy; args dst, src, length:i64

	opCount
)

var opNames = [opCount]string{
	OpInvalid: "invalid", OpConst: "const", OpParam: "param", OpUndef: "undef",
	OpAdd: "add", OpSub: "sub", OpMul: "mul", OpDivS: "div_s", OpDivU: "div_u", OpRemS: "rem_s", OpRemU: "rem_u",
	OpAnd: "and", OpOr: "or", OpXor: "xor", OpShl: "shl", OpShrS: "shr_s", OpShrU: "shr_u", OpRotl: "rotl", OpRotr: "rotr",
	OpClz: "clz", OpCtz: "ctz", OpPopcnt: "popcnt", OpExtend8S: "extend8_s", OpExtend16S: "extend16_s", OpExtend32S: "extend32_s",
	OpEqz: "eqz", OpEq: "eq", OpNe: "ne", OpLtS: "lt_s", OpLtU: "lt_u", OpGtS: "gt_s", OpGtU: "gt_u",
	OpLeS: "le_s", OpLeU: "le_u", OpGeS: "ge_s", OpGeU: "ge_u",
	OpFAdd: "fadd", OpFSub: "fsub", OpFMul: "fmul", OpFDiv: "fdiv", OpFMin: "fmin", OpFMax: "fmax", OpFCopysign: "copysign",
	OpFAbs: "fabs", OpFNeg: "fneg", OpFSqrt: "sqrt", OpFCeil: "ceil", OpFFloor: "floor", OpFTrunc: "ftrunc", OpFNearest: "nearest",
	OpFEq: "feq", OpFNe: "fne", OpFLt: "flt", OpFGt: "fgt", OpFLe: "fle", OpFGe: "fge",
	OpWrap: "wrap", OpExtendS: "extend_s", OpExtendU: "extend_u", OpTruncS: "trunc_s", OpTruncU: "trunc_u",
	OpTruncSatS: "trunc_sat_s", OpTruncSatU: "trunc_sat_u", OpConvertS: "convert_s", OpConvertU: "convert_u",
	OpDemote: "demote", OpPromote: "promote", OpReinterpret: "reinterpret", OpSelect: "select",
	OpLoad: "load", OpStore: "store", OpMemSize: "memory.size", OpMemGrow: "memory.grow", OpMemCopy: "memory.copy",
	OpMemFill: "memory.fill", OpMemInit: "memory.init", OpDataDrop: "data.drop",
	OpCopyForward: "copy.forward",
	OpGlobalGet:   "global.get", OpGlobalSet: "global.set", OpCall: "call", OpCallIndirect: "call_indirect", OpExtract: "extract",
	OpRefNull: "ref.null", OpRefIsNull: "ref.is_null", OpRefFunc: "ref.func", OpTableGet: "table.get", OpTableSet: "table.set",
	OpTableSize: "table.size", OpTableGrow: "table.grow", OpTableFill: "table.fill", OpTableCopy: "table.copy",
	OpTableInit: "table.init", OpElemDrop: "elem.drop",
}

func (op Op) String() string {
	return opNames[op]
}

// Type is the type of an IR value: a wasm value type, or none for a value
// used only for its effect, or tuple for a call with several results.
type Type = ValType

const (
	TNone  ValType = 0
	TTuple ValType = 1
)

// memKind describes a load or store: its width in bytes, whether a narrow
// load sign-extends, and the type of the value in the register.
type memKind int32

const (
	memSigned memKind = 1 << 4
	memProven memKind = 1 << 5 // guarded loop version proved the entire access range
)

func makeMemKind(width int, signed bool) memKind {
	k := memKind(width)

	if signed {
		k |= memSigned
	}

	return k
}

func (k memKind) width() int {
	return int(k & 15)
}

func (k memKind) signed() bool {
	return k&memSigned != 0
}

type Value struct {
	op    Op
	typ   Type
	nargs uint16
	// args indexes Func.pool.
	args  int32
	aux   int64
	aux2  int32
	block int32
}

type Edge struct {
	block int32
	args  []int32
}

type TermKind uint8

const (
	TermNone TermKind = iota
	TermJump
	TermIf     // value: condition; edges: then, else
	TermSwitch // value: index; edges: cases, then the default
	TermReturn // values: results
	TermTrap   // aux: trap code
)

type Term struct {
	kind   TermKind
	value  int32
	edges  []Edge
	values []int32
	aux    int64
}

type Block struct {
	params []int32
	instrs []int32
	term   Term
	preds  []int32
	// dead marks a block removed by an optimization.
	dead bool
}

type Function struct {
	module *Module
	index  int
	typ    *FuncType
	values []Value
	pool   []int32
	blocks []Block
}

func (f *Function) args(v int32) []int32 {
	val := &f.values[v]

	return f.pool[val.args : val.args+int32(val.nargs)]
}

func (f *Function) newValue(op Op, typ Type, block int32, aux int64, args ...int32) int32 {
	id := int32(len(f.values))
	start := int32(len(f.pool))
	f.pool = append(f.pool, args...)
	f.values = append(f.values, Value{op: op, typ: typ, nargs: uint16(len(args)), args: start, aux: aux, block: block})

	return id
}

func (f *Function) newBlock() int32 {
	f.blocks = append(f.blocks, Block{})

	return int32(len(f.blocks) - 1)
}

// successors calls cb for each edge of block b's terminator.
func (f *Function) successors(b int32) []Edge {
	return f.blocks[b].term.edges
}

// computePreds fills each block's predecessor list from the terminators, one
// entry per edge.
func (f *Function) computePreds() {
	for i := range f.blocks {
		f.blocks[i].preds = f.blocks[i].preds[:0]
	}

	for i := range f.blocks {
		if f.blocks[i].dead {
			continue
		}

		for _, e := range f.blocks[i].term.edges {
			f.blocks[e.block].preds = append(f.blocks[e.block].preds, int32(i))
		}
	}
}

func (f *Function) dump() string {
	var sb strings.Builder

	for b := range f.blocks {
		blk := &f.blocks[b]

		if blk.dead {
			continue
		}

		fmt.Fprintf(&sb, "b%d(", b)

		for i, p := range blk.params {
			if i > 0 {
				sb.WriteString(", ")
			}

			fmt.Fprintf(&sb, "v%d:%s", p, typeName(f.values[p].typ))
		}

		fmt.Fprintf(&sb, ") preds=%v\n", blk.preds)

		for _, v := range blk.instrs {
			val := &f.values[v]
			fmt.Fprintf(&sb, "  v%d:%s = %s", v, typeName(val.typ), val.op)

			for _, a := range f.args(v) {
				fmt.Fprintf(&sb, " v%d", a)
			}

			if val.op == OpConst || val.aux != 0 {
				fmt.Fprintf(&sb, " [%d]", val.aux)
			}

			if val.aux2 != 0 {
				fmt.Fprintf(&sb, " {%d}", val.aux2)
			}

			sb.WriteString("\n")
		}

		t := &blk.term
		sb.WriteString("  ")

		switch t.kind {
		case TermJump:
			sb.WriteString("jump")
		case TermIf:
			fmt.Fprintf(&sb, "if v%d", t.value)
		case TermSwitch:
			fmt.Fprintf(&sb, "switch v%d", t.value)
		case TermReturn:
			sb.WriteString("return")

			for _, v := range t.values {
				fmt.Fprintf(&sb, " v%d", v)
			}
		case TermTrap:
			fmt.Fprintf(&sb, "trap %d", t.aux)
		default:
			sb.WriteString("none")
		}

		for _, e := range t.edges {
			fmt.Fprintf(&sb, " b%d%v", e.block, e.args)
		}

		sb.WriteString("\n")
	}

	return sb.String()
}

func typeName(t Type) string {
	switch t {
	case TNone:
		return "none"
	case TTuple:
		return "tuple"
	case I32:
		return "i32"
	case I64:
		return "i64"
	case F32:
		return "f32"
	case F64:
		return "f64"
	case V128:
		return "v128"
	case FuncRef:
		return "funcref"
	case ExternRef:
		return "externref"
	}

	return fmt.Sprintf("t%d", t)
}

func isFloat(t Type) bool {
	return t == F32 || t == F64
}

// hasEffect reports whether a value must stay even when nothing uses it:
// it writes state, or it may trap.
func hasEffect(op Op) bool {
	switch op {
	case OpDivS, OpDivU, OpRemS, OpRemU, OpTruncS, OpTruncU,
		OpLoad, OpStore, OpMemGrow, OpMemCopy, OpMemFill, OpMemInit, OpDataDrop, OpCopyForward,
		OpGlobalSet, OpCall, OpCallIndirect,
		OpTableGet, OpTableSet, OpTableGrow, OpTableFill, OpTableCopy, OpTableInit, OpElemDrop:
		return true
	}

	return false
}

// readsState reports whether a value depends on mutable state, so two equal
// instructions may produce different results.
func readsState(op Op) bool {
	switch op {
	case OpLoad, OpMemSize, OpGlobalGet, OpTableGet, OpTableSize, OpExtract, OpParam, OpUndef:
		return true
	}

	return hasEffect(op)
}

// Trap codes, the argument of the loader's trap.
const (
	trapUnreachable   = 1
	trapMemoryOOB     = 2
	trapIntOverflow   = 3
	trapDivByZero     = 4
	trapConversion    = 5
	trapIndirectType  = 6
	trapUndefinedElem = 7
	trapUninitElem    = 8
	trapStack         = 9
	trapTableOOB      = 10
	trapNullRef       = 11
)
