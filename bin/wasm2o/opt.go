package main

import (
	"math/bits"
	"slices"
)

// The SSA optimizer: algebraic simplification and constant folding, jump
// threading through blocks that only branch on a parameter, CFG cleanup,
// removal of trivial block parameters, and dead code elimination. Passes
// record replacements as aliases that one rewrite applies.

type optimizer struct {
	f      *Function
	alias  []int32
	consts map[constKey]int32
	uses   []int32
}

func optimize(f *Function) {
	o := &optimizer{f: f, consts: map[constKey]int32{}}

	for v := range f.values {
		val := &f.values[v]

		if val.op == OpConst && val.block < 0 {
			key := constKey{val.typ, val.aux}

			if _, ok := o.consts[key]; !ok {
				o.consts[key] = int32(v)
			}
		}
	}

	f.computePreds()

	for round := 0; round < 8; round++ {
		changed := o.simplify()
		changed = o.thread() || changed
		changed = o.cleanup() || changed
		changed = o.trivialParams() || changed

		if !changed {
			break
		}
	}

	o.deadCode()
}

func (o *optimizer) resolve(v int32) int32 {
	for int(v) < len(o.alias) && o.alias[v] >= 0 {
		v = o.alias[v]
	}

	return v
}

func (o *optimizer) replace(v, w int32) {
	for len(o.alias) < len(o.f.values) {
		o.alias = append(o.alias, -1)
	}

	if v != w {
		o.alias[v] = w
	}
}

// apply rewrites every operand through the aliases.
func (o *optimizer) apply() {
	f := o.f

	if len(o.alias) == 0 {
		return
	}

	for v := range f.values {
		val := &f.values[v]

		for i := int32(0); i < int32(val.nargs); i++ {
			f.pool[val.args+i] = o.resolve(f.pool[val.args+i])
		}
	}

	for b := range f.blocks {
		t := &f.blocks[b].term

		if t.kind == TermIf || t.kind == TermSwitch {
			t.value = o.resolve(t.value)
		}

		for i := range t.values {
			t.values[i] = o.resolve(t.values[i])
		}

		for i := range t.edges {
			for j := range t.edges[i].args {
				t.edges[i].args[j] = o.resolve(t.edges[i].args[j])
			}
		}
	}
}

func (o *optimizer) constant(t Type, bits int64) int32 {
	if t == I32 {
		bits = int64(int32(bits))
	}

	key := constKey{t, bits}

	if v, ok := o.consts[key]; ok {
		return v
	}

	v := o.f.newValue(OpConst, t, -1, bits)
	o.consts[key] = v

	return v
}

func (o *optimizer) isConst(v int32) bool {
	return o.f.values[v].op == OpConst
}

func (o *optimizer) constOf(v int32) int64 {
	return o.f.values[v].aux
}

func (o *optimizer) countUses() {
	f := o.f
	o.uses = make([]int32, len(f.values))

	for b := range f.blocks {
		blk := &f.blocks[b]

		if blk.dead {
			continue
		}

		for _, v := range blk.instrs {
			for _, a := range f.args(v) {
				o.uses[a]++
			}
		}

		t := &blk.term

		if t.kind == TermIf || t.kind == TermSwitch {
			o.uses[t.value]++
		}

		for _, v := range t.values {
			o.uses[v]++
		}

		for _, e := range t.edges {
			for _, a := range e.args {
				o.uses[a]++
			}
		}
	}
}

// setArgs replaces the operands of v.
func (o *optimizer) setArgs(v int32, args ...int32) {
	f := o.f
	val := &f.values[v]

	if int(val.nargs) >= len(args) {
		copy(f.pool[val.args:], args)
		val.nargs = uint16(len(args))

		return
	}

	val.args = int32(len(f.pool))
	val.nargs = uint16(len(args))
	f.pool = append(f.pool, args...)
}

var invertCmp = map[Op]Op{
	OpEq: OpNe, OpNe: OpEq, OpLtS: OpGeS, OpGeS: OpLtS, OpLtU: OpGeU, OpGeU: OpLtU,
	OpGtS: OpLeS, OpLeS: OpGtS, OpGtU: OpLeU, OpLeU: OpGtU,
}

func mask(t Type) uint64 {
	if t == I32 {
		return 0xffffffff
	}

	return ^uint64(0)
}

