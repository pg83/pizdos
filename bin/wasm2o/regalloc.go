package main

import (
	"fmt"
	"math/bits"
	"os"
	"sort"
)

// Linear scan register allocation after Wimmer and Mössenböck: lifetime
// intervals with holes over a linear block order, splitting instead of
// spilling whole intervals, and moves at split positions and block edges.
//
// Every instruction occupies two positions: its uses read at the even one,
// its definitions write at the odd one. Every block starts with a free even
// position for the moves that resolve its incoming edges.

type lrange struct {
	from, to int32
}

type interval struct {
	vreg   VReg
	class  uint8
	ranges []lrange
	uses   []int32
	// reg is the assigned register, or noReg for a spilled interval.
	reg VReg
	// parent is the interval this one was split from; children, on the
	// parent, lists all its split children in order.
	parent   *interval
	children []*interval
	hint     VReg
	fixed    bool
	// cursor indexes the first range not wholly before the scan position,
	// which only grows.
	cursor int
}

const maxPos = int32(1 << 30)

func (it *interval) start() int32 {
	return it.ranges[0].from
}

func (it *interval) end() int32 {
	return it.ranges[len(it.ranges)-1].to
}

func (it *interval) covers(p int32) bool {
	k := sort.Search(len(it.ranges), func(i int) bool { return it.ranges[i].to > p })

	return k < len(it.ranges) && it.ranges[k].from <= p
}

// coversAt is covers for the scan position: p never decreases between
// calls.
func (it *interval) coversAt(p int32) bool {
	for it.cursor < len(it.ranges) && it.ranges[it.cursor].to <= p {
		it.cursor++
	}

	return it.cursor < len(it.ranges) && it.ranges[it.cursor].from <= p
}

func (it *interval) endsBefore(p int32) bool {
	return it.end() <= p
}

// intersection returns the first position both intervals cover, or maxPos;
// it starts from both cursors.
func (it *interval) intersection(o *interval) int32 {
	if it.cursor > len(it.ranges) {
		it.cursor = len(it.ranges)
	}

	i, j := it.cursor, o.cursor

	for i < len(it.ranges) && j < len(o.ranges) {
		a, b := it.ranges[i], o.ranges[j]

		if a.to <= b.from {
			i++

			continue
		}

		if b.to <= a.from {
			j++

			continue
		}

		if a.from > b.from {
			return a.from
		}

		return b.from
	}

	return maxPos
}

// prevUse returns the last use before p, or -1.
func (it *interval) prevUse(p int32) int32 {
	k := sort.Search(len(it.uses), func(i int) bool { return it.uses[i] >= p })

	if k > 0 {
		return it.uses[k-1]
	}

	return -1
}

func (it *interval) nextUse(p int32) int32 {
	k := sort.Search(len(it.uses), func(i int) bool { return it.uses[i] >= p })

	if k < len(it.uses) {
		return it.uses[k]
	}

	return maxPos
}

// addRange adds [from, to) during the backward walk, which keeps the ranges
// in decreasing order: they arrive in decreasing order and merge with the
// last one when they touch. finish puts them in order.
func (it *interval) addRange(from, to int32) {
	if n := len(it.ranges); n > 0 {
		last := &it.ranges[n-1]

		if to >= last.from {
			if from < last.from {
				last.from = from
			}

			if to > last.to {
				last.to = to
			}

			return
		}
	}

	it.ranges = append(it.ranges, lrange{from, to})
}

// cutAt shortens the earliest range, which covers a definition at p, to
// start there; it reports false when no range covers p.
func (it *interval) cutAt(p int32) bool {
	n := len(it.ranges)

	if n > 0 && it.ranges[n-1].from <= p && it.ranges[n-1].to > p {
		it.ranges[n-1].from = p

		return true
	}

	return false
}

func (it *interval) finish() {
	r := it.ranges

	for i, j := 0, len(r)-1; i < j; i, j = i+1, j-1 {
		r[i], r[j] = r[j], r[i]
	}

	sort.Slice(it.uses, func(i, j int) bool { return it.uses[i] < it.uses[j] })
	it.uses = dedup(it.uses)
}

