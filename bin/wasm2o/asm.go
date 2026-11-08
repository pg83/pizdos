package main

import (
	"encoding/binary"
	"sort"
)

// asm encodes x86-64 instructions into a byte buffer. Registers are VReg
// numbers of physical registers; xmm registers are encoded by their low four
// bits.

type amem struct {
	base  int // -1: none
	index int // -1: none
	scale uint8
	disp  int32
	// label makes the operand RIP-relative; the displacement is fixed up
	// once the label is placed.
	label int32
	// labelKind tells which label space the label belongs to.
	labelKind uint8
}

const (
	labelLocal  = 0 // a position in the current function
	labelModule = 1 // a constant or data segment in .rodata
	labelFunc   = 2 // the start of a function
	labelGot    = 3 // the GOT entry of an external symbol
)

type ripFixup struct {
	// at is the position of the 32-bit displacement, end the end of the
	// instruction it is relative to.
	at, end int32
	label   int32
	kind    uint8
}

// spEntry records that from pc on the stack pointer is delta bytes below
// its value at the entry of the code region around pc.
type spEntry struct {
	pc    int32
	delta int32
}

type asm struct {
	buf    []byte
	fixups []ripFixup
	// sp is the current delta; sps logs every change, for Go's unwinder,
	// which must know the frame at each instruction.
	sp  int32
	sps []spEntry
}