// foldInt evaluates a pure integer operation on constants.
func foldInt(op Op, t Type, a, b uint64) (uint64, bool) {
	w := uint(64)

	if t == I32 {
		w = 32
	}

	m := mask(t)
	a &= m
	b &= m
	sa := int64(a<<(64-w)) >> (64 - w)
	sb := int64(b<<(64-w)) >> (64 - w)
	sh := b & uint64(w-1)
	boolean := func(c bool) (uint64, bool) {
		if c {
			return 1, true
		}

		return 0, true
	}

	switch op {
	case OpAdd:
		return (a + b) & m, true
	case OpSub:
		return (a - b) & m, true
	case OpMul:
		return (a * b) & m, true
	case OpAnd:
		return a & b, true
	case OpOr:
		return a | b, true
	case OpXor:
		return a ^ b, true
	case OpShl:
		return (a << sh) & m, true
	case OpShrU:
		return a >> sh, true
	case OpShrS:
		return uint64(sa>>sh) & m, true
	case OpRotl:
		if w == 32 {
			return uint64(bits.RotateLeft32(uint32(a), int(sh))), true
		}

		return bits.RotateLeft64(a, int(sh)), true
	case OpRotr:
		if w == 32 {
			return uint64(bits.RotateLeft32(uint32(a), -int(sh))), true
		}

		return bits.RotateLeft64(a, -int(sh)), true
	case OpDivU:
		if b == 0 {
			return 0, false
		}

		return a / b, true
	case OpRemU:
		if b == 0 {
			return 0, false
		}

		return a % b, true
	case OpDivS:
		if b == 0 || (sb == -1 && a == 1<<(w-1)) {
			return 0, false
		}

		return uint64(sa/sb) & m, true
	case OpRemS:
		if b == 0 {
			return 0, false
		}

		if sb == -1 {
			return 0, true
		}

		return uint64(sa%sb) & m, true
	case OpEq:
		return boolean(a == b)
	case OpNe:
		return boolean(a != b)
	case OpLtS:
		return boolean(sa < sb)
	case OpLtU:
		return boolean(a < b)
	case OpGtS:
		return boolean(sa > sb)
	case OpGtU:
		return boolean(a > b)
	case OpLeS:
		return boolean(sa <= sb)
	case OpLeU:
		return boolean(a <= b)
	case OpGeS:
		return boolean(sa >= sb)
	case OpGeU:
		return boolean(a >= b)
	}

	return 0, false
}

func foldUnary(op Op, t, from Type, a uint64) (uint64, bool) {
	m := mask(t)

	switch op {
	case OpEqz:
		if a&mask(from) == 0 {
			return 1, true
		}

		return 0, true
	case OpClz:
		if t == I32 {
			return uint64(bits.LeadingZeros32(uint32(a))), true
		}

		return uint64(bits.LeadingZeros64(a)), true
	case OpCtz:
		if t == I32 {
			return uint64(bits.TrailingZeros32(uint32(a))), true
		}

		return uint64(bits.TrailingZeros64(a)), true
	case OpPopcnt:
		return uint64(bits.OnesCount64(a & m)), true
	case OpExtend8S:
		return uint64(int64(int8(a))) & m, true
	case OpExtend16S:
		return uint64(int64(int16(a))) & m, true
	case OpExtend32S:
		return uint64(int64(int32(a))) & m, true
	case OpWrap:
		return a & 0xffffffff, true
	case OpExtendU:
		return a & 0xffffffff, true
	case OpExtendS:
		return uint64(int64(int32(a))), true
	}

	return 0, false
}

func isIntOp(op Op) bool {
	return op >= OpAdd && op <= OpGeU
}

// simplify folds constants and rewrites operations into simpler ones.
func (o *optimizer) simplify() bool {
	f := o.f
	changed := false
	o.countUses()

	for b := range f.blocks {
		blk := &f.blocks[b]

		if blk.dead {
			continue
		}

		for _, v := range blk.instrs {
			if o.simplifyValue(v) {
				changed = true
			}
		}

		if o.simplifyTerm(int32(b)) {
			changed = true
		}
	}

	o.apply()

	return changed
}