// split cuts the interval at p: the returned child covers p and later.
func (it *interval) split(p int32) *interval {
	child := &interval{vreg: it.vreg, class: it.class, reg: noReg, hint: it.hint}
	k := 0

	for k < len(it.ranges) && it.ranges[k].to <= p {
		k++
	}

	if k < len(it.ranges) && it.ranges[k].from < p {
		child.ranges = append(child.ranges, lrange{p, it.ranges[k].to})
		child.ranges = append(child.ranges, it.ranges[k+1:]...)
		it.ranges = append(it.ranges[:k:k], lrange{it.ranges[k].from, p})
	} else {
		child.ranges = append(child.ranges, it.ranges[k:]...)
		it.ranges = it.ranges[:k:k]
	}

	if it.cursor > len(it.ranges) {
		it.cursor = len(it.ranges)
	}

	u := sort.Search(len(it.uses), func(i int) bool { return it.uses[i] >= p })
	child.uses = append(child.uses, it.uses[u:]...)
	it.uses = it.uses[:u:u]
	root := it

	if it.parent != nil {
		root = it.parent
	}

	child.parent = root
	idx := sort.Search(len(root.children), func(i int) bool { return root.children[i].start() > p })
	root.children = append(root.children, nil)
	copy(root.children[idx+1:], root.children[idx:])
	root.children[idx] = child

	return child
}

type bitset []uint64

func newBitset(n int) bitset {
	return make(bitset, (n+63)/64)
}

func (s bitset) set(i int)         { s[i>>6] |= 1 << uint(i&63) }
func (s bitset) clear(i int)       { s[i>>6] &^= 1 << uint(i&63) }
func (s bitset) has(i int) bool    { return s[i>>6]&(1<<uint(i&63)) != 0 }
func (s bitset) copyFrom(o bitset) { copy(s, o) }

// union adds o to s and reports whether s changed.
func (s bitset) union(o bitset) bool {
	changed := false

	for i := range s {
		n := s[i] | o[i]

		if n != s[i] {
			s[i] = n
			changed = true
		}
	}

	return changed
}

func (s bitset) each(cb func(int)) {
	for i, w := range s {
		for w != 0 {
			b := bits.TrailingZeros64(w)
			cb(i*64 + b)
			w &= w - 1
		}
	}
}

type allocator struct {
	mf        *MFunc
	order     []int32
	blockFrom []int32
	blockTo   []int32
	instPos   [][]int32
	liveIn    []bitset
	liveOut   []bitset
	intervals []*interval
	fixed     [32]*interval
	unhandled []*interval
	active    []*interval
	inactive  []*interval
	slots     []int32
	nslots    int32
	// depthAt maps a block boundary position to its loop depth.
	posBlock []int32
	// callPos lists the positions of calls, for spill decisions.
	used [32]bool
}

var dumpIntervals = os.Getenv("W2X_INTERVALS") != ""

// dumping is set while the function named by W2X_DUMP is compiled.
var dumping bool

func allocate(mf *MFunc) *allocator {
	a := &allocator{mf: mf}
	a.linearize()
	a.liveness()
	a.buildIntervals()
	a.scan()

	if dumpIntervals && dumping {
		a.dumpIntervals()
	}

	a.resolve()

	return a
}

func (a *allocator) dumpIntervals() {
	for x, it := range a.intervals {
		if it == nil {
			continue
		}

		fmt.Fprintf(os.Stderr, "%s slot=%d:", it.vreg, a.slots[x])

		for _, c := range append([]*interval{it}, it.children...) {
			fmt.Fprintf(os.Stderr, " [%s %v uses=%v]", locName(a.loc(c)), c.ranges, c.uses)
		}

		fmt.Fprintln(os.Stderr)
	}
}

