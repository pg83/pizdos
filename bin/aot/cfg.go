package main

// CFG analyses over the SSA form: reverse postorder, dominators and loop
// nesting. Wasm control flow is structured, so the graph is reducible and
// every edge to an earlier block in reverse postorder is a loop back edge.

type cfgInfo struct {
	order []int32
	// rpoIndex maps a block to its position in order, -1 if unreachable.
	rpoIndex []int32
	idom     []int32
	depth    []int32
	// header marks loop headers.
	header []bool
}

func (f *Function) rpo() []int32 {
	n := len(f.blocks)
	visited := make([]bool, n)
	post := make([]int32, 0, n)

	type frame struct {
		b int32
		i int
	}

	stack := []frame{{0, 0}}
	visited[0] = true

	for len(stack) > 0 {
		top := &stack[len(stack)-1]
		edges := f.blocks[top.b].term.edges

		if top.i < len(edges) {
			// Visit successors last to first so the first one directly
			// follows its block: the taken arm of an if, the target of a
			// branch.
			s := edges[len(edges)-1-top.i].block
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

	for i, j := 0, len(post)-1; i < j; i, j = i+1, j-1 {
		post[i], post[j] = post[j], post[i]
	}

	return post
}

func (f *Function) analyze() *cfgInfo {
	ci := &cfgInfo{order: f.rpo()}
	n := len(f.blocks)
	ci.rpoIndex = make([]int32, n)

	for i := range ci.rpoIndex {
		ci.rpoIndex[i] = -1
	}

	for i, b := range ci.order {
		ci.rpoIndex[b] = int32(i)
	}

	f.computePreds()
	ci.dominators(f)
	ci.loops(f)

	return ci
}

// dominators is Cooper, Harvey and Kennedy's iterative algorithm.
func (ci *cfgInfo) dominators(f *Function) {
	n := len(f.blocks)
	ci.idom = make([]int32, n)

	for i := range ci.idom {
		ci.idom[i] = -1
	}

	entry := ci.order[0]
	ci.idom[entry] = entry

	for changed := true; changed; {
		changed = false

		for _, b := range ci.order[1:] {
			newIdom := int32(-1)

			for _, p := range f.blocks[b].preds {
				if ci.idom[p] < 0 {
					continue
				}

				if newIdom < 0 {
					newIdom = p
				} else {
					newIdom = ci.intersect(p, newIdom)
				}
			}

			if newIdom != ci.idom[b] {
				ci.idom[b] = newIdom
				changed = true
			}
		}
	}
}

func (ci *cfgInfo) intersect(a, b int32) int32 {
	for a != b {
		for ci.rpoIndex[a] > ci.rpoIndex[b] {
			a = ci.idom[a]
		}

		for ci.rpoIndex[b] > ci.rpoIndex[a] {
			b = ci.idom[b]
		}
	}

	return a
}

func (ci *cfgInfo) dominates(a, b int32) bool {
	for {
		if a == b {
			return true
		}

		if ci.rpoIndex[b] <= ci.rpoIndex[a] {
			return false
		}

		b = ci.idom[b]
	}
}

func (ci *cfgInfo) loops(f *Function) {
	n := len(f.blocks)
	ci.depth = make([]int32, n)
	ci.header = make([]bool, n)
	var work []int32
	inLoop := make([]int32, n)

	for i := range inLoop {
		inLoop[i] = -1
	}

	for _, h := range ci.order {
		var latches []int32

		for _, p := range f.blocks[h].preds {
			if ci.rpoIndex[p] >= ci.rpoIndex[h] {
				latches = append(latches, p)
			}
		}

		if len(latches) == 0 {
			continue
		}

		ci.header[h] = true
		// Walk back from the latches to the header: every block on the way
		// is in the loop.
		inLoop[h] = h
		ci.depth[h]++
		work = append(work[:0], latches...)

		for len(work) > 0 {
			b := work[len(work)-1]
			work = work[:len(work)-1]

			if inLoop[b] == h || ci.rpoIndex[b] < 0 {
				continue
			}

			inLoop[b] = h
			ci.depth[b]++
			work = append(work, f.blocks[b].preds...)
		}
	}
}