func (o *optimizer) simplifyValue(v int32) bool {
	f := o.f
	val := &f.values[v]

	if int(v) < len(o.alias) && o.alias[v] >= 0 {
		return false
	}

	args := f.args(v)

	for i, a := range args {
		args[i] = o.resolve(a)
	}

	t := val.typ

	switch {
	case val.nargs == 2 && isIntOp(val.op) && val.op != OpEqz && !isFloat(f.values[args[0]].typ):
		a, b := args[0], args[1]
		at := f.values[a].typ

		if o.isConst(a) && o.isConst(b) {
			if r, ok := foldInt(val.op, at, uint64(o.constOf(a)), uint64(o.constOf(b))); ok {
				o.replace(v, o.constant(t, int64(r)))

				return true
			}

			return false
		}

		return o.identity(v, val.op, a, b, at)
	case val.nargs == 1 && (val.op >= OpClz && val.op <= OpEqz || val.op == OpWrap || val.op == OpExtendU || val.op == OpExtendS):
		a := args[0]
		from := f.values[a].typ

		if isFloat(from) {
			return false
		}

		if o.isConst(a) {
			if r, ok := foldUnary(val.op, t, from, uint64(o.constOf(a))); ok {
				o.replace(v, o.constant(t, int64(r)))

				return true
			}
		}

		return o.unaryIdentity(v, val.op, a)
	case val.op == OpSelect:
		c, x, y := args[0], args[1], args[2]

		if x == y {
			o.replace(v, x)

			return true
		}

		if o.isConst(c) {
			if o.constOf(c) != 0 {
				o.replace(v, x)
			} else {
				o.replace(v, y)
			}

			return true
		}
	}

	return false
}

func (o *optimizer) identity(v int32, op Op, a, b int32, t Type) bool {
	f := o.f

	if o.isConst(a) && !o.isConst(b) && (op == OpAdd || op == OpMul || op == OpAnd || op == OpOr || op == OpXor || op == OpEq || op == OpNe) {
		a, b = b, a
		o.setArgs(v, a, b)
	}

	if o.isConst(b) {
		c := uint64(o.constOf(b)) & mask(t)

		switch {
		case c == 0 && (op == OpAdd || op == OpSub || op == OpOr || op == OpXor || op == OpShl || op == OpShrS || op == OpShrU || op == OpRotl || op == OpRotr):
			o.replace(v, a)

			return true
		case c == 0 && (op == OpMul || op == OpAnd):
			o.replace(v, b)

			return true
		case c == 1 && (op == OpMul || op == OpDivS || op == OpDivU):
			o.replace(v, a)

			return true
		case c == mask(t) && op == OpAnd:
			o.replace(v, a)

			return true
		case c == 0 && op == OpEq && t == I32:
			// eq x 0 is eqz x
			f.values[v].op = OpEqz
			o.setArgs(v, a)

			return true
		case c == 0 && op == OpGeU:
			o.replace(v, o.constant(I32, 1))

			return true
		case c == 0 && op == OpLtU:
			o.replace(v, o.constant(I32, 0))

			return true
		}

		if op == OpMul && c&(c-1) == 0 {
			// multiplication by a power of two is a shift
			f.values[v].op = OpShl
			o.setArgs(v, a, o.constant(t, int64(bits.TrailingZeros64(c))))

			return true
		}

		if op == OpAnd {
			// and of an extended value with the low ones keeps it
			src := &f.values[a]

			if src.op == OpExtendU && c == 0xffffffff {
				o.replace(v, a)

				return true
			}
		}
	}

	if a == b {
		switch op {
		case OpSub, OpXor:
			o.replace(v, o.constant(t, 0))

			return true
		case OpAnd, OpOr:
			o.replace(v, a)

			return true
		case OpEq, OpLeS, OpLeU, OpGeS, OpGeU:
			o.replace(v, o.constant(I32, 1))

			return true
		case OpNe, OpLtS, OpLtU, OpGtS, OpGtU:
			o.replace(v, o.constant(I32, 0))

			return true
		}
	}

	return false
}