// linearize orders blocks in reverse postorder of the machine CFG and
// numbers instruction positions.
func (a *allocator) linearize() {
	mf := a.mf
	n := len(mf.blocks)
	visited := make([]bool, n)
	var post []int32

	type frame struct {
		b int32
		i int
	}

	stack := []frame{{0, 0}}
	visited[0] = true

	for len(stack) > 0 {
		top := &stack[len(stack)-1]
		succs := mf.blocks[top.b].succs

		if top.i < len(succs) {
			s := succs[len(succs)-1-top.i]
			top.i++

			if !visited[s] {
				visited[s] = true
				stack = append(stack, frame{s, 0})
			}

			continue
		}

		post = append(post, top.b)
		stack = stack[:len(stack)-1]
	}

	for i := len(post) - 1; i >= 0; i-- {
		a.order = append(a.order, post[i])
	}

	a.blockFrom = make([]int32, n)
	a.blockTo = make([]int32, n)
	a.instPos = make([][]int32, n)
	pos := int32(0)

	for _, b := range a.order {
		a.blockFrom[b] = pos
		pos += 2
		insts := mf.blocks[b].insts
		a.instPos[b] = make([]int32, len(insts))

		for i := range insts {
			a.instPos[b][i] = pos
			pos += 2
		}

		a.blockTo[b] = pos
	}

	a.posBlock = make([]int32, pos/2+1)

	for _, b := range a.order {
		for p := a.blockFrom[b]; p < a.blockTo[b]; p += 2 {
			a.posBlock[p/2] = b
		}
	}
}

func (a *allocator) vindex(r VReg) int {
	return int(r - firstVirtual)
}

func (a *allocator) liveness() {
	mf := a.mf
	n := len(mf.blocks)
	nv := mf.nregs
	gen := make([]bitset, n)
	kill := make([]bitset, n)
	a.liveIn = make([]bitset, n)
	a.liveOut = make([]bitset, n)

	for _, b := range a.order {
		g := newBitset(nv)
		k := newBitset(nv)

		for i := range mf.blocks[b].insts {
			mf.blocks[b].insts[i].forEachOperand(func(r *VReg, role int) {
				if r.isPhys() || *r == noReg {
					return
				}

				x := a.vindex(*r)

				if role&roleUse != 0 && role != roleTemp && !k.has(x) {
					g.set(x)
				}

				if role&roleDef != 0 || role == roleTemp {
					k.set(x)
				}
			})
		}

		gen[b] = g
		kill[b] = k
		a.liveIn[b] = newBitset(nv)
		a.liveOut[b] = newBitset(nv)
	}

	tmp := newBitset(nv)

	for changed := true; changed; {
		changed = false

		for i := len(a.order) - 1; i >= 0; i-- {
			b := a.order[i]
			out := a.liveOut[b]

			for _, s := range mf.blocks[b].succs {
				out.union(a.liveIn[s])
			}

			for j := range tmp {
				tmp[j] = gen[b][j] | (out[j] &^ kill[b][j])
			}

			if a.liveIn[b].union(tmp) {
				changed = true
			}
		}
	}
}

func (a *allocator) interval(r VReg) *interval {
	if r.isPhys() {
		if a.fixed[r] == nil {
			a.fixed[r] = &interval{vreg: r, reg: r, fixed: true}
		}

		return a.fixed[r]
	}

	x := a.vindex(r)

	if a.intervals[x] == nil {
		a.intervals[x] = &interval{vreg: r, class: a.mf.class[x], reg: noReg, hint: noReg}
	}

	return a.intervals[x]
}

// allocatable reports registers the allocator may hand out.
func allocatable(r VReg) bool {
	return r != RSP && r != RBP && r != R14 && r != ctxReg && r != memReg && r != XMM0+15
}

func (a *allocator) buildIntervals() {
	mf := a.mf
	a.intervals = make([]*interval, mf.nregs)

	for i := len(a.order) - 1; i >= 0; i-- {
		b := a.order[i]
		from, to := a.blockFrom[b], a.blockTo[b]

		a.liveOut[b].each(func(x int) {
			a.interval(VReg(x)+firstVirtual).addRange(from, to)
		})

		insts := mf.blocks[b].insts

		for j := len(insts) - 1; j >= 0; j-- {
			in := &insts[j]
			p := a.instPos[b][j]

			for _, r := range in.clobberSet() {
				a.interval(r).addRange(p+1, p+2)
			}

			// Operands of a parallel move may live in stack slots.
			needReg := in.op != mParMove

			in.forEachOperand(func(r *VReg, role int) {
				if *r == noReg || (r.isPhys() && !allocatable(*r)) {
					return
				}

				it := a.interval(*r)

				switch {
				case role == roleTemp:
					// a scratch register is held through the whole
					// instruction: it is never evicted inside it
					it.addRange(p, p+2)
					it.uses = append(it.uses, p, p+1)
				case role&roleDef != 0:
					// The range from the block start is cut at the
					// definition; a dead definition still gets a position.
					if !it.cutAt(p + 1) {
						it.addRange(p+1, p+2)
					}

					if needReg || r.isPhys() {
						it.uses = append(it.uses, p+1)
					}
				}
			})

			in.forEachOperand(func(r *VReg, role int) {
				if *r == noReg || (r.isPhys() && !allocatable(*r)) {
					return
				}

				if role&roleUse != 0 && role != roleTemp {
					it := a.interval(*r)
					it.addRange(from, p+1)

					if needReg || r.isPhys() {
						it.uses = append(it.uses, p)
					}
				}
			})

			if in.op == mMov || in.op == mFMov {
				a.hint(in.dst, in.src)
			}

			for _, m := range in.moves {
				a.hint(m.dst, m.src)
			}
		}
	}

	for _, it := range a.intervals {
		if it != nil {
			it.finish()
		}
	}

	for _, it := range a.fixed {
		if it != nil {
			it.finish()
		}
	}
}