// Standard x86 NOP encodings. Padding can be crossed on every entry into
// an inner loop, so emitting one instruction per byte is expensive.
var nopBytes = [...][]byte{
	nil,
	{0x90},
	{0x66, 0x90},
	{0x0f, 0x1f, 0x00},
	{0x0f, 0x1f, 0x40, 0x00},
	{0x0f, 0x1f, 0x44, 0x00, 0x00},
	{0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},
	{0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00},
	{0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
}

func (a *asm) nops(n int32) {
	for n > 0 {
		width := min(n, 9)
		a.buf = append(a.buf, nopBytes[width]...)
		n -= width
	}
}

// setSP declares the stack pointer's delta from here on: at the start of a
// region and where control arrives from elsewhere.
func (a *asm) setSP(delta int32) {
	n := len(a.sps)

	switch {
	case n > 0 && a.sps[n-1].pc == a.pos():
		a.sps[n-1].delta = delta
	case n == 0 || a.sps[n-1].delta != delta:
		a.sps = append(a.sps, spEntry{a.pos(), delta})
	}

	a.sp = delta
}

func (a *asm) moveSP(by int32) {
	a.setSP(a.sp + by)
}

// spTable returns the deltas of the region [start, end): the one at its
// start, then every change, at offsets from start.
func (a *asm) spTable(start, end int32) []spEntry {
	k := sort.Search(len(a.sps), func(i int) bool { return a.sps[i].pc > start })
	out := []spEntry{{0, 0}}

	if k > 0 {
		out[0].delta = a.sps[k-1].delta
	}

	for _, e := range a.sps[k:] {
		if e.pc >= end {
			break
		}

		if e.delta != out[len(out)-1].delta {
			out = append(out, spEntry{e.pc - start, e.delta})
		}
	}

	return out
}

func (a *asm) pos() int32 {
	return int32(len(a.buf))
}

func (a *asm) b(bytes ...byte) {
	a.buf = append(a.buf, bytes...)
}

func (a *asm) u32(x uint32) {
	a.buf = binary.LittleEndian.AppendUint32(a.buf, x)
}

func (a *asm) u64(x uint64) {
	a.buf = binary.LittleEndian.AppendUint64(a.buf, x)
}

func lo3(r int) byte {
	return byte(r & 7)
}

func hi(r int) byte {
	return byte(r>>3) & 1
}

func regNum(r VReg) int {
	if r.isXMM() {
		return int(r - XMM0)
	}

	return int(r)
}

func memOf(m Mem) amem {
	am := amem{base: -1, index: -1, scale: m.scale, disp: m.disp, label: -1}

	if m.label >= 0 {
		am.label = m.label
		am.labelKind = labelModule

		return am
	}

	if m.base != noReg {
		am.base = regNum(m.base)
	}

	if m.index != noReg {
		am.index = regNum(m.index)
	}

	if am.scale == 0 {
		am.scale = 1
	}

	return am
}

func rspAt(disp int32) amem {
	return amem{base: int(RSP), index: -1, scale: 1, disp: disp, label: -1}
}

func baseAt(base VReg, disp int32) amem {
	return amem{base: int(base), index: -1, scale: 1, disp: disp, label: -1}
}

// rex emits a REX prefix when needed: w for 64-bit operands, r extends
// ModRM.reg, x SIB.index, b ModRM.rm or SIB.base; force for byte registers
// spl..dil.
func (a *asm) rex(w bool, r, x, b int, force bool) {
	v := byte(0x40)

	if w {
		v |= 8
	}

	if r >= 0 {
		v |= hi(r) << 2
	}

	if x >= 0 {
		v |= hi(x) << 1
	}

	if b >= 0 {
		v |= hi(b)
	}

	if v != 0x40 || force {
		a.b(v)
	}
}

func (a *asm) modrmReg(reg, rm int) {
	a.b(0xc0 | lo3(reg)<<3 | lo3(rm))
}

// modrmMem emits ModRM, SIB and displacement for a memory operand; imm is
// the number of immediate bytes that follow, for RIP-relative fixups.
func (a *asm) modrmMem(reg int, m amem, imm int) {
	r := lo3(reg) << 3

	if m.label >= 0 {
		a.b(r | 0x05)
		a.fixups = append(a.fixups, ripFixup{at: a.pos(), end: a.pos() + 4 + int32(imm), label: m.label, kind: m.labelKind})
		a.u32(0)

		return
	}

	if m.base < 0 {
		// [index*scale + disp32]
		a.b(r | 0x04)
		a.b(scaleBits(m.scale)<<6 | lo3(m.index)<<3 | 5)
		a.u32(uint32(m.disp))

		return
	}

	var mod byte

	switch {
	case m.disp == 0 && lo3(m.base) != 5:
		mod = 0
	case m.disp == int32(int8(m.disp)):
		mod = 1
	default:
		mod = 2
	}

	if m.index >= 0 || lo3(m.base) == 4 {
		index := byte(4)

		if m.index >= 0 {
			index = lo3(m.index)
		}

		a.b(mod<<6 | r | 4)
		a.b(scaleBits(m.scale)<<6 | index<<3 | lo3(m.base))
	} else {
		a.b(mod<<6 | r | lo3(m.base))
	}

	switch mod {
	case 1:
		a.b(byte(m.disp))
	case 2:
		a.u32(uint32(m.disp))
	}
}

func scaleBits(s uint8) byte {
	switch s {
	case 2:
		return 1
	case 4:
		return 2
	case 8:
		return 3
	}

	return 0
}

func memX(m amem) int {
	if m.label >= 0 {
		return -1
	}

	return m.index
}

func memB(m amem) int {
	if m.label >= 0 {
		return -1
	}

	return m.base
}

// opRR emits an instruction "op reg, rm" between registers.
func (a *asm) opRR(w bool, op []byte, reg, rm int, byteRegs bool) {
	a.rex(w, reg, -1, rm, byteRegs && (reg >= 4 || rm >= 4))
	a.b(op...)
	a.modrmReg(reg, rm)
}

func (a *asm) opRM(w bool, op []byte, reg int, m amem, imm int, byteReg bool) {
	a.rex(w, reg, memX(m), memB(m), byteReg && reg >= 4)
	a.b(op...)
	a.modrmMem(reg, m, imm)
}

// prefixed emits a mandatory prefix (66, F2, F3) before the REX prefix.
func (a *asm) prefixed(prefix byte, w bool, op []byte, reg, rm int) {
	a.b(prefix)
	a.opRR(w, op, reg, rm, false)
}

// general purpose instructions

func (a *asm) movRR(size uint8, dst, src VReg) {
	a.opRR(size == 8, []byte{0x8b}, int(dst), int(src), false)
}

func (a *asm) movImm(size uint8, dst VReg, x int64) {
	d := int(dst)

	switch {
	case x == 0:
		// xor clears the whole register
		a.opRR(false, []byte{0x33}, d, d, false)
	case size == 4 || (x >= 0 && x <= 0xffffffff):
		a.rex(false, -1, -1, d, false)
		a.b(0xb8 + lo3(d))
		a.u32(uint32(x))
	case x == int64(int32(x)):
		a.rex(true, -1, -1, d, false)
		a.b(0xc7)
		a.modrmReg(0, d)
		a.u32(uint32(x))
	default:
		a.rex(true, -1, -1, d, false)
		a.b(0xb8 + lo3(d))
		a.u64(uint64(x))
	}
}

// load emits a load of width bytes into a register of dsize bytes.
func (a *asm) load(width, dsize uint8, signed bool, dst VReg, m amem) {
	d := int(dst)

	switch {
	case width == 8:
		a.opRM(true, []byte{0x8b}, d, m, 0, false)
	case width == 4 && signed && dsize == 8:
		a.opRM(true, []byte{0x63}, d, m, 0, false)
	case width == 4:
		a.opRM(false, []byte{0x8b}, d, m, 0, false)
	case width == 2:
		op := byte(0xb7)

		if signed {
			op = 0xbf
		}

		a.opRM(signed && dsize == 8, []byte{0x0f, op}, d, m, 0, false)
	default:
		op := byte(0xb6)

		if signed {
			op = 0xbe
		}

		a.opRM(signed && dsize == 8, []byte{0x0f, op}, d, m, 0, false)
	}
}

func (a *asm) store(width uint8, src VReg, m amem) {
	s := int(src)

	switch width {
	case 8:
		a.opRM(true, []byte{0x89}, s, m, 0, false)
	case 4:
		a.opRM(false, []byte{0x89}, s, m, 0, false)
	case 2:
		a.b(0x66)
		a.opRM(false, []byte{0x89}, s, m, 0, false)
	default:
		a.opRM(false, []byte{0x88}, s, m, 0, true)
	}
}

func (a *asm) storeImm(width uint8, m amem, x int64) {
	switch width {
	case 8:
		a.opRM(true, []byte{0xc7}, 0, m, 4, false)
		a.u32(uint32(x))
	case 4:
		a.opRM(false, []byte{0xc7}, 0, m, 4, false)
		a.u32(uint32(x))
	case 2:
		a.b(0x66)
		a.opRM(false, []byte{0xc7}, 0, m, 2, false)
		a.b(byte(x), byte(x>>8))
	default:
		a.opRM(false, []byte{0xc6}, 0, m, 1, false)
		a.b(byte(x))
	}
}

func (a *asm) lea(size uint8, dst VReg, m amem) {
	a.opRM(size == 8, []byte{0x8d}, int(dst), m, 0, false)

	if dst == RSP {
		if m.base != int(RSP) || m.index >= 0 || m.label >= 0 {
			throwFmt("internal error: untracked change of rsp")
		}

		a.moveSP(-m.disp)
	}
}

// aluRR emits "op dst, src" for add, or, adc, sbb, and, sub, xor, cmp.
func (a *asm) aluRR(op aluOp, size uint8, dst, src VReg) {
	if op == aluImul {
		a.opRR(size == 8, []byte{0x0f, 0xaf}, int(dst), int(src), false)

		return
	}

	a.opRR(size == 8, []byte{byte(op)<<3 | 3}, int(dst), int(src), false)
}

func (a *asm) aluRI(op aluOp, size uint8, dst VReg, x int64) {
	a.aluRIRaw(op, size, dst, x)

	if dst == RSP {
		switch op {
		case aluSub:
			a.moveSP(int32(x))
		case aluAdd:
			a.moveSP(-int32(x))
		case aluCmp:
		default:
			throwFmt("internal error: untracked change of rsp")
		}
	}
}

func (a *asm) aluRIRaw(op aluOp, size uint8, dst VReg, x int64) {
	d := int(dst)

	if x == int64(int8(x)) {
		a.rex(size == 8, -1, -1, d, false)
		a.b(0x83)
		a.modrmReg(int(op), d)
		a.b(byte(x))

		return
	}

	a.rex(size == 8, -1, -1, d, false)
	a.b(0x81)
	a.modrmReg(int(op), d)
	a.u32(uint32(x))
}

func (a *asm) aluRM(op aluOp, size uint8, dst VReg, m amem) {
	if op == aluImul {
		a.opRM(size == 8, []byte{0x0f, 0xaf}, int(dst), m, 0, false)

		return
	}

	a.opRM(size == 8, []byte{byte(op)<<3 | 3}, int(dst), m, 0, false)
}

func (a *asm) aluMI(op aluOp, size uint8, m amem, x int64) {
	if x == int64(int8(x)) {
		a.opRM(size == 8, []byte{0x83}, int(op), m, 1, false)
		a.b(byte(x))

		return
	}

	a.opRM(size == 8, []byte{0x81}, int(op), m, 4, false)
	a.u32(uint32(x))
}

func (a *asm) imul3(size uint8, dst, src VReg, x int64) {
	if x == int64(int8(x)) {
		a.opRR(size == 8, []byte{0x6b}, int(dst), int(src), false)
		a.b(byte(x))

		return
	}

	a.opRR(size == 8, []byte{0x69}, int(dst), int(src), false)
	a.u32(uint32(x))
}

func (a *asm) test(size uint8, x, y VReg) {
	a.opRR(size == 8, []byte{0x85}, int(y), int(x), false)
}

func (a *asm) testImm(size uint8, x VReg, imm int64) {
	a.rex(size == 8, -1, -1, int(x), false)
	a.b(0xf7)
	a.modrmReg(0, int(x))
	a.u32(uint32(imm))
}

func (a *asm) setcc(cc uint8, dst VReg) {
	d := int(dst)
	a.rex(false, -1, -1, d, d >= 4)
	a.b(0x0f, 0x90+cc)
	a.modrmReg(0, d)
	// movzx dst32, dst8
	a.rex(false, d, -1, d, d >= 4)
	a.b(0x0f, 0xb6)
	a.modrmReg(d, d)
}

func (a *asm) cmov(cc uint8, size uint8, dst, src VReg) {
	a.opRR(size == 8, []byte{0x0f, 0x40 + cc}, int(dst), int(src), false)
}

func (a *asm) shiftImm(kind uint8, size uint8, dst VReg, n int64) {
	d := int(dst)
	a.rex(size == 8, -1, -1, d, false)

	if n == 1 {
		a.b(0xd1)
		a.modrmReg(int(kind), d)

		return
	}

	a.b(0xc1)
	a.modrmReg(int(kind), d)
	a.b(byte(n))
}

func (a *asm) shiftCL(kind uint8, size uint8, dst VReg) {
	d := int(dst)
	a.rex(size == 8, -1, -1, d, false)
	a.b(0xd3)
	a.modrmReg(int(kind), d)
}

// vex emits a VEX prefix: pp selects 66/F3/F2, mmmmm the opcode map,
// vvvv the extra source register.
func (a *asm) vex(r, x, b int, mmmmm byte, w bool, vvvv int, l bool, pp byte) {
	R, X, B := byte(1), byte(1), byte(1)

	if r >= 0 && hi(r) == 1 {
		R = 0
	}

	if x >= 0 && hi(x) == 1 {
		X = 0
	}

	if b >= 0 && hi(b) == 1 {
		B = 0
	}

	v := byte(15)

	if vvvv >= 0 {
		v = byte(^vvvv) & 15
	}

	L := byte(0)

	if l {
		L = 1
	}

	if X == 1 && B == 1 && mmmmm == 1 && !w {
		a.b(0xc5, R<<7|v<<3|L<<2|pp)

		return
	}

	W := byte(0)

	if w {
		W = 1
	}

	a.b(0xc4, R<<7|X<<6|B<<5|mmmmm, W<<7|v<<3|L<<2|pp)
}

// vexRR emits a VEX instruction with register operands reg, vvvv and rm.
func (a *asm) vexRR(pp, mmmmm byte, w bool, op byte, reg, vvvv, rm int) {
	a.vex(reg, -1, rm, mmmmm, w, vvvv, false, pp)
	a.b(op)
	a.modrmReg(reg, rm)
}

func (a *asm) vexRM(pp, mmmmm byte, w bool, op byte, reg, vvvv int, m amem, imm int) {
	a.vex(reg, memX(m), memB(m), mmmmm, w, vvvv, false, pp)
	a.b(op)
	a.modrmMem(reg, m, imm)
}

const (
	ppNone = 0
	pp66   = 1
	ppF3   = 2
	ppF2   = 3
)

func scalarPP(size uint8) byte {
	if size == 8 {
		return ppF2
	}

	return ppF3
}

func packedPP(size uint8) byte {
	if size == 8 {
		return pp66
	}

	return ppNone
}

func (a *asm) shiftX(kind uint8, size uint8, dst, src, count VReg) {
	pp := byte(pp66)

	switch kind {
	case shiftShr:
		pp = ppF2
	case shiftSar:
		pp = ppF3
	}

	a.vexRR(pp, 2, size == 8, 0xf7, int(dst), int(count), int(src))
}

func (a *asm) bitCount(kind uint8, size uint8, dst, src VReg) {
	op := []byte{0x0f, 0xbd, 0x0f, 0xbc, 0x0f, 0xb8}[kind*2 : kind*2+2]
	a.prefixed(0xf3, size == 8, op, int(dst), int(src))
}

func (a *asm) ext(width, dsize uint8, signed bool, dst, src VReg) {
	d, s := int(dst), int(src)

	switch {
	case width == 4 && signed:
		a.opRR(true, []byte{0x63}, d, s, false)
	case width == 4:
		a.opRR(false, []byte{0x8b}, d, s, false)
	case width == 2:
		op := byte(0xb7)

		if signed {
			op = 0xbf
		}

		a.opRR(signed && dsize == 8, []byte{0x0f, op}, d, s, false)
	default:
		op := byte(0xb6)

		if signed {
			op = 0xbe
		}

		a.opRR(signed && dsize == 8, []byte{0x0f, op}, d, s, true)
	}
}

func (a *asm) unary(kind uint8, size uint8, dst VReg) {
	d := int(dst)
	a.rex(size == 8, -1, -1, d, false)
	a.b(0xf7)
	a.modrmReg(3-int(kind), d)
}

func (a *asm) div(signed bool, size uint8, src VReg) {
	s := int(src)
	a.rex(size == 8, -1, -1, s, false)
	a.b(0xf7)

	if signed {
		a.modrmReg(7, s)
	} else {
		a.modrmReg(6, s)
	}
}

func (a *asm) push(r VReg) {
	a.rex(false, -1, -1, int(r), false)
	a.b(0x50 + lo3(int(r)))
	a.moveSP(8)
}

func (a *asm) pop(r VReg) {
	a.rex(false, -1, -1, int(r), false)
	a.b(0x58 + lo3(int(r)))
	a.moveSP(-8)
}

func (a *asm) pushf() {
	a.b(0x9c)
	a.moveSP(8)
}

func (a *asm) popf() {
	a.b(0x9d)
	a.moveSP(-8)
}

func (a *asm) pushMem(m amem) {
	a.opRM(false, []byte{0xff}, 6, m, 0, false)
	a.moveSP(8)
}

func (a *asm) popMem(m amem) {
	a.opRM(false, []byte{0x8f}, 0, m, 0, false)
	a.moveSP(-8)
}

// callMem calls the address stored at m.
func (a *asm) callMem(m amem) {
	a.opRM(false, []byte{0xff}, 2, m, 0, false)
}

func (a *asm) repMovsb() {
	a.b(0xf3, 0xa4)
}

func (a *asm) repStosb() {
	a.b(0xf3, 0xaa)
}

func (a *asm) xchg(x, y VReg) {
	a.opRR(true, []byte{0x87}, int(x), int(y), false)
}

func (a *asm) ret() {
	a.b(0xc3)
}

func (a *asm) ud2() {
	a.b(0x0f, 0x0b)
}

// jumps with a 32-bit displacement to a position resolved later

func (a *asm) jmpRel() int32 {
	a.b(0xe9)
	a.u32(0)

	return a.pos() - 4
}

func (a *asm) jccRel(cc uint8) int32 {
	a.b(0x0f, 0x80+cc)
	a.u32(0)

	return a.pos() - 4
}

func (a *asm) callRel() int32 {
	a.b(0xe8)
	a.u32(0)

	return a.pos() - 4
}

func (a *asm) callReg(r VReg) {
	a.rex(false, -1, -1, int(r), false)
	a.b(0xff)
	a.modrmReg(2, int(r))
}

func (a *asm) jmpReg(r VReg) {
	a.rex(false, -1, -1, int(r), false)
	a.b(0xff)
	a.modrmReg(4, int(r))
}

// patch32 stores a displacement to target, relative to the end of the
// field at at.
func (a *asm) patch32(at int32, target int32) {
	binary.LittleEndian.PutUint32(a.buf[at:], uint32(target-(at+4)))
}

// short forward jumps inside one expansion

func (a *asm) jcc8(cc uint8) int32 {
	a.b(0x70+cc, 0)

	return a.pos() - 1
}

func (a *asm) jmp8() int32 {
	a.b(0xeb, 0)

	return a.pos() - 1
}

func (a *asm) here8(at int32) {
	d := a.pos() - (at + 1)

	if d > 127 {
		throwFmt("internal error: short jump too far")
	}

	a.buf[at] = byte(d)
}

// patch8 points the short jump whose displacement is at at to target.
func (a *asm) patch8(at int32, target int32) {
	d := target - (at + 1)

	if d != int32(int8(d)) {
		throwFmt("internal error: short jump too far")
	}

	a.buf[at] = byte(int8(d))
}

// floating point

func (a *asm) fmov(dst, src VReg) {
	a.vexRR(ppNone, 1, false, 0x28, regNum(dst), -1, regNum(src))
}

func (a *asm) fload(size uint8, dst VReg, m amem) {
	a.vexRM(scalarPP(size), 1, false, 0x10, regNum(dst), -1, m, 0)
}

func (a *asm) fstore(size uint8, src VReg, m amem) {
	a.vexRM(scalarPP(size), 1, false, 0x11, regNum(src), -1, m, 0)
}

var fAluOpcodes = map[fAluOp]byte{fAdd: 0x58, fSub: 0x5c, fMul: 0x59, fDiv: 0x5e, fMin: 0x5d, fMax: 0x5f,
	fAnd: 0x54, fAndn: 0x55, fOr: 0x56, fXor: 0x57}

func fAluPP(op fAluOp, size uint8) byte {
	if op >= fAnd {
		return packedPP(size)
	}

	return scalarPP(size)
}

func (a *asm) falu(op fAluOp, size uint8, dst, x, y VReg) {
	a.vexRR(fAluPP(op, size), 1, false, fAluOpcodes[op], regNum(dst), regNum(x), regNum(y))
}

func (a *asm) faluM(op fAluOp, size uint8, dst, x VReg, m amem) {
	a.vexRM(fAluPP(op, size), 1, false, fAluOpcodes[op], regNum(dst), regNum(x), m, 0)
}

func (a *asm) fsqrt(size uint8, dst, src VReg) {
	a.vexRR(scalarPP(size), 1, false, 0x51, regNum(dst), regNum(src), regNum(src))
}

func (a *asm) fround(size uint8, dst, src VReg, mode int64) {
	op := byte(0x0a)

	if size == 8 {
		op = 0x0b
	}

	a.vexRR(pp66, 3, false, op, regNum(dst), regNum(src), regNum(src))
	a.b(byte(mode) | 8)
}

func (a *asm) fcmp(size uint8, x, y VReg) {
	a.vexRR(packedPP(size), 1, false, 0x2e, regNum(x), -1, regNum(y))
}

func (a *asm) fcmpM(size uint8, x VReg, m amem) {
	a.vexRM(packedPP(size), 1, false, 0x2e, regNum(x), -1, m, 0)
}

func (a *asm) fcmpMask(size uint8, dst, x, y VReg, predicate byte) {
	a.vexRR(scalarPP(size), 1, false, 0xc2, regNum(dst), regNum(x), regNum(y))
	a.b(predicate)
}

func (a *asm) fblend(size uint8, dst, x, y, mask VReg) {
	op := byte(0x4a)

	if size == 8 {
		op = 0x4b
	}

	a.vexRR(pp66, 3, false, op, regNum(dst), regNum(x), regNum(y))
	a.b(byte(regNum(mask) << 4))
}

func (a *asm) fxor(dst VReg) {
	r := regNum(dst)
	a.vexRR(ppNone, 1, false, 0x57, r, r, r)
}

// cvtIF converts a signed integer of isize bytes to a float of fsize bytes.
func (a *asm) cvtIF(isize, fsize uint8, dst, src VReg) {
	d := regNum(dst)
	// Go's X15 is always zero: merging its unused upper lanes avoids a
	// false dependency on dst without a separate xor before the conversion.
	a.vexRR(scalarPP(fsize), 1, isize == 8, 0x2a, d, 15, int(src))
}

// cvtFI truncates a float of fsize bytes to a signed integer of isize bytes.
func (a *asm) cvtFI(isize, fsize uint8, dst, src VReg) {
	a.vexRR(scalarPP(fsize), 1, isize == 8, 0x2c, int(dst), -1, regNum(src))
}

func (a *asm) cvtFF(from uint8, dst, src VReg) {
	d := regNum(dst)
	a.vexRR(scalarPP(from), 1, false, 0x5a, d, 15, regNum(src))
}

func (a *asm) movGX(size uint8, dst, src VReg) {
	a.vexRR(pp66, 1, size == 8, 0x6e, regNum(dst), -1, int(src))
}

func (a *asm) movXG(size uint8, dst, src VReg) {
	a.vexRR(pp66, 1, size == 8, 0x7e, regNum(src), -1, int(dst))
}

func (a *asm) rorx(size uint8, dst, src VReg, n int64) {
	a.vexRR(ppF2, 3, size == 8, 0xf0, int(dst), -1, int(src))
	a.b(byte(n))
}