func (o *optimizer) unaryIdentity(v int32, op Op, a int32) bool {
	f := o.f
	src := &f.values[a]

	switch op {
	case OpEqz:
		if invert, ok := invertCmp[src.op]; ok && o.uses[a] == 1 && src.block == f.values[v].block {
			// eqz of a single-use comparison is the opposite comparison
			sa := f.args(a)
			x, y := sa[0], sa[1]
			f.values[v].op = invert
			o.setArgs(v, x, y)

			return true
		}

		if src.op == OpEqz && f.values[f.args(a)[0]].typ == I32 {
			// eqz eqz x is x != 0
			x := f.args(a)[0]
			f.values[v].op = OpNe
			o.setArgs(v, x, o.constant(I32, 0))

			return true
		}
	case OpWrap:
		switch src.op {
		case OpExtendU, OpExtendS:
			o.replace(v, f.args(a)[0])

			return true
		case OpAdd, OpSub, OpMul, OpAnd, OpOr, OpXor, OpShl:
			// the low half of a 64-bit operation of extended or constant
			// operands is the 32-bit operation
			sa := f.args(a)
			x, ok1 := o.low32(sa[0])
			y, ok2 := o.low32(sa[1])

			if ok1 && ok2 && (src.op != OpShl || o.isConst(sa[1])) {
				if src.op == OpShl && uint64(o.constOf(sa[1]))&63 >= 32 {
					return false
				}

				f.values[v].op = src.op
				o.setArgs(v, x, y)

				return true
			}
		}
	case OpExtendU:
		if src.op == OpWrap && o.isZext(f.args(a)[0]) {
			o.replace(v, f.args(a)[0])

			return true
		}
	}

	return false
}

// low32 returns an i32 value equal to the low half of i64 value v, without
// new instructions.
func (o *optimizer) low32(v int32) (int32, bool) {
	f := o.f
	val := &f.values[v]

	switch val.op {
	case OpConst:
		return o.constant(I32, int64(uint32(val.aux))), true
	case OpExtendU, OpExtendS:
		return f.args(v)[0], true
	}

	return 0, false
}

// isZext reports an i64 value known to have its upper half zero.
func (o *optimizer) isZext(v int32) bool {
	val := &o.f.values[v]

	return val.op == OpExtendU || (val.op == OpConst && uint64(val.aux) <= 0xffffffff) ||
		(val.op == OpLoad && memKind(val.aux2).width() < 8 && !memKind(val.aux2).signed())
}

func (o *optimizer) simplifyTerm(b int32) bool {
	f := o.f
	t := &f.blocks[b].term

	switch t.kind {
	case TermIf:
		c := o.resolve(t.value)
		t.value = c
		val := &f.values[c]

		if val.op == OpConst {
			e := t.edges[1]

			if val.aux != 0 {
				e = t.edges[0]
			}

			*t = Term{kind: TermJump, edges: []Edge{e}}

			return true
		}

		if val.op == OpEqz && f.values[f.args(c)[0]].typ == I32 {
			// branch on eqz x: branch on x with the arms swapped
			t.value = f.args(c)[0]
			t.edges[0], t.edges[1] = t.edges[1], t.edges[0]

			return true
		}

		if val.op == OpNe && o.isConst(f.args(c)[1]) && o.constOf(f.args(c)[1]) == 0 && f.values[f.args(c)[0]].typ == I32 {
			t.value = f.args(c)[0]

			return true
		}

		if t.edges[0].block == t.edges[1].block && sameArgs(o, t.edges[0].args, t.edges[1].args) {
			*t = Term{kind: TermJump, edges: []Edge{t.edges[0]}}

			return true
		}
	case TermSwitch:
		c := o.resolve(t.value)
		t.value = c

		if f.values[c].op == OpConst {
			i := uint64(uint32(f.values[c].aux))

			if i >= uint64(len(t.edges)-1) {
				i = uint64(len(t.edges) - 1)
			}

			*t = Term{kind: TermJump, edges: []Edge{t.edges[i]}}

			return true
		}
	}

	return false
}

func sameArgs(o *optimizer, x, y []int32) bool {
	if len(x) != len(y) {
		return false
	}

	for i := range x {
		if o.resolve(x[i]) != o.resolve(y[i]) {
			return false
		}
	}

	return true
}

// paramIndex returns the position of value v among block b's parameters.
func (o *optimizer) paramIndex(b int32, v int32) int {
	for i, p := range o.f.blocks[b].params {
		if p == v {
			return i
		}
	}

	return -1
}

// substitute maps arguments of an edge leaving block b to what they are on
// an incoming edge with args: b's parameters become those.
func (o *optimizer) substitute(b int32, out []int32, in []int32) []int32 {
	res := make([]int32, len(out))

	for i, a := range out {
		a = o.resolve(a)

		if k := o.paramIndex(b, a); k >= 0 {
			res[i] = in[k]
		} else {
			res[i] = a
		}
	}

	return res
}