func dedup(xs []int32) []int32 {
	if len(xs) < 2 {
		return xs
	}

	out := xs[:1]

	for _, x := range xs[1:] {
		if x != out[len(out)-1] {
			out = append(out, x)
		}
	}

	return out
}

func (a *allocator) hint(dst, src VReg) {
	if dst.isPhys() && src.isPhys() {
		return
	}

	if !dst.isPhys() {
		it := a.interval(dst)

		if it.hint == noReg {
			it.hint = src
		}
	}

	if !src.isPhys() {
		it := a.interval(src)

		if it.hint == noReg {
			it.hint = dst
		}
	}
}

func (a *allocator) scan() {
	for _, it := range a.intervals {
		if it != nil && len(it.ranges) > 0 {
			a.unhandled = append(a.unhandled, it)
		}
	}

	sort.Slice(a.unhandled, func(i, j int) bool {
		return a.unhandled[i].start() < a.unhandled[j].start()
	})

	for r := range a.fixed {
		if a.fixed[r] != nil && len(a.fixed[r].ranges) > 0 {
			a.inactive = append(a.inactive, a.fixed[r])
		}
	}

	a.slots = make([]int32, a.mf.nregs)

	for i := range a.slots {
		a.slots[i] = -1
	}

	for len(a.unhandled) > 0 {
		cur := a.unhandled[0]
		a.unhandled = a.unhandled[1:]
		pos := cur.start()
		a.advance(pos)

		if !a.tryFree(cur) {
			a.allocateBlocked(cur)
		}

		if cur.reg != noReg {
			a.active = append(a.active, cur)
		}
	}
}

func (a *allocator) advance(pos int32) {
	active := a.active[:0]
	var moved []*interval

	for _, it := range a.active {
		switch {
		case it.endsBefore(pos):
		case !it.coversAt(pos):
			moved = append(moved, it)
		default:
			active = append(active, it)
		}
	}

	a.active = active
	inactive := a.inactive[:0]

	for _, it := range a.inactive {
		switch {
		case it.endsBefore(pos):
		case it.coversAt(pos):
			a.active = append(a.active, it)
		default:
			inactive = append(inactive, it)
		}
	}

	a.inactive = append(inactive, moved...)
}

func (a *allocator) candidates(class uint8) []VReg {
	if class == classXMM {
		return allocXMMs
	}

	return allocGPRs
}

// hintReg returns the register the hint holds at the start of cur.
func (a *allocator) hintReg(cur *interval) VReg {
	h := cur.hint

	if h == noReg {
		return noReg
	}

	if h.isPhys() {
		return h
	}

	it := a.intervals[a.vindex(h)]

	if it == nil {
		return noReg
	}

	// the hint's register around the start of cur: the move is just before
	p := cur.start()

	for _, c := range append([]*interval{it}, it.children...) {
		if c.reg != noReg && (c.covers(p-1) || c.covers(p) || c.end() == p || c.end() == p-1) {
			return c.reg
		}
	}

	return noReg
}

