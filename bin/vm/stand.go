//go:build linux && amd64

package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

// vm stand runs the work of the stand (dev/stand) over files with a
// pure module of impulse-desktop/decode compiled by vm: what
// dev/stand/decode.c does with the module's sources compiled natively. One
// line per file says the result; native pixels may differ:
//
//	im              decode(file, name): WxH and the digest of the image, or fail
//	pdf, djvu, svg  open(file), then each of the first standPages pages rendered
//	                with its longer side standPixels: the page count and the
//	                digest of the renders, or fail
//	magic           magic_mime(file): the type, or fail
//
// A trap is "trap", and the next file gets a new instance. A digest is
// FNV-1a of 64 bits over each image's width and height, little endian, and
// its RGBA bytes; a page of no size, or one whose render fails, adds a
// zero word. The work's time, less host hashing, goes to standard error.

const (
	standPages  = 16
	standPixels = 1600
)

type fnv64 uint64

func newFNV() fnv64 {
	return 0xcbf29ce484222325
}

func (h *fnv64) write(b []byte) {
	for _, c := range b {
		*h = (*h ^ fnv64(c)) * 0x100000001b3
	}
}

func (h *fnv64) word(v uint32) {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], v)
	h.write(b[:])
}

// standImports: the modules import nothing.
type standImports struct{}

func (standImports) function(*ImportMeta) any         { return nil }
func (standImports) global(*ImportMeta) uintptr       { return 0 }
func (standImports) memory(*ImportMeta) *LinearMemory { return nil }
func (standImports) table(*ImportMeta) uintptr        { return 0 }

// module is an instance and the exports the work calls.
type module struct {
	i       *Instance
	malloc  func(uintptr, uint32) uint32
	free    func(uintptr, uint32)
	call    map[string]any
	untimed time.Duration
}

func (m *module) fn(name string) any {
	f, ok := m.call[name]

	if !ok {
		v, _ := m.i.export(name)
		f = v.Interface()
		m.call[name] = f
	}

	return f
}

func (m *module) mem() []byte {
	return m.i.memoryBytes()
}

// put copies b into the module's memory.
func (m *module) put(b []byte) uint32 {
	p := m.malloc(m.i.ctx(), uint32(len(b)))

	if p == 0 && len(b) > 0 {
		throwFmt("malloc of %d bytes failed", len(b))
	}

	moveMemory(m.mem()[p:], b)

	return p
}

// image adds the image block at p to h and frees it.
func (m *module) image(h *fnv64, p uint32) (uint32, uint32) {
	mem := m.mem()
	w, ht := binary.LittleEndian.Uint32(mem[p:]), binary.LittleEndian.Uint32(mem[p+4:])
	start := time.Now()
	h.write(mem[p : p+8])
	h.write(mem[p+8 : uint64(p)+8+uint64(w)*uint64(ht)*4])
	m.untimed += time.Since(start)
	m.free(m.i.ctx(), p)

	return w, ht
}

// scaled is a page's size in pixels for a render whose longer side is
// standPixels.
func scaled(w, h float32) (uint32, uint32) {
	k := standPixels / math.Max(float64(w), float64(h))

	return uint32(max(1, math.Floor(float64(w)*k+0.5))), uint32(max(1, math.Floor(float64(h)*k+0.5)))
}