// localParams reports whether the parameters of block b are used only by
// its own terminator: edges can bypass b without breaking the dominance of
// their definitions.
func (o *optimizer) localParams(b int32) bool {
	f := o.f
	blk := &f.blocks[b]

	if len(blk.params) == 0 {
		return true
	}

	own := map[int32]int32{}
	t := &blk.term

	if t.kind == TermIf || t.kind == TermSwitch {
		own[o.resolve(t.value)]++
	}

	for _, v := range t.values {
		own[o.resolve(v)]++
	}

	for _, e := range t.edges {
		for _, a := range e.args {
			own[o.resolve(a)]++
		}
	}

	for _, p := range blk.params {
		if o.uses[p] != own[p] {
			return false
		}
	}

	return true
}

// thread sends edges that pass a constant to a block that only branches on
// it straight to the branch's target. The block's parameters may be used
// past it, as the loop of a Go function that dispatches on its resume
// point reads its locals in every case: an edge bypassing the block goes
// through a block of its own where they are the values the edge passed,
// and repair makes the uses read whichever definition reaches them.
func (o *optimizer) thread() bool {
	f := o.f
	changed := false
	o.apply()
	o.countUses()

	for h := int32(0); h < int32(len(f.blocks)); h++ {
		blk := &f.blocks[h]

		if blk.dead || h == 0 || len(blk.instrs) != 0 || (blk.term.kind != TermSwitch && blk.term.kind != TermIf) {
			continue
		}

		k := o.paramIndex(h, o.resolve(blk.term.value))

		if k < 0 {
			continue
		}

		local := o.localParams(h)
		bypasses := map[int32]bypass{}
		// h may be its own predecessor: the targets are those of before
		kind := f.blocks[h].term.kind
		targets := slices.Clone(f.blocks[h].term.edges)

		for _, p := range f.blocks[h].preds {
			for i := range f.blocks[p].term.edges {
				e := f.blocks[p].term.edges[i]

				if e.block != h || len(e.args) <= k {
					continue
				}

				c := o.resolve(e.args[k])

				if !o.isConst(c) {
					continue
				}

				x := uint64(uint32(o.constOf(c)))
				var target Edge

				if kind == TermIf {
					target = targets[1]

					if x != 0 {
						target = targets[0]
					}
				} else {
					if x >= uint64(len(targets)-1) {
						x = uint64(len(targets) - 1)
					}

					target = targets[x]
				}

				args := o.substitute(h, target.args, e.args)
				changed = true

				if local {
					f.blocks[p].term.edges[i] = Edge{block: target.block, args: args}

					continue
				}

				b := f.newBlock()
				f.blocks[b].term = Term{kind: TermJump, edges: []Edge{{block: target.block, args: args}}}
				bypasses[b] = bypass{pred: p, args: append([]int32(nil), e.args...)}
				f.blocks[p].term.edges[i] = Edge{block: b}
			}
		}

		if len(bypasses) > 0 {
			f.computePreds()
			o.repair(h, bypasses)
			o.countUses()
		}
	}

	if changed {
		f.computePreds()
	}

	return changed
}

// bypass is a block on an edge that thread sent past a block h: the
// predecessor it leaves and the arguments it passed to h.
type bypass struct {
	pred int32
	args []int32
}