func (a *allocator) tryFree(cur *interval) bool {
	var freeUntil [32]int32

	for _, r := range a.candidates(cur.class) {
		freeUntil[r] = maxPos
	}

	for _, it := range a.active {
		freeUntil[it.reg] = 0
	}

	for _, it := range a.inactive {
		if freeUntil[it.reg] == 0 {
			continue
		}

		if x := it.intersection(cur); x < freeUntil[it.reg] {
			freeUntil[it.reg] = x
		}
	}

	start, end := cur.start(), cur.end()

	// A register free for only a part is usable when the interval can be
	// split at an instruction boundary inside that part.
	for _, r := range a.candidates(cur.class) {
		if freeUntil[r] < end {
			freeUntil[r] &^= 1

			if freeUntil[r] <= start {
				freeUntil[r] = 0
			}
		}
	}

	reg := noReg

	if h := a.hintReg(cur); h != noReg && allocatable(h) && a.regClass(h) == cur.class && freeUntil[h] >= end {
		reg = h
	}

	if reg == noReg {
		// The first register free for the whole interval, else the one free
		// the longest.
		best := noReg

		for _, r := range a.candidates(cur.class) {
			if freeUntil[r] >= end {
				reg = r

				break
			}

			if best == noReg || freeUntil[r] > freeUntil[best] {
				best = r
			}
		}

		if reg == noReg {
			reg = best
		}
	}

	if freeUntil[reg] == 0 {
		return false
	}

	cur.reg = reg
	a.used[reg] = true

	if freeUntil[reg] < end {
		at := a.splitPos(start+1, freeUntil[reg])
		a.addUnhandled(cur.split(at))
	}

	return true
}

func (a *allocator) regClass(r VReg) uint8 {
	if r.isXMM() {
		return classXMM
	}

	return classGPR
}

func (a *allocator) allocateBlocked(cur *interval) {
	var nextUse, blockPos [32]int32

	for _, r := range a.candidates(cur.class) {
		nextUse[r] = maxPos
		blockPos[r] = maxPos
	}

	start := cur.start()

	for _, it := range a.active {
		if it.fixed {
			nextUse[it.reg] = 0
			blockPos[it.reg] = 0

			continue
		}

		u := it.nextUse(start)

		if u == start {
			// needed at this very position: cannot be taken
			u = 0
		}

		if u < nextUse[it.reg] {
			nextUse[it.reg] = u
		}
	}

	for _, it := range a.inactive {
		x := it.intersection(cur)

		if x == maxPos {
			continue
		}

		if it.fixed {
			if x < blockPos[it.reg] {
				blockPos[it.reg] = x
			}

			if x < nextUse[it.reg] {
				nextUse[it.reg] = x
			}

			continue
		}

		if u := it.nextUse(start); u < nextUse[it.reg] {
			nextUse[it.reg] = u
		}
	}

	reg := noReg

	for _, r := range a.candidates(cur.class) {
		if reg == noReg || nextUse[r] > nextUse[reg] {
			reg = r
		}
	}

	firstUse := cur.nextUse(start)

	if firstUse > nextUse[reg] || blockPos[reg] <= start+1 || nextUse[reg] == 0 {
		// Every register is needed before cur needs one: spill cur up to its
		// first use.
		a.spill(cur)

		if firstUse < maxPos {
			at := a.splitPos(start+1, firstUse)

			if at < 0 {
				throwFmt("internal error: no register for %s at %d", cur.vreg, start)
			}

			a.addUnhandled(cur.split(at))
		}

		return
	}

	cur.reg = reg
	a.used[reg] = true

	if blockPos[reg] < cur.end() {
		at := a.splitPos(start+1, blockPos[reg])

		if at < 0 {
			throwFmt("internal error: no split position for %s before %d", cur.vreg, blockPos[reg])
		}

		a.addUnhandled(cur.split(at))
	}

	// Evict the other intervals holding reg where they overlap cur.
	active := a.active[:0]

	for _, it := range a.active {
		if it.reg != reg || it.fixed {
			active = append(active, it)

			continue
		}

		a.evict(it, start)
	}

	a.active = active
	inactive := a.inactive[:0]

	for _, it := range a.inactive {
		if it.reg != reg || it.fixed {
			inactive = append(inactive, it)

			continue
		}

		x := it.intersection(cur)

		if x == maxPos {
			inactive = append(inactive, it)

			continue
		}

		lo := it.prevUse(x) + 1

		if lo < start {
			lo = start
		}

		at := a.splitPos(lo, x)

		if at < 0 || at <= it.start() {
			a.evict(it, start)

			continue
		}

		child := it.split(at)
		a.evict(child, child.start())
		inactive = append(inactive, it)
	}

	a.inactive = inactive
}

