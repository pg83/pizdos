package main

type TailRangeGroup struct {
	start  int
	src    VReg
	expr   AddressExpression
	checks []int
}

type TailRangeRegion struct {
	groups     []*TailRangeGroup
	start, end int
}

func copyMachineInst(in MInst) MInst {
	in.uses = append([]VReg(nil), in.uses...)
	in.defs = append([]VReg(nil), in.defs...)
	in.temps = append([]VReg(nil), in.temps...)
	in.moves = append([]Move(nil), in.moves...)
	in.table = append([]int32(nil), in.table...)

	return in
}

// In padded mode p <= memory.size guarantees the next 64 KiB physically.
// Clone only the overlapping spans from a group's first to last check.
// The checked copy rejoins the shared continuation immediately after the
// last check, before the final memory access. Calls and terminators outside
// those spans are therefore never copied into a fallback.
// The fast copy retains i32 address arithmetic and drops covered checks;
// entry into the tail itself therefore remains supported by the cold copy.
func (l *lowerer) versionTailRanges() {
	f := l.f
	mf := l.mf
	ranges := &AddressExpressions{f: f, cache: make([]AddressExpression, len(f.values)), seen: make([]bool, len(f.values))}
	values := make([]int32, mf.nregs)

	for j := range values {
		values[j] = -1
	}

	for v, reg := range l.vregs {
		if reg >= firstVirtual {
			values[reg-firstVirtual] = int32(v)
		}
	}

	address := func(reg VReg) AddressExpression {
		if reg >= firstVirtual && values[reg-firstVirtual] >= 0 {
			return ranges.of(values[reg-firstVirtual], 0)
		}

		return addressAtom(-2 - int32(reg))
	}
	n := len(mf.blocks)
	changed := false

	for b := 0; b < n; b++ {
		original := mf.blocks[b]
		active := map[[4]AddressTerm][]*TailRangeGroup{}
		var groups []*TailRangeGroup

		for j, in := range original.insts {
			if in.op != mCheck {
				switch in.op {
				case mCall, mCallInd, mParMove, mGrowMem, mMemCopy, mMemFill, mMemOp, mCopyForward:
					clear(active)
				}

				continue
			}

			expr := address(in.src)
			var group *TailRangeGroup

			for _, prior := range active[expr.terms] {
				delta := uint64(expr.offset - prior.expr.offset)

				if delta+uint64(in.imm) <= l.m.memoryTail {
					group = prior

					break
				}
			}

			if group == nil {
				group = &TailRangeGroup{start: j, src: in.src, expr: expr}
				active[expr.terms] = append(active[expr.terms], group)
				groups = append(groups, group)
			}

			group.checks = append(group.checks, j)
		}

		selected := groups[:0]
		skip := make([]bool, len(original.insts))

		for _, group := range groups {
			if len(group.checks) < 2 {
				continue
			}

			selected = append(selected, group)

			for _, j := range group.checks {
				skip[j] = true
			}
		}

		if len(selected) == 0 {
			continue
		}

		changed = true
		var regions []TailRangeRegion

		for _, group := range selected {
			end := group.checks[len(group.checks)-1] + 1

			if len(regions) == 0 || group.start > regions[len(regions)-1].end {
				regions = append(regions, TailRangeRegion{start: group.start, end: end})
			}

			region := &regions[len(regions)-1]
			region.end = max(region.end, end)
			region.groups = append(region.groups, group)
		}

		jump := func(target int32) MInst {
			in := inst(mJmp)
			in.target = target

			return in
		}
		continuation := int32(b)
		start := 0

		for r, region := range regions {
			guards := make([]int32, len(region.groups))
			fast := make([]int32, len(region.groups))
			cold := make([]int32, len(region.groups))

			for j := range region.groups {
				guards[j] = int32(len(mf.blocks))
				mf.blocks = append(mf.blocks, MBlock{id: -1, depth: original.depth})
				fast[j] = int32(len(mf.blocks))
				mf.blocks = append(mf.blocks, MBlock{id: -1, depth: original.depth})
				cold[j] = int32(len(mf.blocks))
				mf.blocks = append(mf.blocks, MBlock{id: -1, depth: original.depth, cold: true})
			}

			prefix := append([]MInst(nil), original.insts[start:region.start]...)
			mf.blocks[continuation].insts = append(prefix, jump(guards[0]))
			mf.blocks[continuation].succs = []int32{guards[0]}
			continuation = int32(len(mf.blocks))
			mf.blocks = append(mf.blocks, MBlock{id: -1, depth: original.depth})

			for j, group := range region.groups {
				end := region.end
				nextFast, nextCold := continuation, continuation

				if j+1 < len(region.groups) {
					end = region.groups[j+1].start
					nextFast, nextCold = guards[j+1], cold[j+1]
				}

				cmp := inst(mCmpMem)
				cmp.src, cmp.mem, cmp.size = group.src, memAt(ctxReg, offMemSize), 8
				branch := inst(mJcc)
				branch.cc, branch.target = ccG, cold[j]
				mf.blocks[guards[j]].insts = []MInst{cmp, branch, jump(fast[j])}
				mf.blocks[guards[j]].succs = []int32{fast[j], cold[j]}

				if l.m.tailProfile {
					counter := l.tailCounter(0, 1)
					mf.blocks[guards[j]].insts = append([]MInst{counter}, mf.blocks[guards[j]].insts...)
					miss := int32(len(mf.blocks))
					mf.blocks = append(mf.blocks, MBlock{id: -1, depth: original.depth, cold: true, insts: []MInst{l.tailCounter(8, 1), jump(cold[j])}, succs: []int32{cold[j]}})
					mf.blocks[guards[j]].insts[2].target = miss
					mf.blocks[guards[j]].succs[1] = miss
				}

				var fastBody []MInst
				var coldBody []MInst
				omitted := int64(0)

				for k := group.start; k < end; k++ {
					in := original.insts[k]
					coldBody = append(coldBody, copyMachineInst(in))

					if skip[k] {
						omitted++
					} else {
						fastBody = append(fastBody, copyMachineInst(in))
					}
				}

				if l.m.tailProfile {
					fastBody = append([]MInst{l.tailCounter(16, omitted)}, fastBody...)
				}

				mf.blocks[fast[j]].insts = append(fastBody, jump(nextFast))
				mf.blocks[cold[j]].insts = append(coldBody, jump(nextCold))
				mf.blocks[fast[j]].succs = []int32{nextFast}
				mf.blocks[cold[j]].succs = []int32{nextCold}
			}

			start = region.end

			if r+1 == len(regions) {
				mf.blocks[continuation].insts = append([]MInst(nil), original.insts[start:]...)
				mf.blocks[continuation].succs = append([]int32(nil), original.succs...)
			}
		}
	}

	if changed {
		for b := range mf.blocks {
			mf.blocks[b].preds = nil
		}

		for b := range mf.blocks {
			for _, succ := range mf.blocks[b].succs {
				mf.blocks[succ].preds = append(mf.blocks[succ].preds, int32(b))
			}
		}

		// Cloned terminators can now share an edge block, and a later
		// guard's cold successor also has the preceding cold copy as a
		// predecessor. Give allocation moves on those edges a home.
		n := len(mf.blocks)

		for b := 0; b < n; b++ {
			if len(mf.blocks[b].succs) < 2 {
				continue
			}

			for j, succ := range mf.blocks[b].succs {
				if len(mf.blocks[succ].preds) < 2 {
					continue
				}

				edge := int32(len(mf.blocks))
				jump := inst(mJmp)
				jump.target = succ
				mf.blocks = append(mf.blocks, MBlock{id: -1, depth: mf.blocks[b].depth, cold: mf.blocks[b].cold || mf.blocks[succ].cold, insts: []MInst{jump}, succs: []int32{succ}, preds: []int32{int32(b)}})
				mf.blocks[b].succs[j] = edge

				for k := range mf.blocks[b].insts {
					in := &mf.blocks[b].insts[k]

					switch in.op {
					case mJmp, mJcc:
						if in.target == succ {
							in.target = edge
						}
					case mSwitch:
						for t := range in.table {
							if in.table[t] == succ {
								in.table[t] = edge
							}
						}
					}
				}

				for k, pred := range mf.blocks[succ].preds {
					if pred == int32(b) {
						mf.blocks[succ].preds[k] = edge
					}
				}
			}
		}
	}
}

func (l *lowerer) tailCounter(offset int32, amount int64) MInst {
	in := inst(mCounter)
	in.mem = memAt(ctxReg, l.lay.tailProfile+int32(l.f.index)*tailCounterBytes+offset)
	in.imm = amount

	return in
}