// repair rewrites the uses of h's parameters outside h after thread: the
// value of a parameter in a block is h's on the paths through h and the
// argument of the edge on the paths through a bypass, and a block where
// both meet gets a parameter for it.
func (o *optimizer) repair(h int32, bypasses map[int32]bypass) {
	f := o.f
	params := append([]int32(nil), f.blocks[h].params...)
	index := map[int32]int{}

	for j, p := range params {
		index[p] = j
	}

	memo := map[[2]int32]int32{}
	preds := map[int32][]int32{}
	var value func(b int32, j int) int32

	// seen is a value at the end of block b: a parameter of h stands for
	// the value it has there.
	seen := func(b int32, v int32) int32 {
		v = o.resolve(v)

		if j, ok := index[v]; ok {
			return value(b, j)
		}

		return v
	}

	value = func(b int32, j int) int32 {
		if b == h {
			return params[j]
		}

		if d, ok := bypasses[b]; ok {
			return seen(d.pred, d.args[j])
		}

		key := [2]int32{b, int32(j)}

		if v, ok := memo[key]; ok {
			return v
		}

		from, ok := preds[b]

		if !ok {
			from = unique(f.blocks[b].preds)
			preds[b] = from
		}

		if len(from) == 0 {
			v := f.newValue(OpUndef, f.values[params[j]].typ, -1, 0)
			memo[key] = v

			return v
		}

		if len(from) == 1 {
			v := value(from[0], j)
			memo[key] = v

			return v
		}

		// The argument's place on every edge in is taken first: finding
		// the arguments may give b parameters for other values, as an edge
		// passes one local for another.
		at := len(f.blocks[b].params)
		x := f.newValue(OpParam, f.values[params[j]].typ, b, int64(at))
		f.blocks[b].params = append(f.blocks[b].params, x)
		memo[key] = x

		for _, q := range from {
			for i := range f.blocks[q].term.edges {
				if e := &f.blocks[q].term.edges[i]; e.block == b {
					e.args = append(e.args, x)
				}
			}
		}

		for _, q := range from {
			for i := range f.blocks[q].term.edges {
				if f.blocks[q].term.edges[i].block == b {
					arg := value(q, j)
					f.blocks[q].term.edges[i].args[at] = arg
				}
			}
		}

		return x
	}

	for b := int32(0); b < int32(len(f.blocks)); b++ {
		if b == h || f.blocks[b].dead {
			continue
		}

		_, isBypass := bypasses[b]
		at := func(v int32) int32 {
			if isBypass {
				return seen(bypasses[b].pred, v)
			}

			return seen(b, v)
		}

		for _, v := range f.blocks[b].instrs {
			args := f.args(v)

			for i, a := range args {
				if _, ok := index[o.resolve(a)]; ok {
					args[i] = at(a)
				}
			}
		}

		t := &f.blocks[b].term

		if t.kind == TermIf || t.kind == TermSwitch {
			t.value = at(t.value)
		}

		for i := range t.values {
			t.values[i] = at(t.values[i])
		}

		for i := range t.edges {
			for k, a := range t.edges[i].args {
				t.edges[i].args[k] = at(a)
			}
		}
	}
}

// unique is a list of blocks without repeats, in order.
func unique(blocks []int32) []int32 {
	var out []int32

	for _, b := range blocks {
		if !slices.Contains(out, b) {
			out = append(out, b)
		}
	}

	return out
}

// cleanup removes unreachable blocks, sends edges past blocks that only
// jump, and merges blocks into their only predecessor.
func (o *optimizer) cleanup() bool {
	f := o.f
	changed := false

	// bypass empty jump blocks
	o.countUses()

	for b := range f.blocks {
		blk := &f.blocks[b]

		if blk.dead || b == 0 || len(blk.instrs) != 0 || blk.term.kind != TermJump || blk.term.edges[0].block == int32(b) {
			continue
		}

		if !o.localParams(int32(b)) {
			continue
		}

		next := blk.term.edges[0]

		for _, p := range blk.preds {
			pt := &f.blocks[p].term

			for i := range pt.edges {
				e := &pt.edges[i]

				if e.block == int32(b) {
					args := o.substitute(int32(b), next.args, e.args)
					*e = Edge{block: next.block, args: args}
					changed = true
				}
			}
		}
	}

	if changed {
		f.computePreds()
	}

	changed = o.unreachable() || changed

	// merge a block into its only predecessor ending in a jump to it
	merged := false

	for b := range f.blocks {
		blk := &f.blocks[b]

		if blk.dead || b == 0 || len(blk.preds) != 1 {
			continue
		}

		p := blk.preds[0]
		pb := &f.blocks[p]

		if p == int32(b) || pb.term.kind != TermJump {
			continue
		}

		e := pb.term.edges[0]

		for i, param := range blk.params {
			o.replace(param, o.resolve(e.args[i]))
		}

		for _, v := range blk.instrs {
			f.values[v].block = p
		}

		pb.instrs = append(pb.instrs, blk.instrs...)
		pb.term = blk.term
		*blk = Block{dead: true}

		// successors now come from p
		for _, s := range pb.term.edges {
			sb := &f.blocks[s.block]

			for i, q := range sb.preds {
				if q == int32(b) {
					sb.preds[i] = p
				}
			}
		}

		merged = true
	}

	if merged {
		o.apply()
		f.computePreds()
	}

	return changed || merged
}