// evict takes the register from it at pos, which it may cover: the part
// from there is spilled until its next use, which goes back to the
// unhandled list.
func (a *allocator) evict(it *interval, pos int32) {
	tail := it

	if it.start() < pos {
		var at int32

		if pos&1 != 0 && it.prevUse(pos) == pos-1 {
			// Read by the instruction that defines the new holder of the
			// register: spilled from right after the read, the store goes
			// before the instruction.
			at = pos
		} else {
			lo := it.prevUse(pos) + 1

			if lo <= it.start() {
				lo = it.start() + 1
			}

			at = a.splitPos(lo, pos)
		}

		if at < 0 || at <= it.start() {
			throwFmt("internal error: cannot evict %s at %d", it.vreg, pos)
		}

		tail = it.split(at)
	}

	a.spill(tail)
	u := tail.nextUse(pos)

	if u == maxPos {
		return
	}

	lo := pos

	if lo <= tail.start() {
		lo = tail.start() + 1
	}

	at := a.splitPos(lo, u)

	if at < 0 || at <= tail.start() {
		throwFmt("internal error: cannot reload %s before %d", it.vreg, u)
	}

	a.addUnhandled(tail.split(at))
}

func (a *allocator) spill(it *interval) {
	it.reg = noReg
	x := a.vindex(it.vreg)

	if a.slots[x] < 0 {
		a.slots[x] = a.nslots
		a.nslots++
	}
}

// splitPos chooses an instruction boundary between min and max, both
// included: the start of the block with the lowest loop depth in that
// range, else the latest boundary. It returns -1 when there is none.
func (a *allocator) splitPos(min, max int32) int32 {
	max &^= 1

	if min&1 != 0 {
		min++
	}

	if min > max {
		return -1
	}

	best := max
	bestDepth := a.mf.blocks[a.posBlock[max/2]].depth

	for p := max; p > min; {
		b := a.posBlock[(p-1)/2]
		from := a.blockFrom[b]

		if from < min {
			break
		}

		if d := a.mf.blocks[b].depth; d < bestDepth {
			best = from
			bestDepth = d
		}

		p = from
	}

	return best
}

func (a *allocator) addUnhandled(it *interval) {
	s := it.start()
	k := sort.Search(len(a.unhandled), func(i int) bool { return a.unhandled[i].start() > s })
	a.unhandled = append(a.unhandled, nil)
	copy(a.unhandled[k+1:], a.unhandled[k:])
	a.unhandled[k] = it
}

// location of vreg r at position p: a register, or a stack slot encoded as
// a negative value below noReg.
func (a *allocator) location(r VReg, p int32) VReg {
	it := a.intervals[a.vindex(r)]

	if it.covers(p) {
		return a.loc(it)
	}

	for _, c := range it.children {
		if c.covers(p) {
			return a.loc(c)
		}
	}

	throwFmt("internal error: v%d not live at %d", r, p)

	return noReg
}

func (a *allocator) loc(it *interval) VReg {
	if it.reg != noReg {
		return it.reg
	}

	return slotLoc(a.slots[a.vindex(it.vreg)])
}

func slotLoc(slot int32) VReg {
	return VReg(-2 - slot)
}

func isSlot(r VReg) bool {
	return r < noReg
}

func slotOf(r VReg) int32 {
	return int32(-2 - r)
}

