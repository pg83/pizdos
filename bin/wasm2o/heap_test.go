//go:build linux && amd64

package main

import (
	"math/rand/v2"
	"syscall"
	"testing"
)

// heapProcess is a process with a heap on a module's memory and nothing
// else.
func heapProcess(t *testing.T) *Process {
	i := load(t, &testModule{memMin: 1, memMax: 4096}, testImports{})
	p := &Process{inst: i}
	p.heap = newHeap(1024)

	return p
}

// aborts is whether f ends the process as abort does.
func aborts(f func()) (ended bool) {
	defer func() {
		if e := recover(); e != nil {
			x, ok := e.(*procExit)

			if !ok || x.status != int(syscall.SIGABRT) {
				panic(e)
			}

			ended = true
		}
	}()

	f()

	return false
}

// TestHeap runs the allocator on a module's memory, where only the
// program's data is. Blocks of every class and runs neither overlap nor
// change what the program wrote, however it wrote over the blocks it freed;
// a freed slot comes back first; a page whose slots are all freed goes back
// to its segment; a run grows and shrinks in place; alignments hold; a
// fork's copy is a heap of its own; and a pointer that is not a block in use
// ends the process.
func TestHeap(t *testing.T) {
	t.Run("blocks", func(t *testing.T) {
		p := heapProcess(t)
		rng := rand.New(rand.NewPCG(1, 2))

		type block struct {
			at, n uint32
			fill  byte
		}

		var live []block

		intact := func(b block) {
			for k, c := range p.span(b.at, b.n) {
				if c != b.fill {
					t.Fatalf("block %#x of %d bytes: byte %d is %#x, wrote %#x", b.at, b.n, k, c, b.fill)
				}
			}
		}

		for step := range 30000 {
			if len(live) > 0 && rng.IntN(5) < 2 {
				k := rng.IntN(len(live))
				b := live[k]
				intact(b)
				p.free(b.at)
				// the program goes on writing where it freed
				clear(p.span(b.at, b.n))
				live[k] = live[len(live)-1]
				live = live[:len(live)-1]

				continue
			}

			var n uint32

			switch r := rng.IntN(100); {
			case r < 70:
				n = 1 + rng.Uint32N(256)
			case r < 95:
				n = 1 + rng.Uint32N(smallLimit)
			default:
				n = smallLimit + 1 + rng.Uint32N(300_000)
			}

			at := p.malloc(n)
			_, _, size, ok := p.usedBlock(at)

			if at == 0 || at%16 != 0 || !ok || size < n {
				t.Fatalf("step %d: malloc(%d) = %#x, a block of %d (%v)", step, n, at, size, ok)
			}

			b := block{at, n, byte(step%251 + 1)}
			data := p.span(at, n)

			for k := range data {
				data[k] = b.fill
			}

			live = append(live, b)
		}

		for _, b := range live {
			intact(b)
		}
	})

	t.Run("reuse", func(t *testing.T) {
		p := heapProcess(t)
		a := p.malloc(40)
		p.malloc(40)
		p.free(a)

		if b := p.malloc(48); b != a {
			t.Fatalf("malloc(48) after free(%#x) = %#x", a, b)
		}

		// 16 KiB blocks, four to a page: three pages of them freed leave
		// the class's current page alone
		var big []uint32

		for range 12 {
			big = append(big, p.malloc(smallLimit))
		}

		for _, b := range big {
			p.free(b)
		}

		pages := map[int32]bool{}

		for _, b := range big {
			pages[int32((b-p.heap.base)>>heapPageShift)] = true
		}

		left := 0

		for page := range pages {
			if p.heap.pages[page].kind != pageFree {
				left++
			}
		}

		if len(pages) != 3 || left != 1 {
			t.Fatalf("16 KiB blocks on %d pages, %d of them not given back", len(pages), left)
		}
	})

	t.Run("runs", func(t *testing.T) {
		p := heapProcess(t)
		r := p.malloc(100_000)

		if (r-p.heap.base)%heapPageSize != 0 {
			t.Fatalf("a run at %#x", r)
		}

		copy(p.span(r, 5), "run!!")

		if g := p.realloc(r, 300_000); g != r || string(p.span(r, 5)) != "run!!" {
			t.Fatalf("realloc to 300000 moved %#x to %#x", r, g)
		}

		// beyond a segment, by segments the heap grows by at its end
		if g := p.realloc(r, 5*segmentSize+1); g != r || string(p.span(r, 5)) != "run!!" {
			t.Fatalf("realloc past a segment moved %#x to %#x", r, g)
		}

		if g := p.realloc(r, 70_000); g != r || p.heap.pages[(r-p.heap.base)>>heapPageShift].run != 2 {
			t.Fatalf("realloc to 70000 moved %#x to %#x", r, g)
		}

		// the pages it gave back take the next run, the heap as it was
		pages := len(p.heap.pages)

		if q := p.malloc(3 * heapPageSize); q < r+2*heapPageSize || q >= r+161*heapPageSize || len(p.heap.pages) != pages {
			t.Fatalf("a run after the shrink at %#x, %d pages then %d", q, pages, len(p.heap.pages))
		}

		if g := p.realloc(r, 0); g != 0 {
			t.Fatalf("realloc(..., 0) = %#x", g)
		}

		// a run with a page in use after it moves to grow
		q := heapProcess(t)
		a := q.malloc(2 * heapPageSize)
		b := q.malloc(heapPageSize)
		copy(q.span(a, 3), "abc")

		if b != a+2*heapPageSize {
			t.Fatalf("runs at %#x and %#x", a, b)
		}

		if g := q.realloc(a, 3*heapPageSize); g == a || string(q.span(g, 3)) != "abc" {
			t.Fatalf("realloc over a page in use: %#x to %#x", a, g)
		}

		if huge := p.malloc(heapMax + 1); huge != 0 {
			t.Fatalf("malloc(%d) = %#x", heapMax+1, huge)
		}
	})

	t.Run("align", func(t *testing.T) {
		p := heapProcess(t)

		for _, align := range []uint32{32, 64, 256, 4096, heapPageSize, 2 * heapPageSize, 1 << 20} {
			for _, n := range []uint32{1, 100, 5000, smallLimit, 70_000} {
				a := p.memalign(align, n)

				if a == 0 || a%align != 0 {
					t.Fatalf("memalign(%d, %d) = %#x", align, n, a)
				}

				clear(p.span(a, n))
				p.free(a)
			}
		}
	})

	t.Run("fork", func(t *testing.T) {
		p := heapProcess(t)
		p.malloc(100)
		c := &Process{inst: p.inst, heap: p.heap.clone()}

		// the same state, each its own
		if a, b := p.malloc(100), c.malloc(100); a != b {
			t.Fatalf("after the copy the heaps gave %#x and %#x", a, b)
		}
	})

	t.Run("invalid", func(t *testing.T) {
		p := heapProcess(t)
		a := p.malloc(32)
		r := p.malloc(200_000)
		freed := p.malloc(32)
		p.free(freed)
		h := p.heap

		for _, c := range []struct {
			what string
			f    func()
		}{
			{"inside a block", func() { p.free(a + 16) }},
			{"a freed block", func() { p.free(freed) }},
			{"a slot never handed out", func() { p.free(a + 64*32) }},
			{"a run's second page", func() { p.free(r + heapPageSize) }},
			{"below the heap", func() { p.free(h.base - 16) }},
			{"past the heap", func() { p.free(h.address(int32(len(h.pages)))) }},
			{"realloc of a freed block", func() { p.realloc(freed, 10) }},
			{"the size of a freed block", func() { p.libcMalloc_usable_size(freed) }},
		} {
			if !aborts(c.f) {
				t.Errorf("%s: the process goes on", c.what)
			}
		}

		p.free(a)
		p.free(r)
	})
}