func (o *optimizer) unreachable() bool {
	f := o.f
	reached := make([]bool, len(f.blocks))
	work := []int32{0}
	reached[0] = true

	for len(work) > 0 {
		b := work[len(work)-1]
		work = work[:len(work)-1]

		for _, e := range f.blocks[b].term.edges {
			if !reached[e.block] {
				reached[e.block] = true
				work = append(work, e.block)
			}
		}
	}

	changed := false

	for b := range f.blocks {
		if !reached[b] && !f.blocks[b].dead {
			f.blocks[b] = Block{dead: true}
			changed = true
		}
	}

	if changed {
		f.computePreds()
	}

	return changed
}

// trivialParams replaces a parameter that receives one value on every edge,
// apart from itself, by that value.
func (o *optimizer) trivialParams() bool {
	f := o.f
	changed := false

	for b := range f.blocks {
		blk := &f.blocks[b]

		if blk.dead || b == 0 || len(blk.params) == 0 {
			continue
		}

		var keep []bool
		removed := false
		in := o.incoming(int32(b))

		for k, param := range blk.params {
			same := int32(-1)
			trivial := true

			for _, e := range in {
				a := o.resolve(e.args[k])

				if a == param || a == same {
					continue
				}

				if same >= 0 {
					trivial = false

					break
				}

				same = a
			}

			if keep == nil {
				keep = make([]bool, len(blk.params))
			}

			keep[k] = true

			if trivial {
				if same < 0 {
					same = f.newValue(OpUndef, f.values[param].typ, -1, 0)
				}

				o.replace(param, same)
				keep[k] = false
				removed = true
			}
		}

		if !removed {
			continue
		}

		o.dropParams(int32(b), keep)
		changed = true
	}

	if changed {
		o.apply()
	}

	return changed
}

// dropParams removes the parameters of block b not kept, and their
// arguments on every incoming edge.
func (o *optimizer) dropParams(b int32, keep []bool) {
	f := o.f
	blk := &f.blocks[b]
	var params []int32

	for i, p := range blk.params {
		if keep[i] {
			f.values[p].aux = int64(len(params))
			params = append(params, p)
		}
	}

	seen := map[int32]bool{}

	for _, p := range blk.preds {
		if seen[p] {
			continue
		}

		seen[p] = true
		pt := &f.blocks[p].term

		for i := range pt.edges {
			e := &pt.edges[i]

			if e.block != b {
				continue
			}

			var args []int32

			for j, a := range e.args {
				if keep[j] {
					args = append(args, a)
				}
			}

			e.args = args
		}
	}

	blk.params = params
}

// deadCode removes pure values nothing uses and parameters only their own
// edges use.
func (o *optimizer) deadCode() {
	f := o.f

	for {
		o.countUses()
		removed := false

		for b := range f.blocks {
			blk := &f.blocks[b]

			if blk.dead {
				continue
			}

			out := make([]int32, 0, len(blk.instrs))

			for i := len(blk.instrs) - 1; i >= 0; i-- {
				v := blk.instrs[i]

				if o.uses[v] == 0 && !hasEffect(f.values[v].op) {
					for _, a := range f.args(v) {
						o.uses[a]--
					}

					removed = true

					continue
				}

				out = append(out, v)
			}

			for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
				out[i], out[j] = out[j], out[i]
			}

			blk.instrs = out
		}

		// parameters used by nothing but edges into their own block chain
		for b := range f.blocks {
			blk := &f.blocks[b]

			if blk.dead || b == 0 || len(blk.params) == 0 {
				continue
			}

			keep := make([]bool, len(blk.params))
			drop := false
			in := o.incoming(int32(b))

			for k, p := range blk.params {
				keep[k] = o.uses[p] > selfUses(in, k, p)

				if !keep[k] {
					drop = true
				}
			}

			if drop {
				o.dropParams(int32(b), keep)
				removed = true
			}
		}

		if !removed {
			return
		}
	}
}

// selfUses counts the uses of parameter p of block b that are its own
// argument on the edges in, into b.
func selfUses(in []*Edge, k int, p int32) int32 {
	n := int32(0)

	for _, e := range in {
		if k < len(e.args) && e.args[k] == p {
			n++
		}
	}

	return n
}

// incoming lists the edges into block b, each once.
func (o *optimizer) incoming(b int32) []*Edge {
	f := o.f
	var out []*Edge
	preds := f.blocks[b].preds

	for i, q := range preds {
		if slices.Contains(preds[:i], q) {
			continue
		}

		for e := range f.blocks[q].term.edges {
			if f.blocks[q].term.edges[e].block == b {
				out = append(out, &f.blocks[q].term.edges[e])
			}
		}
	}

	return out
}