// resolve rewrites operands to registers and inserts the moves for split
// intervals inside blocks and on edges.
func (a *allocator) resolve() {
	mf := a.mf
	// moves before the instruction at each position p, keyed 2p; the stores
	// of values spilled right after their read by the instruction, keyed
	// 2p+1, come after them: their register may only just have been
	// loaded
	inner := map[int32][]Move{}

	for x, it := range a.intervals {
		if it == nil {
			continue
		}

		all := append([]*interval{it}, it.children...)

		for i := 1; i < len(all); i++ {
			prev, c := all[i-1], all[i]
			p := c.start()

			if prev.end() != p {
				// a hole between them: data flow resolution handles it
				continue
			}

			if b := a.posBlock[p/2]; a.blockFrom[b] == p {
				continue
			}

			src, dst := a.loc(prev), a.loc(c)
			key := 2 * p

			if p&1 != 0 {
				// a spill right after a read: store before the instruction
				if !isSlot(dst) {
					throwFmt("internal error: %s split at odd position %d into a register", it.vreg, p)
				}

				key = 2*(p-1) + 1
			}

			if src != dst {
				inner[key] = append(inner[key], Move{dst, src})
			}

			_ = x
		}
	}

	// edge moves: at the end of a block with one successor, else at the
	// start of the successor
	atEnd := map[int32][]Move{}
	atStart := map[int32][]Move{}

	for _, b := range a.order {
		blk := &mf.blocks[b]

		for _, s := range blk.succs {
			var moves []Move

			a.liveIn[s].each(func(x int) {
				r := VReg(x) + firstVirtual
				src := a.location(r, a.blockTo[b]-1)
				dst := a.location(r, a.blockFrom[s])

				if src != dst {
					moves = append(moves, Move{dst, src})
				}
			})

			if len(moves) == 0 {
				continue
			}

			if len(blk.succs) == 1 {
				atEnd[b] = append(atEnd[b], moves...)
			} else if len(mf.blocks[s].preds) == 1 {
				atStart[s] = append(atStart[s], moves...)
			} else {
				throwFmt("internal error: critical edge B%d->B%d needs moves", b, s)
			}
		}
	}

	origs := make([][]origInst, len(mf.blocks))

	for _, b := range a.order {
		blk := &mf.blocks[b]
		var out []MInst
		var orig []origInst

		if mv := atStart[b]; len(mv) > 0 {
			out = append(out, parMove(mv))
			orig = append(orig, origInst{})
		}

		branches := firstBranch(blk.insts)

		for i := range blk.insts {
			p := a.instPos[b][i]

			if i == branches {
				if mv := atEnd[b]; len(mv) > 0 {
					out = append(out, parMove(mv))
					orig = append(orig, origInst{})
				}
			}

			for _, key := range [2]int32{2 * p, 2*p + 1} {
				if mv := inner[key]; len(mv) > 0 {
					out = append(out, parMove(mv))
					orig = append(orig, origInst{})
				}
			}

			in := blk.insts[i]
			in.uses = append([]VReg(nil), in.uses...)
			in.defs = append([]VReg(nil), in.defs...)
			in.temps = append([]VReg(nil), in.temps...)
			in.moves = append([]Move(nil), in.moves...)
			var ops []origOperand

			in.forEachOperand(func(r *VReg, role int) {
				if *r == noReg {
					return
				}

				if r.isPhys() {
					ops = append(ops, origOperand{*r, *r, role})

					return
				}

				at := p

				if role == roleDef {
					at = p + 1
				}

				v := *r
				*r = a.location(v, at)
				ops = append(ops, origOperand{v, *r, role})

				if isSlot(*r) && in.op != mParMove {
					throwFmt("internal error: operand of %s in a stack slot", in.String())
				}
			})

			if in.op == mParMove {
				in.moves = dropSelfMoves(in.moves)
			}

			out = append(out, in)
			orig = append(orig, origInst{ops: ops})
		}

		if branches == len(blk.insts) {
			if mv := atEnd[b]; len(mv) > 0 {
				out = append(out, parMove(mv))
				orig = append(orig, origInst{})
			}
		}

		blk.insts = out
		origs[b] = orig
	}

	if checkAlloc {
		a.check(origs)
	}
}

func dropSelfMoves(moves []Move) []Move {
	out := moves[:0]

	for _, m := range moves {
		if m.dst != m.src {
			out = append(out, m)
		}
	}

	return out
}

func parMove(moves []Move) MInst {
	in := inst(mParMove)
	in.moves = moves

	return in
}

// firstBranch returns the index of the branch sequence ending a block.
func firstBranch(insts []MInst) int {
	i := len(insts)

	for i > 0 {
		switch insts[i-1].op {
		case mJmp, mJcc, mSwitch, mRet, mTrap:
			i--

			continue
		}

		break
	}

	return i
}

// calleeSavedUsed lists the callee-saved registers the function writes.
func (a *allocator) calleeSavedUsed() []VReg {
	var out []VReg

	for _, r := range calleeSaved {
		if a.used[r] {
			out = append(out, r)
		}
	}

	return out
}