func (m *module) work(kind, name string, data []byte) string {
	ctx := m.i.ctx()

	switch kind {
	case "im":
		p, n := m.put(data), m.put([]byte(name))
		r := m.fn("decode").(func(uintptr, uint32, uint32, uint32, uint32) uint32)(ctx, p, uint32(len(data)), n, uint32(len(name)))
		m.free(ctx, p)
		m.free(ctx, n)

		if r == 0 {
			return "fail"
		}

		h := newFNV()
		w, ht := m.image(&h, r)

		return fmt.Sprintf("%dx%d %016x", w, ht, uint64(h))
	case "pdf", "djvu", "svg":
		p := m.put(data)
		doc := m.fn(kind+"_open").(func(uintptr, uint32, uint32) uint32)(ctx, p, uint32(len(data)))

		if doc == 0 {
			m.free(ctx, p)

			return "fail"
		}

		pages := m.fn(kind+"_pages").(func(uintptr, uint32) uint32)(ctx, doc)
		width := m.fn(kind + "_width").(func(uintptr, uint32, uint32) float32)
		height := m.fn(kind + "_height").(func(uintptr, uint32, uint32) float32)
		render := m.fn(kind + "_render").(func(uintptr, uint32, uint32, uint32, uint32) uint32)
		h := newFNV()
		geometry := newFNV()

		for page := range min(pages, standPages) {
			w, ht := width(ctx, doc, page), height(ctx, doc, page)

			if !(w > 0 && ht > 0) {
				h.word(0)
				geometry.word(0)

				continue
			}

			pw, ph := scaled(w, ht)

			if r := render(ctx, doc, page, pw, ph); r != 0 {
				iw, ih := m.image(&h, r)
				geometry.word(iw)
				geometry.word(ih)
			} else {
				h.word(0)
				geometry.word(0)
			}
		}

		m.fn(kind+"_close").(func(uintptr, uint32))(ctx, doc)
		m.free(ctx, p)

		return fmt.Sprintf("%d %016x %016x", pages, uint64(geometry), uint64(h))
	case "magic":
		p := m.put(data)
		r := m.fn("magic_mime").(func(uintptr, uint32, uint32) uint32)(ctx, p, uint32(len(data)))
		m.free(ctx, p)

		if r == 0 {
			return "fail"
		}

		mem := m.mem()
		end := r

		for mem[end] != 0 {
			end++
		}

		return string(mem[r:end])
	}

	throwFmt("no work of kind %s", kind)

	return ""
}

func standCommand(args []string) int {
	repeat := 1

	if len(args) >= 2 && args[0] == "--repeat" {
		repeat = throw2(strconv.Atoi(args[1]))
		args = args[2:]
	}

	if repeat < 1 || repeat > 10000 {
		throwFmt("repeat must be in 1..10000")
	}

	if len(args) < 3 {
		usage()
	}

	kind, path, files := args[0], args[1], args[2:]
	obj := readObject(throw2(os.ReadFile(path)))
	code := link(path, obj, standImports{})
	var m *module

	fresh := func() {
		if m != nil {
			m.i.release()
		}

		i := newInstance(code, standImports{}, runStack)
		i.start()
		malloc, _ := i.export("malloc")
		free, _ := i.export("free")
		m = &module{i: i, malloc: malloc.Interface().(func(uintptr, uint32) uint32), free: free.Interface().(func(uintptr, uint32)), call: map[string]any{}}
	}

	fresh()

	defer func() { m.i.release() }()

	total := time.Duration(0)

	for _, file := range files {
		data := throw2(os.ReadFile(file))
		name := filepath.Base(file)
		var result string
		var want string

		for run := range repeat {
			m.untimed = 0
			start := time.Now()

			if code := trapOf(func() { result = m.work(kind, name, data) }); code >= 0 {
				result = "trap"
			}

			total += time.Since(start) - m.untimed

			if result == "trap" {
				fresh()
			}

			if run == 0 {
				want = result
			} else if result != want {
				throwFmt("%s: result changed on repetition %d", name, run+1)
			}
		}

		fmt.Printf("%s %s\n", name, result)
	}

	fmt.Fprintf(os.Stderr, "time %.6f\n", total.Seconds())

	return 0
}

// trapOf runs f and returns the code of the trap it ends with, or -1.
func trapOf(f func()) (code int) {
	defer func() {
		r := recover()

		if trap, ok := r.(*Trap); ok {
			code = int(trap.code)

			return
		}

		if r != nil {
			panic(r)
		}
	}()

	f()

	return -1
}
